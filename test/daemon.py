#!/usr/bin/env python3
# The daemon must keep pause policy in control of the renderer after a load
# fails: the renderer keeps the previous media, so pause still has to reach it.
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

MOCK = """#!/usr/bin/env python3
import json
import os
import socket
import time
import sys
from pathlib import Path

root = Path(os.environ["XDG_RUNTIME_DIR"])
path = Path(sys.argv[2]) if len(sys.argv) > 2 and sys.argv[1] == "--socket" else root / "owe/render.sock"
path.parent.mkdir(parents=True, exist_ok=True)
path.unlink(missing_ok=True)
server = socket.socket(socket.AF_UNIX)
server.bind(str(path))
server.listen()
log = (root / "commands.jsonl").open("a")
state = {"status": "ok", "paused": False, "path": "", "kind": "", "outputs": 1,
         "ready": False, "error": "", "has_transition": False,
         "transition_busy": False, "transition_done": False}
pending = None
intro_end = None
transition_end = None
while True:
    client, _ = server.accept()
    with client:
        data = b""
        while not data.endswith(b"\\n"):
            chunk = client.recv(65536)
            if not chunk:
                break
            data += chunk
        if not data:
            continue
        request = json.loads(data)
        log.write(json.dumps(request) + "\\n")
        log.flush()
        command = request["cmd"]
        reply = {"status": "ok"}
        if command == "load":
            intro_end = time.monotonic() + 0.3 if request.get("once") else None
            state["eof"] = False
            state.update(has_transition=bool(request.get("from")), transition_busy=False,
                         transition_done=False)
            if "reject" in request["path"]:
                reply = {"status": "error", "message": "rejected"}
            elif "slow" in request["path"]:
                if request.get("async"):
                    pending = (time.monotonic() + 7, request["path"], request["kind"])
                else:
                    time.sleep(7)
                    state.update(path=request["path"], kind=request["kind"], ready=True)
            else:
                pending = None
                state.update(path=request["path"], kind=request["kind"], error="")
                if "bad" in request["path"]:
                    state["error"] = "decode failed"
                    state["ready"] = False
                else:
                    state["ready"] = request["kind"] == "video"
        elif command == "pause":
            state["paused"] = True
        elif command == "resume":
            state["paused"] = False
        elif command == "cancel-load":
            pending = None
        elif command == "intro-show":
            pass
        elif command == "intro-finish":
            state.update(has_transition=True, transition_busy=False, transition_done=False)
            transition_end = time.monotonic() + 0.3
        elif command == "status":
            if intro_end and time.monotonic() >= intro_end:
                state["eof"] = True
            if transition_end and time.monotonic() >= transition_end:
                state["transition_done"] = True
            if pending and time.monotonic() >= pending[0]:
                state.update(path=pending[1], kind=pending[2], ready=True)
                pending = None
            reply = dict(state)
            if pending:
                reply["ready"] = False
        try:
            client.sendall(json.dumps(reply).encode() + b"\\n")
        except BrokenPipeError:
            pass
"""


def call(socket_path, command, **fields):
    with socket.socket(socket.AF_UNIX) as client:
        client.settimeout(10)
        client.connect(str(socket_path))
        client.sendall(json.dumps(dict(cmd=command, **fields)).encode() + b"\n")
        data = b""
        while not data.endswith(b"\n"):
            chunk = client.recv(65536)
            if not chunk:
                raise RuntimeError(f"{command}: daemon closed the connection")
            data += chunk
        return json.loads(data)


def check(condition, message, detail=""):
    if not condition:
        print(f"FAIL: {message} {detail}".rstrip(), file=sys.stderr)
        sys.exit(1)


def main():
    owed = Path(sys.argv[1]).resolve()
    owe = Path(sys.argv[2]).resolve()
    check(owed.exists(), "owed binary exists", str(owed))
    with tempfile.TemporaryDirectory(prefix="owe-daemon-") as directory:
        root = Path(directory)
        shutil.copy2(owed, root / "owed")
        mock = root / "owe-render"
        mock.write_text(MOCK)
        mock.chmod(0o755)
        current = root / "omarchy/current"
        current.mkdir(parents=True)
        (root / "good.mp4").write_bytes(b"test")
        (root / "reject.mp4").write_bytes(b"not a video")
        (root / "bad.mp4").write_bytes(b"test")
        (root / "still.png").write_bytes(b"test")
        (root / "slow.png").write_bytes(b"test")
        (current / "background").symlink_to(root / "good.mp4")
        bin_dir = root / "bin"
        bin_dir.mkdir()
        shell_stub = bin_dir / "omarchy-shell"
        shell_stub.write_text(
            "#!/bin/sh\nprintf '%s\\n' \"$*\" >>\"$OWE_TEST_SHELL_LOG\"\n"
            "if [ \"$4\" = true ] && [ -e \"$OWE_TEST_SHELL_FAIL\" ]; then exit 1; fi\n"
            "exit 0\n")
        shell_stub.chmod(0o755)
        shell_log = root / "shell.log"
        env = os.environ.copy()
        env.update(XDG_RUNTIME_DIR=str(root), XDG_CONFIG_HOME=str(root),
                   XDG_STATE_HOME=str(root), XDG_CACHE_HOME=str(root),
                   DBUS_SYSTEM_BUS_ADDRESS=f"unix:path={root}/no-system-bus", OWE_DRM_DPMS="0")
        env["OWE_TEST_SHELL_LOG"] = str(shell_log)
        env["OWE_TEST_SHELL_FAIL"] = str(root / "shell-fail")
        env["PATH"] = f"{bin_dir}:{env.get('PATH', '')}"
        env.pop("HYPRLAND_INSTANCE_SIGNATURE", None)
        daemon_socket = root / "custom/control.sock"
        render_socket = root / "custom/render.sock"
        cli = [str(owe), "--socket", str(daemon_socket)]
        with (root / "daemon.log").open("w") as log:
            daemon = subprocess.Popen([str(root / "owed"), "--socket", str(daemon_socket)],
                                      env=env, stdout=log, stderr=log)
            try:
                deadline = time.monotonic() + 10
                while not render_socket.exists():
                    if daemon.poll() is not None or time.monotonic() > deadline:
                        print((root / "daemon.log").read_text(), file=sys.stderr)
                        check(False, "renderer socket appeared")
                    time.sleep(0.05)
                deadline = time.monotonic() + 10
                loaded = False
                commands_path = root / "commands.jsonl"
                while time.monotonic() < deadline:
                    lines = commands_path.read_text().splitlines() if commands_path.exists() else []
                    if any(json.loads(line).get("cmd") == "load" for line in lines):
                        loaded = True
                        break
                    time.sleep(0.05)
                check(loaded, "daemon loaded the initial video")
                disable = False
                deadline = time.monotonic() + 8
                while time.monotonic() < deadline:
                    text = shell_log.read_text() if shell_log.exists() else ""
                    if "setPluginEnabled omarchy.background false" in text:
                        disable = True
                        break
                    time.sleep(0.1)
                check(disable, "daemon disabled the shell background for the video")
                result = subprocess.run(cli + ["status"], env=env, capture_output=True, text=True, timeout=5)
                check(result.returncode == 0 and json.loads(result.stdout)["status"] == "ok",
                      "the CLI reaches a custom daemon socket", result.stderr)
                check(json.loads(result.stdout)["drm_dpms"] is False,
                      "the daemon reports the DRM diagnostic override")
                result = subprocess.run(cli + ["render", '{"cmd":"status"}'], env=env,
                                        capture_output=True, text=True, timeout=5)
                check(result.returncode == 0 and json.loads(result.stdout)["path"].endswith("good.mp4"),
                      "the CLI finds the renderer beside the custom daemon socket", result.stderr)

                check(call(daemon_socket, "set", path=str(root / "reject.mp4"))["status"] == "ok",
                      "daemon accepts a video the renderer rejects")
                time.sleep(0.3)
                check(call(daemon_socket, "pause")["status"] == "ok", "pause command accepted")
                time.sleep(0.3)
                renderer = call(render_socket, "status")
                status = call(daemon_socket, "status")
                check(status["paused"] is True, "daemon reports paused")
                check(status["manual_pause"] is True, "daemon reports manual pause")
                check(renderer["paused"] is True, "pause reached the renderer after a failed load",
                      json.dumps({"renderer": renderer, "daemon": status}))
                check(renderer["kind"] == "video", "renderer kept the previous video")

                check(call(daemon_socket, "idle-pause")["status"] == "ok", "idle pause accepted")
                time.sleep(0.2)
                check(call(daemon_socket, "idle-resume")["status"] == "ok", "idle resume accepted")
                time.sleep(0.3)
                status = call(daemon_socket, "status")
                check(status["idle_pause"] is False and status["manual_pause"] is True,
                      "idle resume keeps the manual pause", json.dumps(status))
                renderer = call(render_socket, "status")
                check(renderer["paused"] is True, "renderer stays paused after idle resume")

                check(call(daemon_socket, "set", path=str(root / "bad.mp4"))["status"] == "ok",
                      "daemon accepts a video that fails to decode")
                recovered = False
                deadline = time.monotonic() + 8
                while time.monotonic() < deadline:
                    lines = commands_path.read_text().splitlines()
                    loads = [json.loads(line).get("path", "") for line in lines
                             if json.loads(line).get("cmd") == "load"]
                    if len(loads) >= 2 and loads[-1].endswith("good.mp4"):
                        recovered = True
                        break
                    time.sleep(0.1)
                check(recovered, "daemon recovered to the last good video")
                status = {}
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline:
                    status = call(daemon_socket, "status")
                    if status["media_ready"] and status["loaded_path"].endswith("good.mp4"):
                        break
                    time.sleep(0.1)
                check(status["failed_path"].endswith("bad.mp4"), "failed path reported",
                      json.dumps(status))
                check(status["media_ready"] is True and status["loaded_path"].endswith("good.mp4"),
                      "last good media is loaded and ready", json.dumps(status))

                (root / "shell-fail").touch()
                check(call(daemon_socket, "set", path=str(root / "slow.png"))["status"] == "ok",
                      "daemon accepts a slow fallback still")
                deadline = time.monotonic() + 2
                while time.monotonic() < deadline:
                    start = time.monotonic()
                    status = call(daemon_socket, "status")
                    check(time.monotonic() - start < 0.5, "status stays responsive during still decode")
                    if status["loaded_path"].endswith("slow.png") and not status["media_ready"]:
                        break
                    time.sleep(0.05)
                check(status["loaded_path"].endswith("slow.png") and not status["media_ready"],
                      "the daemon waits for asynchronous still completion", json.dumps(status))
                start = time.monotonic()
                check(call(daemon_socket, "pause")["status"] == "ok", "pause accepted during still decode")
                check(time.monotonic() - start < 0.5, "pause does not wait for still decode")
                deadline = time.monotonic() + 9
                while time.monotonic() < deadline:
                    status = call(daemon_socket, "status")
                    if status["media_ready"]:
                        break
                    time.sleep(0.1)
                check(not status["failed_path"] and status["loaded_path"].endswith("slow.png") and status["media_ready"],
                      "a still can complete after the old five-second IPC timeout", json.dumps(status))
                (root / "shell-fail").unlink()
                # A new still selection must retry the shell after the earlier failure.

                check(call(daemon_socket, "set", path=str(root / "still.png"))["status"] == "ok",
                      "daemon accepts a still")
                enabled = False
                deadline = time.monotonic() + 8
                while time.monotonic() < deadline:
                    text = shell_log.read_text() if shell_log.exists() else ""
                    if "setPluginEnabled omarchy.background true" in text:
                        enabled = True
                        break
                    time.sleep(0.1)
                check(enabled, "daemon handed a still back to the shell")
                status = {}
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline:
                    status = call(daemon_socket, "status")
                    if status["engine"] == "shell":
                        break
                    time.sleep(0.1)
                check(status["engine"] == "shell", "daemon reports the shell engine",
                      json.dumps(status))
                renderer_gone = False
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline:
                    try:
                        call(render_socket, "status")
                    except (ConnectionRefusedError, FileNotFoundError, OSError):
                        renderer_gone = True
                        break
                    time.sleep(0.1)
                check(renderer_gone, "renderer stopped while the shell draws the still")

                result = subprocess.run(cli + ["intro", "good.mp4"], env=env, cwd=root,
                                        capture_output=True, text=True, timeout=8)
                check(result.returncode == 0, "the CLI resolves relative intro paths and detects EOF", result.stderr)
                intro_commands = [json.loads(line) for line in commands_path.read_text().splitlines()]
                intro_load = next(command for command in reversed(intro_commands)
                                  if command.get("cmd") == "load" and command.get("once"))
                check(intro_load.get("from") == str(root / "still.png"),
                      "the intro prepares from the exact shell still", json.dumps(intro_load))
                check(any(command.get("cmd") == "intro-show" for command in intro_commands) and
                      any(command.get("cmd") == "intro-finish" and command.get("path") == str(root / "still.png")
                          for command in intro_commands),
                      "the daemon reveals the intro over its still and brackets both transitions",
                      json.dumps(intro_commands))

                previous_loads = [json.loads(line) for line in commands_path.read_text().splitlines()
                                  if json.loads(line).get("cmd") == "load"]
                check(call(daemon_socket, "resume")["status"] == "ok", "resume command accepted")
                check(call(daemon_socket, "set", path=str(root / "good.mp4"))["status"] == "ok",
                      "daemon accepts a video after a shell still")
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline:
                    loads = [json.loads(line) for line in commands_path.read_text().splitlines()
                             if json.loads(line).get("cmd") == "load"]
                    if len(loads) > len(previous_loads):
                        break
                    time.sleep(0.05)
                check(len(loads) == len(previous_loads) + 1,
                      "still-to-video handoff sends one load", json.dumps(loads))
                check(loads[-1].get("from") == str(root / "still.png"),
                      "renderer receives the outgoing shell still", json.dumps(loads[-1]))
                check(loads[-1]["path"] == str(root / "good.mp4") and loads[-1]["kind"] == "video",
                      "transition loads the selected video", json.dumps(loads[-1]))
                check(not any(load["kind"] == "still" and load["path"].endswith("/still.png") for load in loads),
                      "the shell retains normal still wallpaper ownership")
                print("daemon still-to-video transition command passed")
            finally:
                daemon.send_signal(signal.SIGTERM)
                try:
                    daemon.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    daemon.kill()
                    daemon.wait()
    print("daemon pause after failed load passed")


if __name__ == "__main__":
    main()

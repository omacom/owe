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
from pathlib import Path

root = Path(os.environ["XDG_RUNTIME_DIR"])
path = root / "owe/render.sock"
path.parent.mkdir(parents=True, exist_ok=True)
path.unlink(missing_ok=True)
server = socket.socket(socket.AF_UNIX)
server.bind(str(path))
server.listen()
log = (root / "commands.jsonl").open("a")
state = {"status": "ok", "paused": False, "path": "", "kind": "", "outputs": 1, "ready": False, "error": ""}
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
            if "reject" in request["path"]:
                reply = {"status": "error", "message": "rejected"}
            else:
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
        elif command == "status":
            reply = state
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
        (root / "reject.png").write_bytes(b"not an image")
        (current / "background").symlink_to(root / "good.mp4")
        env = os.environ.copy()
        env.update(XDG_RUNTIME_DIR=str(root), XDG_CONFIG_HOME=str(root),
                   XDG_STATE_HOME=str(root), XDG_CACHE_HOME=str(root))
        env.pop("HYPRLAND_INSTANCE_SIGNATURE", None)
        daemon_socket = root / "owe/owed.sock"
        render_socket = root / "owe/render.sock"
        with (root / "daemon.log").open("w") as log:
            daemon = subprocess.Popen([str(root / "owed")], env=env, stdout=log, stderr=log)
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

                check(call(daemon_socket, "set", path=str(root / "reject.png"))["status"] == "ok",
                      "daemon accepts a file the renderer rejects")
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

                (root / "bad.mp4").write_bytes(b"test")
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

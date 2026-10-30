#!/usr/bin/env python3
# End-to-end playback check. It starts a headless sway session, the real owed
# daemon, the real owe-render renderer and the owe CLI, and reads the pixels on
# the output with grim. Run it through test/e2e/run.sh.
#
# Usage: e2e.py BIN_DIR --api opengl|gles [--gpu] [--hwdec NAME]
#
# --api gles caps desktop OpenGL at 3.1 with MESA_GL_VERSION_OVERRIDE. The
# renderer must then fall back to OpenGL ES, as on Raspberry Pi V3D and Mali.
#
# --gpu renders on /dev/dri/renderD128 instead of llvmpipe, as in the virtual
# machine of test/e2e/run-gpu.sh. The check fails if llvmpipe draws the frames.
#
# --hwdec NAME sets OWE_HWDEC to an mpv decoder name, such as vaapi. The check
# fails if mpv does not decode both clips with that decoder.
import argparse
import json
import os
import platform
import re
import signal
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

WIDTH, HEIGHT = 1280, 720
RENDER_NODE = "/dev/dri/renderD128"
ORANGE = (224, 128, 32)
BLUE = (32, 96, 192)
TOLERANCE = 24
# Software rendering under QEMU is slow, so the waits are long.
TIMEOUT = float(os.environ.get("OWE_E2E_TIMEOUT", "90"))
GL_FAILURES = ("shader compile failed", "program link failed", "OpenGL render failed",
               "eglMakeCurrent failed", "eglSwapBuffers failed", "eglCreateWindowSurface failed",
               "No provider of")


def check(condition, message, detail=""):
    if not condition:
        print(f"FAIL: {message} {detail}".rstrip(), file=sys.stderr)
        raise SystemExit(1)
    print(f"ok: {message}")


def wait_for(predicate, message, timeout=TIMEOUT, interval=0.25):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        try:
            last = predicate()
        except (OSError, ValueError, subprocess.SubprocessError) as error:
            last = error
        if last and not isinstance(last, Exception):
            print(f"ok: {message}")
            return last
        time.sleep(interval)
    check(False, message, f"(last result: {last!r})")


def request(path, payload, timeout=10):
    with socket.socket(socket.AF_UNIX) as client:
        client.settimeout(timeout)
        client.connect(str(path))
        client.sendall((json.dumps(payload) + "\n").encode())
        data = b""
        while not data.endswith(b"\n"):
            chunk = client.recv(65536)
            if not chunk:
                break
            data += chunk
    return json.loads(data)


def read_ppm(path):
    data = Path(path).read_bytes()
    match = re.match(rb"P6\s+(\d+)\s+(\d+)\s+255\s", data)
    if not match:
        raise ValueError(f"{path} is not a binary PPM")
    width, height = int(match.group(1)), int(match.group(2))
    pixels = data[match.end():]
    if len(pixels) != width * height * 3:
        raise ValueError(f"{path} has {len(pixels)} pixel bytes for {width}x{height}")
    return width, height, pixels


def pixel(image, x, y):
    width, _, pixels = image
    offset = (y * width + x) * 3
    return tuple(pixels[offset:offset + 3])


def near(color, expected):
    return all(abs(a - b) <= TOLERANCE for a, b in zip(color, expected))


def right_half(image):
    """Return the moving testsrc2 half of the frame."""
    width, height, pixels = image
    rows = [pixels[(y * width + width // 2) * 3:(y * width + width) * 3]
            for y in range(height // 4, 3 * height // 4)]
    return b"".join(rows)


def make_media(root):
    """Write an H.264 and an HEVC clip. The left half of each frame has one
    flat color, so a channel swap or a black frame fails the check. The right
    half moves, so a frozen frame fails the check."""
    clips = {"orange.mp4": (ORANGE, ["-c:v", "libx264", "-preset", "ultrafast"]),
             "blue.mp4": (BLUE, ["-c:v", "libx265", "-preset", "ultrafast", "-tag:v", "hvc1",
                                 "-x265-params", "log-level=error"])}
    for name, (color, codec) in clips.items():
        hex_color = "0x%02X%02X%02X" % color
        subprocess.run(
            ["ffmpeg", "-v", "error", "-y",
             "-f", "lavfi", "-i", f"color=c={hex_color}:s=320x360:r=30:d=8",
             "-f", "lavfi", "-i", "testsrc2=s=320x360:r=30:d=8",
             "-filter_complex", "[0][1]hstack,format=yuv420p", *codec, str(root / name)],
            check=True, timeout=600)
    return root / "orange.mp4", root / "blue.mp4"


def process(pid):
    """Return the name, state and parent of a process, or None if it is gone."""
    try:
        text = Path(f"/proc/{pid}/stat").read_text()
    except OSError:
        return None
    rest = text[text.rindex(")") + 2:].split()
    return text[text.index("(") + 1:text.rindex(")")], rest[0], int(rest[1])


def renderer_pid(parent):
    for entry in Path("/proc").iterdir():
        if entry.name.isdigit() and (info := process(entry.name)):
            if info[0] == "owe-render" and info[2] == parent:
                return int(entry.name)
    return None


def start_sway(root, run_dir, gpu):
    config = root / "sway.conf"
    config.write_text(f"xwayland disable\noutput HEADLESS-1 resolution {WIDTH}x{HEIGHT} position 0 0\n")
    env = os.environ.copy()
    env.update(XDG_RUNTIME_DIR=str(run_dir), WLR_BACKENDS="headless",
               WLR_HEADLESS_OUTPUTS="1", WLR_LIBINPUT_NO_DEVICES="1")
    if gpu:
        env.update(WLR_RENDERER="gles2", WLR_RENDER_DRM_DEVICE=RENDER_NODE)
    else:
        env["WLR_RENDERER"] = "pixman"
    env.pop("WAYLAND_DISPLAY", None)
    log = (root / "sway.log").open("w")
    sway = subprocess.Popen(["sway", "-c", str(config)], env=env, stdout=log, stderr=log)

    def socket_name():
        if sway.poll() is not None:
            raise SystemExit(f"FAIL: sway exited\n{(root / 'sway.log').read_text()}")
        names = [p.name for p in run_dir.glob("wayland-*") if not p.name.endswith(".lock")]
        return names[0] if names else None

    return sway, wait_for(socket_name, "sway created a Wayland socket", timeout=60)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("bin_dir", type=Path)
    parser.add_argument("--api", choices=("opengl", "gles"), required=True)
    parser.add_argument("--gpu", action="store_true")
    parser.add_argument("--hwdec", default="")
    args = parser.parse_args()
    bin_dir = args.bin_dir.resolve()

    # A cleanup error must not hide the result of the checks.
    with tempfile.TemporaryDirectory(prefix="owe-e2e-", ignore_cleanup_errors=True) as directory:
        root = Path(directory)
        run_dir = root / "run"
        run_dir.mkdir(mode=0o700)
        state = root / "state"
        (state / "omarchy/current").mkdir(parents=True)
        shim = root / "shim"
        shim.mkdir()
        # owed hands the background layer to omarchy-shell. The session has none.
        (shim / "omarchy-shell").write_text("#!/bin/sh\nexit 0\n")
        (shim / "omarchy-shell").chmod(0o755)

        orange, blue = make_media(root)
        (state / "omarchy/current/background").symlink_to(orange)

        sway, display = start_sway(root, run_dir, args.gpu)
        env = os.environ.copy()
        env.update(XDG_RUNTIME_DIR=str(run_dir), WAYLAND_DISPLAY=display,
                   XDG_CONFIG_HOME=str(root / "config"), XDG_STATE_HOME=str(state),
                   XDG_CACHE_HOME=str(root / "cache"),
                   DBUS_SYSTEM_BUS_ADDRESS=f"unix:path={root}/no-system-bus", OWE_DRM_DPMS="0",
                   PATH=f"{shim}:{bin_dir}:{env.get('PATH', '')}")
        env.pop("HYPRLAND_INSTANCE_SIGNATURE", None)
        if not args.gpu:
            env["LIBGL_ALWAYS_SOFTWARE"] = "1"
        if args.hwdec:
            env["OWE_HWDEC"] = args.hwdec
        if args.api == "gles":
            env["MESA_GL_VERSION_OVERRIDE"] = "3.1"
        render_socket = run_dir / "owe/render.sock"
        daemon_log = root / "owed.log"
        daemon = subprocess.Popen([str(bin_dir / "owed"), "--verbose"], env=env,
                                  stdout=daemon_log.open("w"), stderr=subprocess.STDOUT)
        def capture():
            path = root / "capture.ppm"
            subprocess.run(["grim", "-t", "ppm", str(path)], env=env, check=True, timeout=60)
            return read_ppm(path)

        def status():
            return request(render_socket, {"cmd": "status"})

        def owe(*command):
            result = subprocess.run([str(bin_dir / "owe"), *command], env=env,
                                    capture_output=True, text=True, timeout=30)
            check(result.returncode == 0, f"owe {' '.join(command)} succeeded", result.stderr)
            return result.stdout

        passed = False
        try:
            print(f"machine: {platform.machine()}, api: {args.api}, gpu: {args.gpu}")
            wait_for(lambda: render_socket.exists(), "owed started owe-render")
            playing = wait_for(lambda: (s := status())["has_video"] and s["time_pos"] > 0 and s,
                               "the renderer plays the H.264 clip")
            check(playing["error"] == "", "the renderer reports no playback error", playing["error"])
            print(f"hwdec: {playing['hwdec']}")
            if args.hwdec:
                check(playing["hwdec"] == args.hwdec, f"mpv decodes the H.264 clip with {args.hwdec}",
                      playing["hwdec"])

            image = wait_for(lambda: (i := capture()) and near(pixel(i, WIDTH // 4, HEIGHT // 2), ORANGE) and i,
                             "the output shows the clip's orange half")
            check(image[:2] == (WIDTH, HEIGHT), "the capture covers the whole output", str(image[:2]))
            first = right_half(image)
            wait_for(lambda: right_half(capture()) != first, "the frames on the output change")

            before, start = status(), time.monotonic()
            time.sleep(5)
            after, elapsed = status(), time.monotonic() - start
            check(after["swaps"] > before["swaps"], "the renderer presents new frames")
            check(after["swap_failures"] == 0, "no buffer swap failed", str(after["swap_failures"]))
            print(f"presented: {(after['swaps'] - before['swaps']) / elapsed:.1f} frames/s, "
                  f"playback speed: {(after['time_pos'] - before['time_pos']) / elapsed:.2f}x")

            owe("pause")
            wait_for(lambda: status()["paused"], "the renderer paused")
            time.sleep(1)
            held, held_pos = right_half(capture()), status()["time_pos"]
            time.sleep(1.5)
            check(right_half(capture()) == held, "the output holds the frame while paused")
            check(status()["time_pos"] == held_pos, "the playback position holds while paused")
            owe("resume")
            wait_for(lambda: not status()["paused"], "the renderer resumed")

            snapshot = root / "snapshot.ppm"
            reply = request(render_socket, {"cmd": "snapshot", "path": str(snapshot)}, timeout=60)
            check(reply["status"] == "ok", "the renderer saved a snapshot", json.dumps(reply))
            frame = read_ppm(snapshot)
            check(near(pixel(frame, frame[0] // 4, frame[1] // 2), ORANGE),
                  "the snapshot has the clip's colors", str(pixel(frame, frame[0] // 4, frame[1] // 2)))

            live = subprocess.run([str(bin_dir / "owe-live-test")], env=env, capture_output=True,
                                  text=True, timeout=120)
            check(live.returncode == 0, "owe-live-test passed", live.stdout + live.stderr)

            owe("set", str(blue))
            wait_for(lambda: status()["path"] == str(blue), "the daemon loaded the HEVC clip")
            wait_for(lambda: near(pixel(capture(), WIDTH // 4, HEIGHT // 2), BLUE),
                     "the output shows the HEVC clip's blue half")
            switched = status()
            check(switched["error"] == "", "the HEVC clip plays without error", switched["error"])
            if args.hwdec:
                check(switched["hwdec"] == args.hwdec, f"mpv decodes the HEVC clip with {args.hwdec}",
                      switched["hwdec"])
            check(json.loads(owe("status"))["status"] == "ok", "the daemon reports ok")

            renderer = renderer_pid(daemon.pid)
            check(renderer, "owed is the parent of owe-render")
            daemon.send_signal(signal.SIGTERM)
            check(daemon.wait(timeout=30) == 0, "owed stopped cleanly")
            wait_for(lambda: (info := process(renderer)) is None or info[1] in "ZX",
                     "owe-render exited with owed", timeout=15)

            log = daemon_log.read_text()
            versions = re.findall(r"OpenGL vendor=.* version=(.*)", log)
            check(versions, "the renderer logged its OpenGL version")
            print(f"OpenGL: {versions[-1]}")
            renderers = re.findall(r"OpenGL vendor=.* renderer=(.*) version=", log)
            print(f"OpenGL renderer: {renderers[-1]}")
            if args.gpu:
                check(not any("llvmpipe" in r for r in renderers), "a GPU driver drew the frames")
            starts = re.findall(r"owe-render ready after (\d+) ms", log)
            print(f"renderer start: {', '.join(starts) or 'not logged'} ms, limit 7000 ms")
            if args.api == "gles":
                check("eglCreateContext OpenGL failed" in log, "desktop OpenGL 3.2 was not available")
                check(all(v.startswith("OpenGL ES") for v in versions), "the renderer used OpenGL ES")
            else:
                check(not any(v.startswith("OpenGL ES") for v in versions),
                      "the renderer used desktop OpenGL")
            failures = [line for line in log.splitlines() if any(f in line for f in GL_FAILURES)]
            check(not failures, "the log has no GL or EGL failure", "\n".join(failures))
            check("opaque region on for" in log, "the renderer marks its video surface opaque")
            passed = True
        finally:
            if daemon.poll() is None:
                # SIGTERM lets owed stop owe-render, which writes to the
                # temporary directory until it exits.
                leftover = renderer_pid(daemon.pid)
                daemon.terminate()
                try:
                    daemon.wait(timeout=30)
                except subprocess.TimeoutExpired:
                    daemon.kill()
                    daemon.wait()
                if leftover and (info := process(leftover)) and info[1] not in "ZX":
                    os.kill(leftover, signal.SIGKILL)
            sway.terminate()
            sway.wait(timeout=30)
            if not passed:
                for name in ("owed.log", "sway.log"):
                    print(f"---- {name} ----\n{(root / name).read_text()}", file=sys.stderr)
        print(f"PASS: {args.api} playback on {platform.machine()}")


if __name__ == "__main__":
    main()

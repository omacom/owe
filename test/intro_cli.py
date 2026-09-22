#!/usr/bin/env python3
import json
import socket
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path


def serve_late(path, requests):
    time.sleep(0.25)
    with socket.socket(socket.AF_UNIX) as server:
        server.bind(str(path))
        server.listen()
        while len(requests) < 3:
            client, _ = server.accept()
            with client:
                stream = client.makefile("rwb")
                while len(requests) < 3:
                    data = stream.readline()
                    if not data:
                        break
                    requests.append(json.loads(data))
                    stream.write(b'{"status":"ok"}\n')
                    stream.flush()


def check(condition, message, detail=""):
    if not condition:
        print(f"FAIL: {message} {detail}".rstrip(), file=sys.stderr)
        sys.exit(1)


def main():
    owe = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="owe-intro-cli-") as directory:
        root = Path(directory)
        socket_path = root / "control.sock"
        video = root / "intro.mp4"
        video.write_bytes(b"test")
        requests = []
        server = threading.Thread(target=serve_late, args=(socket_path, requests))
        server.start()
        started_at = time.monotonic()
        result = subprocess.run([owe, "--socket", socket_path, "intro-start", video],
                                capture_output=True, text=True, timeout=5)
        elapsed = time.monotonic() - started_at
        check(result.returncode == 0, "intro-start waits for a late daemon", result.stderr)
        check(elapsed >= 0.2, "intro-start exercised the connection retry")
        result = subprocess.run([owe, "--socket", socket_path, "intro-stop"],
                                capture_output=True, text=True, timeout=5)
        check(result.returncode == 0, "intro-stop reaches the daemon directly", result.stderr)
        server.join(timeout=5)
        check(not server.is_alive(), "fake daemon completed")
        check(requests == [
            {"cmd": "intro", "path": str(video)},
            {"cmd": "intro-commit"},
            {"cmd": "intro-stop"},
        ], "intro CLI sends the expected requests", json.dumps(requests))
    print("intro CLI readiness and cancellation passed")


if __name__ == "__main__":
    main()

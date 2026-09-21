#!/usr/bin/env python3
"""Measure verified wallpaper states and restore the original selection and flags."""
import json
import math
import os
from pathlib import Path
import signal
import socket
import statistics
import struct
import sys
import time


class Benchmark:
    def __init__(self, output, seconds, runs):
        self.output = Path(output)
        self.output.mkdir(parents=True, exist_ok=True)
        self.seconds = seconds
        self.runs = runs
        runtime = Path(os.environ.get("XDG_RUNTIME_DIR", f"/run/user/{os.getuid()}"))
        self.runtime = runtime / "owe"
        self.initial = None
        self.changed_background = False

    def request(self, command, renderer=False, **fields):
        path = self.runtime / ("render.sock" if renderer else "owed.sock")
        with socket.socket(socket.AF_UNIX) as peer:
            peer.settimeout(10)
            peer.connect(str(path))
            pid, _, _ = struct.unpack("3i", peer.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12))
            peer.sendall(json.dumps(dict(cmd=command, **fields)).encode() + b"\n")
            data = bytearray()
            while b"\n" not in data:
                chunk = peer.recv(65536)
                if not chunk or len(data) + len(chunk) > 65536:
                    raise RuntimeError(f"{path}: incomplete or oversized reply")
                data.extend(chunk)
        reply = json.loads(data.split(b"\n", 1)[0])
        if reply.get("status") != "ok":
            raise RuntimeError(f"{command}: {reply}")
        return pid, reply

    def command(self, command, **fields):
        return self.request(command, **fields)[1]

    @staticmethod
    def process(pid):
        fields = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
        return dict(pid=pid, parent=int(fields[1]), started=int(fields[19]),
                    ticks=int(fields[11]) + int(fields[12]),
                    rss=int(fields[21]) * os.sysconf("SC_PAGE_SIZE") / 1048576)

    def snapshot(self, state):
        daemon_pid, daemon = self.request("status")
        if daemon.get("failed_path") or daemon.get("job_running"):
            raise RuntimeError("media failed or is still converting")
        processes = {"daemon": self.process(daemon_pid)}
        renderer = None
        if state == "still":
            if daemon.get("engine") != "shell" or daemon.get("render_alive"):
                raise RuntimeError("still handoff has not stopped the renderer")
        else:
            renderer_pid, renderer = self.request("status", renderer=True)
            processes["renderer"] = self.process(renderer_pid)
            if processes["renderer"]["parent"] != daemon_pid:
                raise RuntimeError("renderer does not belong to the tested daemon")
            if (daemon.get("engine") != "renderer" or not daemon.get("media_ready") or
                    not renderer.get("ready") or not renderer.get("has_video") or renderer.get("error")):
                raise RuntimeError("video is not ready")
            want_pause = state != "playing"
            if daemon.get("paused") != want_pause or renderer.get("paused") != want_pause:
                raise RuntimeError(f"{state}: playback state does not match")
            if daemon.get("locked"):
                raise RuntimeError("lock-feed playback needs a separate benchmark")
            expected = {"playing": "always-animate", "paused": "manual", "policy": "idle"}[state]
            if daemon.get("reason") != expected:
                raise RuntimeError(f"{state}: unexpected policy reason {daemon.get('reason')}")
        return dict(daemon=daemon, renderer=renderer, processes=processes)

    def wait_ready(self, state):
        deadline = time.monotonic() + 15
        while True:
            try:
                return self.snapshot(state)
            except (RuntimeError, OSError) as error:
                if time.monotonic() >= deadline:
                    raise RuntimeError(f"{state}: did not become ready: {error}") from error
                time.sleep(0.1)

    @staticmethod
    def verify_same(before, after):
        if before["processes"].keys() != after["processes"].keys():
            raise RuntimeError("process set changed during the sample")
        for name, process in before["processes"].items():
            current = after["processes"][name]
            if (process["pid"], process["started"]) != (current["pid"], current["started"]):
                raise RuntimeError(f"{name} restarted during the sample")
        for key in ("source_path", "loaded_path", "loaded_kind"):
            if before["daemon"].get(key) != after["daemon"].get(key):
                raise RuntimeError("media changed during the sample")

    def sample(self, state):
        self.wait_ready(state)
        results = []
        for _ in range(self.runs):
            before = self.snapshot(state)
            start = time.monotonic()
            while True:
                remaining = self.seconds - (time.monotonic() - start)
                if remaining <= 0:
                    break
                time.sleep(min(1, remaining))
                after = self.snapshot(state)
                self.verify_same(before, after)
                if state in ("paused", "policy") and abs(
                        after["renderer"]["time_pos"] - before["renderer"]["time_pos"]) > 0.05:
                    raise RuntimeError("playback advanced while paused")
            elapsed = time.monotonic() - start
            row = dict(elapsed=elapsed, before=before, after=after, resources={})
            for name, process in before["processes"].items():
                current = after["processes"][name]
                cpu = (current["ticks"] - process["ticks"]) / os.sysconf("SC_CLK_TCK") / elapsed * 100
                row["resources"][name] = dict(cpu=cpu, rss_mib=current["rss"])
            results.append(row)
        (self.output / f"{state}.json").write_text(json.dumps(results, indent=2) + "\n")
        summary = []
        for name in results[0]["resources"]:
            values = [row["resources"][name] for row in results]
            summary.append(f"{name} median CPU={statistics.median(v['cpu'] for v in values):.3f}% "
                           f"RSS={max(v['rss_mib'] for v in values):.1f}MiB")
        line = f"{state}: " + "; ".join(summary)
        print(line, flush=True)
        with (self.output / "summary.txt").open("a") as file:
            file.write(line + "\n")

    def restore(self):
        if not self.initial:
            return
        commands = []
        if self.changed_background:
            commands.append(("set", dict(path=self.initial["source_path"])))
        commands.extend([
            ("pause" if self.initial["manual_pause"] else "resume", {}),
            ("idle-pause" if self.initial["idle_pause"] else "idle-resume", {}),
            ("always-animate", dict(value=self.initial["always_animate"])),
        ])
        errors = []
        for command, fields in commands:
            try:
                self.command(command, **fields)
            except (OSError, RuntimeError) as error:
                errors.append(str(error))
        if errors:
            raise RuntimeError("could not restore benchmark state: " + "; ".join(errors))

    def run(self, still=None):
        self.initial = self.command("status")
        (self.output / "config.json").write_text(json.dumps(self.command("config"), indent=2) + "\n")
        try:
            self.command("resume")
            self.command("always-animate", value=True)
            self.sample("playing")
            self.command("always-animate", value=False)
            self.command("pause")
            self.sample("paused")
            self.command("idle-pause")
            self.command("resume")
            self.sample("policy")
            if still:
                self.changed_background = True
                self.command("set", path=str(Path(still).resolve(strict=True)))
                self.sample("still")
        finally:
            self.restore()


def main():
    def stop(signum, _frame):
        # Let the finally block restore state once, even after repeated signals.
        signal.signal(signal.SIGINT, signal.SIG_IGN)
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        raise SystemExit(128 + signum)

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)
    seconds = float(os.environ.get("OWE_BENCH_SECONDS", "5"))
    runs = int(os.environ.get("OWE_BENCH_RUNS", "3"))
    if not math.isfinite(seconds) or seconds <= 0 or runs <= 0:
        raise ValueError("benchmark duration and run count must be positive")
    output = sys.argv[1] if len(sys.argv) > 1 else f"bench/results-{int(time.time())}"
    Benchmark(output, seconds, runs).run(os.environ.get("OWE_BENCH_STILL"))


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError) as error:
        print(f"benchmark failed: {error}", file=sys.stderr)
        sys.exit(1)

#!/bin/bash
# owe benchmark: sample renderer and daemon resources per verified state.
# Usage: bench/bench.sh [outdir]
# Optional: OWE_BENCH_STILL=/path/to/still.jpg adds a still sample and
# restores the previous background afterwards.
set -u

OUT="${1:-bench/results-$(date +%s)}"
RUNS="${OWE_BENCH_RUNS:-3}"
SECONDS_PER_RUN="${OWE_BENCH_SECONDS:-5}"
mkdir -p "$OUT"

need() { command -v "$1" >/dev/null 2>&1 || { echo "missing: $1" >&2; exit 1; }; }
need owe
need python3

reason() {
  owe status 2>/dev/null | python3 -c 'import json,sys; print(json.load(sys.stdin)["reason"])' 2>/dev/null || echo "unknown"
}

MANUAL_PAUSED=$(owe status 2>/dev/null | python3 -c 'import json,sys; print(json.load(sys.stdin)["manual_pause"])' 2>/dev/null || echo "false")
ANIMATE=$(owe status 2>/dev/null | python3 -c 'import json,sys; print(json.load(sys.stdin)["always_animate"])' 2>/dev/null || echo "false")
ORIGINAL_BACKGROUND=$(readlink -f "$HOME/.local/state/omarchy/current/background" 2>/dev/null || true)
STILL_CHANGED=0

restore() {
  if [[ $STILL_CHANGED == 1 && -n $ORIGINAL_BACKGROUND && -f $ORIGINAL_BACKGROUND ]]; then
    owe set "$ORIGINAL_BACKGROUND" >/dev/null 2>&1 || true
  fi
  if [[ $MANUAL_PAUSED == "true" ]]; then owe pause >/dev/null 2>&1 || true; else owe resume >/dev/null 2>&1 || true; fi
  if [[ $ANIMATE == "true" ]]; then owe always-animate on >/dev/null 2>&1 || true; else owe always-animate off >/dev/null 2>&1 || true; fi
}
trap restore EXIT INT TERM

sample_state() { # name seconds runs
  local name="$1" secs="$2" runs="$3" state
  state=$(reason)
  owe status >"$OUT/status-$name.json" 2>/dev/null || true
  owe render-status >"$OUT/render-$name.json" 2>/dev/null || true
  printf '%s: reason=%s\n' "$name" "$state" | tee -a "$OUT/summary.txt"
  python3 - "$name" "$secs" "$runs" "$OUT" <<'PY'
import json
import os
import statistics
import subprocess
import sys
import time
from pathlib import Path

name = sys.argv[1]
seconds = float(sys.argv[2])
runs = int(sys.argv[3])
out = Path(sys.argv[4])


def proc_stat(pid):
    fields = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
    rss = 0
    for line in Path(f"/proc/{pid}/status").read_text().splitlines():
        if line.startswith("VmRSS:"):
            rss = int(line.split()[1])
    return int(fields[11]) + int(fields[12]), rss


def find_pids():
    owed_list = subprocess.check_output(["pgrep", "-x", "owed"], text=True).split()
    if not owed_list:
        return None, None
    owed = int(owed_list[0])
    for entry in subprocess.check_output(["pgrep", "-x", "owe-render"], text=True).split():
        pid = int(entry)
        try:
            ppid = int(Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()[1])
        except OSError:
            continue
        if ppid == owed:
            return owed, pid
    return owed, None


owed, renderer = find_pids()
if renderer is None:
    print(f"{name}: no owe renderer; the shell draws this state")
    sys.exit(0)
results = []
for _ in range(runs):
    base = {pid: proc_stat(pid) for pid in (owed, renderer)}
    start = time.monotonic()
    time.sleep(seconds)
    elapsed = time.monotonic() - start
    now = {pid: proc_stat(pid) for pid in (owed, renderer)}
    tick = os.sysconf("SC_CLK_TCK")
    results.append((
        (now[renderer][0] - base[renderer][0]) / tick / elapsed * 100,
        (now[owed][0] - base[owed][0]) / tick / elapsed * 100,
        now[renderer][1] / 1024,
        now[owed][1] / 1024,
    ))


def spread(values, unit):
    return f"min={min(values):.3f}{unit} median={statistics.median(values):.3f}{unit} max={max(values):.3f}{unit}"


line = (f"{name}: renderer cpu {spread([r[0] for r in results], '%')}"
        f" rss {min(r[2] for r in results):.1f}-{max(r[2] for r in results):.1f}MiB;"
        f" daemon cpu {spread([r[1] for r in results], '%')}"
        f" rss {min(r[3] for r in results):.1f}-{max(r[3] for r in results):.1f}MiB")
print(line)
with (out / "summary.txt").open("a") as handle:
    handle.write(line + "\n")
PY
  if command -v gputop >/dev/null 2>&1; then
    timeout $((secs + 4)) gputop -d 2 -n 2 2>/dev/null | grep -E "PID|owe-render|owed" >"$OUT/gpu-$name.txt" || true
  fi
}

echo "owe benchmark $(date)" | tee "$OUT/summary.txt"
echo "states: playing, paused, policy when active, still when OWE_BENCH_STILL is set" | tee -a "$OUT/summary.txt"
echo "runs per state: $RUNS, seconds per run: $SECONDS_PER_RUN" | tee -a "$OUT/summary.txt"

# Playing. always-animate forces motion regardless of windows or battery.
owe always-animate on >/dev/null 2>&1
sample_state "playing" "$SECONDS_PER_RUN" "$RUNS"

# Manual pause. The renderer must stop while the last frame stays presented.
owe always-animate off >/dev/null 2>&1
owe pause >/dev/null 2>&1
sample_state "paused" "$SECONDS_PER_RUN" "$RUNS"
owe resume >/dev/null 2>&1

# Policy pause. Only sample when a real policy reason is active.
if [[ $(reason) != "visible" && $(reason) != "always-animate" ]]; then
  sample_state "policy" "$SECONDS_PER_RUN" "$RUNS"
else
  echo "policy: skipped, no policy condition is active" | tee -a "$OUT/summary.txt"
fi

# Still. Only when a still file is supplied, then restore the background.
if [[ -n ${OWE_BENCH_STILL:-} && -f ${OWE_BENCH_STILL:-} ]]; then
  owe set "$OWE_BENCH_STILL" >/dev/null 2>&1
  STILL_CHANGED=1
  sleep 2
  sample_state "still" "$SECONDS_PER_RUN" "$RUNS"
else
  echo "still: skipped, set OWE_BENCH_STILL to a still image to sample it" | tee -a "$OUT/summary.txt"
fi

echo "results in $OUT"

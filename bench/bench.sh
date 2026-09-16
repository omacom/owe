#!/bin/bash
# owe benchmark: capture CPU ticks and GPU load per state.
# Usage: bench/bench.sh [outdir]
set -u

OUT="${1:-bench/results-$(date +%s)}"
mkdir -p "$OUT"

need() { command -v "$1" >/dev/null 2>&1 || { echo "missing: $1" >&2; exit 1; }; }
need owe

render_pid() { pgrep -f "owe-render" | head -n 1; }

proc_ticks() {
  python3 -c "import sys; print(sum(map(int, open('/proc/' + sys.argv[1] + '/stat').read().rsplit(')', 1)[1].split()[11:13])))" "$1" 2>/dev/null
}

cpu_ticks() {
  local pid="$1" secs="$2"
  local c1 c2
  c1="$(proc_ticks "$pid")" || { echo 0; return; }
  sleep "$secs"
  c2="$(proc_ticks "$pid")" || { echo 0; return; }
  echo $((c2 - c1))
}

sample() {
  local name="$1" secs="${2:-5}"
  local pid
  pid="$(render_pid)"
  if [[ -z $pid ]]; then
    echo "$name: no renderer" | tee -a "$OUT/summary.txt"
    return
  fi
  local ticks
  ticks="$(cpu_ticks "$pid" "$secs")"
  local pct
  pct="$(python3 -c "print(round($ticks * 100 / ($secs * 100), 2))")"
  echo "$name: pid=$pid ticks/${secs}s=$ticks ~${pct}% of one core" | tee -a "$OUT/summary.txt"
  if command -v gputop >/dev/null 2>&1; then
    timeout $((secs + 4)) gputop -d 2 -n 2 2>/dev/null | grep -i "owe-render" >"$OUT/gpu-$name.txt" || true
  fi
  owe status >"$OUT/status-$name.json" 2>/dev/null || true
}

echo "owe benchmark $(date)" | tee "$OUT/summary.txt"
echo "states: still, video-playing, video-paused, occupied" | tee -a "$OUT/summary.txt"

owe always-animate on >/dev/null 2>&1
sample "video-playing" 5
owe always-animate off >/dev/null 2>&1
sleep 2
sample "policy-paused" 5
owe pause >/dev/null 2>&1
sleep 2
sample "manual-paused" 5
owe resume >/dev/null 2>&1

echo "results in $OUT"

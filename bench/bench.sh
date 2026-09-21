#!/bin/bash
# Usage: bench/bench.sh [outdir]
exec python3 "$(dirname "${BASH_SOURCE[0]}")/bench.py" "$@"

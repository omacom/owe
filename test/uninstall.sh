#!/bin/bash
set -euo pipefail

ROOT=$(mktemp -d)
trap 'rm -rf "$ROOT"' EXIT
export HOME="$ROOT/home"
mkdir -p "$HOME/.config/omarchy" "$ROOT/bin"
for cmd in systemctl omarchy; do
  printf '#!/bin/sh\nexit 0\n' >"$ROOT/bin/$cmd"
  chmod +x "$ROOT/bin/$cmd"
done
export PATH="$ROOT/bin:$PATH"
printf '%s\n' '{"version":1,"bar":{"custom":42},"disabledPlugins":["other.plugin","omarchy.background"]}' \
  >"$HOME/.config/omarchy/shell.json"
printf '%s\n' '{"version":1,"bar":{"custom":0},"disabledPlugins":["omarchy.background"]}' \
  >"$HOME/.config/omarchy/shell.json.bak.owe.123"

bash "$1"
python3 - <<'PY'
import json, os
with open(os.path.join(os.environ["HOME"], ".config/omarchy/shell.json")) as f:
    config = json.load(f)
assert config["bar"]["custom"] == 42
assert config["disabledPlugins"] == ["other.plugin"]
PY

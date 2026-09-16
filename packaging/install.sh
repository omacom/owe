#!/bin/bash
# owe install script. Builds with meson, installs to ~/.local, wires Omarchy hooks.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PREFIX="${HOME}/.local"
SHELL_JSON="${HOME}/.config/omarchy/shell.json"
BACKUP="${SHELL_JSON}.bak.owe.$(date +%s)"

need() {
  command -v "$1" >/dev/null 2>&1 || { echo "missing: $1" >&2; exit 1; }
}

need meson
need ninja
need gcc
need pkg-config
need ffmpeg
need socat

echo "==> build"
meson setup --reconfigure "${ROOT}/build" "${ROOT}" >/dev/null 2>&1 || meson setup "${ROOT}/build" "${ROOT}"
ninja -C "${ROOT}/build"
meson test -C "${ROOT}/build"

echo "==> install binaries to ${PREFIX}/bin"
install -Dm755 "${ROOT}/build/src/cli/owe" "${PREFIX}/bin/owe"
install -Dm755 "${ROOT}/build/src/daemon/owed" "${PREFIX}/bin/owed"
install -Dm755 "${ROOT}/build/src/render/owe-render" "${PREFIX}/bin/owe-render"
install -Dm755 "${ROOT}/hooks/owe-idle" "${PREFIX}/bin/owe-idle"

echo "==> install config"
if [[ ! -f "${HOME}/.config/owe/config.toml" ]]; then
  install -Dm644 "${ROOT}/config/config.toml" "${HOME}/.config/owe/config.toml"
else
  echo "    keep existing ~/.config/owe/config.toml"
fi

echo "==> install systemd unit"
install -Dm644 "${ROOT}/systemd/owed.service" "${HOME}/.config/systemd/user/owed.service"
systemctl --user daemon-reload

echo "==> install theme-set hook"
if command -v omarchy >/dev/null 2>&1; then
  omarchy hook install theme-set "${ROOT}/hooks/theme-set.d/10-owe-sync" || true
else
  echo "    omarchy CLI not found, skip hook install"
fi

echo "==> disable shell background plugin in ${SHELL_JSON}"
if [[ -f $SHELL_JSON ]]; then
  cp "$SHELL_JSON" "$BACKUP"
  echo "    backup at $BACKUP"
  python3 - "$SHELL_JSON" <<'EOF'
import json, sys
path = sys.argv[1]
with open(path) as f:
    data = json.load(f)
disabled = data.setdefault("disabledPlugins", [])
if "omarchy.background" not in disabled:
    disabled.append("omarchy.background")
with open(path, "w") as f:
    json.dump(data, f, indent=2)
    f.write("\n")
print("    omarchy.background disabled")
EOF
else
  echo "    no shell.json, skip"
fi

echo "==> enable and (re)start owed.service"
systemctl --user enable owed.service
systemctl --user restart owed.service

echo "done. run: owe status"

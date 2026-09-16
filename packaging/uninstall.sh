#!/bin/bash
# owe uninstall script. Restores the shell background plugin and removes owe files.
set -euo pipefail

PREFIX="${HOME}/.local"
SHELL_JSON="${HOME}/.config/omarchy/shell.json"

echo "==> stop owed.service"
systemctl --user disable --now owed.service 2>/dev/null || true
rm -f "${HOME}/.config/systemd/user/owed.service"
systemctl --user daemon-reload

echo "==> restore shell.json"
latest="$(ls -t "${SHELL_JSON}".bak.owe.* 2>/dev/null | head -n 1 || true)"
if [[ -n ${latest:-} ]]; then
  cp "$latest" "$SHELL_JSON"
  echo "    restored from $latest"
else
  python3 - "$SHELL_JSON" <<'EOF'
import json, sys
path = sys.argv[1]
try:
    with open(path) as f:
        data = json.load(f)
except FileNotFoundError:
    sys.exit(0)
disabled = data.get("disabledPlugins", [])
if "omarchy.background" in disabled:
    disabled.remove("omarchy.background")
    data["disabledPlugins"] = disabled
    with open(path, "w") as f:
        json.dump(data, f, indent=2)
        f.write("\n")
print("    omarchy.background re-enabled")
EOF
fi
omarchy restart shell 2>/dev/null || true

echo "==> remove theme-set hook"
rm -f "${HOME}/.config/omarchy/hooks/theme-set.d/10-owe-sync"

echo "==> remove binaries"
rm -f "${PREFIX}/bin/owe" "${PREFIX}/bin/owed" "${PREFIX}/bin/owe-render" "${PREFIX}/bin/owe-idle"

echo "done. shell background renderer is active again."

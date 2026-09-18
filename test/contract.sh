#!/bin/bash
# owe contract tests. Assert Omarchy integration contracts hold.
# Usage: test/contract.sh
set -u

PASS=0
FAIL=0

ok() { PASS=$((PASS + 1)); echo "PASS: $1"; }
bad() { FAIL=$((FAIL + 1)); echo "FAIL: $1"; }

LINK="$HOME/.local/state/omarchy/current/background"

if [[ -L $LINK ]]; then
  ok "background symlink exists"
else
  bad "background symlink exists"
fi

if command -v owe >/dev/null 2>&1; then
  ok "owe on PATH"
else
  bad "owe on PATH"
fi

if [[ -S "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/owe/owed.sock" ]]; then
  ok "owed socket exists"
else
  bad "owed socket exists"
fi

STATUS=$(owe status 2>/dev/null || true)
if printf '%s' "$STATUS" | grep -q '"status":"ok"'; then
  ok "owe status replies ok"
else
  bad "owe status replies ok"
fi

if printf '%s' "$STATUS" | grep -q '"engine":"shell"'; then
  if hyprctl layers 2>/dev/null | grep -q "omarchy-background"; then
    ok "shell background layer present (still)"
  else
    bad "shell background layer present (still)"
  fi
else
  if [[ -S "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/owe/render.sock" ]]; then
    ok "render socket exists"
  else
    bad "render socket exists"
  fi
  if owe render-status 2>/dev/null | grep -q '"status":"ok"'; then
    ok "owe render-status replies ok"
  else
    bad "owe render-status replies ok"
  fi
  if hyprctl layers 2>/dev/null | grep -q "owe-background"; then
    ok "owe-background layer present"
  else
    bad "owe-background layer present"
  fi
fi

echo "pass=$PASS fail=$FAIL"
[[ $FAIL -eq 0 ]]

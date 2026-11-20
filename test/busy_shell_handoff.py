"""A shell call that times out can still run once the busy shell gets to it.

The stub shell below applies every call, but while it is busy it reports
"not responding" as omarchy-shell does when its IPC timeout expires. An intro
started over a still then fails its handoff, yet the shell's background has
been switched off. Once the shell answers again, owed must hand the still back
to it instead of leaving the desktop without a background.
"""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

from daemon import MOCK, call

owed = Path(sys.argv[1]).resolve()

# While busy, a release still runs late but the calls after it are lost, so
# only an ask made once the shell answers again brings the still back.
SHELL = r"""#!/bin/bash
echo "$*" >>"$XDG_RUNTIME_DIR/shell.log"
busy=0; [[ -e $XDG_RUNTIME_DIR/busy ]] && busy=1
case "$*" in
  "background setSuspended true"|"shell setPluginEnabled omarchy.background false") echo off >"$XDG_RUNTIME_DIR/bg-state" ;;
  "background setSuspended false"|"shell setPluginEnabled omarchy.background true") (( busy )) || echo on >"$XDG_RUNTIME_DIR/bg-state" ;;
esac
if (( busy )); then echo 'omarchy-shell is not responding' >&2; exit 1; fi
exit 0
"""


def wait(condition, seconds):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if condition():
            return True
        time.sleep(0.05)
    return condition()


def fail(message, root):
    print(f"FAIL: {message}", file=sys.stderr)
    for name in ('daemon.log', 'shell.log'):
        path = root / name
        if path.exists():
            print(f"--- {name}\n{path.read_text()}", file=sys.stderr)
    sys.exit(1)


with tempfile.TemporaryDirectory(prefix='owe-busy-') as directory:
    root = Path(directory)
    shutil.copy2(owed, root / 'owed')
    (root / 'owe-render').write_text(MOCK)
    (root / 'owe-render').chmod(0o755)
    current = root / 'omarchy/current'
    current.mkdir(parents=True)
    for name in ('still.png', 'intro.mp4'):
        (root / name).write_bytes(b'clip')
    (current / 'background').symlink_to(root / 'still.png')
    (root / 'bin').mkdir()
    (root / 'bin/omarchy-shell').write_text(SHELL)
    (root / 'bin/omarchy-shell').chmod(0o755)
    env = os.environ.copy()
    env.update(XDG_RUNTIME_DIR=str(root), XDG_STATE_HOME=str(root), XDG_CONFIG_HOME=str(root), XDG_CACHE_HOME=str(root),
               DBUS_SYSTEM_BUS_ADDRESS=f'unix:path={root}/no-system-bus', OWE_DRM_DPMS='0',
               PATH=str(root / 'bin') + ':' + env['PATH'])
    env.pop('HYPRLAND_INSTANCE_SIGNATURE', None)
    env.pop('OMARCHY_PATH', None)
    sock = root / 'owe/owed.sock'
    state = root / 'bg-state'

    with (root / 'daemon.log').open('w') as log:
        process = subprocess.Popen([str(root / 'owed')], env=env, stdout=log, stderr=log)
        try:
            if not wait(lambda: sock.exists() and call(sock, 'status').get('engine') == 'shell', 8):
                fail('the shell takes the still at startup', root)

            # A shell busy for a moment, as one still loading at login is, only
            # delays the intro: owed asks again and the clip plays.
            (root / 'busy').touch()
            reply = call(sock, 'intro', path=str(root / 'intro.mp4'))
            if reply.get('status') != 'ok':
                fail(f'the daemon accepts the intro: {reply}', root)
            time.sleep(1.5)
            (root / 'busy').unlink()
            if not wait(lambda: not call(sock, 'intro-status').get('running'), 20):
                fail('the intro ends once the shell answers', root)
            result = call(sock, 'intro-status').get('result')
            if result != 'ok':
                fail(f'an intro plays once a busy shell releases its still; result={result}', root)
            if not wait(lambda: state.read_text().strip() == 'on', 10):
                fail('the shell takes the still back after the intro', root)
            print('ok: an intro waits for a busy shell instead of ending')

            # A shell that stays busy: the handoff calls land but time out
            # until the intro's deadline.
            (root / 'busy').touch()
            reply = call(sock, 'intro', path=str(root / 'intro.mp4'))
            if reply.get('status') != 'ok':
                fail(f'the daemon accepts the intro: {reply}', root)
            if not wait(lambda: not call(sock, 'intro-status').get('running'), 40):
                fail('the intro ends', root)
            if not (state.exists() and state.read_text().strip() == 'off'):
                fail('the busy shell applied the timed-out disable (test precondition)', root)

            # The shell answers again. The still is the shell's, so it must be on.
            (root / 'busy').unlink()
            if not wait(lambda: state.read_text().strip() == 'on', 10):
                status = call(sock, 'status')
                fail(f'the shell background is enabled again after a failed handoff; status={json.dumps(status)}', root)
            print('ok: the shell background comes back after a timed-out handoff')
        finally:
            process.terminate()
            try:
                process.wait(timeout=8)
            except subprocess.TimeoutExpired:
                process.kill()

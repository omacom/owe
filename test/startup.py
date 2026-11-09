import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

from daemon import MOCK, call

binary = Path(sys.argv[1]).resolve()


def stage(directory, selected, shell):
    root = Path(directory)
    shutil.copy2(binary, root / 'owed')
    (root / 'owe-render').write_text(MOCK)
    (root / 'owe-render').chmod(0o755)
    current = root / 'omarchy/current'
    current.mkdir(parents=True)
    for name in ('still.png', 'other.png', 'video.mp4', 'intro.mp4'):
        (root / name).write_bytes(b'clip')
    (current / 'background').symlink_to(root / selected)
    (root / 'bin').mkdir()
    helper = root / 'bin/omarchy-shell'
    helper.write_text(shell)
    helper.chmod(0o755)
    env = os.environ.copy()
    env.update(XDG_RUNTIME_DIR=str(root), XDG_STATE_HOME=str(root), XDG_CONFIG_HOME=str(root), XDG_CACHE_HOME=str(root),
               DBUS_SYSTEM_BUS_ADDRESS=f'unix:path={root}/no-system-bus', OWE_DRM_DPMS='0', PATH=str(root / 'bin') + ':' + env['PATH'])
    env.pop('HYPRLAND_INSTANCE_SIGNATURE', None)
    env.pop('OMARCHY_PATH', None)
    return root, env


def wait(condition, seconds, message):
    deadline = time.monotonic() + seconds
    while not condition():
        assert time.monotonic() < deadline, message()
        time.sleep(.01)


# What happens after startup: nothing (the shell never asks for the intro),
# the shell asking for it, or a new still or a video chosen before it asks.
cases = [('still.png', 'intro.mp4', 'request'), ('still.png', 'intro.mp4', 'switch'), ('still.png', 'intro.mp4', 'video'),
         ('still.png', 'intro.mp4', 'unasked'),
         ('still.png', '', None), ('still.png', 'missing.mp4', None), ('video.mp4', 'intro.mp4', None)]
for selected, intro, then in cases:
    with tempfile.TemporaryDirectory(prefix='owe-start-') as directory:
        # A shell that is still starting: it takes each request and answers only once released.
        root, env = stage(directory, selected,
                          '#!/bin/bash\necho "$*" >>"$XDG_RUNTIME_DIR/shell.log"\ntouch "$XDG_RUNTIME_DIR/handoff"\n'
                          'while [[ ! -e $XDG_RUNTIME_DIR/release ]]; do sleep 0.01; done\n')
        current = root / 'omarchy/current'
        log_path = root / 'daemon.log'
        commands = root / 'commands.jsonl'
        shell_log = root / 'shell.log'

        def sent():
            return [json.loads(line) for line in commands.read_text().splitlines()] if commands.exists() else []

        def shell_calls():
            return shell_log.read_text().splitlines() if shell_log.exists() else []

        with log_path.open('w') as log:
            process = subprocess.Popen([str(root / 'owed'), '--prepare-intro', str(root / intro) if intro else ''], env=env, stdout=log, stderr=log)
            try:
                preparing = selected == 'still.png' and intro == 'intro.mp4'
                if preparing:
                    # The shell draws the still by itself at login. The daemon
                    # answers at once, without first asking the shell for it.
                    wait(lambda: (root / 'owe/owed.sock').exists(), 4, lambda: log_path.read_text())
                    status = call(root / 'owe/owed.sock', 'status')
                    assert status['status'] == 'ok' and status['engine'] == 'none', status
                    loads = [command for command in sent() if command['cmd'] == 'load']
                    assert len(loads) == 1 and loads[0].get('prepare') is True and loads[0].get('mute') is True, loads
                    assert not any(command['cmd'] == 'intro-start' for command in sent()), sent()
                    assert not shell_calls(), shell_calls()
                    if then == 'request':
                        reply = call(root / 'owe/owed.sock', 'intro', path=str(root / intro), start='first-frame')
                        assert reply['status'] == 'ok', reply
                        # The shell's cover hides the login, so the shell lets go of its
                        # still first, is asked nothing else, and the clip starts after.
                        wait(lambda: (root / 'handoff').exists(), 4, lambda: log_path.read_text())
                        assert shell_calls() == ['background setSuspended true'], shell_calls()
                        time.sleep(.2)
                        assert not any(command['cmd'] == 'intro-start' for command in sent()), sent()
                        (root / 'release').touch()
                        # It resumes the prepared decoder rather than loading the clip again.
                        wait(lambda: any(command['cmd'] == 'intro-start' for command in sent()), 4, lambda: log_path.read_text())
                        assert not any(command.get('once') for command in sent()), sent()
                        wait(lambda: not call(root / 'owe/owed.sock', 'intro-status')['running'], 4, lambda: log_path.read_text())
                        assert call(root / 'owe/owed.sock', 'intro-status')['result'] == 'ok', log_path.read_text()
                        # The intro ends on the still, which goes back to the shell.
                        wait(lambda: call(root / 'owe/owed.sock', 'status')['engine'] == 'shell', 4, lambda: log_path.read_text())
                        assert shell_calls()[-1] == 'background setSuspended false', shell_calls()
                    elif then == 'switch':
                        (current / 'background').unlink()
                        (current / 'background').symlink_to(root / 'other.png')
                        wait(lambda: (root / 'handoff').exists(), 4, lambda: log_path.read_text())
                        assert shell_calls() == ['background setSuspended false'], shell_calls()
                        (root / 'release').touch()
                        wait(lambda: call(root / 'owe/owed.sock', 'status')['engine'] == 'shell', 4, lambda: log_path.read_text())
                        reply = call(root / 'owe/owed.sock', 'intro', path=str(root / intro), start='first-frame')
                        assert reply['status'] == 'ok', reply
                        assert any(command.get('once') for command in sent()), sent()
                        assert not any(command['cmd'] == 'intro-start' for command in sent()), sent()
                    elif then == 'video':
                        # A video still loads behind the shell's still and
                        # fades in from it, so the shell is handed the still first.
                        (current / 'background').unlink()
                        (current / 'background').symlink_to(root / 'video.mp4')
                        wait(lambda: (root / 'handoff').exists(), 4, lambda: log_path.read_text())
                        assert shell_calls() == ['background setSuspended false'], shell_calls()
                        (root / 'release').touch()
                        wait(lambda: call(root / 'owe/owed.sock', 'status')['engine'] == 'renderer', 4, lambda: log_path.read_text())
                        assert shell_calls() == ['background setSuspended false', 'background setSuspended true'], shell_calls()
                        video = [command for command in sent() if command.get('path') == str(root / 'video.mp4')]
                        assert len(video) == 1 and video[0].get('from') == str(root / 'still.png') and video[0].get('hold') is True, sent()
                    else:
                        # A login that never asks, such as a restart of this
                        # daemon in a running session, hands the still over.
                        time.sleep(12)
                        assert not shell_calls(), shell_calls()
                        wait(lambda: (root / 'handoff').exists(), 6, lambda: log_path.read_text())
                        assert shell_calls() == ['background setSuspended false'], shell_calls()
                        (root / 'release').touch()
                        wait(lambda: call(root / 'owe/owed.sock', 'status')['engine'] == 'shell', 4, lambda: log_path.read_text())
                else:
                    wait(lambda: (root / 'handoff').exists(), 4, lambda: log_path.read_text())
                    assert not [command for command in sent() if command['cmd'] == 'load'], sent()
                    (root / 'release').touch()
                    status = call(root / 'owe/owed.sock', 'status')
                    assert status['status'] == 'ok', status
                    if selected == 'still.png':
                        assert status['engine'] == 'shell', status
                print(f'startup with {selected}, intro {intro!r}, then {then}: ok')
            finally:
                (root / 'release').touch()
                process.terminate()
                process.wait(timeout=8)

# At login owed can start before the shell answers: first the shell is not
# running, then it is busy loading and calls time out after they went out. The
# still waits for the shell, which takes it as soon as it answers. Suspending
# the shell for a renderer fallback would land late and hide the wallpaper
# until the next retry. The busy shell fails both commands of one handoff, so
# the still must also wait through a failure of a shell that is up.
with tempfile.TemporaryDirectory(prefix='owe-start-') as directory:
    root, env = stage(directory, 'still.png', '''#!/bin/bash
echo "$*" >> "$XDG_RUNTIME_DIR/shell-calls"
if [[ ! -e $XDG_RUNTIME_DIR/shell-up ]]; then echo 'omarchy-shell is not running' >&2; exit 1; fi
echo "$*" >> "$XDG_RUNTIME_DIR/shell-busy"
if (( $(wc -l < "$XDG_RUNTIME_DIR/shell-busy") <= 2 )); then sleep 0.2; echo 'omarchy-shell is not responding' >&2; exit 1; fi
''')
    with (root / 'daemon.log').open('w') as log:
        process = subprocess.Popen([str(root / 'owed'), '--prepare-intro', ''], env=env, stdout=log, stderr=log)
        try:
            calls = root / 'shell-calls'
            deadline = time.monotonic() + 4
            while not calls.exists():
                assert process.poll() is None and time.monotonic() < deadline, (root / 'daemon.log').read_text()
                time.sleep(.01)
            time.sleep(1.5)
            (root / 'shell-up').touch()
            deadline = time.monotonic() + 6
            while call(root / 'owe/owed.sock', 'status').get('engine') != 'shell':
                assert process.poll() is None and time.monotonic() < deadline, (root / 'daemon.log').read_text()
                time.sleep(.1)
            sent = calls.read_text().splitlines()
            busy = (root / 'shell-busy').read_text().splitlines()
            assert len(busy) >= 3 and busy[-1] == 'background setSuspended false', busy
            assert 'background setSuspended true' not in sent, sent
            assert 'shell setPluginEnabled omarchy.background false' not in sent, sent
            print('a still waits for a starting shell instead of suspending it')
        finally:
            process.terminate()
            process.wait(timeout=8)

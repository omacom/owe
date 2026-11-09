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
    for name in ('still.png', 'video.mp4', 'intro.mp4'):
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


for selected, intro in [('still.png', 'intro.mp4'), ('still.png', ''), ('still.png', 'missing.mp4'), ('video.mp4', 'intro.mp4')]:
    with tempfile.TemporaryDirectory(prefix='owe-start-') as directory:
        root, env = stage(directory, selected,
                          '#!/bin/bash\ntouch "$XDG_RUNTIME_DIR/handoff"\nwhile [[ ! -e $XDG_RUNTIME_DIR/release ]]; do sleep 0.01; done\n')
        with (root / 'daemon.log').open('w') as log:
            process = subprocess.Popen([str(root / 'owed'), '--prepare-intro', str(root / intro) if intro else ''], env=env, stdout=log, stderr=log)
            try:
                deadline = time.monotonic() + 4
                while not (root / 'handoff').exists():
                    assert process.poll() is None and time.monotonic() < deadline, (root / 'daemon.log').read_text()
                    time.sleep(.01)
                commands = root / 'commands.jsonl'
                loads = [json.loads(line) for line in commands.read_text().splitlines() if json.loads(line)['cmd'] == 'load'] if commands.exists() else []
                preparing = selected == 'still.png' and intro == 'intro.mp4'
                assert bool(loads) == preparing, loads
                if preparing:
                    assert any(command.get('prepare') is True and command.get('mute') is True for command in loads), loads
                    assert not any(command['cmd'] == 'intro-start' for command in loads), loads
                (root / 'release').touch()
                status = call(root / 'owe/owed.sock', 'status')
                assert status['status'] == 'ok', status
                if selected == 'still.png':
                    assert status['engine'] == 'shell', status
                print(f'prepared startup respects pending handoff and falls back: {selected}, {intro!r}')
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

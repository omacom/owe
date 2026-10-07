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
for selected, intro in [('still.png', 'intro.mp4'), ('still.png', ''), ('still.png', 'missing.mp4'), ('video.mp4', 'intro.mp4')]:
    with tempfile.TemporaryDirectory(prefix='owe-start-') as directory:
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
        helper.write_text('#!/bin/bash\ntouch "$XDG_RUNTIME_DIR/handoff"\nwhile [[ ! -e $XDG_RUNTIME_DIR/release ]]; do sleep 0.01; done\n')
        helper.chmod(0o755)
        env = os.environ.copy()
        env.update(XDG_RUNTIME_DIR=str(root), XDG_STATE_HOME=str(root), XDG_CONFIG_HOME=str(root), XDG_CACHE_HOME=str(root),
                   DBUS_SYSTEM_BUS_ADDRESS=f'unix:path={root}/no-system-bus', OWE_DRM_DPMS='0', PATH=str(root / 'bin') + ':' + env['PATH'])
        env.pop('HYPRLAND_INSTANCE_SIGNATURE', None)
        env.pop('OMARCHY_PATH', None)
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

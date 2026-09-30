# owe — high-performance wallpaper engine for Omarchy

`owe` plays video files, `GIF`, and still images as the Hyprland background.
Video containers include `mp4`, `mkv`, `webm`, `mov`, `avi`, `mpeg`, `ts`, `wmv`, `flv`, `ogv`, and `3gp`.
It joins the Omarchy theme switch through existing contracts only.
It changes no packaged Omarchy file.

> This project is experimental. **Use it at your own risk.**
> See the [performance follow-up document](docs/performance-follow-up.md) for known issues and incomplete checks.

## Binaries

- `owed` — policy daemon. It watches the background symlink, tracks Hyprland,
  lock, idle, DPMS, and battery state, and commands the renderer.
- `owe-render` — C renderer. It owns all layer-shell surfaces while a
  moving background plays. It decodes video once through `libmpv` with
  hardware decode and draws N outputs. It draws a poster still as a
  static GL texture only in battery poster mode. The daemon stops it
  while a still background shows and gives the layer back to the shell.
- `owe` — CLI. It controls every daemon and renderer function.
- `owe-idle` — hypridle helper. It pauses on idle and resumes on activity.

## Install

From the AUR:

```bash
omarchy pkg add owe
systemctl --user enable --now owed.service
```

To install the optional theme hook, run:

```bash
omarchy hook install theme-set /usr/share/owe/10-owe-sync
```

The daemon also watches the background symlink directly.
The hook requests a refresh after the theme changes.

Or build from source, as below.

## Quick start

Install the build and runtime packages on Omarchy first. Packages already
installed are skipped:

```bash
omarchy pkg add meson ninja gcc pkgconf wayland wayland-protocols libglvnd \
  libepoxy mpv ffmpeg systemd-libs socat python cmake qt6-declarative
```

Then build and install `owe`:

```bash
./packaging/install.sh
owe status
owe set ~/Videos/loop.mp4
owe pause
owe resume
owe always-animate on
```

## CLI reference

```bash
owe set <path>            # Update the Omarchy background symlink
owe next                  # Cycle to next theme background
owe current               # Show current background name
owe refresh               # Re-read the background symlink now
owe pause                 # Pause video manually
owe resume                # Clear manual pause
owe always-animate on|off # Force animation regardless of policy
owe intro <video>         # Play a one-shot intro video and wait
owe status                # Daemon status as JSON
owe config                # Effective config as JSON
owe render-status         # Renderer status as JSON
owe render <json>         # Send raw JSON to the renderer
owe raw <json>            # Send raw JSON to the daemon
owe reload-config         # Reload config.toml
owe render-restart        # Restart the renderer process
owe shutdown              # Stop the daemon
owe --version             # Show the build version
```

The daemon accepts `--socket PATH` and `--verbose`.
The renderer accepts the same options.
The CLI accepts `--socket PATH` before the command.
The renderer socket and lock feed socket use the daemon socket directory.

```bash
owed --socket "$XDG_RUNTIME_DIR/owe-test/owed.sock" --verbose
owe --socket "$XDG_RUNTIME_DIR/owe-test/owed.sock" status
```

One daemon can run in each runtime directory.
An intro requires a still background and ends after 30 seconds at most.
The CLI accepts an absolute or relative intro path.

## IPC reference

Sockets live under `$XDG_RUNTIME_DIR/owe/`. One JSON message uses one line.
Each command gets one JSON reply line.

### Daemon socket: `owed.sock`

Commands:

- `{"cmd":"hello"}` — reply `{"status":"ok","version":1}`.
- `{"cmd":"status"}` — full daemon state: source path and kind, loaded path
  and kind, job and failure state, pause state, reason, Hyprland flags,
  battery, lock, and renderer liveness.
- `{"cmd":"config"}` — effective config values.
- `{"cmd":"set","path":"/abs/file"}` validates a local file and atomically updates the Omarchy background symlink.
- `{"cmd":"refresh"}` — re-resolve the symlink and load now.
- `{"cmd":"pause"}` — set manual pause.
- `{"cmd":"resume"}` — clear manual pause.
- `{"cmd":"idle-pause"}` — set idle pause. It does not clear a manual pause.
- `{"cmd":"idle-resume"}` — clear idle pause.
- `{"cmd":"always-animate","value":true}` — force or release animation.
- `{"cmd":"reload-config"}` — reload `~/.config/owe/config.toml`.
- `{"cmd":"render-status"}` — proxy the renderer status reply.
- `{"cmd":"render-restart"}` — restart `owe-render` and reload current media.
- `{"cmd":"intro","path":"/abs/file.mp4"}` — play a one-shot intro video over a still background, muted. Reply means it started. Cancel with `intro-stop`.
- `{"cmd":"intro-status"}` — reply `{"running":bool,"result":"running|ok|error"}`.
- `{"cmd":"intro-stop"}` — cancel a running intro.
- `{"cmd":"shutdown"}` — stop the daemon.

### Renderer socket: `render.sock`

Commands:

- `{"cmd":"hello"}` — reply `{"status":"ok","version":1}`.
- `{"cmd":"load","path":"/abs/file","kind":"video|still"}` — load media. `"once":true` and `"mute":true` load a one-shot intro.
- Add `"async":true` to a still load for an immediate acknowledgement.
  Poll `status` until `ready` is true and `path` matches the request.
  An asynchronous still load has a 30-second deadline.
  A synchronous still load has a four-second deadline.
- `{"cmd":"cancel-load"}` cancels a pending still load and retains the current media.
- `{"cmd":"pause"}` — pause decode. The last frame stays presented.
- `{"cmd":"resume"}` — resume decode.
- `{"cmd":"stop"}` — unload all media.
- `{"cmd":"status"}` reports the path, kind, pause state, outputs, `time_pos`, `hwdec`, and playback `error`.
- `{"cmd":"fade","ms":250}` — set the still fade length.
- `{"cmd":"intro-show"}` — reveal a loaded intro once its outgoing still is ready.
- `{"cmd":"intro-finish","path":"/abs/still.png","ms":750}` — fade the final intro frame into a still and hold it for the shell handoff.
- `{"cmd":"feed"}` — start muted video output to the lock feed.
- `{"cmd":"feed-stop"}` — stop the lock feed and release its buffers.
- `{"cmd":"skip","outputs":["DP-1"]}` — stop desktop swaps on the named outputs.

The daemon uses asynchronous still loads for posters and shell fallback.
Its `media_ready` field confirms readiness for `loaded_path`.
The renderer ignores transition paths that do not name readable local files.
Malformed replies terminate their connection.
Reply deadlines include all fragments of a reply.

## Examples

```bash
# Status through the raw socket
printf '{"cmd":"status"}\n' | socat - UNIX-CONNECT:$XDG_RUNTIME_DIR/owe/owed.sock

# Load a video direct on the renderer
printf '{"cmd":"load","path":"/tmp/loop.mp4","kind":"video"}\n' \
  | socat - UNIX-CONNECT:$XDG_RUNTIME_DIR/owe/render.sock

# Pause through the daemon
owe raw '{"cmd":"pause"}'
```

## Pause policy

By default the wallpaper keeps moving. The engine pauses only for real
interruptions: a fullscreen window, the lock screen, monitor DPMS off,
and sleep. Reasons in priority order:

1. `manual` pause from `owe pause`.
2. `sleep` pauses playback before suspend.
3. `always-animate` bypasses automatic pause rules.
4. `idle` pause from the `owe-idle` helper.
5. `locked` follows logind or Hyprland session state.
6. `dpms-off` applies when every connected monitor reports a known off state.
   Missing DRM data falls back to Hyprland state.
7. `battery` holds or replaces playback while on battery. Off by default.
   Set `battery_mode = "pause"` to hold the current frame, or
   `battery_mode = "poster"` to show a still poster. `battery_poster = true`
   is the same as poster mode.
8. `blocklist` while a listed process runs. Off by default.
9. `fullscreen` applies when fullscreen windows cover all active outputs. Hidden workspaces do not count.
10. `occupied` applies when all active outputs have visible windows. Off by default, enable with
   `occupied_workspace = true` to save power on busy desktops.

Idle pauses are optional. Wire the `owe-idle` helper into `hypridle`
and it sets the idle pause. `owe resume` clears only the manual pause,
so an idle resume never releases a manual pause.

To bypass automatic pause rules, run `owe always-animate on`.
Manual pause and sleep still take precedence.

A locked session can keep video moving through the muted lock feed instead
of the desktop layer. The feed honors manual pause, sleep, DPMS, idle,
blocklist and battery pause/poster settings. The animation override bypasses
the automatic rules, but the feed still stops for manual pause, sleep and DPMS.
Fullscreen and occupied desktop windows do not stop the lock feed.
A lock screen can import `Owe.LockFeed` and display a `LockFeed` item;
it should provide its own still fallback when the feed is inactive.

## GIF handling

Each `GIF` becomes cached muted `mp4` through one `ffmpeg` pass at first
select. Later selects serve from `~/.cache/owe/gif/`. With
`battery_poster = true`, battery mode shows a poster frame from the
same cache.

The GIF and poster cache keeps the newest 512 MiB by default. Set
`cache_max_mb` in `[transcode]` to change the budget. `0` disables
eviction. The current conversion and its cached source are retained even when
they exceed the budget; older unused entries are evicted first.

## Audio

A video wallpaper plays its audio track through the default audio output.
One renderer serves every monitor, so the track plays once. Generated GIF
media is silent. Source files remain intact.

## Performance design

- Stills never start the renderer. While a still background shows, `owed`
  enables the shell background plugin, stops `owe-render`, and keeps only
  the 4 MiB daemon. A video or GIF starts the renderer again.
- Video renders only when `libmpv` signals a new frame. The render rate
  tracks the media frame rate, not the display refresh rate.
- A switch from a still to a video crossfades. The renderer draws the
  outgoing still over the incoming video and fades it out.
- Video draws straight into the window framebuffer. No intermediate
  copy per frame per output.
- Video redraws wait for the compositor's frame callback. A blanked
  output stops the draw loop instead of stalling the event loop, and an
  output covered by a fullscreen window stops swapping buffers.
- Hardware decode is on by default (`hwdec=auto-safe`). Override with
  the `OWE_HWDEC` environment variable.
- `libmpv` runs without its lua scripts. The ytdl hook, stats overlay,
  console, and the other scripts only add threads and memory. Advanced
  mpv tuning uses `OWE_MPV_OPTIONS`, a semicolon separated list such as
  `demuxer-max-bytes=16MiB;hwdec-extra-frames=1`.
- Stills decode once at the size the cover crop needs, upload one texture,
  and then stop the frame loop. Paused video stops the loop too.
- Layer surfaces use `wp_fractional_scale_v1` and `wp_viewporter` when the
  compositor offers them. A 1.25 scale output draws 1.25x pixels, not the 2x
  that `wl_output.scale` alone would ask for. Without the protocols the
  renderer falls back to the integer `wl_output.scale`.
- Stills and video cover the output. Aspect ratio is preserved with a
  center crop, like `PreserveAspectCrop` in the Omarchy shell.
- The renderer returns freed large allocations to the OS. glibc keeps
  them resident by default, which inflates RSS by about 30 MiB with
  media buffers.
- Poster extraction and GIF transcode run in a worker thread. The
  daemon event loop never blocks on `ffmpeg`.
- The daemon sends pause and resume only when playback state changes.

## Recovery

For a black background, follow [the diagnostic steps](docs/troubleshooting.md).
Software decode still uses the GPU display path.
The guide covers GPU logs, swap failures, and an optional DRM DPMS diagnostic override.

`owed` supervises `owe-render`. A killed or crashed renderer is restarted
and the current media reloads. The daemon holds a `flock` on
`$XDG_RUNTIME_DIR/owe/owed.lock`, so a second instance refuses to start.

## Config

Copy `config/config.toml` to `~/.config/owe/config.toml` and run
`owe reload-config`. The installer does this once and never overwrites
an existing file.

The daemon uses `$XDG_CONFIG_HOME/owe/config.toml` when `XDG_CONFIG_HOME` is set.
An invalid reload preserves the current configuration.
The journal identifies the file and line of a syntax error.
Unknown keys produce a warning.

| Section | Key | Default | Accepted values |
| --- | --- | --- | --- |
| `pause` | `fullscreen` | `true` | Boolean |
| `pause` | `occupied_workspace` | `false` | Boolean |
| `pause` | `battery_poster` | `false` | Boolean |
| `pause` | `battery_mode` | `"play"` | `"play"`, `"pause"`, `"poster"` |
| `pause` | `blocklist` | `[]` | At most 16 process names, at most 63 bytes per name |
| `transcode` | `gif_fps` | `20` | 5 to 50 |
| `transcode` | `gif_crf` | `20` | 0 to 51 |
| `transcode` | `max_width` | `2560` | 320 to 16384 |
| `transcode` | `max_height` | `1440` | 200 to 16384 |
| `transcode` | `cache_max_mb` | `512` | 0 to 65536 MiB |
| `render` | `fade_ms` | `250` | 0 to 2000 milliseconds |

Numeric values outside these limits use the nearest limit.
`battery_poster = true` takes precedence over `battery_mode`.
Set `battery_poster = false` to control battery behavior through `battery_mode` alone.
The blocklist accepts single-line and multiline arrays.
Quoted names can contain `#` and commas.
Oversized arrays and names cause a load error.
Each configuration line has a limit of 510 bytes before its newline.

To inspect the effective configuration and recent errors, run:

```bash
owe config
journalctl --user -u owed.service -n 100 --no-pager
```

## Local uninstall

To remove a source installation, run:

```bash
./packaging/uninstall.sh
```

The script restores the shell background plugin.
It retains the configuration and media cache.

## Build

```bash
meson setup build
ninja -C build
meson test -C build
```

The tests cover IPC framing, quoted paths, worker cancellation, atomic cache publication, display state, actual GIF conversion, still decode for PNG, JPEG, and AVIF, and pause policy after a rejected load.
The lock feed tests cover feed restarts, client pause state, frame ownership, DPMS, and transitions to still images.

The transition test reads actual OpenGL pixels from two offscreen output buffers with libmpv video playback.
It checks the blend, delayed frames, pause, cancellation, texture cleanup, unavailable outputs, and transition image timeouts.

```bash
meson test -C build transition --print-errorlogs
```

To build and test the QML lock feed plugin, use CMake with Qt 6 Quick and Qt 6 Test:

```bash
cmake -S qml-plugin -B build-qml -DBUILD_TESTING=ON
cmake --build build-qml
ctest --test-dir build-qml --output-on-failure
```

The AUR package installs the plugin to `/usr/lib/qt6/qml/Owe/LockFeed`.
The local installer uses `~/.local/lib/qt6/qml/Owe/LockFeed`; add
`~/.local/lib/qt6/qml` to the consuming application's `QML_IMPORT_PATH`.
Installing the module does not change the lock screen's QML layout.

The plugin tests use an offscreen Qt platform and local sockets. They cover reconnects, fragmented messages, and descriptor ownership.

To run sanitizer checks, use a separate build directory:

```bash
meson setup build-sanitize -Db_sanitize=address,undefined -Db_lundef=false -Dwerror=true
meson test -C build-sanitize --print-errorlogs
```

`test/owe-live-test` requires a running test daemon and a video wallpaper.
To check live playback and IPC, run `build/test/owe-live-test`.

Build and install deps: `meson`, `ninja`, `gcc`, `pkgconf`, `wayland`,
`wayland-protocols`, `libglvnd`, `libepoxy`, `mpv`, `ffmpeg`, `systemd-libs`,
`socat`, `python`, `cmake`, `qt6-declarative`.
Runtime deps: `mpv`, `ffmpeg`, `wayland`, `libglvnd`, `libepoxy`, `systemd-libs`, `socat`, `qt6-declarative`.

Docs: `docs/architecture.md`, `docs/theme-contract.md`, `docs/benchmarks.md`.

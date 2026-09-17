# owe — high-performance wallpaper engine for Omarchy

`owe` plays `mp4`, `GIF`, and still images as the Hyprland background.
It joins the Omarchy theme switch through existing contracts only.
It changes no packaged Omarchy file.

> This project is experimental. **Use it at your own risk.**
> See the [performance follow-up document](docs/performance-follow-up.md) for known issues and incomplete checks.

## Binaries

- `owed` — policy daemon. It watches the background symlink, tracks Hyprland,
  lock, idle, DPMS, and battery state, and commands the renderer.
- `owe-render` — C renderer. It owns all layer-shell surfaces. It decodes
  video once through `libmpv` with hardware decode and draws N outputs.
  It draws stills as static GL textures with no frame loop.
- `owe` — CLI. It controls every daemon and renderer function.
- `owe-idle` — hypridle helper. It pauses on idle and resumes on activity.

## Quick start

Install the build and runtime packages on Omarchy first. Packages already
installed are skipped:

```bash
omarchy pkg add meson ninja gcc pkgconf wayland wayland-protocols libglvnd \
  libepoxy mpv ffmpeg systemd-libs socat python
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
owe status                # Daemon status as JSON
owe config                # Effective config as JSON
owe render-status         # Renderer status as JSON
owe render <json>         # Send raw JSON to the renderer
owe raw <json>            # Send raw JSON to the daemon
owe reload-config         # Reload config.toml
owe render-restart        # Restart the renderer process
owe shutdown              # Stop the daemon
```

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
- `{"cmd":"shutdown"}` — stop the daemon.

### Renderer socket: `render.sock`

Commands:

- `{"cmd":"hello"}` — reply `{"status":"ok","version":1}`.
- `{"cmd":"load","path":"/abs/file","kind":"video|still"}` — load media.
- `{"cmd":"pause"}` — pause decode. The last frame stays presented.
- `{"cmd":"resume"}` — resume decode.
- `{"cmd":"stop"}` — unload all media.
- `{"cmd":"status"}` reports the path, kind, pause state, outputs, `time_pos`, `hwdec`, and playback `error`.
- `{"cmd":"fade","ms":250}` — set the still fade length.

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
5. `locked` follows logind session state.
6. `dpms-off` applies when every connected monitor reports a known off state.
   Missing DRM data falls back to Hyprland state.
7. `battery` shows a still poster while on battery. Off by default,
   enable with `battery_poster = true`.
8. `blocklist` while a listed process runs. Off by default.
9. `fullscreen` applies when fullscreen windows cover all active outputs. Hidden workspaces do not count.
10. `occupied` applies when all active outputs have visible windows. Off by default, enable with
   `occupied_workspace = true` to save power on busy desktops.

Idle pauses are optional. Wire the `owe-idle` helper into `hypridle`
and it sets the idle pause. `owe resume` clears only the manual pause,
so an idle resume never releases a manual pause.

To force motion regardless of policy, run `owe always-animate on`.

## GIF handling

Each `GIF` becomes cached muted `mp4` through one `ffmpeg` pass at first
select. Later selects serve from `~/.cache/owe/gif/`. With
`battery_poster = true`, battery mode shows a poster frame from the
same cache.

The GIF and poster cache keeps the newest 512 MiB by default. Set
`cache_max_mb` in `[transcode]` to change the budget. `0` disables
eviction.

## No audio

Playback disables audio with `audio=no` and `aid=no`.
Generated media contains only video. Source files remain intact.

## Performance design

- Video renders only when `libmpv` signals a new frame. The render rate
  tracks the media frame rate, not the display refresh rate.
- Video draws straight into the window framebuffer. No intermediate
  copy per frame per output.
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
- Poster extraction and GIF transcode run in a worker thread. The
  daemon event loop never blocks on `ffmpeg`.
- The daemon sends pause and resume only when playback state changes.

## Recovery

`owed` supervises `owe-render`. A killed or crashed renderer is restarted
and the current media reloads. The daemon holds a `flock` on
`$XDG_RUNTIME_DIR/owe/owed.lock`, so a second instance refuses to start.

## Config

Copy `config/config.toml` to `~/.config/owe/config.toml` and run
`owe reload-config`. The installer does this once and never overwrites
an existing file.

## Build

```bash
meson setup build
ninja -C build
meson test -C build
```

The tests cover IPC framing, quoted paths, worker cancellation, atomic cache publication, display state, actual GIF conversion, still decode for PNG, JPEG, and AVIF, and pause policy after a rejected load.
To run sanitizer checks, use a separate build directory:

```bash
meson setup build-sanitize -Db_sanitize=address,undefined -Db_lundef=false -Dwerror=true
meson test -C build-sanitize --print-errorlogs
```

`test/owe-live-test` requires a running test daemon and a video wallpaper.
To check live playback and IPC, run `build/test/owe-live-test`.

Build and install deps: `meson`, `ninja`, `gcc`, `pkgconf`, `wayland`,
`wayland-protocols`, `libglvnd`, `libepoxy`, `mpv`, `ffmpeg`, `systemd-libs`,
`socat`, `python`.
Runtime deps: `mpv`, `ffmpeg`, `socat`.

Docs: `docs/architecture.md`, `docs/theme-contract.md`, `docs/benchmarks.md`.

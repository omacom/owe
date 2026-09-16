# owe — high-performance wallpaper engine for Omarchy

`owe` plays `mp4`, `GIF`, and still images as the Hyprland background.
It joins the Omarchy theme switch through existing contracts only.
It changes no packaged Omarchy file.

## Binaries

- `owed` — policy daemon. It watches the background symlink, tracks Hyprland,
  lock, idle, DPMS, and battery state, and commands the renderer.
- `owe-render` — C renderer. It owns all layer-shell surfaces. It decodes
  video once through `libmpv` with hardware decode and draws N outputs.
  It draws stills as static GL textures with no frame loop.
- `owe` — CLI. It controls every daemon and renderer function.
- `owe-idle` — hypridle helper. It pauses on idle and resumes on activity.

## Quick start

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
owe set <path>            # Set background through omarchy-theme-bg-set
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
- `{"cmd":"set","path":"/abs/file"}` — set background through
  `omarchy-theme-bg-set`.
- `{"cmd":"refresh"}` — re-resolve the symlink and load now.
- `{"cmd":"pause"}` — set manual pause.
- `{"cmd":"resume"}` — clear manual pause.
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
- `{"cmd":"status"}` — path, kind, pause, outputs, video and still flags.
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
interruptions: a fullscreen window, the lock screen, DPMS off, and sleep.
Reasons in priority order:

1. `always-animate` override forces play.
2. `manual` pause from `owe pause`.
3. `locked` from logind.
4. `dpms-off` when all monitors turn off.
5. `battery` shows a still poster while on battery. Off by default,
   enable with `battery_poster = true`.
6. `blocklist` while a listed process runs. Off by default.
7. `fullscreen` when any window turns fullscreen. On by default.
8. `occupied` when any window shows. Off by default, enable with
   `occupied_workspace = true` to save power on busy desktops.

Idle pauses come from the `owe-idle` helper wired into `hypridle`.

To force motion regardless of policy, run `owe always-animate on`.

## GIF handling

Each `GIF` becomes cached muted `mp4` through one `ffmpeg` pass at first
select. Later selects serve from `~/.cache/owe/gif/`. With
`battery_poster = true`, battery mode shows a poster frame from the
same cache.

## No audio

The engine removes audio at every layer. `libmpv` runs with `audio=no`
and `aid=no`. No audio client ever opens. Transcodes add `-an`.

## Performance design

- Video renders only when `libmpv` signals a new frame. The render rate
  tracks the media frame rate, not the display refresh rate.
- Video draws straight into the window framebuffer. No intermediate
  copy per frame per output.
- Hardware decode is on by default (`hwdec=auto-safe`). Override with
  the `OWE_HWDEC` environment variable.
- Stills decode once at output size, upload one texture, and then stop
  the frame loop. Paused video stops the loop too.
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

Build deps: `meson`, `ninja`, `gcc`, `pkgconf`, `wayland`, `wayland-protocols`,
`libepoxy`, `mpv`, `ffmpeg` libs, `systemd-libs`.
Runtime deps: `mpv`, `ffmpeg`, `socat`.

Docs: `docs/architecture.md`, `docs/theme-contract.md`, `docs/benchmarks.md`.

# Architecture

## Processes

`owed` owns policy. `owe-render` owns pixels. `owe` owns control.
`owed` spawns `owe-render` as a child and restarts it on crash.
`systemd` manages `owed` only. One supervisor keeps recovery in one place.

## Renderer

One `mpv_handle` plus one `mpv_render_context` with type
`MPV_RENDER_API_TYPE_OPENGL` and `MPV_RENDER_PARAM_WL_DISPLAY`.
One `zwlr_layer_surface_v1` per output on the background layer.
Full anchor, `exclusive_zone=-1`, empty input region, namespace
`owe-background`. When the compositor offers `wp_fractional_scale_v1`
and `wp_viewporter`, the buffer is sized at the preferred fractional
scale and the viewport maps it to the logical surface size. A 1.25
scale output then renders 1.25x pixels, not the 2x that an integer
`wl_output.scale` would ask for. Without those protocols the buffer is
sized `width*scale` by `height*scale` and the surface carries
`buffer_scale=scale`.

`libmpv` options are forced in code. User `mpv.conf` never loads.
`hwdec` defaults to `auto-safe` and `OWE_HWDEC` overrides it.
Audio plays through the default audio output. `panscan=1.0` crops
video to cover the output at its native aspect ratio.

Video rendering is paced by `mpv_render_context_update` frame flags
delivered over a self-pipe. The renderer draws only when the decoder
produces a frame, so a 30 fps video costs 30 renders per second on any
display. Rendering is also paced on `wl_surface.frame` callbacks. The
renderer swaps only after the compositor signals readiness, so a blanked
or stalled output stops the draw loop instead of blocking inside the
graphics driver. An output whose DRM connector reports DPMS off is
skipped. The daemon also publishes the monitors covered by a fullscreen
window, and the renderer stops swapping buffers for those outputs.
`mpv_render_context_render` writes directly into the window
framebuffer. No intermediate framebuffer or blit exists.

The blit shader owns one VAO, one VBO, and one program. Still images
apply a cover crop in the fragment shader: the visible UV sub-rect is
centered and scaled to fill the output without distortion.

Stills decode once through `libavformat`, `libavcodec`, and `libswscale`.
The RGBA output is decoded for the cover crop, not for the output bounds,
so a portrait source keeps its detail when the shader crops the sides.
The decoder rejects images above 64 million pixels.
A still decodes on a worker thread and uploads its texture on the render
thread, so a large image does not stall the control loop.
When an output grows, the current still decodes again at the larger size.
Replacement requests cancel the worker's I/O and keep only the newest queued
request. The event loop reaps the old decoder when it completes; it never
joins an unfinished decoder during media changes. Shutdown joins the worker
before freeing its state.
The daemon requests asynchronous still loads, so its policy loop does not wait for the decoder.
These loads have a 30-second deadline.
Synchronous renderer clients retain deferred replies with a four-second decode deadline.
An expired load cancels its worker and preserves the previous media.
Readiness requires the reported path and kind to match the accepted request.
The renderer uploads one texture, presents the fade, and stops frame requests after the fully opaque final frame.

The outgoing transition image can upload through any available output surface.
Video waits at most one second for that optional image.
If the image is not ready, the renderer cancels the transition and continues video playback.
The event loop discards a cancelled image when its worker completes.

Pause sets `pause` to `yes`. Decode stops, update callbacks stop,
and the last frame stays presented. No frame callbacks get requested
while paused, so the main loop sleeps in `poll`.

## One-shot intros

A still background can play one short intro video at login. The shell
resolves the matching intro, calls `owe intro-prepare <path>`, records the
boot only after the daemon accepts it, then calls `owe intro-commit`. The daemon
starts the renderer if needed and loads the file with `once`, `mute`, and the
current still as its transition source. The renderer presents that still before
the shell releases its layer, then fades it out over the intro. At EOF the renderer
fades the final frame into the matching still and holds it while the shell returns. The
daemon ends an intro early when the screen locks, the session sleeps, a
fullscreen window appears, the background changes, or the renderer dies.
After an intro the daemon returns the engine to the shell. `intro-status`
reports `running` and the last result, and `intro-stop` cancels.
The renderer observes `eof-reached` because `keep-open` can retain playback without an `END_FILE` event.

## Lock feed

A locked session keeps its video. The daemon sends `feed` to the renderer
instead of `pause` when logind or Hyprland reports the session locked and the loaded
media is a video. The renderer resumes playback muted and writes frames
into a three slot shared memory ring, one memfd per slot. `lock-feed.sock`
sits next to `render.sock` and carries a fixed size binary protocol. A
client gets one `HELLO` with the slot descriptors and their fds over
`SCM_RIGHTS`, then one `FRAME` per presented frame and one `ACK` per
consumed frame. Buffers return to the ring when every client acknowledges
them. `owe pause` and `feed-stop` both end the feed. Each output of the
lock screen owns a client, so all outputs show the same decode.

The feed sends buffer descriptors to clients that connect before it starts.
It pauses decode when no clients remain and resumes decode when a client connects.
Each buffer tracks the clients that must acknowledge its frame.
Duplicate acknowledgements and client disconnects cannot release another client's buffer.
Frame sequence numbers remain unique across buffer size changes.
Clients that hold a frame for a second without acknowledging it are disconnected,
so one stalled output cannot exhaust the ring for every other output.
Readback resolution follows the outputs' physical cover requirements, capped
at the source resolution and the protocol's 3840×2160 limit. QML items in the
same process share one CPU copy of a frame. Geometry-only updates reuse the
existing texture. The feed still uses GPU readback and texture uploads;
these changes reduce their cost without claiming zero-copy GPU transport.
The QML client queues acknowledgements when its socket cannot accept a complete reply.
The queue holds at most eight protocol messages.

The QML client reads one protocol message at a time, so descriptors stay with their `HELLO` message.
It retries the connection after a disconnect while `active` remains true.
The daemon ends the feed before sleep, when all monitors turn off, or after a still replaces the video.
The renderer also ends the feed after a successful still decode and requests a desktop redraw.
Stopping the feed releases its shared-memory and GPU buffers. Manual, idle,
blocklist and battery pause rules also apply to a locked session; the animation
override bypasses the automatic rules, but never manual pause, sleep or DPMS.

## Daemon

`owed` watches `~/.local/state/omarchy/current/` with `inotify` and
resolves `background` with `readlink -f`. It watches the directory
because `ln -nsf` swaps the entry atomically.

It owns the engine choice. While a video or GIF plays it starts
`owe-render` and disables the shell background plugin. A still always
belongs to the shell: it enables the plugin, stops the renderer, and
keeps only the daemon. The plugin is disabled after the renderer starts, before waiting for its first
frame: an occluded background surface may never receive a frame callback.
Readiness and fallback are tracked separately from releasing the shell layer.

It connects to the Hyprland event socket for `fullscreen`, `openwindow`,
`closewindow`, `movewindow`, `workspace`, and monitor events. It queries
client and monitor state over the Hyprland request socket with receive
timeouts and parses JSON with vendored `yyjson`. It never spawns
`hyprctl` per event. A 2 second tick refreshes monitor state so DPMS
changes are caught even when Hyprland emits no event.

It uses `sd-bus` for UPower `OnBattery`, logind lock, and
`PrepareForSleep`. Poster extraction and GIF transcodes run on a worker
thread and report completion through an eventfd.
Cancellation terminates FFmpeg and joins the worker before it frees job memory.
Only the worker reaps its FFmpeg child.
Conversions use temporary files and publish complete results with an atomic rename.
Cache keys include the source path, size, nanosecond timestamps, conversion version, and encoding settings.
Startup removes abandoned conversion temporary files older than a day, leaving
recent files alone so an orphaned worker can finish safely.

The daemon tracks what the renderer currently shows. A load is sent only
when the target path or kind changes. Pause and resume are sent only on
state transitions. A video load reply means the renderer accepted the
request, so the daemon polls the renderer status until the first frame
presents. A video that fails to decode, or that never presents a frame
within five seconds, falls back to the last media that proved it plays.
The renderer child is reaped on `SIGCHLD` and restarted with the current
media when it dies. Only the daemon restart path spawns a replacement;
individual IPC sends fail if the child has exited, preserving state recovery.
Retry delays prevent a renderer failure from causing a rapid restart loop.
The daemon retries a failed shell disable without recording a successful handoff.
An explicit media selection clears the handoff retry delay.

## IPC

Both sockets use JSON lines. Each command gets one reply line.
`owed.sock` serves CLI and hook clients. `render.sock` serves the
daemon and direct debug clients. No broadcast exists. Each reply
goes to its own requester, including asynchronous replies after a client slot
is reused. Both servers use the same bounded transport: idle connections and
incomplete requests expire after two minutes, and partial writes are queued
with a four-message-size limit. Slow readers cannot grow the queue indefinitely.
Client reply deadlines cover the complete reply, including fragmented data.
An invalid or oversized reply terminates its connection.

## Shutdown

`owe shutdown` stops the daemon. The daemon stops the renderer child,
frees the bus, and unlinks its socket. The renderer unlinks its own
socket on exit.
The renderer releases media resources and output surfaces before it destroys the EGL context.
It unbinds the context before destruction and closes the Wayland connection last.

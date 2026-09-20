# Performance checks and follow-up work

## Goal

This document tracks the performance checks and follow-up work from the 2026-09-17 review.
Complete the correctness fixes before using affected playback states as benchmark results.

Related documents:

- [Performance budgets and benchmark method](benchmarks.md)
- [Renderer and daemon architecture](architecture.md)
- [Omarchy integration contracts](theme-contract.md)

## Measured baseline

The live sample uses a 3840×2160 H.264 video at 24 FPS with one 2880×1800 output.
The renderer reports VAAPI hardware decode.

| Metric | Result | Sample |
| --- | --- | --- |
| Active renderer CPU | 2.47% of one core | 15 seconds |
| Active daemon CPU | No measured CPU ticks | 15 seconds |
| Renderer resident memory | 232.8 MiB | Active playback |
| Daemon resident memory | 4.0 MiB | Active playback |
| Paused renderer CPU | No measured CPU ticks | 10 seconds |
| Paused daemon CPU | No measured CPU ticks | 10 seconds |
| Playback position change while paused | 0 seconds | 10 seconds |
| Status CLI median latency | 0.54 ms | 30 requests |
| Status CLI p95 latency | 0.68 ms | 30 requests |

CPU results use process ticks and the system clock tick rate.
No measured ticks means usage falls below the sample resolution.
The latency measurement includes the CLI process start.

These samples establish a single-output baseline.
They do not establish battery power consumption, frame delivery quality, or performance on other GPUs.

### Renderer changes measured on 2026-09-17

The renderer now drains the still decoder at EOF, decodes stills for the cover crop,
premultiplies the fade output, runs mpv without its lua scripts and ytdl hook,
uses `wp_fractional_scale_v1` and `wp_viewporter` when the compositor offers them,
processes mpv updates once per loop iteration, skips redundant `eglMakeCurrent` calls,
and accepts `OWE_MPV_OPTIONS` overrides for testing.

Measured on AC with the same fixture and a 2880x1800 output:

| Metric | Before | After |
| --- | --- | --- |
| Renderer CPU | 2.29 to 2.31% of one core | 2.27 to 2.29% |
| Renderer RSS after load | 194.8 to 195.1 MiB | 185.7 MiB |
| Renderer threads | 21 | 14 |

CPU stays within run-to-run noise. The thread and memory reductions come from dropping unused mpv scripts.
The same machine on battery measures about twice the CPU of an AC sample.
Compare samples only within one power state.

On a 3840x2160 output at scale 1.25, the fractional-scale change measures:

| Metric | Integer scale only | Fractional scale |
| --- | --- | --- |
| Render buffer | 6144x3456 | 3840x2160 |
| GPU render engine | 2.9 to 6.9% | 1.5 to 3.3% |
| Primary GPU allocation | 181 MiB | 81 MiB |

A fractional-scale output also stops the extra upscale and downscale of the wallpaper.

### Final benchmark, 2026-09-17

`bench/bench.sh` with three runs of five seconds on the same fixture, one 3840x2160 output at scale 1.25, on AC:

| State | Renderer CPU | Renderer RSS | Daemon CPU |
| --- | --- | --- | --- |
| playing, always-animate | 2.2 to 2.4% of one core | 197.7 to 207.7 MiB | no measured ticks |
| paused, manual | no measured ticks | 207.6 to 207.7 MiB | no measured ticks |
| still | no measured ticks | 226.1 MiB | no measured ticks |

GPU while playing: render engine 2.4 to 8.5%, video engine 27.8%.
The primary GPU allocation is 81 MiB.
The renderer holds 14 threads.
Resident memory grows during the first minute as decoder buffers fill, then settles.

### Memory deep dive, 2026-09-17

A five minute soak held the renderer at about 210 MiB after the first minute, with 137 MiB anonymous and 74 MiB file backed.
The top anonymous mappings were three glibc arenas of 20 to 27 MiB each.
Reducing the arena count made RSS worse, from 184 MiB to 196 MiB.
Forcing allocations above 128 KiB through mmap and returning freed top chunks to the OS dropped RSS to 175 to 184 MiB.

| Setting | RSS at 90 seconds |
| --- | --- |
| Defaults | 211 MiB |
| `M_TRIM_THRESHOLD=0` | 183 MiB |
| `M_MMAP_THRESHOLD=128KiB` | 183 MiB |
| Both, arena count 1 | 196 MiB |
| `hwdec=vaapi-copy` | 271 MiB |

The renderer now sets both thresholds at startup. Decode options did not change RSS, and the GPU context holds 220 MiB of device memory either way.
The remaining anonymous memory is live media and Mesa buffers.
The file backed memory is the Mesa driver, LLVM, and the iHD driver, shared with other processes.

### Multi-output and battery, 2026-09-17

A headless output joined the session as a second 3840x2160-capable output.
The renderer saw both outputs and one decode served them. The second 1080p
output added 0.2 percentage points of one core, from 2.6 to 2.8 percent,
and 3 MiB of resident memory. Thread count stayed at 14. Removing the
output at runtime returned the same values, so hotplug and removal work.

`battery_mode` now selects playback on battery: `play`, `pause` to hold the
current frame, or `poster` to extract a still. `battery_poster = true`
remains an alias for poster mode. `owe config` reports the mode.

### DPMS stall and per-output skip, 2026-09-17

A DPMS blank used to stall the renderer inside Mesa's buffer swap for the
whole blank. The renderer did not answer IPC until the screen returned,
and the delay to pause playback depended on the pending swap.

The DRM connector state lags the compositor by about 800 ms, and Mesa's
swap waits in `poll` for a buffer that a blanked compositor never
releases. The renderer now paces swaps on `wl_surface.frame` callbacks.
When the compositor stops presenting, no callback arrives, so no swap
starts. It also re-reads the DRM connector state without a cache
immediately before each swap.

Live checks on eDP-1 with a 4K24 video:

| Check | Result |
| --- | --- |
| `owe render-status` during a blank | Replies, `paused=true` |
| Playback position while paused | Frozen at 3.542 s |
| Resume on DPMS on | Advances from 6.708 s to 7.708 s |
| Renderer during a blank | No stall, no blocked swap |

A fullscreen window now stops swapping only on the covered output. With a
headless second output, a fullscreen window on eDP-1 gave `skipped=eDP-1`
in `owe render-status` while the policy stayed `visible` and the headless
output kept its frames. Removing the window cleared the skip.

### Still handoff, 2026-09-18

A still background never runs the renderer. While a still shows, owed
enables the shell background plugin, stops the renderer, and keeps only
the daemon. A video or GIF starts the renderer again.

Measured on eDP-1 with a still background:

| Measure | Value |
| --- | --- |
| Renderer process | stopped |
| Renderer RSS | 0 |
| Daemon RSS | 3.8 MiB |
| Shell background plugin | enabled |
| CPU | 0 |

An always-on renderer for stills measured about 121 MiB RSS and is no
longer supported.

The handoff disables the shell plugin before the renderer starts. An
occluded renderer receives no frame callbacks, so waiting for readiness
first would deadlock.

The renderer stops its swap loop while the compositor presents nothing,
which handles a blanked screen. The daemon pauses playback when Hyprland
reports the monitors off.

### Completed checks

- All six registered test suites pass.
- All six suites pass under AddressSanitizer and UndefinedBehaviorSanitizer.
- The full sanitizer build passes with compiler warnings treated as errors.
- The tree builds at `-O3` with `-Werror`. It did not before, so release builds were impossible.
- All seven live integration contract checks pass.
- Still decode, pause-after-failure, fade alpha, configuration, and IPC client-capacity defects were confirmed by isolated probes.
- Still decode, pause-after-failure, idle pause, fade alpha, and fractional scale are now fixed and covered by tests or measured probes.

The sanitizer tests do not exercise the full renderer lifecycle in a live Wayland session.
The integration contract checks verify presence and basic responses, not resource use or frame delivery.

## Priority 1: Fix defects that invalidate measurements

### Still decode and battery posters

PNG and AVIF fixtures failed in the still decoder because it never drained delayed frames after input ends.
Generated battery posters failed for the same reason because they use PNG.
The decoder now flushes with a NULL packet at EOF.
The new `decode` test covers PNG, JPEG, AVIF, and the size cap.
A live PNG background now loads and presents.

- [x] Drain the decoder correctly at EOF.
- [x] Add successful decode tests for PNG, JPEG, and AVIF.
- [ ] Test the other advertised still formats against the installed FFmpeg build.
- [ ] Confirm that a still load reaches the final opaque frame.
- [ ] Measure settled still CPU and GPU use after the fix.

References: `src/render/still.c:94–133`, `src/daemon/transcode.c:171–213`, `test/decode.c`.

### Pause policy after a failed load

A rejected load used to clear the daemon's loaded-media state, although the renderer keeps the previous video.
The failure guard then prevented later pause commands from reaching the renderer.
The daemon now keeps the last successful loaded media and applies playback state even while the source is failed.
The new `daemon` test drives a mock renderer and asserts that pause reaches it after a rejected load.

- [x] Preserve the last successful loaded-media state after a rejection.
- [x] Apply pause policy independently from media-load failures.
- [x] Test manual pause after an invalid still load.
- [ ] Test a failed GIF conversion and the lock, fullscreen, DPMS, and sleep policies after a failed load.
- [ ] Verify renderer state and playback position for every pause benchmark.

References: `src/daemon/main.c:57–75`, `src/daemon/main.c:108–122`, `test/daemon.py`.

### Playback readiness and asynchronous failure

A video load reply acknowledges the request before decode succeeds.
The daemon does not track later decode failure or first-frame readiness.
A successful IPC reply is therefore insufficient evidence that playback starts.

- [x] Track requested, ready, and failed media states.
- [x] Retain the previous wallpaper until replacement media becomes ready.
- [x] Expose asynchronous playback failures through daemon status.
- [ ] Measure request-to-first-presented-frame latency.
- [x] Test corrupt and unsupported videos during active playback.

References: `src/render/render_ipc.c:115–124`, `src/render/mpv.c:241–246`, `src/daemon/main.c:57–68`.

## Priority 2: Repair the benchmark method

The current script labels states without verifying them.
With the default configuration, visible ordinary windows do not cause the `policy-paused` state.
An existing manual pause can also invalidate the `video-playing` state.

- [x] Save the initial manual-pause and animation-override flags.
- [x] Restore the initial flags on normal exit and interruption.
- [x] Verify daemon policy and renderer state before each sample.
- [x] Create an explicit policy condition for the policy-pause sample.
- [x] Add an actual still-image sample.
- [ ] Reject samples when media fails, the renderer exits, or the renderer PID changes.
- [x] Select the renderer from the tested daemon's child process.
- [x] Read `SC_CLK_TCK` instead of assuming 100 ticks per second.
- [x] Record elapsed monotonic time instead of assuming the requested sleep duration.
- [x] Capture daemon resources alongside renderer resources.
- [x] Capture renderer status, hardware decode state, source dimensions, codec, and frame rate.
- [ ] Record output dimensions, refresh rates, scales, GPU, driver, and build options.
- [ ] Separate startup, warm-up, transition, and settled-state samples.
- [x] Repeat each settled-state sample to expose run-to-run variation.
- [ ] Summarize latency distributions and memory peaks, not only averages.

Until the script restores playback state, run it only in a dedicated test session.
Treat existing benchmark state labels as unverified unless the recorded status confirms them.

Reference: `bench/bench.sh:12–57`.

## Priority 3: Complete the missing performance matrix

| Area | Missing checks | Evidence to collect |
| --- | --- | --- |
| Stills | Small images, large images, portrait images, panoramas, alpha images, and the pixel limit | Decode time, peak RSS, upload time, texture size, settled CPU, and GPU activity |
| Video | 1080p and 4K sources at 24, 30, and 60 FPS | CPU, RSS, GPU engine use, delivered frames, dropped frames, and presentation intervals |
| Decode fallback | Supported hardware decode, unsupported codecs, and forced software decode | Actual decoder selection, CPU, GPU use, frame delivery, and recovery |
| Multiple outputs | Two or more outputs with equal and different dimensions | Decoder count, per-output frame delivery, CPU, GPU use, and memory growth |
| Display timing | Mixed refresh rates and fractional scales | Frame pacing, buffer dimensions, redundant work, and visible image quality |
| Output lifecycle | Hotplug, unplug, resolution changes, scale changes, and no connected outputs | Recovery time, texture size updates, resource release, and IPC response time |
| Pause states | Manual, fullscreen, lock, occupied workspace, blocklist, and DPMS | Policy latency, renderer pause state, stable playback position, CPU, and GPU activity |
| Sleep | Suspend and resume during video playback and conversion | Pause delivery, recovery time, process health, and stuck jobs |
| Battery | AC-to-battery and battery-to-AC changes with poster mode | Poster readiness, decode shutdown, power use, and playback recovery |
| GIF conversion | Cold cache, warm cache, large GIFs, cancellation, and rapid selection changes | Conversion duration, child-process CPU, peak RSS, cache size, and cancellation latency |
| Media changes | Still-to-still, still-to-video, video-to-still, and video-to-video | First-frame latency, transition cost, memory peaks, and control latency |
| IPC load | Concurrent clients, fragmented requests, idle clients, and slow readers | p95 and p99 latency, response loss, client cleanup, and renderer frame delivery |
| Recovery | Renderer exit, decode failure, slow media, and repeated failed requests | Restart latency, fallback behavior, retry rate, resource use, and status accuracy |
| Long sessions | Repeated media changes and output changes over an extended run | RSS trend, GPU memory trend, file descriptors, threads, cache growth, and orphan processes |

Measure battery power or energy directly under controlled conditions.
Low CPU usage alone does not establish low power consumption.

For the comparison with the Omarchy shell, use the same source, output configuration, and system state.
Keep only the tested background renderer active during each comparison sample.

## Performance follow-up work

### Move still decode off the control loop

Still decode currently runs inside the renderer's IPC handler.
The daemon synchronously waits for the load reply with a five-second receive timeout.

- [x] Decode still images in a worker.
- [x] Keep OpenGL texture upload on the render thread.
- [x] Cancel obsolete decode requests after a new selection.
- [ ] Measure pause and status latency during large-image loads.
- [ ] Test slow loads that exceed the current IPC timeout.

References: `src/render/render_ipc.c:127–135`, `src/render/still.c:192–222`, `src/daemon/supervisor.c:189–199`.

### Correct texture size and transition output

The decoder reduces images to fit inside the output bounds before the shader applies a cover crop.
A 1200×2400 fixture becomes 540×1080 for a 1920×1080 target, which discards detail before enlargement.
The renderer also retains the existing texture after a larger output appears.

The fade shader changes alpha without premultiplying RGB.
The renderer deletes the previous texture before the transition completes.

The fade shader now premultiplies its output, so a half fade reads as (128,128,128,128) for white, not (255,255,255,128).

- [x] Size decoded textures for the required cover crop.
- [x] Update texture requirements after output changes.
- [x] Premultiply the fade output for Wayland.
- [ ] Verify the final transition frame before measuring settled resource use.
- [ ] Compare memory cost and image quality across portrait and landscape outputs.
- [ ] Test extreme aspect ratios against the 64 million pixel decoder limit.

References: `src/render/still.c:123–138`, `src/render/still.c:228–235`, `src/render/egl.c:43–54`, `src/render/wayland.c:95–111`.

### Bound control-loop work

Hyprland state queries and renderer requests run synchronously in the daemon.
Sixteen idle IPC clients occupy all client slots and prevent new status requests.

- [ ] Measure control latency during Hyprland event bursts and request timeouts.
- [x] Add client idle deadlines.
- [x] Handle partial replies and slow readers without losing responses.
- [ ] Measure blocklist scan and log costs with the feature enabled.
- [x] Reduce duplicate state queries if profiling shows measurable cost.
- [x] Recover the Hyprland connection after a compositor restart with a new signature.

References: `src/daemon/hypr.c:44–74`, `src/daemon/daemon_ipc.c:278–295`, `src/common/common_ipc.c:84–105`, `src/daemon/main.c:264–307`.

### Make power-policy configuration reliable

The configuration parser silently ignores a valid multiline blocklist array.
The idle helper also clears the manual-pause flag when activity resumes.
These behaviors can invalidate policy measurements and user expectations.

- [x] Parse supported TOML syntax correctly or reject unsupported syntax explicitly.
- [x] Expose blocklist entries and transcode dimensions in effective configuration output.
- [x] Track idle pause separately from manual pause.
- [ ] Record effective configuration with each benchmark result.

References: `src/common/config.c:81–121`, `src/daemon/daemon_ipc.c:99–110`, `hooks/owe-idle:15–17`.

### Measure optimized builds and bound cache growth

The current installer uses Meson's default debug build.
The reviewed build has optimization level `0`.
GIF and poster caches have no size limit or eviction policy.

- [x] Compare debug and release builds with identical inputs.
- [x] Select an explicit build type for installed binaries.
- [x] Align the package binary path with the service executable path.
- [ ] Measure cache growth across source changes and encoding-setting changes.
- [x] Add cache size reporting and cleanup controls.
- [x] Define a cache budget and eviction policy.

References: `packaging/install.sh:23`, `packaging/PKGBUILD:14–28`, `systemd/owed.service:8`, `src/daemon/transcode.c`.

## Verification commands

To create an optimized comparison build, run:

```bash
meson setup build-release --buildtype=release -Dwerror=true
meson compile -C build-release
meson test -C build-release --print-errorlogs
```

To check memory and undefined behavior in the registered tests, run:

```bash
meson setup build-sanitize -Db_sanitize=address,undefined -Db_lundef=false -Dwerror=true
meson compile -C build-sanitize
meson test -C build-sanitize --print-errorlogs
```

The live test requires an explicitly started test daemon and a video wallpaper.
To check live IPC and basic playback in that session, run:

```bash
build-release/test/owe-live-test
```

These commands do not replace the performance matrix or live renderer lifecycle tests.

## Completion criteria

- [ ] Each benchmark sample includes a verified playback state and environment metadata.
- [ ] Tests cover successful still decode and pause policy after media failures.
- [ ] Settled stills and paused video meet the existing CPU budget within measurement resolution.
- [ ] Measurements establish multi-output decode sharing and frame delivery behavior.
- [ ] Reports separate CPU use, GPU use, memory, latency, and power consumption.
- [ ] Long-session tests show bounded process resources and a defined cache budget.
- [ ] Updated benchmark documentation includes reproducible results and remaining coverage gaps.

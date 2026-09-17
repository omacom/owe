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

### Completed checks

- All four registered test suites pass.
- All four suites pass under AddressSanitizer and UndefinedBehaviorSanitizer.
- The full sanitizer build passes with compiler warnings treated as errors.
- All seven live integration contract checks pass.
- Isolated probes confirm still decode, pause-after-failure, fade alpha, configuration, and IPC client-capacity defects.

The sanitizer tests do not exercise the full renderer lifecycle in a live Wayland session.
The integration contract checks verify presence and basic responses, not resource use or frame delivery.

## Priority 1: Fix defects that invalidate measurements

### Still decode and battery posters

PNG and AVIF fixtures fail in the current still decoder.
The decoder does not drain delayed frames after input ends.
Generated battery posters also fail because they use PNG.

- [ ] Drain the decoder correctly at EOF.
- [ ] Add successful decode tests for PNG, JPEG, AVIF, and generated posters.
- [ ] Test the other advertised still formats against the installed FFmpeg build.
- [ ] Confirm that a still load reaches the final opaque frame.
- [ ] Measure settled still CPU and GPU use after the fix.

References: `src/render/still.c:94–119`, `src/daemon/transcode.c:171–213`.

### Pause policy after a failed load

A rejected load clears the daemon's loaded-media state, although the renderer can retain the previous video.
The failure guard then prevents later pause commands from reaching the renderer.
An isolated probe reports manual pause in daemon status while the renderer continues playback.

- [ ] Preserve the last successful loaded-media state after a rejection.
- [ ] Apply pause policy independently from media-load failures.
- [ ] Test manual pause after an invalid still load and a failed GIF conversion.
- [ ] Test lock, fullscreen, DPMS, and sleep policy after a failed load.
- [ ] Verify renderer state and playback position for every pause benchmark.

References: `src/daemon/main.c:60–65`, `src/daemon/main.c:103–116`.

### Playback readiness and asynchronous failure

A video load reply acknowledges the request before decode succeeds.
The daemon does not track later decode failure or first-frame readiness.
A successful IPC reply is therefore insufficient evidence that playback starts.

- [ ] Track requested, ready, and failed media states.
- [ ] Retain the previous wallpaper until replacement media becomes ready.
- [ ] Expose asynchronous playback failures through daemon status.
- [ ] Measure request-to-first-presented-frame latency.
- [ ] Test corrupt and unsupported videos during active playback.

References: `src/render/render_ipc.c:115–124`, `src/render/mpv.c:241–246`, `src/daemon/main.c:57–68`.

## Priority 2: Repair the benchmark method

The current script labels states without verifying them.
With the default configuration, visible ordinary windows do not cause the `policy-paused` state.
An existing manual pause can also invalidate the `video-playing` state.

- [ ] Save the initial manual-pause and animation-override flags.
- [ ] Restore the initial flags on normal exit and interruption.
- [ ] Verify daemon policy and renderer state before each sample.
- [ ] Create an explicit policy condition for the policy-pause sample.
- [ ] Add an actual still-image sample.
- [ ] Reject samples when media fails, the renderer exits, or the renderer PID changes.
- [ ] Select the renderer from the tested daemon's child process.
- [ ] Read `SC_CLK_TCK` instead of assuming 100 ticks per second.
- [ ] Record elapsed monotonic time instead of assuming the requested sleep duration.
- [ ] Capture daemon resources alongside renderer resources.
- [ ] Capture renderer status, hardware decode state, source dimensions, codec, and frame rate.
- [ ] Record output dimensions, refresh rates, scales, GPU, driver, and build options.
- [ ] Separate startup, warm-up, transition, and settled-state samples.
- [ ] Repeat each settled-state sample to expose run-to-run variation.
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

- [ ] Decode still images in a worker.
- [ ] Keep OpenGL texture upload on the render thread.
- [ ] Cancel obsolete decode requests after a new selection.
- [ ] Measure pause and status latency during large-image loads.
- [ ] Test slow loads that exceed the current IPC timeout.

References: `src/render/render_ipc.c:127–135`, `src/render/still.c:192–222`, `src/daemon/supervisor.c:189–199`.

### Correct texture size and transition output

The decoder reduces images to fit inside the output bounds before the shader applies a cover crop.
A 1200×2400 fixture becomes 540×1080 for a 1920×1080 target, which discards detail before enlargement.
The renderer also retains the existing texture after a larger output appears.

The fade shader changes alpha without premultiplying RGB.
The renderer deletes the previous texture before the transition completes.

- [ ] Size decoded textures for the required cover crop.
- [ ] Update texture requirements after output changes.
- [ ] Produce correct alpha or blend both images into an opaque framebuffer.
- [ ] Verify the final transition frame before measuring settled resource use.
- [ ] Compare memory cost and image quality across portrait and landscape outputs.

References: `src/render/still.c:123–138`, `src/render/still.c:228–235`, `src/render/egl.c:43–54`, `src/render/wayland.c:95–111`.

### Bound control-loop work

Hyprland state queries and renderer requests run synchronously in the daemon.
Sixteen idle IPC clients occupy all client slots and prevent new status requests.

- [ ] Measure control latency during Hyprland event bursts and request timeouts.
- [ ] Add client idle deadlines.
- [ ] Handle partial replies and slow readers without losing responses.
- [ ] Measure blocklist scan and log costs with the feature enabled.
- [ ] Reduce duplicate state queries if profiling shows measurable cost.

References: `src/daemon/hypr.c:44–74`, `src/daemon/daemon_ipc.c:278–295`, `src/common/common_ipc.c:84–105`, `src/daemon/main.c:264–307`.

### Make power-policy configuration reliable

The configuration parser silently ignores a valid multiline blocklist array.
The idle helper also clears the manual-pause flag when activity resumes.
These behaviors can invalidate policy measurements and user expectations.

- [ ] Parse supported TOML syntax correctly or reject unsupported syntax explicitly.
- [ ] Expose blocklist entries and transcode dimensions in effective configuration output.
- [ ] Track idle pause separately from manual pause.
- [ ] Record effective configuration with each benchmark result.

References: `src/common/config.c:81–121`, `src/daemon/daemon_ipc.c:99–110`, `hooks/owe-idle:15–17`.

### Measure optimized builds and bound cache growth

The current installer uses Meson's default debug build.
The reviewed build has optimization level `0`.
GIF and poster caches have no size limit or eviction policy.

- [ ] Compare debug and release builds with identical inputs.
- [ ] Select an explicit build type for installed binaries.
- [ ] Align the package binary path with the service executable path.
- [ ] Measure cache growth across source changes and encoding-setting changes.
- [ ] Add cache size reporting and cleanup controls.
- [ ] Define a cache budget and eviction policy.

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

# Benchmarks

For missing checks and follow-up work, see [Performance checks and follow-up work](performance-follow-up.md).

## Method

Run `bench/bench.sh [outdir]` with a working video wallpaper. It measures
renderer and daemon CPU ticks and resident memory using `/proc`. The script
identifies the daemon through its socket credentials and verifies the renderer
belongs to that daemon. It records effective configuration and each sample's
before/after status and resource measurements as JSON.

States sampled:

- `playing` clears manual pause and enables `always-animate`.
- `paused` uses manual pause.
- `policy` sets the idle pause explicitly.
- `still` runs when `OWE_BENCH_STILL` names an image, and measures the daemon
  after the renderer has stopped.

Each state is sampled three times for five seconds by default. Set
`OWE_BENCH_RUNS` and `OWE_BENCH_SECONDS` to change these durations. The summary
reports median CPU and maximum sampled RSS. Playback state is verified before,
during and after each sample; a process restart, media change, failed load or
advancing paused video aborts the run. Locked sessions require a separate feed
benchmark. The initial manual pause, idle pause, animation override and any
changed background are restored on normal exit, failure, SIGINT or SIGTERM.

The periodic verification itself adds a small amount of control traffic. These
measurements do not establish GPU utilization, frame pacing or battery power;
capture those separately under a controlled display configuration. No new
hardware performance claims are implied by the regression tests.

## Budget

- Paused video costs the same CPU as a still image, within noise.
- Active 1080p30 H.264 shows hardware decode in `gputop` and costs
  under half the CPU of the QtMultimedia path on the same file.
- One shared file runs one decode on N monitors.
- Stills cost zero sustained CPU after the fade settles.
- With `battery_poster = true`, battery mode stops decode and holds a
  poster frame.
- No poll loop runs faster than 1 s except bounded transients.

## Reference results

These numbers come from the development machine: Intel Panther Lake Arc
iGPU on the Xe driver, Hyprland 0.56, one 2880x1800@120 output at scale 2.
Compare against local results, not across machines.

Renderer CPU while a 4K30 H.264 video plays with hardware decode,
measured over 15 seconds from `/proc/<pid>/stat`:

- Playing: about 2.6 percent of one core.
- Policy paused: about 0 percent.
- Manual paused: about 0 percent.
- Video engine busy about 25 percent in `gputop`.
- Resident memory about 220 MB for a 4K source.
- 1080p60 playing: about 4.8 percent of one core.

The daemon itself costs about 4 MB and near zero CPU.

The video renders at its own frame rate. The display runs at 120 Hz,
and the renderer does not follow the display.

A second output shares the one decode. On the development machine a
second 1080p output added about 0.2 percentage points of one core, from
2.6 to 2.8 percent, and 3 MiB of resident memory. Thread count did not
change. Removing the output at runtime recovered the same values.

## GPU notes

This machine uses Intel Panther Lake Arc iGPU on the Xe driver.
`intel_gpu_top` does not support Xe. Use `gputop` instead.
Columns `rcs` and `vcs` show render and video engine load.

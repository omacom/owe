# Benchmarks

For missing checks and follow-up work, see [Performance checks and follow-up work](performance-follow-up.md).

## Method

Run `bench/bench.sh [outdir]`. It samples renderer CPU ticks from
`/proc/<pid>/stat` per state and captures `gputop` rows per state.
It stores `owe status` JSON per state. No absolute numbers ship here.
Compare each run against the local baseline.

States sampled:

- `video-playing` with `always-animate on`.
- `policy-paused` with `always-animate off` and windows visible.
- `manual-paused` after `owe pause`.

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

## GPU notes

This machine uses Intel Panther Lake Arc iGPU on the Xe driver.
`intel_gpu_top` does not support Xe. Use `gputop` instead.
Columns `rcs` and `vcs` show render and video engine load.

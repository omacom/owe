# Black background diagnosis

`OWE_HWDEC=no` selects software video decode.
The renderer still uses OpenGL, EGL, and Wayland to display frames.
A black background with this setting does not establish a decoder failure or a driver failure.

## Capture the failure

Run these commands on the affected machine while the background is black:

```bash
owe --version
owe status
owe render-status
owe config
systemctl --user show owed.service --property=ExecStart --property=FragmentPath
journalctl --user -u owed.service -b -n 200 --no-pager -o cat
hyprctl -j monitors
```

The service executable path identifies the daemon that systemd actually starts.
The current source build logs the EGL vendor, OpenGL renderer, OpenGL version, and requested hardware decoder.
It also sends libmpv warnings and errors to the journal.

For NVIDIA reports, include these results:

```bash
nvidia-smi --query-gpu=name,driver_version --format=csv,noheader
uname -sr
pacman -Q mpv ffmpeg libglvnd egl-wayland nvidia-utils
```

To compare DRM connector state with Hyprland monitor state, run:

```bash
for connector in /sys/class/drm/card*-*; do
  [ -f "$connector/status" ] || continue
  printf '%s\n' "${connector##*/}"
  for key in status enabled dpms; do
    [ -r "$connector/$key" ] || continue
    IFS= read -r value < "$connector/$key"
    printf '  %s=%s\n' "$key" "$value"
  done
done
```

Useful fields in the current renderer status:

| Field | Interpretation |
| --- | --- |
| `outputs` | Zero means the renderer has no output surfaces. |
| `skipped` | Lists outputs that OWE skips because of fullscreen coverage or DRM DPMS state. |
| `swaps` | Counts successful desktop EGL swaps across current outputs. |
| `swap_failures` | Counts failed desktop EGL swaps across current outputs. |
| `drm_dpms` | Shows whether the process uses DRM sysfs DPMS checks. |
| `error` | Reports media or libmpv render failures. |

Swap counts restart with the renderer and decrease when an output disappears.
A successful swap confirms buffer submission, not compositor presentation or correct pixel contents.
Lock-feed frames do not use desktop swaps.

## Isolate DRM DPMS checks

OWE normally uses DRM sysfs state as an additional guard against swaps on powered-off outputs.
An incorrect `Off` result can therefore prevent every desktop frame, independently of the decoder.
This is one possible cause to test, not a confirmed explanation for the NVIDIA report.

The following diagnostic switch requires OWE 0.2.7 or later.

1. Open a dedicated user-service override:

   ```bash
   systemctl --user edit --drop-in=owe-dpms-debug.conf owed.service
   ```

2. Put these lines in the override:

   ```ini
   [Service]
   Environment=OWE_DRM_DPMS=0
   ```

3. Restart the service:

   ```bash
   systemctl --user restart owed.service
   ```

4. Capture both status replies again:

   ```bash
   owe status
   owe render-status
   ```

Both replies report `drm_dpms:false` while the renderer runs.
The daemon still follows Hyprland DPMS state and its other pause rules.
The renderer still waits for compositor frame callbacks.

If the background appears only with this override, include the before and after results in the issue.
If it remains black, include the EGL and libmpv journal messages and the swap counters.

To remove this diagnostic override:

1. Delete its dedicated file:

   ```bash
   rm "${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user/owed.service.d/owe-dpms-debug.conf"
   ```

2. Reload the service definitions:

   ```bash
   systemctl --user daemon-reload
   ```

3. Restart the service:

   ```bash
   systemctl --user restart owed.service
   ```

## Submit the report

Open [a new OWE issue](https://github.com/omacom/owe/issues/new).
Include the hardware details, commands tried, and diagnostic output.
If GitHub rejects the issue, provide its exact error message through the current support channel.

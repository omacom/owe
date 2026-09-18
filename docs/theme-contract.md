# Theme contract

`owe` joins the Omarchy theme switch through supported contracts only.
It adds no Omarchy-side hooks and edits no packaged file.

## Contracts used

- Symlink: `~/.local/state/omarchy/current/background`. `owe set` updates this entry atomically after file validation. `owed` watches
  it with `inotify`. This single path covers `theme bg set`,
  `theme bg next`, `theme set`, headless mode, and skip-background mode.
- Hook: `theme-set.d/10-owe-sync` installed through
  `omarchy hook install theme-set`. It receives the theme slug in `$1`
  and sends `{"cmd":"refresh"}` to the daemon. It does no heavy work.
- Shell config: `owed` controls `omarchy.background` in `shell.json` at
  runtime through `omarchy-shell shell setPluginEnabled`. It disables the
  plugin while a video or GIF plays, and enables it while a still shows.
  With `renderer_mode = "always"` the plugin stays disabled.
- Lock screen: `LockView` renders its own `BackgroundMedia` from the
  symlink. It needs no background plugin.
- Bar sampler, `bg-switcher`, and `bg-cache`: they read files and the
  symlink. They keep their behavior.

## Shell handoff

Only one background layer owner wins. The installer disables the shell
renderer so `owe-render` owns the background layer alone.
Uninstall re-enables the background plugin in the current `shell.json`.
It preserves shell settings added after installation.

## Transitions

The shell reveal wipe goes away with the shell renderer. Still to still
switch uses the renderer GPU fade. Video changes use a hard cut.
A renderer load reply acknowledges the request. It does not guarantee successful asynchronous video decode.
`owe render-status` reports decode failures in its `error` field.

## Drift checks

Re-run `test/contract.sh` after each `omarchy update`. It asserts the
symlink path, both sockets, both status replies, and the
`owe-background` layer.

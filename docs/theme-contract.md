# Theme contract

`owe` joins the Omarchy theme switch through supported contracts only.
It adds no Omarchy-side hooks and edits no packaged file.

## Contracts used

- Symlink: `~/.local/state/omarchy/current/background`. `owed` watches
  it with `inotify`. This single path covers `theme bg set`,
  `theme bg next`, `theme set`, headless mode, and skip-background mode.
- Hook: `theme-set.d/10-owe-sync` installed through
  `omarchy hook install theme-set`. It receives the theme slug in `$1`
  and sends `{"cmd":"refresh"}` to the daemon. It does no heavy work.
- Shell config: the installer adds `omarchy.background` to
  `disabledPlugins[]` in `~/.config/omarchy/shell.json` with a
  timestamped backup. `background set` and `background themeTransition`
  then fail quietly. `omarchy-theme-set` falls back to
  `shell applyTheme` in each branch, so theme colors still apply live.
- Lock screen: `LockView` renders its own `BackgroundMedia` from the
  symlink. It needs no background plugin.
- Bar sampler, `bg-switcher`, and `bg-cache`: they read files and the
  symlink. They keep their behavior.

## Shell handoff

Only one background layer owner wins. The installer disables the shell
renderer so `owe-render` owns the background layer alone. Uninstall
restores `shell.json` from backup. The shell renderer takes over again
on the next `theme bg set`.

## Transitions

The shell reveal wipe goes away with the shell renderer. Still to still
switch uses the renderer GPU fade. Each video switch cuts hard after
first-frame confirm. Theme switch with video on one side already cuts
instantly in the shell today, so a cut matches current behavior.

## Drift checks

Re-run `test/contract.sh` after each `omarchy update`. It asserts the
symlink path, both sockets, both status replies, and the
`owe-background` layer.

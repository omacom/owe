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
  The renderer handles battery posters and still fallback if the shell is unavailable.
- Lock screen: a consumer can import `Owe.LockFeed` to show the daemon's
  shared video feed. `BackgroundMedia` remains the fallback for stills or
  when the feed is inactive. It needs no background plugin.
- Bar sampler, `bg-switcher`, and `bg-cache`: they read files and the
  symlink. They keep their behavior.

## Shell handoff

The daemon manages ownership at runtime: it enables the shell background
plugin for stills and releases that layer after the video renderer starts.
Uninstall re-enables the background plugin in the current `shell.json`.
It preserves shell settings added after installation.

## Transitions

Stills belong to the shell, so a still to still switch uses the shell
reveal wipe. Video and GIF changes use a hard cut. A battery poster fades
in through the renderer GPU fade.
A renderer load reply acknowledges the request. It does not guarantee successful asynchronous video decode.
`owe render-status` reports decode failures in its `error` field.

## Drift checks

Re-run `test/contract.sh` after each `omarchy update`. It asserts the
symlink path, both sockets, both status replies, and the
`owe-background` layer.

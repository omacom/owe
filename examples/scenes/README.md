# Example scenes

Two scenes for backgrounds Omarchy already ships, each made to match the video it could stand in for. They are found by the still's name, so with the `omarchy-themes-backgrounds` package installed they run as they are:

```sh
owe scene info examples/scenes/catppuccin/1-totoro/scene.jsonc
owe scene frame examples/scenes/catppuccin/1-totoro/scene.jsonc --part arrive --at 2 --out totoro.png
owe scene frame examples/scenes/catppuccin-latte/1-color-fade/scene.jsonc --at 6 --out latte.png
```

To see one play, copy its directory to `backgrounds/scenes/<name>/` beside the still in the current theme and select that background.

| Scene | What it is | Size | The videos it matches |
|---|---|---|---|
| `catppuccin/1-totoro` | Cut pieces: eleven soot sprites that bob, eyelids that blink, and an arrive part where the circle grows, Totoro rises and the sprites pop in. | 210 KB | The loop and the intro, 414 KB together |
| `catppuccin-latte/1-color-fade` | No pieces: one shader of the scene's own moves the whole picture on slow waves. | 1.8 KB | The loop, 4.1 MB |

[`docs/scenes.md`](../../docs/scenes.md) describes the format.

# Scenes

A scene moves a still without a video. It is a small file that names pieces cut from the still and areas of it, and says how they move. OWE draws the still and draws the moving parts over it, so nothing is decoded and what ships is the description, not the frames.

A scene belongs to one still. With nothing moving it is that still, to the pixel, at any screen size.

## Where a scene lives

A still at `backgrounds/road.webp` has its scene at `backgrounds/scenes/road/scene.jsonc`, with the scene's pictures and shaders beside it. The background link keeps naming the still.

- The scene's **idle** part plays where a theme loop would, under the same rules: while animations are on and the machine is on AC power. A scene wins over a loop of the same name, and a scene that will not play leaves the still to its loop.
- The scene's **arrive** part, when it has one, plays where an intro would: `owe intro` and `owe intro-prepare` take a `scene.jsonc` in place of a video.

## The file

JSON with comments. Places are shares of the still: `at` is left, top, width and height from 0 to 1, and distances a piece moves are shares of the still's height.

```jsonc
{
  // The still this scene belongs to, and its checksum. A scene is refused for any other picture.
  "still": "catppuccin/1-totoro.webp",
  "sha256": "4896da62…",

  // Shapes in the still. "mask" is a picture of the shape; without one the area is its whole rectangle.
  "areas": [
    { "name": "eyes", "mask": "pieces/eyes.png", "at": [0.451, 0.578, 0.101, 0.043] }
  ],

  // Pieces cut from the still, back to front.
  "pieces": [
    { "name": "sprite-1", "picture": "pieces/sprite-1.png", "behind": "pieces/sprite-1-behind.png", "at": [0.39, 0.33, 0.05, 0.09] },
    { "name": "eyelids", "picture": "pieces/eyelids.png", "at": [0.451, 0.578, 0.101, 0.043], "inside": "eyes", "rest": { "y": -0.047 } }
  ],

  // Repeats for as long as the desktop may move.
  "idle": {
    "frames_a_second": 24,
    "sprite-1": { "y": { "swing": 0.004, "seconds": 8, "phase": 40 }, "x": { "swing": 0.002, "seconds": 8 } },
    "eyelids": { "y": { "pulse": 0.047, "at": [1.4, 4.0, 4.4, 6.6], "lasts": 0.3, "every": 8 } }
  },

  // Plays once when the background appears, and ends on the still.
  "arrive": {
    "frames_a_second": 30,
    "seconds": 5.2,
    "sprite-1": { "size": { "from": 0, "after": 1.75, "over": 0.17, "peak": 0.25 } }
  }
}
```

### Pieces

A piece is a shape cut from the still that can move.

- `picture` is the piece's shape. A piece takes its colour from the still, so the picture only has to say where the piece is solid: a cut-out with transparency, or a grey picture that is white where it is solid.
- `behind` is what the still would show where the piece was: a patch, drawn under the piece once it moves away.
- `"colour": "own"` makes the picture the piece's colour too, for something the still does not show whole.
- `"on": "<piece>"` says the piece sits on another one, so it needs no patch while that one is drawn.
- `"inside": "<area>"` shows the piece only inside an area's shape.
- `"rest": { "x": …, "y": … }` is where the piece sits at rest, away from where it was cut.

No two pieces may share a pixel of the still. A part lists the pieces it moves; a piece a part does not list is not drawn in it, and stays part of the still.

### Motion

A piece has four values a part can move: `x`, `y`, `size` and `fade`. Each takes one of three motions.

| Motion | Keys | What it does |
|---|---|---|
| Swing | `swing`, `seconds`, `phase` | Back and forth through rest, for ever. `phase` is degrees into the cycle at the start. |
| Pulse | `pulse`, `at`, `lasts`, `every` | Away from rest and back, at the given seconds of each cycle. |
| From | `from`, `after`, then `for`, or `over` and `peak` | Starts away from rest and comes to it once. `over` lets it swing past rest by that share, furthest `peak` seconds after it sets off. |

Every motion passes through rest, which is what lets a scene stop and start anywhere.

### Effects

An effect is a small shader that redraws an area of the still. A part puts an effect to work by listing it under the area:

```jsonc
"idle": {
  "frames_a_second": 12,
  "sky": { "twinkle": { "amount": 0.7, "size": 0.003, "seconds": 7 } },
  "sea": { "drift": { "amount": [0.005, 0.0015], "seconds": 9 } }
}
```

OWE has four:

| Effect | For | Values |
|---|---|---|
| `twinkle` | Stars, city lights, sparkles. Finds the points of light itself. | `amount`, `size` |
| `glow` | Lamps, windows, a sun, embers. | `amount`, `flicker` |
| `drift` | Water, mist, clouds, foliage pushed back and forth. | `amount` (across, down), `size` |
| `flow` | A river, smoke, clouds travelling one way. | `amount` (across, down) |

`seconds` is the length of the effect's cycle, and `strength` takes a `from` motion to bring an effect up during the arrive part.

A scene can bring effects of its own, each a shader file beside it:

```jsonc
"effects": [
  { "name": "waves", "shader": "effects/waves.glsl", "values": { "amount": 0.016, "seconds": 16 } }
]
```

The shader writes one function, `vec4 effect(vec2 uv)`: the colour at a place in the still now. It declares a uniform for each value the scene sets, and is given:

| Name | What it is |
|---|---|
| `vec4 still(vec2 uv)` | The still's colour at a place; `uv` runs 0 to 1 from its top left. |
| `float mask(vec2 uv)` | The area's shape at a place, 0 outside it. |
| `float phase` | 0 to 1 over the effect's `seconds`, then again. |
| `float seconds` | The length of that cycle. |
| `vec2 pixel`, `float aspect` | One pixel of the still in `uv`, and its width over its height. |
| `vec4 area` | The area's left, top, width and height. |
| `float grain(vec2 p)`, `float cloud(vec2 p)` | A fixed random number for each `p`, and the same made smooth. |

The player lays the effect's answer over what is already drawn, by the area's shape, so an effect cannot reach outside its area and effects on overlapping areas each keep their own work. It fades an effect in and out, so it starts and stops at the still. The source must be valid as both GLSL 1.50 and GLSL ES 3.00. A shader that does not compile, or a value its shader does not take, refuses the scene and says why.

## What the player holds a scene to

- A scene stops when OWE says. Paused, it settles into its still over 0.4 seconds and stops drawing; resumed, its idle part starts again from rest. The daemon waits for it to settle before the shell takes the still back.
- The last 0.4 seconds of the arrive part settle into the still, so the hand-over to the idle part is the same picture.
- A scene draws at its own `frames_a_second`, from 1 to 60, not at the display's rate.
- A scene reads only files under its own directory.
- A scene that is broken, or made for another picture, is refused with the reason, and the still, its loop or its intro clip is used as before.

## Looking at a scene without playing it

```sh
# What a scene holds, as JSON, or why it will not play
owe scene info scene.jsonc

# One moment of it as a picture, with no window: 3 seconds into the idle part
owe scene frame scene.jsonc --at 3 --out frame.png

# Every frame of the arrive part
owe scene frame scene.jsonc --part arrive --frames 156 --fps 30 --out frames/
```

`owe scene frame` uses the code that draws a playing scene, so what it writes is what plays. It is how a scene is checked against a reference, and how whoever makes one sees it.

## Not built yet

- A guard that gives way to the still when a scene is too heavy for the machine. A scene's own shader runs unchecked today.
- Drawing only the part of the screen that changed.
- Scenes on the lock screen have not been tried.

#!/usr/bin/env python3
"""Draw moments of a small scene to files with `owe-render --scene-frame` and check the pixels."""
import json
import struct
import subprocess
import sys
import tempfile
import zlib
from hashlib import sha256
from pathlib import Path

render = sys.argv[1]
SIDE, CUT = 64, 16
GROUND, RED = (40, 60, 80), (200, 10, 10)


def check(condition, message, detail=""):
    if not condition:
        print(f"FAIL: {message} {detail}", file=sys.stderr)
        sys.exit(1)


def png(path, width, height, pixel):
    """Write a PNG whose pixel at (x, y) is pixel(x, y): three values, or four with how solid it is."""
    first = pixel(0, 0)
    rows = b"".join(b"\0" + bytes(v for x in range(width) for v in pixel(x, y)) for y in range(height))
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    head = struct.pack(">IIBBBBB", width, height, 8, 6 if len(first) == 4 else 2, 0, 0, 0)
    Path(path).write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", head) + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def ppm(path):
    """The pixels of a PPM file, as a function of (x, y)."""
    data = Path(path).read_bytes()
    magic, size, depth, body = data.split(b"\n", 3)
    width, height = map(int, size.split())
    check(magic == b"P6" and depth == b"255" and len(body) == width * height * 3, "the frame is a whole PPM", path)
    return lambda x, y: tuple(body[(y * width + x) * 3:(y * width + x) * 3 + 3])


def near(a, b):
    return all(abs(p - q) <= 1 for p, q in zip(a, b))


def draw(*args):
    return subprocess.run([render, "--scene-frame", *map(str, args)], capture_output=True, text=True)


with tempfile.TemporaryDirectory(prefix="owe-scene-frame-") as tmp:
    root = Path(tmp)
    (root / "pieces").mkdir()
    in_square = lambda x, y: 20 <= x < 28 and 30 <= y < 38
    png(root / "still.png", SIDE, SIDE, lambda x, y: RED if in_square(x, y) else GROUND)
    # The square as a piece: its shape in a cut 16 pixels wide at (16, 26), and the ground behind it.
    solid = lambda x, y: 255 if in_square(x + 16, y + 26) else 0
    png(root / "pieces/square.png", CUT, CUT, lambda x, y: (255, 255, 255, solid(x, y)))
    png(root / "pieces/square-behind.png", CUT, CUT, lambda x, y: (*GROUND, solid(x, y)))
    scene = {
        "still": "test/still.png",
        "sha256": sha256((root / "still.png").read_bytes()).hexdigest(),
        "pieces": [{"name": "square", "picture": "pieces/square.png", "behind": "pieces/square-behind.png",
                    "at": [16 / SIDE, 26 / SIDE, CUT / SIDE, CUT / SIDE]}],
        # Four pixels to the right a quarter of the way through its swing.
        "idle": {"frames_a_second": 4, "square": {"x": {"swing": 4 / SIDE, "seconds": 4}}},
        "arrive": {"seconds": 2, "square": {"size": {"from": 0, "after": 0.5, "for": 0.5}}},
    }
    (root / "scene.jsonc").write_text("// a test scene\n" + json.dumps(scene))
    common = [root / "scene.jsonc", "--still", root / "still.png", "--size", f"{SIDE}x{SIDE}"]

    result = draw(*common, "--out", root / "rest.ppm")
    if "no display to draw without a window" in result.stderr:
        sys.exit(77)
    check(result.returncode == 0, "a frame of the scene is drawn", result.stderr)
    seen = ppm(root / "rest.ppm")
    still = lambda x, y: RED if in_square(x, y) else GROUND
    check(all(near(seen(x, y), still(x, y)) for y in range(SIDE) for x in range(SIDE)),
          "at rest the scene is its still, to the pixel")

    check(draw(*common, "--at", 1, "--out", root / "moved.ppm").returncode == 0, "a later moment is drawn")
    seen = ppm(root / "moved.ppm")
    check(near(seen(21, 33), GROUND) and near(seen(25, 33), RED) and near(seen(30, 33), RED) and near(seen(32, 33), GROUND),
          "a quarter through its swing the piece is four pixels over", str([seen(x, 33) for x in (21, 25, 30, 32)]))

    check(draw(*common, "--at", 0, "--frames", 3, "--ppm", "--out", root / "run").returncode == 0, "three frames are drawn")
    check(sorted(p.name for p in (root / "run").iterdir()) == ["0000.ppm", "0001.ppm", "0002.ppm"], "frames are numbered files")
    # At four frames a second the second frame is a quarter of a second in, where the swing is about a pixel and a half out.
    check(near(ppm(root / "run/0000.ppm")(27, 33), RED) and near(ppm(root / "run/0002.ppm")(29, 33), RED),
          "each frame is a step later than the last")

    check(draw(*common, "--part", "arrive", "--at", 0.25, "--out", root / "before.ppm").returncode == 0, "the arrive part is drawn")
    check(near(ppm(root / "before.ppm")(24, 34), GROUND), "before it sets off, a piece that grows from nothing is not there")
    check(draw(*common, "--part", "arrive", "--at", 1.2, "--out", root / "grown.ppm").returncode == 0, "the arrive part is drawn later")
    check(near(ppm(root / "grown.ppm")(24, 34), RED), "once it has grown, it is")
    # A paused scene settles into its still; fully settled it is the still even mid-swing.
    check(draw(*common, "--at", 1, "--settling", 1, "--out", root / "settled.ppm").returncode == 0, "a settled moment is drawn")
    check(near(ppm(root / "settled.ppm")(21, 33), RED), "a settled scene is its still")

    check(draw(*common, "--out", root / "frame.png").returncode == 0 and (root / "frame.png").read_bytes()[:4] == b"\x89PNG",
          "a frame is written as a PNG too")

    # Effects redraw an area of the still. One of the scene's own is a small shader beside it.
    (root / "fx").mkdir()
    (root / "fx/paint.glsl").write_text("uniform vec3 colour;\nvec4 effect(vec2 uv) { return vec4(colour, 1.0); }\n")
    # The area is the top left quarter; its shape is its left half, white on black.
    png(root / "pieces/corner.png", 8, 8, lambda x, y: (255, 255, 255) if x < 4 else (0, 0, 0))
    painted = dict(scene, areas=[{"name": "corner", "mask": "pieces/corner.png", "at": [0, 0, 0.25, 0.25]}],
                   effects=[{"name": "paint", "shader": "fx/paint.glsl", "values": {"colour": [0, 0, 1]}}],
                   idle={"frames_a_second": 4, "corner": {"paint": {}}},
                   arrive={"seconds": 2, "corner": {"paint": {"colour": [0, 1, 0], "strength": {"from": 0, "after": 1, "for": 0.5}}}})
    (root / "painted.jsonc").write_text(json.dumps(painted))
    effect = [root / "painted.jsonc", "--still", root / "still.png", "--size", f"{SIDE}x{SIDE}"]
    result = draw(*effect, "--out", root / "fx-rest.ppm")
    check(result.returncode == 0, "a scene with an effect of its own is drawn", result.stderr)
    seen = ppm(root / "fx-rest.ppm")
    check(all(near(seen(x, y), still(x, y)) for y in range(SIDE) for x in range(SIDE)),
          "an effect starts from the still: at rest the scene is its still, to the pixel")
    check(draw(*effect, "--at", 1, "--out", root / "fx-on.ppm").returncode == 0, "an effect at work is drawn")
    seen = ppm(root / "fx-on.ppm")
    check(near(seen(3, 5), (0, 0, 255)) and near(seen(12, 5), GROUND) and near(seen(3, 20), GROUND) and near(seen(40, 40), GROUND),
          "an effect redraws its area where its shape is white, and nothing else",
          str([seen(3, 5), seen(12, 5), seen(3, 20), seen(40, 40)]))
    # It is up half a second after it sets off at 1, just before the part's last moments settle into the still.
    check(draw(*effect, "--part", "arrive", "--at", 0.5, "--out", root / "fx-before.ppm").returncode == 0 and
          draw(*effect, "--part", "arrive", "--at", 1.55, "--out", root / "fx-after.ppm").returncode == 0, "an effect in the arrive part is drawn")
    check(near(ppm(root / "fx-before.ppm")(3, 5), GROUND) and near(ppm(root / "fx-after.ppm")(3, 5), (0, 255, 0)),
          "a part gives an effect its own values, and brings it up when it says", str(ppm(root / "fx-after.ppm")(3, 5)))
    # One of OWE's: a glow a quarter through its cycle is the still, brighter by its amount.
    glowing = dict(painted, effects=[], idle={"frames_a_second": 4, "corner": {"glow": {"amount": 0.5, "seconds": 4}}}, arrive={"seconds": 0})
    del glowing["arrive"]
    (root / "glowing.jsonc").write_text(json.dumps(glowing))
    result = draw(root / "glowing.jsonc", "--still", root / "still.png", "--size", f"{SIDE}x{SIDE}", "--at", 1, "--out", root / "glow.ppm")
    check(result.returncode == 0 and near(ppm(root / "glow.ppm")(3, 5), tuple(round(v * 1.5) for v in GROUND)),
          "one of OWE's effects works by its name", result.stderr + str(ppm(root / "glow.ppm")(3, 5)))
    # A shader that will not compile is refused, with the line that is wrong; so is a value its shader does not take.
    (root / "fx/paint.glsl").write_text("uniform vec3 colour;\nvec4 effect(vec2 uv) {\n  return colour;\n}\n")
    result = draw(*effect, "--out", root / "no.ppm")
    check(result.returncode != 0 and "paint" in result.stderr and not (root / "no.ppm").exists(),
          "a scene whose shader does not compile draws nothing, and says which", result.stderr)
    (root / "fx/paint.glsl").write_text("uniform vec3 colour;\nvec4 effect(vec2 uv) { return vec4(colour, 1.0); }\n")
    painted["idle"]["corner"]["paint"] = {"color": [1, 0, 0]}
    (root / "painted.jsonc").write_text(json.dumps(painted))
    result = draw(*effect, "--out", root / "no.ppm")
    check(result.returncode != 0 and "color" in result.stderr, "a value its shader does not take is named", result.stderr)

    # What a scene holds, for whoever has to decide what to do with it.
    def info(*args):
        result = subprocess.run([render, "--scene-info", *map(str, args)], capture_output=True, text=True)
        return result.returncode, json.loads(result.stdout or "{}")
    code, told = info(root / "scene.jsonc", "--still", root / "still.png")
    check(code == 0 and told.get("status") == "ok" and told.get("still_matches") is True and
          told.get("arrive", {}).get("seconds") == 2 and told.get("idle", {}).get("frames_a_second") == 4,
          "a scene's parts and its still are reported", json.dumps(told))
    png(root / "other.png", SIDE, SIDE, lambda x, y: GROUND)
    code, told = info(root / "scene.jsonc", "--still", root / "other.png")
    check(code == 0 and told.get("still_matches") is False, "a still the scene was not made for is reported", json.dumps(told))
    result = draw(root / "scene.jsonc", "--still", root / "other.png", "--size", f"{SIDE}x{SIDE}", "--out", root / "no.ppm")
    check(result.returncode != 0 and "not the picture the scene was made for" in result.stderr and not (root / "no.ppm").exists(),
          "a scene refuses a still it was not made for", result.stderr)
    (root / "pieces/square.png").unlink()
    result = draw(*common, "--out", root / "no.ppm")
    check(result.returncode != 0 and not (root / "no.ppm").exists(), "a scene with a picture missing draws nothing", result.stderr)
    code, told = info(root / "scene.jsonc")
    check(code == 1 and told.get("status") == "error" and "square" in told.get("message", ""),
          "a scene with a picture missing is reported as one that will not play", json.dumps(told))
    check(draw(root / "scene.jsonc").returncode == 2, "a call with nowhere to write is refused")

print("scene frames passed")

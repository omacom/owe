// Waves pass through the picture from left to right, and its bands rise and
// fall on them. One wave and a smaller one twice as fast move it up and down;
// a third moves it a little from side to side. The picture leans the waves
// more the further down it is, so the bands do not move as one.
//
// Everything is measured against where the waves stand at the start of the
// cycle, so at the start, and each time it comes round, the picture is the
// still exactly.

uniform float amount; // how far a band rises and falls, as a share of the still's height

const float TURN = 6.2831853;

float rise(float x, float lean) {
  return sin(TURN * x + lean) + 0.45 * sin(TURN * 2.0 * x + 1.7 + lean * 1.6);
}

float sway(float x, float lean) {
  return sin(TURN * x + lean + 0.9);
}

vec4 effect(vec2 uv) {
  float lean = 1.1 * uv.y;
  float down = rise(uv.x - phase, lean) - rise(uv.x, lean);
  float across = sway(uv.x - phase, lean) - sway(uv.x, lean);
  return still(uv + vec2(across * 0.5 / aspect, down) * amount);
}

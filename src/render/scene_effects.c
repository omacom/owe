#include "scene_effects.h"

#include <string.h>

/* What every effect is given, by owe_egl_effect_new:
 *
 *   vec4 still(vec2 uv)   the still's colour at a place; uv runs 0 to 1 from
 *                         its top left corner
 *   float mask(vec2 uv)   how strongly the effect works at a place: the
 *                         area's shape, 0 outside it
 *   float phase           0 to 1 over the effect's "seconds", then again
 *   float seconds         the length of that cycle
 *   vec2 pixel            the size of one pixel of the still, in uv
 *   float aspect          the still's width over its height
 *   vec4 area             the area's left, top, width and height, in uv
 *   float grain(vec2 p)   a fixed random number from 0 to 1 for each p
 *   float cloud(vec2 p)   the same, smooth between whole numbers
 *
 * It writes `vec4 effect(vec2 uv)`, the colour at uv now, and declares a
 * uniform for each value a scene can set. The player mixes its answer into
 * the still by the area's shape, so an effect cannot reach outside its area,
 * and fades it in and out, so it starts and stops at the still. Everything in
 * it should come round with `phase`, so the cycle has no seam. */

/* Points of light brighten and dim, each in its own time. For stars, city
 * lights, sparkles on water. It finds the points itself, as what is brighter
 * than what is around it, so the area can be the whole sky: the sky between
 * the stars is left alone. A point fades towards what surrounds it, never
 * towards black. */
static const char twinkle[] =
    "uniform float amount; // how much brighter and dimmer than at rest, as a share\n"
    "uniform float size;   // how wide a point of light is at most, as a share of the still's height\n"
    "vec4 effect(vec2 uv) {\n"
    "  vec3 c = still(uv).rgb;\n"
    "  vec2 r = vec2(size / aspect, size);\n"
    "  vec3 around = (still(uv + vec2(r.x, 0.0)).rgb + still(uv - vec2(r.x, 0.0)).rgb +\n"
    "                 still(uv + vec2(0.0, r.y)).rgb + still(uv - vec2(0.0, r.y)).rgb) * 0.25;\n"
    "  float point = smoothstep(0.04, 0.2, dot(c - around, vec3(0.299, 0.587, 0.114)));\n"
    "  // Neighbouring points keep their own time: the patches are a few points wide.\n"
    "  vec2 p = vec2(uv.x * aspect, uv.y) / (size * 4.0);\n"
    "  float wave = 0.0;\n"
    "  for (int k = 1; k <= 2; k++) {\n"
    "    float turns = float(k);\n"
    "    float own = cloud(p * 0.5 + 31.0 * turns);\n"
    "    wave += (cloud(p + 17.0 * turns) - 0.5) * 2.0 * sin(6.2831853 * (phase * turns + own * 3.0));\n"
    "  }\n"
    "  return vec4(c + (c - around) * amount * wave * point, 1.0);\n"
    "}\n";

/* The light of the area breathes. For lamps, windows, a sun, embers, neon.
 * With flicker it is restless, as a flame is. */
static const char glow[] =
    "uniform float amount;  // how much brighter and dimmer, as a share\n"
    "uniform float flicker; // 0 for slow breathing, 1 for a restless flame\n"
    "vec4 effect(vec2 uv) {\n"
    "  vec4 c = still(uv);\n"
    "  float turn = 6.2831853 * phase;\n"
    "  float calm = sin(turn);\n"
    "  float restless = sin(turn * 3.0) * 0.5 + sin(turn * 7.0 + 2.0 * sin(turn)) * 0.3 + sin(turn * 13.0) * 0.2;\n"
    "  return vec4(c.rgb * (1.0 + amount * mix(calm, restless, flicker)), 1.0);\n"
    "}\n";

/* The picture is pushed gently back and forth by slow swells. For water, mist,
 * clouds, foliage. A grey shape that is darker towards what holds still (a
 * shore, a trunk) moves less there. */
static const char drift[] =
    "uniform vec2 amount; // how far across and down, as shares of the still's height\n"
    "uniform float size;  // how wide one swell is, as a share of the still's height\n"
    "vec4 effect(vec2 uv) {\n"
    "  vec2 p = vec2(uv.x * aspect, uv.y) / size;\n"
    "  float turn = 6.2831853 * phase;\n"
    "  vec2 round = vec2(cos(turn), sin(turn)) * 0.5;\n"
    "  vec2 push = vec2(cloud(p + round) - cloud(p + round.yx + 7.3), cloud(p - round + 3.1) - cloud(p - round.yx + 11.7));\n"
    "  return still(uv + push * amount * vec2(1.0 / aspect, 1.0) * mask(uv));\n"
    "}\n";

/* The picture travels one way without ever getting anywhere. For a river, a
 * waterfall, smoke, clouds crossing the sky. Two copies take turns, each
 * hidden while it jumps back. */
static const char flow[] =
    "uniform vec2 amount; // how far it travels across and down in one cycle, as shares of the still's height\n"
    "vec4 effect(vec2 uv) {\n"
    "  vec2 way = amount * vec2(1.0 / aspect, 1.0) * mask(uv);\n"
    "  float one = fract(phase + 0.5);\n"
    "  float two = fract(phase);\n"
    "  vec4 first = still(uv - way * (one - 0.5));\n"
    "  vec4 second = still(uv - way * (two - 0.5));\n"
    "  return mix(first, second, abs(1.0 - 2.0 * one));\n"
    "}\n";

static const struct owe_scene_builtin builtins[] = {
    { "twinkle", twinkle, 6.0f, { { "amount", { 0.6f }, 1 }, { "size", { 0.004f }, 1 } }, 2 },
    { "glow", glow, 5.0f, { { "amount", { 0.12f }, 1 }, { "flicker", { 0.0f }, 1 } }, 2 },
    { "drift", drift, 12.0f, { { "amount", { 0.004f, 0.002f }, 2 }, { "size", { 0.08f }, 1 } }, 2 },
    { "flow", flow, 10.0f, { { "amount", { 0.02f, 0.0f }, 2 } }, 1 },
};

const struct owe_scene_builtin *owe_scene_builtin_at(int i) {
    return i >= 0 && i < (int)(sizeof(builtins) / sizeof(*builtins)) ? &builtins[i] : NULL;
}

const struct owe_scene_builtin *owe_scene_builtin(const char *name) {
    const struct owe_scene_builtin *b;
    int i;
    for (i = 0; name && (b = owe_scene_builtin_at(i)); i++) {
        if (strcmp(b->name, name) == 0) return b;
    }
    return NULL;
}

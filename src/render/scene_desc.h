#pragma once

#include <stdbool.h>
#include <stddef.h>

/* What a scene file says, read and checked, with nothing loaded or drawn.
 *
 * A scene belongs to one still. With every piece at home it is that still.
 * Pieces are cut from the still and move over it. An area is a shape in the
 * still: a piece can be kept inside it, and an effect can work on it. A scene
 * has an idle part that repeats and may have an arrive part that plays once
 * and ends at rest. */

#define OWE_SCENE_MAX_PIECES 32
#define OWE_SCENE_MAX_AREAS 8
#define OWE_SCENE_MAX_TIMES 8
#define OWE_SCENE_MAX_EFFECTS 8
#define OWE_SCENE_MAX_USES 8
#define OWE_SCENE_MAX_VALUES 8
#define OWE_SCENE_VALUE_NAME 32
#define OWE_SCENE_NAME 64
#define OWE_SCENE_PATH 4096

enum owe_scene_shape {
    OWE_SCENE_STAYS = 0,
    /* Back and forth through rest, for ever. */
    OWE_SCENE_SWING,
    /* Away from rest and back, at given moments of a cycle. */
    OWE_SCENE_PULSE,
    /* Starts away from rest and comes to it once. */
    OWE_SCENE_FROM,
};

/* How one value of a piece changes over time. */
struct owe_scene_motion {
    enum owe_scene_shape shape;
    float amount;  /* swing, pulse: how far */
    float seconds; /* swing: one full cycle */
    float phase;   /* swing: degrees into the cycle at the start */
    float at[OWE_SCENE_MAX_TIMES]; /* pulse: when, in seconds of each cycle */
    int count;
    float lasts;   /* pulse: seconds each one takes */
    float every;   /* pulse: seconds in a cycle */
    float from;    /* from: where the value starts */
    float after;   /* from: seconds before it sets off */
    float over;    /* from: how far past rest it swings, as a share; 0 for none */
    float peak;    /* from: seconds from setting off to its furthest past rest */
    float takes;   /* from, without a swing past rest: seconds it takes */
};

/* What a piece does in one part of the scene. A piece a part does not list is
 * not drawn in it: it stays part of the still. */
struct owe_scene_part {
    bool listed;
    struct owe_scene_motion x;
    struct owe_scene_motion y;
    struct owe_scene_motion size;
    struct owe_scene_motion fade;
};

struct owe_scene_area {
    char name[OWE_SCENE_NAME];
    char mask[OWE_SCENE_PATH]; /* a picture of its shape; empty for the whole rectangle */
    float at[4]; /* left, top, width, height, as shares of the still */
};

struct owe_scene_piece {
    char name[OWE_SCENE_NAME];
    char picture[OWE_SCENE_PATH];
    char behind[OWE_SCENE_PATH]; /* what the still shows where the piece was; may be empty */
    float at[4];
    float rest_x; /* where it sits at rest, away from where it was cut, as shares of the still's height */
    float rest_y;
    int inside; /* an area it is kept inside, or -1 */
    int on;     /* a piece it sits on, or -1 */
    bool own_colour; /* its picture is its colour; otherwise its colour is the still's own */
    struct owe_scene_part idle;
    struct owe_scene_part arrive;
};

/* A value an effect's shader takes: one number, or up to four. */
struct owe_scene_value {
    char name[OWE_SCENE_VALUE_NAME];
    float v[4];
    int count;
};

/* An effect the scene uses: one of OWE's, or a shader of the scene's own. */
struct owe_scene_effect {
    char name[OWE_SCENE_NAME];
    char shader[OWE_SCENE_PATH]; /* the scene's own file; empty for one of OWE's */
    float seconds;               /* one cycle, unless a part says otherwise */
    struct owe_scene_value values[OWE_SCENE_MAX_VALUES]; /* unless a part says otherwise */
    int value_count;
};

/* An effect at work on an area in one part of the scene. */
struct owe_scene_use {
    int area;
    int effect;
    float seconds;
    struct owe_scene_motion strength; /* 1 at rest: the effect in full */
    struct owe_scene_value values[OWE_SCENE_MAX_VALUES];
    int value_count;
};

struct owe_scene_desc {
    char still[OWE_SCENE_PATH]; /* as the file names it */
    char sha256[65];            /* of the still, lower-case hex; may be empty */
    struct owe_scene_area areas[OWE_SCENE_MAX_AREAS];
    int area_count;
    struct owe_scene_piece pieces[OWE_SCENE_MAX_PIECES];
    int piece_count;
    struct owe_scene_effect effects[OWE_SCENE_MAX_EFFECTS];
    int effect_count;
    struct owe_scene_use idle_uses[OWE_SCENE_MAX_USES];
    int idle_use_count;
    struct owe_scene_use arrive_uses[OWE_SCENE_MAX_USES];
    int arrive_use_count;
    int idle_fps;
    int arrive_fps;
    double arrive_seconds; /* 0 when the scene has no arrive part */
};

/* Read and check the scene file at `path`. Picture paths come back absolute.
 * Returns 0, or -1 with the reason in `error`. */
int owe_scene_desc_read(const char *path, struct owe_scene_desc *desc, char *error, size_t error_len);

/* A distance from home at `t` seconds into its part: 0 at rest. */
float owe_scene_offset(const struct owe_scene_motion *m, double t);

/* A size or an opacity at `t` seconds into its part: 1 at rest. */
float owe_scene_level(const struct owe_scene_motion *m, double t);

#pragma once

#include <stdbool.h>
#include <stddef.h>

/* What a scene file says, read and checked, with nothing loaded or drawn.
 *
 * A scene belongs to one still. With every piece at home it is that still.
 * Pieces are cut from the still and move over it; an area is a shape in the
 * still a piece can be kept inside. A scene has an idle part that repeats and
 * may have an arrive part that plays once and ends at rest. */

#define OWE_SCENE_MAX_PIECES 32
#define OWE_SCENE_MAX_AREAS 8
#define OWE_SCENE_MAX_TIMES 8
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
    char mask[OWE_SCENE_PATH];
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

struct owe_scene_desc {
    char still[OWE_SCENE_PATH]; /* as the file names it */
    char sha256[65];            /* of the still, lower-case hex; may be empty */
    struct owe_scene_area areas[OWE_SCENE_MAX_AREAS];
    int area_count;
    struct owe_scene_piece pieces[OWE_SCENE_MAX_PIECES];
    int piece_count;
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

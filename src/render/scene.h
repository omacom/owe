#pragma once

#include <stdbool.h>
#include <stddef.h>

/* A scene playing: the still, with the pieces a scene file moves drawn over
 * it at the scene's own frame rate. The still is drawn once a frame and each
 * piece only where it can reach; no video is decoded.
 *
 * A scene that is asked to arrive plays its arrive part once, then idles. One
 * that is paused settles to rest, which is the still, and stops drawing;
 * resumed, it idles again from rest. So it can stop and start at any moment
 * without a jump.
 *
 * A scene can also stand in for an intro video: played once, it is its arrive
 * part alone, held where it is on a pause, and ended on the still. */
struct owe_scene;
struct owe_wayland;
struct owe_egl;
struct owe_output;

struct owe_scene *owe_scene_new(struct owe_wayland *wl, struct owe_egl *egl);
void owe_scene_free(struct owe_scene *s);

/* Start loading the scene file at `path`. `still` is the picture it belongs to,
 * or NULL to find it from the scene's own name for it. With `arrive` the
 * scene's arrive part plays first, then it idles. With `once` it is an intro:
 * its arrive part plays and it ends on its last frame, which is the still.
 * Returns 0, or -1 with the reason in `error`. Pictures load in the
 * background; owe_scene_ready tells when. */
int owe_scene_load(struct owe_scene *s, const char *path, const char *still, bool arrive, bool once, int max_w,
                   int max_h, char *error, size_t error_len);
void owe_scene_unload(struct owe_scene *s);

/* A scene is loaded or loading. */
bool owe_scene_loaded(struct owe_scene *s);
/* Every picture is loaded: it can be drawn. */
bool owe_scene_ready(struct owe_scene *s);
/* A frame of it has been drawn. */
bool owe_scene_shown(struct owe_scene *s);
/* Why it cannot play, or "". */
const char *owe_scene_error(struct owe_scene *s);

/* The timer that paces it, to wait on, or -1 while nothing moves. */
int owe_scene_fd(struct owe_scene *s);
/* Call each turn of the event loop. Returns true when a frame is due. */
bool owe_scene_tick(struct owe_scene *s);
/* Frames follow one another: the compositor's callback should pace them. */
bool owe_scene_needs_frames(struct owe_scene *s);

void owe_scene_set_paused(struct owe_scene *s, bool paused);
/* An intro has played to its end. */
bool owe_scene_ended(struct owe_scene *s);
/* An intro starts over from its first moment, as it is revealed. */
void owe_scene_rewind(struct owe_scene *s);
/* At rest and not drawing: what is on screen is the still. */
bool owe_scene_resting(struct owe_scene *s);
/* Seconds into the part that is playing. */
double owe_scene_time(struct owe_scene *s);

void owe_scene_render_output(struct owe_scene *s, struct owe_output *out);

/* The loaded pictures of a scene, as textures. */
struct owe_scene_desc;
struct owe_scene_look {
    unsigned int still;
    int still_w;
    int still_h;
    unsigned int pictures[32];
    unsigned int behinds[32];
    unsigned int masks[8];
    bool mask_grey[8];           /* a shape with no transparency is its brightness */
    struct owe_effect *effects[8]; /* compiled, in the order the scene lists them */
};

/* Draw one moment of a scene: `t` seconds into its arrive part or its idle
 * part. `settling` from 0 to 1 fades it into the still, as a paused scene
 * does. The same code draws a playing scene and a frame written to a file. */
void owe_scene_draw(struct owe_egl *egl, struct owe_output *out, const struct owe_scene_desc *desc,
                    const struct owe_scene_look *look, bool arriving, double t, double settling);

/* Compile the effects a scene uses and check the values it gives them. The
 * context must be current. 0, or -1 with the reason in `error`. */
struct owe_effect;
int owe_scene_build_effects(struct owe_egl *egl, const struct owe_scene_desc *desc, struct owe_effect **effects,
                            char *error, size_t len);
void owe_scene_free_effects(struct owe_effect **effects);

/* Where the still a scene names is, for a scene file at `scene_path`. */
int owe_scene_find_still(const struct owe_scene_desc *desc, const char *scene_path, char *out, size_t len);
/* 0 if `path` is the picture the scene was made for, by its checksum. */
int owe_scene_check_still(const struct owe_scene_desc *desc, const char *path, char *error, size_t len);

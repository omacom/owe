#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct owe_egl;
struct owe_wayland;
struct owe_output;

struct owe_egl *owe_egl_new(struct owe_wayland *wl);
/* A context with no window, and a buffer of the given size to draw into, for
 * drawing to a file. The buffer comes back as an output's egl_surface. */
struct owe_egl *owe_egl_new_offscreen(int width, int height, void **surface);
void owe_egl_free(struct owe_egl *egl);
int owe_egl_make_current(struct owe_egl *egl);

void *owe_egl_display(struct owe_egl *egl);
void *owe_egl_context(struct owe_egl *egl);
void *owe_egl_config(struct owe_egl *egl);

void *owe_egl_create_window_surface(struct owe_egl *egl, void *egl_window);
void owe_egl_destroy_output(struct owe_egl *egl, struct owe_output *out);

int owe_egl_prepare_output(struct owe_egl *egl, struct owe_output *out);
int owe_egl_swap_output(struct owe_egl *egl, struct owe_output *out);
void owe_egl_clear_output(struct owe_egl *egl, struct owe_output *out, float r, float g, float b,
                          float a);

unsigned int owe_egl_tex_from_rgba(struct owe_egl *egl, const uint8_t *rgba, int w, int h);
void owe_egl_tex_free(struct owe_egl *egl, unsigned int tex);
void owe_egl_draw_texture(struct owe_egl *egl, struct owe_output *out, unsigned int tex,
                          float alpha, int tex_w, int tex_h);
/* One piece of a scene, drawn over what is already there. Places are shares
 * of the still: left, top, width and height. */
struct owe_piece_draw {
    unsigned int shape;      /* the piece's picture: where it is, and its colour if it has its own */
    unsigned int still;      /* the still, for a piece that takes its colour from it; else 0 */
    unsigned int behind;     /* what the still shows where that piece was; goes with `still` */
    unsigned int inside;     /* a shape the piece is kept inside; 0 with inside_at for the whole of that place */
    int still_w;
    int still_h;
    const float *at;         /* where the piece was cut from the still */
    const float *inside_at;  /* where the shape sits in the still; NULL when the piece is kept inside nothing */
    bool inside_grey;        /* the shape is its picture's brightness, not its alpha */
    float dx;                /* how far it is moved, as shares of the still's width and height */
    float dy;
    float size;              /* 1 is life size */
    float fade;              /* 1 is fully there */
};
void owe_egl_draw_piece(struct owe_egl *egl, struct owe_output *out, const struct owe_piece_draw *d);

/* An effect: a shader that redraws an area of the still. scene_effects.c sets
 * out what its source is given and what it must write. The context must be
 * current to make or free one. */
struct owe_effect;
struct owe_effect_value {
    const char *name;
    const float *v;
    int count;
};
struct owe_effect_draw {
    unsigned int still;
    unsigned int mask;   /* the area's shape, or 0 for the whole of its place */
    int still_w;
    int still_h;
    const float *at;     /* where the area sits in the still */
    bool mask_grey;      /* the shape is its picture's brightness, not its alpha */
    float strength;      /* 0 leaves the still as it is */
    float phase;         /* 0 to 1 through the effect's cycle */
    float seconds;       /* the length of that cycle */
    const struct owe_effect_value *values;
    int value_count;
};
/* NULL with the compiler's words in `error` when the source will not compile. */
struct owe_effect *owe_egl_effect_new(struct owe_egl *egl, const char *source, char *error, size_t error_len);
void owe_egl_effect_free(struct owe_effect *fx);
/* How many numbers the value `name` of the effect takes, 1 to 4; 0 when its
 * shader has no such value, or never uses it. */
int owe_egl_effect_value(struct owe_effect *fx, const char *name);
void owe_egl_draw_effect(struct owe_egl *egl, struct owe_output *out, struct owe_effect *fx,
                         const struct owe_effect_draw *d);
void owe_egl_draw_texture_overlay(struct owe_egl *egl, struct owe_output *out, unsigned int tex,
                                  float alpha, int tex_w, int tex_h);

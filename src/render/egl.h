#pragma once

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
    unsigned int inside;     /* a shape the piece is kept inside, or 0 */
    int still_w;
    int still_h;
    const float *at;         /* where the piece was cut from the still */
    const float *inside_at;  /* where the shape sits in the still */
    float dx;                /* how far it is moved, as shares of the still's width and height */
    float dy;
    float size;              /* 1 is life size */
    float fade;              /* 1 is fully there */
};
void owe_egl_draw_piece(struct owe_egl *egl, struct owe_output *out, const struct owe_piece_draw *d);
void owe_egl_draw_texture_overlay(struct owe_egl *egl, struct owe_output *out, unsigned int tex,
                                  float alpha, int tex_w, int tex_h);

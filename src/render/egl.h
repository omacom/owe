#pragma once

#include <stdint.h>

struct owe_egl;
struct owe_wayland;
struct owe_output;

struct owe_egl *owe_egl_new(struct owe_wayland *wl);
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
void owe_egl_draw_texture_overlay(struct owe_egl *egl, struct owe_output *out, unsigned int tex,
                                  float alpha, int tex_w, int tex_h);

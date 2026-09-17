#pragma once

#include <stdbool.h>
#include <stdint.h>

struct owe_wayland;
struct owe_egl;
struct owe_output;

typedef struct owe_output {
    struct owe_wayland *owner;
    uint32_t registry_name;
    struct wl_callback *frame_callback;
    struct wl_output *wl_output;
    char name[128];
    int32_t width;
    int32_t height;
    int32_t scale;
    int configured;
    int frame_pending;
    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer;
    struct wl_egl_window *egl_window;
    void *egl_surface;
    int egl_w;
    int egl_h;
    struct owe_output *next;
} owe_output_t;

struct owe_wayland *owe_wayland_new(void);
void owe_wayland_free(struct owe_wayland *wl);
void owe_wayland_destroy_outputs(struct owe_wayland *wl);
void owe_wayland_set_egl(struct owe_wayland *wl, struct owe_egl *egl);
void owe_wayland_attach_egl(struct owe_wayland *wl, struct owe_egl *egl);

struct wl_display *owe_wayland_display(struct owe_wayland *wl);
int owe_wayland_fd(struct owe_wayland *wl);
int owe_wayland_dispatch(struct owe_wayland *wl);
int owe_wayland_dispatch_pending(struct owe_wayland *wl);
void owe_wayland_flush(struct owe_wayland *wl);

void owe_wayland_request_render(void *opaque);
void owe_wayland_render_pending(struct owe_wayland *wl);

owe_output_t *owe_wayland_outputs(struct owe_wayland *wl);
int owe_wayland_output_count(struct owe_wayland *wl);

void owe_wayland_outputs_max_size(struct owe_wayland *wl, int *w, int *h);

#include "wayland.h"

#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>
#include <wayland-egl.h>

#include "egl.h"
#include "log.h"
#include "mpv.h"
#include "render.h"
#include "still.h"
#include "fractional-scale-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

struct owe_wayland {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct zwlr_layer_shell_v1 *layer_shell;
    struct wp_viewporter *viewporter;
    struct wp_fractional_scale_manager_v1 *fractional_scale;
    struct owe_egl *egl;
    owe_output_t *outputs;
    int output_count;
};

static void output_free(struct owe_wayland *wl, owe_output_t *out);

void owe_output_buffer_size(const owe_output_t *out, int *w, int *h) {
    int scale120 = out->scale > 0 ? out->scale * 120 : 120;
    if (out->fractional && out->preferred_scale > 0) {
        scale120 = out->preferred_scale;
    }
    if (w) {
        *w = out->width > 0 ? (int)(((int64_t)out->width * scale120 + 119) / 120) : 0;
    }
    if (h) {
        *h = out->height > 0 ? (int)(((int64_t)out->height * scale120 + 119) / 120) : 0;
    }
}

static void output_update_size(owe_output_t *out) {
    owe_output_buffer_size(out, &out->buffer_w, &out->buffer_h);
    if (out->fractional && out->viewport && out->width > 0 && out->height > 0) {
        wp_viewport_set_destination(out->viewport, out->width, out->height);
    }
}

static void output_geometry(void *data, struct wl_output *wl_output, int32_t x, int32_t y,
                            int32_t physical_width, int32_t physical_height, int32_t subpixel,
                            const char *make, const char *model, int32_t transform) {
    owe_output_t *out = data;
    (void)wl_output;
    (void)x;
    (void)y;
    (void)physical_width;
    (void)physical_height;
    (void)subpixel;
    (void)transform;
    snprintf(out->name, sizeof(out->name), "%s %s", make ? make : "", model ? model : "");
}

static void output_mode(void *data, struct wl_output *wl_output, uint32_t flags, int32_t width,
                        int32_t height, int32_t refresh) {
    owe_output_t *out = data;
    (void)wl_output;
    (void)refresh;
    /* Layer configure events provide logical dimensions. Output modes are physical. */
    (void)out;
    (void)flags;
    (void)width;
    (void)height;
}

static void output_done(void *data, struct wl_output *wl_output) {
    (void)data;
    (void)wl_output;
}

static void output_scale(void *data, struct wl_output *wl_output, int32_t factor) {
    owe_output_t *out = data;
    (void)wl_output;
    if (factor > 0 && factor != out->scale) {
        out->scale = factor;
        OWE_INFO("output %s scale %d", out->name, factor);
        if (out->fractional) {
            return;
        }
        if (out->surface) {
            wl_surface_set_buffer_scale(out->surface, factor);
        }
        output_update_size(out);
        out->frame_pending = 1;
        owe_app_request_render();
    }
}

static void fractional_preferred_scale(void *data, struct wp_fractional_scale_v1 *fractional,
                                       uint32_t scale) {
    owe_output_t *out = data;
    (void)fractional;
    if (scale == 0 || (int32_t)scale == out->preferred_scale) {
        return;
    }
    out->preferred_scale = (int32_t)scale;
    output_update_size(out);
    OWE_INFO("output %s preferred scale %d/120", out->name, out->preferred_scale);
    out->frame_pending = 1;
    owe_app_request_render();
}

static const struct wp_fractional_scale_v1_listener fractional_listener = {
    .preferred_scale = fractional_preferred_scale,
};

static void output_name(void *data, struct wl_output *wl_output, const char *name) {
    owe_output_t *out = data;
    (void)wl_output;
    if (name && *name) {
        snprintf(out->name, sizeof(out->name), "%s", name);
    }
}

static void output_description(void *data, struct wl_output *wl_output, const char *description) {
    (void)data;
    (void)wl_output;
    (void)description;
}

static const struct wl_output_listener output_listener = {
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale,
    .name = output_name,
    .description = output_description,
};

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *surface, uint32_t serial,
                            uint32_t width, uint32_t height) {
    owe_output_t *out = data;
    if (width > 0) {
        out->width = (int32_t)width;
    }
    if (height > 0) {
        out->height = (int32_t)height;
    }
    output_update_size(out);
    OWE_DEBUG("layer configure %s %dx%d buffer %dx%d", out->name, out->width, out->height,
              out->buffer_w, out->buffer_h);
    zwlr_layer_surface_v1_ack_configure(surface, serial);
    if (!out->configured) {
        out->configured = 1;
        out->frame_pending = 1;
    } else {
        out->frame_pending = 1;
        owe_app_request_render();
    }
}

static void layer_closed(void *data, struct zwlr_layer_surface_v1 *surface) {
    owe_output_t *out = data;
    (void)surface;
    OWE_WARN("layer surface closed for output %s", out->name);
    output_free(out->owner, out);
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
    .configure = layer_configure,
    .closed = layer_closed,
};

static void frame_done(void *data, struct wl_callback *cb, uint32_t time);

static const struct wl_callback_listener frame_listener = {
    .done = frame_done,
};

static void frame_done(void *data, struct wl_callback *cb, uint32_t time) {
    owe_output_t *out = data;
    (void)time;
    wl_callback_destroy(cb);
    out->frame_callback = NULL;
    /* Present the fully opaque final frame even when the fade timer has expired. */
    out->frame_pending = 1;
}

static owe_output_t *output_new(struct owe_wayland *wl, struct wl_output *wl_output, uint32_t name) {
    owe_app_t *app = owe_app_get();
    owe_output_t *out = calloc(1, sizeof(*out));
    struct wl_region *region;
    if (!out) {
        return NULL;
    }
    out->wl_output = wl_output;
    out->owner = wl;
    out->registry_name = name;
    out->width = 0;
    out->height = 0;
    out->scale = 1;
    snprintf(out->name, sizeof(out->name), "output-%p", (void *)wl_output);
    wl_output_add_listener(wl_output, &output_listener, out);

    out->surface = wl_compositor_create_surface(wl->compositor);
    if (!out->surface) {
        free(out);
        return NULL;
    }
    wl_surface_set_buffer_scale(out->surface, out->scale > 0 ? out->scale : 1);
    if (wl->viewporter) {
        out->viewport = wp_viewporter_get_viewport(wl->viewporter, out->surface);
    }
    if (wl->fractional_scale) {
        out->fractional =
            wp_fractional_scale_manager_v1_get_fractional_scale(wl->fractional_scale, out->surface);
        if (out->fractional) {
            wp_fractional_scale_v1_add_listener(out->fractional, &fractional_listener, out);
        }
    }
    out->layer = zwlr_layer_shell_v1_get_layer_surface(wl->layer_shell, out->surface, wl_output,
                                                       ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND,
                                                       "owe-background");
    if (!out->layer) {
        wl_surface_destroy(out->surface);
        free(out);
        return NULL;
    }
    zwlr_layer_surface_v1_add_listener(out->layer, &layer_listener, out);
    zwlr_layer_surface_v1_set_size(out->layer, 0, 0);
    zwlr_layer_surface_v1_set_anchor(out->layer, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                                                     ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                                                     ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                                                     ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(out->layer, -1);
    zwlr_layer_surface_v1_set_keyboard_interactivity(out->layer, 0);
    region = wl_compositor_create_region(wl->compositor);
    if (region) {
        wl_surface_set_input_region(out->surface, region);
        wl_region_destroy(region);
    }
    wl_surface_commit(out->surface);

    out->egl_window = wl_egl_window_create(out->surface, 64, 64);
    if (!out->egl_window) {
        zwlr_layer_surface_v1_destroy(out->layer);
        wl_surface_destroy(out->surface);
        free(out);
        return NULL;
    }
    if (app && app->egl) {
        out->egl_surface = owe_egl_create_window_surface(app->egl, out->egl_window);
        if (!out->egl_surface) {
            wl_egl_window_destroy(out->egl_window);
            zwlr_layer_surface_v1_destroy(out->layer);
            wl_surface_destroy(out->surface);
            free(out);
            return NULL;
        }
    }

    out->next = wl->outputs;
    wl->outputs = out;
    wl->output_count++;
    return out;
}

static void output_free(struct owe_wayland *wl, owe_output_t *out) {
    owe_app_t *app = owe_app_get();
    owe_output_t **link;
    if (out->frame_callback) wl_callback_destroy(out->frame_callback);
    if (out->fractional) {
        wp_fractional_scale_v1_destroy(out->fractional);
    }
    if (out->viewport) {
        wp_viewport_destroy(out->viewport);
    }
    if (app && app->egl) {
        owe_egl_destroy_output(app->egl, out);
    }
    if (out->egl_window) {
        wl_egl_window_destroy(out->egl_window);
    }
    if (out->layer) {
        zwlr_layer_surface_v1_destroy(out->layer);
    }
    if (out->surface) {
        wl_surface_destroy(out->surface);
    }
    if (out->wl_output) {
        wl_output_destroy(out->wl_output);
    }
    for (link = &wl->outputs; *link; link = &(*link)->next) {
        if (*link == out) {
            *link = out->next;
            break;
        }
    }
    wl->output_count--;
    free(out);
}

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
                            const char *interface, uint32_t version) {
    struct owe_wayland *wl = data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        wl->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
    } else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        wl->layer_shell = wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, 1);
    } else if (strcmp(interface, wp_viewporter_interface.name) == 0) {
        wl->viewporter = wl_registry_bind(registry, name, &wp_viewporter_interface, 1);
    } else if (strcmp(interface, wp_fractional_scale_manager_v1_interface.name) == 0) {
        wl->fractional_scale = wl_registry_bind(registry, name, &wp_fractional_scale_manager_v1_interface, 1);
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        struct wl_output *wo;
        uint32_t v = version >= 4 ? 4 : version;
        wo = wl_registry_bind(registry, name, &wl_output_interface, v);
        if (wo && wl->compositor && wl->layer_shell) {
            if (!output_new(wl, wo, name)) {
                wl_output_destroy(wo);
            }
        } else if (wo) {
            wl_output_destroy(wo);
        }
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
    struct owe_wayland *wl = data;
    (void)registry;
    for (owe_output_t *out = wl->outputs; out; out = out->next) {
        if (out->registry_name == name) {
            output_free(wl, out);
            break;
        }
    }
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

struct owe_wayland *owe_wayland_new(void) {
    struct owe_wayland *wl = calloc(1, sizeof(*wl));
    if (!wl) {
        return NULL;
    }
    wl->display = wl_display_connect(NULL);
    if (!wl->display) {
        OWE_ERROR("wl_display_connect failed");
        free(wl);
        return NULL;
    }
    wl->registry = wl_display_get_registry(wl->display);
    wl_registry_add_listener(wl->registry, &registry_listener, wl);
    wl_display_roundtrip(wl->display);
    if (!wl->compositor || !wl->layer_shell) {
        OWE_ERROR("compositor lacks wl_compositor or wlr-layer-shell");
        wl_registry_destroy(wl->registry);
        wl_display_disconnect(wl->display);
        free(wl);
        return NULL;
    }
    wl_display_roundtrip(wl->display);
    OWE_INFO("wayland ready, outputs=%d", wl->output_count);
    return wl;
}

void owe_wayland_destroy_outputs(struct owe_wayland *wl) {
    owe_output_t *out;
    owe_output_t *next;
    if (!wl) {
        return;
    }
    for (out = wl->outputs; out; out = next) {
        next = out->next;
        output_free(wl, out);
    }
}

void owe_wayland_free(struct owe_wayland *wl) {
    if (!wl) return;
    owe_wayland_destroy_outputs(wl);
    if (wl->fractional_scale) {
        wp_fractional_scale_manager_v1_destroy(wl->fractional_scale);
    }
    if (wl->viewporter) {
        wp_viewporter_destroy(wl->viewporter);
    }
    if (wl->layer_shell) {
        zwlr_layer_shell_v1_destroy(wl->layer_shell);
    }
    if (wl->compositor) {
        wl_compositor_destroy(wl->compositor);
    }
    if (wl->registry) {
        wl_registry_destroy(wl->registry);
    }
    if (wl->display) {
        wl_display_disconnect(wl->display);
    }
    free(wl);
}

void owe_wayland_set_egl(struct owe_wayland *wl, struct owe_egl *egl) {
    if (wl) {
        wl->egl = egl;
    }
}

void owe_wayland_attach_egl(struct owe_wayland *wl, struct owe_egl *egl) {
    owe_output_t *out;
    if (!wl || !egl) {
        return;
    }
    for (out = wl->outputs; out; out = out->next) {
        if (!out->egl_surface && out->egl_window) {
            out->egl_surface = owe_egl_create_window_surface(egl, out->egl_window);
            if (out->egl_surface) {
                OWE_INFO("egl surface attached for output %s (%dx%d@%d)", out->name, out->width,
                         out->height, out->scale);
                out->frame_pending = 1;
            } else {
                OWE_ERROR("egl surface attach failed for output %s", out->name);
            }
        }
    }
}

struct wl_display *owe_wayland_display(struct owe_wayland *wl) {
    return wl ? wl->display : NULL;
}

int owe_wayland_fd(struct owe_wayland *wl) {
    return wl && wl->display ? wl_display_get_fd(wl->display) : -1;
}

int owe_wayland_dispatch(struct owe_wayland *wl) {
    if (!wl || !wl->display) {
        return -1;
    }
    if (wl_display_prepare_read(wl->display) != 0) {
        if (wl_display_dispatch_pending(wl->display) < 0) {
            return -1;
        }
        return 0;
    }
    if (wl_display_read_events(wl->display) < 0) {
        return -1;
    }
    if (wl_display_dispatch_pending(wl->display) < 0) {
        return -1;
    }
    return 0;
}

int owe_wayland_dispatch_pending(struct owe_wayland *wl) {
    return wl ? wl_display_dispatch_pending(wl->display) : -1;
}

void owe_wayland_flush(struct owe_wayland *wl) {
    if (wl && wl->display) {
        wl_display_flush(wl->display);
    }
}

void owe_wayland_request_render(void *opaque) {
    struct owe_wayland *wl = opaque ? opaque : (owe_app_get() ? owe_app_get()->wl : NULL);
    owe_output_t *out;
    if (!wl) {
        return;
    }
    for (out = wl->outputs; out; out = out->next) {
        out->frame_pending = 1;
    }
}

void owe_wayland_render_pending(struct owe_wayland *wl) {
    owe_app_t *app;
    owe_output_t *out;
    int want_video = 0;
    if (!wl) {
        return;
    }
    app = owe_app_get();
    if (!app || !app->egl) {
        return;
    }
    want_video = app->mpv && owe_mpv_has_video(app->mpv) && !app->paused;
    {
        int need_frame_cb = app->still && owe_still_has_image(app->still) &&
                            owe_still_needs_frames(app->still);
        for (out = wl->outputs; out; out = out->next) {
            struct wl_callback *cb;
            int rendered_video = 0;
            if (!out->configured || !out->frame_pending || !out->egl_surface) {
                continue;
            }
            out->frame_pending = 0;
            if (want_video) {
                owe_mpv_render_output(app->mpv, out);
                rendered_video = 1;
            } else if (app->still && owe_still_has_image(app->still)) {
                owe_still_render_output(app->still, out);
            } else if (app->mpv && owe_mpv_has_video(app->mpv)) {
                owe_mpv_render_output(app->mpv, out);
                rendered_video = 1;
            } else {
                owe_egl_clear_output(app->egl, out, 0.0f, 0.0f, 0.0f, 1.0f);
            }
            if (need_frame_cb && !out->frame_callback) {
                cb = wl_surface_frame(out->surface);
                if (cb) {
                    out->frame_callback = cb;
                    wl_callback_add_listener(cb, &frame_listener, out);
                }
            }
            owe_egl_swap_output(app->egl, out);
            (void)rendered_video;
        }
        if (want_video) owe_mpv_report_swap(app->mpv);
    }
    wl_display_flush(wl->display);
}

owe_output_t *owe_wayland_outputs(struct owe_wayland *wl) {
    return wl ? wl->outputs : NULL;
}

int owe_wayland_output_count(struct owe_wayland *wl) {
    return wl ? wl->output_count : 0;
}

void owe_wayland_outputs_max_size(struct owe_wayland *wl, int *w, int *h) {
    owe_output_t *out;
    int mw = 0;
    int mh = 0;
    if (!wl) {
        if (w) {
            *w = 0;
        }
        if (h) {
            *h = 0;
        }
        return;
    }
    for (out = wl->outputs; out; out = out->next) {
        int ow = out->buffer_w;
        int oh = out->buffer_h;
        if (ow > mw) {
            mw = ow;
        }
        if (oh > mh) {
            mh = oh;
        }
    }
    if (w) {
        *w = mw;
    }
    if (h) {
        *h = mh;
    }
}

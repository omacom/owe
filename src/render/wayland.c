#include "wayland.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wayland-client.h>
#include <wayland-egl.h>

#include "display_state.h"
#include "egl.h"
#include "log.h"
#include "mpv.h"
#include "render.h"
#include "still.h"
#include "fractional-scale-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

struct pending_output {
    uint32_t name;
    uint32_t version;
    struct pending_output *next;
};

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
    bool registry_ready;
    struct pending_output *pending_outputs;
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
    (void)data;
    (void)wl_output;
    (void)x;
    (void)y;
    (void)physical_width;
    (void)physical_height;
    (void)subpixel;
    (void)make;
    (void)model;
    (void)transform;
    /* wl_output.name carries the compositor connector name, which the
     * daemon uses to match Hyprland monitors. Keep it. */
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
    out->frame_ready = 1;
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
    owe_app_t *app = owe_app_get();
    (void)time;
    wl_callback_destroy(cb);
    out->frame_callback = NULL;
    /* Animate stills and transition overlays when the compositor is ready.
     * Outside transitions, mpv controls the video frame rate. */
    out->frame_ready = 1;
    if (app && (owe_still_has_image(app->still) || owe_still_has_image(app->transition))) {
        out->frame_pending = 1;
    }
}

static owe_output_t *output_new(struct owe_wayland *wl, struct wl_output *wl_output, uint32_t name) {
    owe_app_t *app = owe_app_get();
    owe_output_t *out = calloc(1, sizeof(*out));
    struct wl_region *region;
    if (!out) {
        wl_output_destroy(wl_output);
        return NULL;
    }
    out->wl_output = wl_output;
    out->owner = wl;
    out->registry_name = name;
    out->width = 0;
    out->height = 0;
    out->scale = 1;
    out->frame_ready = 1;
    snprintf(out->name, sizeof(out->name), "output-%p", (void *)wl_output);
    wl_output_add_listener(wl_output, &output_listener, out);

    out->surface = wl_compositor_create_surface(wl->compositor);
    if (!out->surface) goto fail;
    wl_surface_set_buffer_scale(out->surface, out->scale > 0 ? out->scale : 1);
    if (wl->viewporter) {
        out->viewport = wp_viewporter_get_viewport(wl->viewporter, out->surface);
    }
    if (wl->fractional_scale && out->viewport) {
        out->fractional =
            wp_fractional_scale_manager_v1_get_fractional_scale(wl->fractional_scale, out->surface);
        if (out->fractional) {
            wp_fractional_scale_v1_add_listener(out->fractional, &fractional_listener, out);
        }
    }
    out->layer = zwlr_layer_shell_v1_get_layer_surface(wl->layer_shell, out->surface, wl_output,
                                                       ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND,
                                                       "owe-background");
    if (!out->layer) goto fail;
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
    if (!out->egl_window) goto fail;
    if (app && app->egl) {
        out->egl_surface = owe_egl_create_window_surface(app->egl, out->egl_window);
        if (!out->egl_surface) goto fail;
    }

    out->next = wl->outputs;
    wl->outputs = out;
    wl->output_count++;
    return out;
fail:
    output_free(wl, out);
    return NULL;
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
            wl->output_count--;
            break;
        }
    }
    free(out);
}

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
                            const char *interface, uint32_t version) {
    struct owe_wayland *wl = data;
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        if (version >= 3) {
            wl->compositor = wl_registry_bind(registry, name, &wl_compositor_interface,
                                               version >= 4 ? 4 : version);
        }
    } else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        wl->layer_shell = wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, 1);
    } else if (strcmp(interface, wp_viewporter_interface.name) == 0) {
        wl->viewporter = wl_registry_bind(registry, name, &wp_viewporter_interface, 1);
    } else if (strcmp(interface, wp_fractional_scale_manager_v1_interface.name) == 0) {
        wl->fractional_scale = wl_registry_bind(registry, name, &wp_fractional_scale_manager_v1_interface, 1);
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        uint32_t v = version >= 4 ? 4 : version;
        if (wl->registry_ready && wl->compositor && wl->layer_shell) {
            struct wl_output *wo = wl_registry_bind(registry, name, &wl_output_interface, v);
            if (wo) output_new(wl, wo, name);
        } else {
            struct pending_output *pending = calloc(1, sizeof(*pending));
            if (pending) {
                *pending = (struct pending_output){name, v, wl->pending_outputs};
                wl->pending_outputs = pending;
            }
        }
    }
}

/* Bind all initial globals before the output surfaces use optional protocols. */
static void create_pending_outputs(struct owe_wayland *wl) {
    wl->registry_ready = true;
    while (wl->pending_outputs) {
        struct pending_output *pending = wl->pending_outputs;
        wl->pending_outputs = pending->next;
        struct wl_output *wo = wl_registry_bind(wl->registry, pending->name,
                                                &wl_output_interface, pending->version);
        if (wo) output_new(wl, wo, pending->name);
        free(pending);
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
    struct owe_wayland *wl = data;
    (void)registry;
    for (struct pending_output **link = &wl->pending_outputs; *link; link = &(*link)->next) {
        if ((*link)->name == name) {
            struct pending_output *pending = *link;
            *link = pending->next;
            free(pending);
            return;
        }
    }
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
    if (wl_display_roundtrip(wl->display) < 0 || !wl->compositor || !wl->layer_shell) {
        OWE_ERROR("compositor lacks wl_compositor or wlr-layer-shell");
        owe_wayland_free(wl);
        return NULL;
    }
    create_pending_outputs(wl);
    if (wl_display_roundtrip(wl->display) < 0) {
        owe_wayland_free(wl);
        return NULL;
    }
    OWE_INFO("wayland ready, outputs=%d", wl->output_count);
    return wl;
}

void owe_wayland_destroy_outputs(struct owe_wayland *wl) {
    owe_output_t *out;
    owe_output_t *next;
    if (!wl) {
        return;
    }
    while (wl->pending_outputs) {
        struct pending_output *pending = wl->pending_outputs;
        wl->pending_outputs = pending->next;
        free(pending);
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

static int64_t now_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

/* Skip a compositor output whose connector reports DPMS off. Mesa's swap
 * blocks in poll while nothing consumes buffers, which stalls the whole
 * event loop until the screen returns. The state is re-read once per frame
 * interval so the first frame after a blank is not swapped. */
static bool output_drm_off(owe_output_t *out) {
    int64_t now = now_ms();
    if (now - out->drm_checked_ms > 20) {
        bool off = owe_drm_connector_state("/sys/class/drm", out->name) == 1;
        if (off != (out->drm_off != 0)) {
            OWE_INFO("output %s dpms %s", out->name, off ? "off" : "on");
        }
        out->drm_off = off;
        out->drm_checked_ms = now;
    }
    return out->drm_off != 0;
}

void owe_wayland_set_skipped(struct owe_wayland *wl, const char *names) {
    owe_output_t *out;
    bool changed = false;
    if (!wl) {
        return;
    }
    for (out = wl->outputs; out; out = out->next) {
        bool skip = false;
        const char *cursor = names ? names : "";
        while (*cursor) {
            const char *end = strchr(cursor, ',');
            size_t len = end ? (size_t)(end - cursor) : strlen(cursor);
            if (len == strlen(out->name) && strncmp(cursor, out->name, len) == 0) {
                skip = true;
                break;
            }
            cursor = end ? end + 1 : cursor + len;
        }
        if (skip && !out->skip_render) {
            out->skip_render = 1;
            out->frame_pending = 0;
            changed = true;
        } else if (!skip && out->skip_render) {
            out->skip_render = 0;
            out->frame_pending = 1;
            changed = true;
        }
    }
    if (changed) {
        owe_app_request_render();
    }
}

void owe_wayland_skipped_list(struct owe_wayland *wl, char *out, size_t out_len) {
    owe_output_t *item;
    size_t used = 0;
    if (!out || out_len < 2) {
        return;
    }
    out[0] = '\0';
    for (item = wl ? wl->outputs : NULL; item; item = item->next) {
        if (!item->skip_render && !item->drm_off) {
            continue;
        }
        used += (size_t)snprintf(out + used, out_len - used, "%s%s", used ? "," : "", item->name);
        if (used >= out_len) {
            out[out_len - 1] = '\0';
            return;
        }
    }
}

void owe_wayland_render_pending(struct owe_wayland *wl) {
    owe_app_t *app;
    owe_output_t *out;
    int want_video = 0;
    int rendered_video = 0;
    bool rendered_transition = false;
    if (!wl) {
        return;
    }
    app = owe_app_get();
    if (!app || !app->egl) {
        return;
    }
    /* Keep the last buffer until the outgoing image can cover video startup. */
    if (owe_mpv_has_video(app->mpv) && owe_still_transition_waiting(app->transition)) {
        owe_wayland_flush(wl);
        return;
    }
    want_video = app->mpv && owe_mpv_has_video(app->mpv) && !app->paused;
    {
        int need_frame_cb = (app->still && owe_still_has_image(app->still) &&
                             owe_still_needs_frames(app->still)) ||
                            (app->transition && owe_still_has_image(app->transition) &&
                             owe_still_needs_frames(app->transition));
        for (out = wl->outputs; out; out = out->next) {
            struct wl_callback *cb;
            bool video_drawn = false;
            bool transition_drawn = false;
            if (!out->configured || !out->frame_pending || !out->egl_surface) {
                continue;
            }
            if (out->skip_render || !out->frame_ready || output_drm_off(out)) {
                continue;
            }
            out->frame_pending = 0;
            if (want_video) {
                owe_mpv_render_output(app->mpv, out);
                video_drawn = true;
            } else if (app->still && owe_still_has_image(app->still)) {
                owe_still_render_output(app->still, out);
            } else if (app->mpv && owe_mpv_has_video(app->mpv)) {
                owe_mpv_render_output(app->mpv, out);
                video_drawn = true;
            } else {
                owe_egl_clear_output(app->egl, out, 0.0f, 0.0f, 0.0f, 1.0f);
            }
            if (video_drawn && owe_still_has_image(app->transition)) {
                owe_still_render_overlay(app->transition, out);
                transition_drawn = true;
            }
            /* Re-read the connector state without the cache. A blank between
             * the check above and this swap would block inside Mesa until the
             * screen returns. */
            out->drm_checked_ms = 0;
            if (output_drm_off(out)) {
                out->frame_pending = 1;
                continue;
            }
            /* Request the callback before the commit that triggers it, then
             * pace on the compositor. While it stops presenting frames, no
             * callback arrives and no swap can block in Mesa. */
            if (need_frame_cb || want_video) {
                if (!out->frame_callback) {
                    cb = wl_surface_frame(out->surface);
                    if (cb) {
                        out->frame_callback = cb;
                        wl_callback_add_listener(cb, &frame_listener, out);
                    }
                }
                out->frame_ready = 0;
            }
            if (owe_egl_swap_output(app->egl, out) != 0) {
                out->swap_failures++;
                /* A failed swap cannot deliver the callback that gates the next frame. */
                if (out->frame_callback) wl_callback_destroy(out->frame_callback);
                out->frame_callback = NULL;
                out->frame_ready = 1;
                out->frame_pending = 1;
                continue;
            }
            out->swaps++;
            if (video_drawn) rendered_video = 1;
            if (transition_drawn) rendered_transition = true;
        }
        if (want_video && rendered_video) owe_mpv_report_swap(app->mpv);
    }
    if (rendered_transition && owe_still_has_image(app->transition) &&
        owe_still_fade_done(app->transition)) {
        owe_egl_make_current(app->egl);
        owe_still_unload(app->transition);
        /* Outputs can finish at different times, including while video is paused. */
        owe_wayland_request_render(wl);
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

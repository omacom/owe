#include "mpv.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <epoxy/gl.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <mpv/client.h>
#include <mpv/render.h>
#include <mpv/render_gl.h>

#include "egl.h"
#include "log.h"
#include "render.h"
#include "wayland.h"

struct owe_mpv {
    struct owe_wayland *wl;
    mpv_handle *handle;
    mpv_render_context *ctx;
    char path[4096];
    bool has_video;
    bool paused;
    bool ready;
    char error[256];
    int wakeup_pipe[2];
};

static void on_mpv_wakeup(void *data) {
    struct owe_mpv *m = data;
    char c = 'w';
    if (!m) {
        return;
    }
    if (write(m->wakeup_pipe[1], &c, 1) < 0) {
        return;
    }
}

static void on_render_update(void *data) {
    struct owe_mpv *m = data;
    char c = 'r';
    if (!m) {
        return;
    }
    if (write(m->wakeup_pipe[1], &c, 1) < 0) {
        return;
    }
}

static int set_opt(mpv_handle *h, const char *k, const char *v) {
    int rc = mpv_set_option_string(h, k, v);
    if (rc < 0) {
        OWE_WARN("mpv option %s=%s failed: %s", k, v, mpv_error_string(rc));
        return -1;
    }
    return 0;
}

static void set_extra_options(mpv_handle *h) {
    const char *extra = getenv("OWE_MPV_OPTIONS");
    char *copy;
    char *save = NULL;
    char *token;
    if (!extra || !*extra) {
        return;
    }
    copy = strdup(extra);
    if (!copy) {
        return;
    }
    for (token = strtok_r(copy, ";", &save); token; token = strtok_r(NULL, ";", &save)) {
        char *eq = strchr(token, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        set_opt(h, token, eq + 1);
    }
    free(copy);
}

static void *get_proc_address(void *ctx, const char *name) {
    (void)ctx;
    return (void *)eglGetProcAddress(name);
}

struct owe_mpv *owe_mpv_new(struct owe_wayland *wl) {
    struct owe_mpv *m;
    mpv_render_param rparams[4];
    int nparams = 0;
    mpv_opengl_init_params gl_params = { .get_proc_address = get_proc_address,
                                         .get_proc_address_ctx = NULL };
    int rc;
    if (!wl) {
        return NULL;
    }
    m = calloc(1, sizeof(*m));
    if (!m) {
        return NULL;
    }
    m->wl = wl;
    if (pipe2(m->wakeup_pipe, O_NONBLOCK | O_CLOEXEC) != 0) {
        free(m);
        return NULL;
    }
    m->handle = mpv_create();
    if (!m->handle) {
        close(m->wakeup_pipe[0]);
        close(m->wakeup_pipe[1]);
        free(m);
        return NULL;
    }
    set_opt(m->handle, "vo", "libmpv");
    {
        const char *hwdec = getenv("OWE_HWDEC");
        set_opt(m->handle, "hwdec", hwdec && *hwdec ? hwdec : "auto-safe");
    }
    /* A video wallpaper plays its audio track through the default output.
     * Cover art stays off, and a session with no audio server keeps a null
     * output instead of failing the track. */
    set_opt(m->handle, "audio-display", "no");
    set_opt(m->handle, "audio-fallback-to-null", "yes");
    set_opt(m->handle, "sub", "no");
    set_opt(m->handle, "sid", "no");
    set_opt(m->handle, "loop-file", "inf");
    set_opt(m->handle, "interpolation", "no");
    set_opt(m->handle, "profile", "fast");
    set_opt(m->handle, "scale", "bilinear");
    set_opt(m->handle, "cscale", "bilinear");
    set_opt(m->handle, "framedrop", "no");
    set_opt(m->handle, "panscan", "1.0");
    set_opt(m->handle, "config", "no");
    set_opt(m->handle, "terminal", "no");
    set_opt(m->handle, "osc", "no");
    /* The wallpaper needs no lua scripts: ytdl_hook opens a network path,
     * stats and console add threads and memory that never draw a frame. */
    set_opt(m->handle, "load-scripts", "no");
    set_opt(m->handle, "load-auto-profiles", "no");
    set_opt(m->handle, "load-commands", "no");
    set_opt(m->handle, "load-console", "no");
    set_opt(m->handle, "load-context-menu", "no");
    set_opt(m->handle, "load-positioning", "no");
    set_opt(m->handle, "load-select", "no");
    set_opt(m->handle, "load-stats-overlay", "no");
    set_opt(m->handle, "ytdl", "no");
    set_opt(m->handle, "idle", "yes");
    /* Wallpapers loop a local file. No backward cache, small forward cache,
     * and a small decoder frame pool cut resident memory with no visual cost. */
    set_opt(m->handle, "demuxer-max-bytes", "32MiB");
    set_opt(m->handle, "demuxer-max-back-bytes", "0");
    set_opt(m->handle, "demuxer-readahead-secs", "1");
    set_opt(m->handle, "hwdec-extra-frames", "3");
    /* Audio and video stay in step. A silent file uses the default clock. */
    set_opt(m->handle, "video-sync", "audio");
    set_extra_options(m->handle);
    if (mpv_initialize(m->handle) < 0) {
        OWE_ERROR("mpv_initialize failed");
        mpv_destroy(m->handle);
        close(m->wakeup_pipe[0]);
        close(m->wakeup_pipe[1]);
        free(m);
        return NULL;
    }
    rparams[nparams].type = MPV_RENDER_PARAM_API_TYPE;
    rparams[nparams].data = (void *)MPV_RENDER_API_TYPE_OPENGL;
    nparams++;
    rparams[nparams].type = MPV_RENDER_PARAM_OPENGL_INIT_PARAMS;
    rparams[nparams].data = &gl_params;
    nparams++;
    rparams[nparams].type = MPV_RENDER_PARAM_WL_DISPLAY;
    rparams[nparams].data = owe_wayland_display(wl);
    nparams++;
    rparams[nparams].type = MPV_RENDER_PARAM_INVALID;
    rparams[nparams].data = NULL;
    rc = mpv_render_context_create(&m->ctx, m->handle, rparams);
    if (rc < 0) {
        OWE_ERROR("mpv_render_context_create failed: %s", mpv_error_string(rc));
        mpv_destroy(m->handle);
        close(m->wakeup_pipe[0]);
        close(m->wakeup_pipe[1]);
        free(m);
        return NULL;
    }
    mpv_render_context_set_update_callback(m->ctx, on_render_update, m);
    mpv_set_wakeup_callback(m->handle, on_mpv_wakeup, m);
    OWE_INFO("libmpv ready");
    return m;
}

void owe_mpv_free(struct owe_mpv *m) {
    if (!m) {
        return;
    }
    if (m->ctx) {
        owe_egl_make_current(owe_app_get()->egl);
        mpv_render_context_set_update_callback(m->ctx, NULL, NULL);
        mpv_render_context_free(m->ctx);
    }
    if (m->handle) {
        mpv_set_wakeup_callback(m->handle, NULL, NULL);
        mpv_destroy(m->handle);
    }
    close(m->wakeup_pipe[0]);
    close(m->wakeup_pipe[1]);
    free(m);
}

int owe_mpv_load(struct owe_mpv *m, const char *path) {
    const char *cmd[] = { "loadfile", path, "replace", NULL };
    int rc;
    if (!m || !path || !*path) {
        return -1;
    }
    rc = mpv_command(m->handle, cmd);
    if (rc < 0) {
        OWE_ERROR("loadfile failed: %s", mpv_error_string(rc));
        return -1;
    }
    snprintf(m->path, sizeof(m->path), "%s", path);
    m->error[0] = '\0';
    m->has_video = true;
    m->ready = false;
    return 0;
}

void owe_mpv_stop(struct owe_mpv *m) {
    const char *cmd[] = { "stop", NULL };
    if (!m) {
        return;
    }
    mpv_command(m->handle, cmd);
    m->has_video = false;
    m->path[0] = '\0';
    m->ready = false;
}

void owe_mpv_set_paused(struct owe_mpv *m, bool paused) {
    int v;
    if (!m || m->paused == paused) {
        return;
    }
    m->paused = paused;
    v = paused ? 1 : 0;
    mpv_set_property(m->handle, "pause", MPV_FORMAT_FLAG, &v);
    OWE_INFO("mpv %s", paused ? "paused" : "resumed");
}

bool owe_mpv_has_video(struct owe_mpv *m) {
    return m && m->has_video;
}

bool owe_mpv_is_paused(struct owe_mpv *m) {
    return m && m->paused;
}

bool owe_mpv_ready(struct owe_mpv *m) {
    return m && m->ready;
}

const char *owe_mpv_path(struct owe_mpv *m) {
    return m ? m->path : "";
}

int owe_mpv_fd(struct owe_mpv *m) {
    return m ? m->wakeup_pipe[0] : -1;
}

bool owe_mpv_process_updates(struct owe_mpv *m) {
    char buf[128];
    uint64_t flags;
    if (!m) {
        return false;
    }
    while (read(m->wakeup_pipe[0], buf, sizeof(buf)) > 0) {
    }
    for (;;) {
        mpv_event *event = mpv_wait_event(m->handle, 0);
        if (event->event_id == MPV_EVENT_NONE) break;
        if (event->event_id == MPV_EVENT_END_FILE) {
            mpv_event_end_file *end = event->data;
            if (end && end->reason == MPV_END_FILE_REASON_ERROR) {
                snprintf(m->error, sizeof(m->error), "%s", mpv_error_string(end->error));
                m->has_video = false;
                OWE_ERROR("Video playback failed: %s", m->error);
            }
        }
    }
    if (!m->ctx) {
        return false;
    }
    owe_egl_make_current(owe_app_get()->egl);
    flags = mpv_render_context_update(m->ctx);
    return (flags & MPV_RENDER_UPDATE_FRAME) != 0;
}

const char *owe_mpv_error(struct owe_mpv *m) { return m ? m->error : ""; }

const char *owe_mpv_hwdec(struct owe_mpv *m) {
    static char name[64];
    char *value = m ? mpv_get_property_string(m->handle, "hwdec-current") : NULL;
    snprintf(name, sizeof(name), "%s", value ? value : "no");
    mpv_free(value);
    return name;
}

void owe_mpv_render_output(struct owe_mpv *m, struct owe_output *out) {
    owe_app_t *app;
    int w;
    int h;
    mpv_opengl_fbo fbo;
    mpv_render_param params[4];
    int flip = 1;
    int block = 0;
    if (!m || !m->ctx || !out) {
        return;
    }
    app = owe_app_get();
    if (!app || !app->egl) {
        return;
    }
    if (owe_egl_prepare_output(app->egl, out) != 0) {
        return;
    }
    owe_output_buffer_size(out, &w, &h);
    if (w <= 0 || h <= 0) {
        return;
    }
    fbo.fbo = 0;
    fbo.w = w;
    fbo.h = h;
    fbo.internal_format = 0;
    params[0].type = MPV_RENDER_PARAM_OPENGL_FBO;
    params[0].data = &fbo;
    params[1].type = MPV_RENDER_PARAM_FLIP_Y;
    params[1].data = &flip;
    params[2].type = MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME;
    params[2].data = &block;
    params[3].type = MPV_RENDER_PARAM_INVALID;
    params[3].data = NULL;
    if (mpv_render_context_render(m->ctx, params) < 0) {
        return;
    }
    m->ready = true;
}

void owe_mpv_report_swap(struct owe_mpv *m) {
    if (m && m->ctx) {
        mpv_render_context_report_swap(m->ctx);
    }
}

double owe_mpv_time_pos(struct owe_mpv *m) {
    double pos = -1.0;
    if (m && m->handle) {
        mpv_get_property(m->handle, "time-pos", MPV_FORMAT_DOUBLE, &pos);
    }
    return pos;
}

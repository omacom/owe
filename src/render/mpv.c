#include "mpv.h"

#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <epoxy/gl.h>
#include <libavformat/avformat.h>
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
    /* How many copies of the file the loaded EDL plays back to back; 1 when
     * the file was loaded on its own. */
    int repeats;
    /* The length of one copy, read from the file when it was loaded. */
    double copy_length;
    bool has_video;
    bool paused;
    bool pause_known;
    bool ready;
    bool file_loaded;
    /* mpv has decoded a frame of the current file. Drawing earlier renders
     * black, which would cover the still the video replaces. */
    bool has_frame;
    int video_w;
    int video_h;
    /* The core sends these values as property events. A synchronous read on
     * the render thread can wait 200 ms while the core waits for this thread. */
    char hwdec_current[64];
    bool eof;
    char error[256];
    int wakeup_pipe[2];
    /* See no_fence_sync. */
    bool drop_fences;
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

static void log_message(const mpv_event_log_message *message) {
    if (!message || !message->text) return;
    size_t len = strlen(message->text);
    while (len && (message->text[len - 1] == '\n' || message->text[len - 1] == '\r')) len--;
    const char *prefix = message->prefix ? message->prefix : "unknown";
    if (message->log_level <= MPV_LOG_LEVEL_ERROR) {
        OWE_ERROR("mpv %s: %.*s", prefix, (int)len, message->text);
    } else {
        OWE_WARN("mpv %s: %.*s", prefix, (int)len, message->text);
    }
}

static void log_startup_failure(mpv_handle *handle) {
    for (;;) {
        mpv_event *event = mpv_wait_event(handle, 0);
        if (event->event_id == MPV_EVENT_NONE || event->event_id == MPV_EVENT_SHUTDOWN) break;
        if (event->event_id == MPV_EVENT_LOG_MESSAGE) log_message(event->data);
    }
}

/* libmpv starts every hwdec interop with the render context. The CUDA
 * interop calls cuInit even when GL runs on Intel or AMD, where that interop
 * never works. cuInit wakes an NVIDIA GPU, and with libcuda but no NVIDIA GPU
 * it can take seconds. On Intel and AMD only the VA-API interop works. */
static bool vaapi_interop_only(const char *vendor, const char *renderer) {
    return vendor && renderer &&
           (strstr(vendor, "Intel") || strstr(vendor, "AMD") || strstr(renderer, "radeonsi"));
}

/* auto-safe tries Vulkan and NVDEC before VA-API, and their copy modes after
 * it. On Intel and AMD only VA-API decodes on the GPU that renders, so a
 * failed VA-API fell through to nvdec-copy, which wakes the NVIDIA GPU of a
 * hybrid laptop. Nouveau decodes video only with firmware that
 * linux-firmware does not ship. Without it, its VA-API driver crashed the
 * renderer with SIGBUS instead of returning an error. */
static const char *default_hwdec(const char *vendor, const char *renderer) {
    if (vaapi_interop_only(vendor, renderer)) return "vaapi,vaapi-copy";
    if (vendor && strstr(vendor, "nouveau")) return "no";
    return "auto-safe";
}

/* Omarchy's NVIDIA setup sets LIBVA_DRIVER_NAME=nvidia for the session. When
 * GL renders on the Intel or AMD GPU of a hybrid laptop, that name sends
 * VA-API to the NVIDIA GPU. Without it, libva loads the driver of the GPU
 * that renders. */
static void drop_nvidia_va_driver(const char *vendor, const char *renderer) {
    const char *driver = getenv("LIBVA_DRIVER_NAME");
    if (!vaapi_interop_only(vendor, renderer) || !driver || strcmp(driver, "nvidia") != 0) {
        return;
    }
    OWE_INFO("GL renders on %s, so VA-API ignores LIBVA_DRIVER_NAME=nvidia", vendor);
    unsetenv("LIBVA_DRIVER_NAME");
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

/* mpv 0.40 and 0.41 make a GL fence for each rendered frame and free it only
 * on a buffer swap. The render API never swaps, so each frame leaks its
 * fence: 0.2 kB on Intel and about 11 kB on NVIDIA. mpv commit f74adc4243
 * fixes this upstream. mpv skips a fence that it gets as 0. Its only other
 * fences guard direct rendering, which needs MPV_RENDER_PARAM_ADVANCED_CONTROL. */
static GLsync GLAPIENTRY no_fence_sync(GLenum condition, GLbitfield flags) {
    (void)condition;
    (void)flags;
    return NULL;
}

static void *get_proc_address(void *ctx, const char *name) {
    struct owe_mpv *m = ctx;
    if (m->drop_fences && strcmp(name, "glFenceSync") == 0) {
        return (void *)no_fence_sync;
    }
    return (void *)eglGetProcAddress(name);
}

static void check_fence_leak(struct owe_mpv *m) {
    char *version = mpv_get_property_string(m->handle, "mpv-version");
    int major = -1;
    int minor = -1;
    if (version && sscanf(version, "mpv v%d.%d", &major, &minor) != 2) {
        sscanf(version, "mpv %d.%d", &major, &minor);
    }
    if (major == 0 && (minor == 40 || minor == 41)) {
        m->drop_fences = true;
        OWE_INFO("%s leaks a GL fence per frame, frame fences are off", version);
    }
    mpv_free(version);
}

struct owe_mpv *owe_mpv_new(struct owe_wayland *wl) {
    struct owe_mpv *m;
    mpv_render_param rparams[4];
    int nparams = 0;
    mpv_opengl_init_params gl_params = { .get_proc_address = get_proc_address,
                                         .get_proc_address_ctx = NULL };
    const char *vendor;
    const char *renderer;
    int rc;
    if (!wl) {
        return NULL;
    }
    m = calloc(1, sizeof(*m));
    if (!m) {
        return NULL;
    }
    m->wl = wl;
    gl_params.get_proc_address_ctx = m;
    vendor = (const char *)glGetString(GL_VENDOR);
    renderer = (const char *)glGetString(GL_RENDERER);
    /* Before mpv starts its threads, which read the environment. */
    drop_nvidia_va_driver(vendor, renderer);
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
    mpv_request_log_messages(m->handle, "warn");
    set_opt(m->handle, "vo", "libmpv");
    {
        const char *hwdec = getenv("OWE_HWDEC");
        set_opt(m->handle, "hwdec", hwdec && *hwdec ? hwdec : default_hwdec(vendor, renderer));
    }
    /* A video wallpaper plays its audio track through the default output.
     * Cover art stays off, and a session with no audio server keeps a null
     * output instead of failing the track. */
    set_opt(m->handle, "audio-display", "no");
    set_opt(m->handle, "audio-fallback-to-null", "yes");
    set_opt(m->handle, "sub", "no");
    set_opt(m->handle, "sid", "no");
    set_opt(m->handle, "loop-file", "inf");
    /* A one-shot intro must hold its last frame until the daemon hands the
     * background back to the shell. */
    set_opt(m->handle, "keep-open", "yes");
    set_opt(m->handle, "interpolation", "no");
    set_opt(m->handle, "profile", "fast");
    set_opt(m->handle, "scale", "bilinear");
    set_opt(m->handle, "cscale", "bilinear");
    /* The fast profile shrinks video with plain bilinear sampling, which skips
     * source pixels once a video is larger than the output. Thin lines then
     * come out harsher than the same picture as a still, which is shrunk with
     * an averaging filter, and how much harsher depends on the output size.
     * Hermite with correct downscaling averages the way a still resize does. */
    set_opt(m->handle, "dscale", "hermite");
    set_opt(m->handle, "correct-downscaling", "yes");
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
    if (vaapi_interop_only(vendor, renderer)) {
        set_opt(m->handle, "gpu-hwdec-interop", "vaapi");
    }
    set_extra_options(m->handle);
    rc = mpv_initialize(m->handle);
    if (rc < 0) {
        log_startup_failure(m->handle);
        OWE_ERROR("mpv_initialize failed: %s", mpv_error_string(rc));
        mpv_destroy(m->handle);
        close(m->wakeup_pipe[0]);
        close(m->wakeup_pipe[1]);
        free(m);
        return NULL;
    }
    char *hwdec = mpv_get_property_string(m->handle, "hwdec");
    OWE_INFO("mpv requested hwdec=%s", hwdec ? hwdec : "unknown");
    mpv_free(hwdec);
    check_fence_leak(m);
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
        log_startup_failure(m->handle);
        OWE_ERROR("mpv_render_context_create failed: %s", mpv_error_string(rc));
        mpv_destroy(m->handle);
        close(m->wakeup_pipe[0]);
        close(m->wakeup_pipe[1]);
        free(m);
        return NULL;
    }
    mpv_render_context_set_update_callback(m->ctx, on_render_update, m);
    mpv_set_wakeup_callback(m->handle, on_mpv_wakeup, m);
    mpv_observe_property(m->handle, 1, "eof-reached", MPV_FORMAT_FLAG);
    mpv_observe_property(m->handle, 2, "dwidth", MPV_FORMAT_INT64);
    mpv_observe_property(m->handle, 3, "dheight", MPV_FORMAT_INT64);
    mpv_observe_property(m->handle, 4, "hwdec-current", MPV_FORMAT_STRING);
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

/* mpv's loop-file holds the last frame about one frame too long before it
 * seeks back, then cuts the first frame short to catch up: on a 60 Hz screen a
 * 24 fps loop shows its last frame for ~90 ms instead of ~42 and can skip its
 * first. Played as an EDL of copies of the file, the decoder runs from the
 * last frame of one copy straight into the first of the next with no seek.
 * mpv opens the file once for all of them. loop-file still repeats the whole
 * EDL, so the stall is left once every LOOP_COPIES passes (about an hour for a
 * 14 s loop). The timeline costs a little CPU per copy: 1000 copies took the
 * renderer from about 4.2% to 5.3% of a core, 256 to 4.7%, 16 to no change.
 *
 * An EDL segment only ends where the file does when its timestamps start at 0
 * and its length is known. mpv takes each copy from 0 for the file's length,
 * so a file that starts later loses its tail on every copy (an MP4 starting at
 * 2 s plays one second of three), and one without a length plays only once.
 * Those files loop on their own, as before. */
#define LOOP_COPIES 256
#define LOOP_EDL_MAX (1 << 20)

/* The length of one copy when the file can play as an EDL of copies, and 0
 * otherwise. mpv takes each copy from 0 for the file's length, so this needs:
 * - the first frame's timestamp at 0: a file that starts later, has negative
 *   leading timestamps, or lacks an MP4 edit list loses frames on every copy;
 * - a known length that includes the last frame's display time, which MP4/MOV
 *   and Matroska/WebM give (NUT, for one, leaves the last frame out);
 * - no audio: audio that ends before the video makes mpv play the video's
 *   tail too fast on every copy under video-sync=audio.
 * Other files loop on their own as before. It reads the header and the first
 * few packets without decoding, so it takes a few milliseconds even for a 4K
 * file (finding stream info decoded frames and took up to 300 ms), and it
 * opens local files of those containers only, so a playlist or a FIFO can't
 * block the renderer. */
static double loop_copy_length(const char *path) {
    AVFormatContext *fmt = NULL;
    AVDictionary *options = NULL;
    AVPacket *packet;
    int64_t first = AV_NOPTS_VALUE;
    double length = -1.0;
    int video = -1;
    bool audio = false;
    av_dict_set(&options, "format_whitelist", "mov,mp4,m4a,3gp,3g2,mj2,matroska,webm", 0);
    av_dict_set(&options, "protocol_whitelist", "file", 0);
    if (avformat_open_input(&fmt, path, NULL, &options) < 0) {
        av_dict_free(&options);
        OWE_INFO("Loop plays on its own: not an MP4 or Matroska file");
        return 0.0;
    }
    av_dict_free(&options);
    for (unsigned i = 0; i < fmt->nb_streams; i++) {
        AVStream *stream = fmt->streams[i];
        enum AVMediaType type = stream->codecpar->codec_type;
        if (type == AVMEDIA_TYPE_AUDIO) audio = true;
        if (video < 0 && type == AVMEDIA_TYPE_VIDEO && !(stream->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
            video = (int)i;
        }
        if (stream->duration != AV_NOPTS_VALUE && stream->duration > 0) {
            double d = stream->duration * av_q2d(stream->time_base);
            if (d > length) length = d;
        }
    }
    if (fmt->duration != AV_NOPTS_VALUE && fmt->duration > 0) {
        length = (double)fmt->duration / AV_TIME_BASE;
    }
    /* The earliest timestamp of the first few video packets, in case the
     * first packet is not the first frame shown. */
    packet = audio ? NULL : av_packet_alloc();
    for (int i = 0, seen = 0; packet && video >= 0 && seen < 8 && i < 32 && av_read_frame(fmt, packet) >= 0; i++) {
        if (packet->stream_index == video && packet->pts != AV_NOPTS_VALUE) {
            int64_t t = av_rescale_q(packet->pts, fmt->streams[video]->time_base, AV_TIME_BASE_Q);
            if (first == AV_NOPTS_VALUE || t < first) first = t;
            seen++;
        }
        av_packet_unref(packet);
    }
    av_packet_free(&packet);
    avformat_close_input(&fmt);
    if (audio) {
        OWE_INFO("Loop plays on its own: it has an audio track");
        return 0.0;
    }
    if (first == AV_NOPTS_VALUE || llabs(first) > AV_TIME_BASE / 1000 || length <= 0) {
        OWE_INFO("Loop plays on its own: first frame at %.3f s, length %s",
                 first != AV_NOPTS_VALUE ? (double)first / AV_TIME_BASE : -1.0, length > 0 ? "known" : "unknown");
        return 0.0;
    }
    return length;
}

static char *loop_edl(const char *path, int *copies) {
    size_t len = strlen(path);
    char entry[32];
    int head = snprintf(entry, sizeof(entry), "%%%zu%%", len);
    size_t per = (size_t)head + len + 1;
    int n = LOOP_COPIES;
    char *edl;
    char *p;
    if ((size_t)n * per + 7 > LOOP_EDL_MAX) n = (int)((LOOP_EDL_MAX - 7) / per);
    if (n < 2) return NULL;
    edl = malloc((size_t)n * per + 8);
    if (!edl) return NULL;
    p = edl + sprintf(edl, "edl://");
    for (int i = 0; i < n; i++) {
        memcpy(p, entry, (size_t)head);
        p += head;
        memcpy(p, path, len);
        p += len;
        *p++ = i + 1 < n ? ';' : '\0';
    }
    *copies = n;
    return edl;
}

int owe_mpv_load(struct owe_mpv *m, const char *path, bool loop) {
    char *edl = NULL;
    int copies = 1;
    double length = 0.0;
    const char *cmd[] = { "loadfile", path, "replace", NULL };
    int rc;
    if (!m || !path || !*path) {
        return -1;
    }
    if (loop && (length = loop_copy_length(path)) > 0 && (edl = loop_edl(path, &copies))) {
        cmd[1] = edl;
    }
    mpv_set_property_string(m->handle, "loop-file", loop ? "inf" : "no");
    rc = mpv_command(m->handle, cmd);
    free(edl);
    if (rc < 0) {
        OWE_ERROR("loadfile failed: %s", mpv_error_string(rc));
        return -1;
    }
    snprintf(m->path, sizeof(m->path), "%s", path);
    if (copies > 1) OWE_INFO("Loop plays as %d copies of %.3f s", copies, length);
    m->repeats = copies;
    m->copy_length = copies > 1 ? length : 0.0;
    m->error[0] = '\0';
    m->has_video = true;
    /* Keep-open can change mpv's pause state at EOF. Reapply policy after each load. */
    m->pause_known = false;
    m->ready = false;
    m->file_loaded = false;
    m->has_frame = false;
    m->eof = false;
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
    m->repeats = 1;
    m->copy_length = 0.0;
    m->ready = false;
    m->file_loaded = false;
    m->has_frame = false;
    m->eof = false;
    m->error[0] = '\0';
}

void owe_mpv_set_paused(struct owe_mpv *m, bool paused) {
    int v;
    if (!m || (m->pause_known && m->paused == paused)) {
        return;
    }
    v = paused ? 1 : 0;
    if (mpv_set_property(m->handle, "pause", MPV_FORMAT_FLAG, &v) < 0) {
        m->pause_known = false;
        return;
    }
    m->paused = paused;
    m->pause_known = true;
    OWE_INFO("mpv %s", paused ? "paused" : "resumed");
}

void owe_mpv_set_muted(struct owe_mpv *m, bool muted) {
    int v;
    if (!m) {
        return;
    }
    v = muted ? 1 : 0;
    mpv_set_property(m->handle, "mute", MPV_FORMAT_FLAG, &v);
}

bool owe_mpv_eof(struct owe_mpv *m) {
    return m && m->eof;
}

bool owe_mpv_has_video(struct owe_mpv *m) {
    return m && m->has_video;
}

bool owe_mpv_is_paused(struct owe_mpv *m) {
    int paused = 0;
    return m && mpv_get_property(m->handle, "pause", MPV_FORMAT_FLAG, &paused) >= 0 && paused;
}

int owe_mpv_loop_copies(struct owe_mpv *m) {
    return m ? m->repeats : 0;
}

bool owe_mpv_ready(struct owe_mpv *m) {
    return m && m->ready;
}

const char *owe_mpv_path(struct owe_mpv *m) {
    return m ? m->path : "";
}

/* An unavailable size arrives as MPV_FORMAT_NONE and clears the value. The
 * core sends a size only when it changes, so a new file of the same size keeps
 * the cached one. */
static int property_size(const mpv_event_property *property) {
    int64_t value = property->format == MPV_FORMAT_INT64 && property->data ? *(int64_t *)property->data : 0;
    return value > 0 && value <= INT_MAX ? (int)value : 0;
}

int owe_mpv_video_size(struct owe_mpv *m, int *w, int *h) {
    if (w) *w = m ? m->video_w : 0;
    if (h) *h = m ? m->video_h : 0;
    return m && m->video_w > 0 && m->video_h > 0 ? 0 : -1;
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
        if (event->event_id == MPV_EVENT_LOG_MESSAGE) log_message(event->data);
        if (event->event_id == MPV_EVENT_START_FILE) {
            m->file_loaded = false;
            m->has_frame = false;
            m->ready = false;
            m->eof = false;
            m->has_video = true;
            m->error[0] = '\0';
        } else if (event->event_id == MPV_EVENT_FILE_LOADED) {
            m->file_loaded = true;
        } else if (event->event_id == MPV_EVENT_PROPERTY_CHANGE) {
            mpv_event_property *property = event->data;
            if (property && property->format == MPV_FORMAT_FLAG && property->data &&
                strcmp(property->name, "eof-reached") == 0) {
                m->eof = *(int *)property->data != 0;
            } else if (property && strcmp(property->name, "dwidth") == 0) {
                m->video_w = property_size(property);
            } else if (property && strcmp(property->name, "dheight") == 0) {
                m->video_h = property_size(property);
            } else if (property && strcmp(property->name, "hwdec-current") == 0) {
                snprintf(m->hwdec_current, sizeof(m->hwdec_current), "%s",
                         property->format == MPV_FORMAT_STRING && property->data
                             ? *(char **)property->data : "");
            }
        }
        if (event->event_id == MPV_EVENT_END_FILE) {
            mpv_event_end_file *end = event->data;
            if (end && end->reason == MPV_END_FILE_REASON_ERROR) {
                snprintf(m->error, sizeof(m->error), "%s", mpv_error_string(end->error));
                m->has_video = false;
                m->ready = false;
                OWE_ERROR("Video playback failed: %s", m->error);
            }
        }
    }
    if (!m->ctx) {
        return false;
    }
    owe_egl_make_current(owe_app_get()->egl);
    flags = mpv_render_context_update(m->ctx);
    if ((flags & MPV_RENDER_UPDATE_FRAME) && m->file_loaded) m->has_frame = true;
    return (flags & MPV_RENDER_UPDATE_FRAME) != 0;
}

bool owe_mpv_has_frame(struct owe_mpv *m) {
    return m && m->has_frame;
}

const char *owe_mpv_error(struct owe_mpv *m) { return m ? m->error : ""; }

const char *owe_mpv_hwdec(struct owe_mpv *m) {
    return m && *m->hwdec_current ? m->hwdec_current : "no";
}

static int render_target(struct owe_mpv *m, mpv_render_param *params) {
    int rc = mpv_render_context_render(m->ctx, params);
    if (rc < 0) {
        if (!*m->error) OWE_ERROR("mpv OpenGL render failed: %s", mpv_error_string(rc));
        snprintf(m->error, sizeof(m->error), "OpenGL render failed: %s", mpv_error_string(rc));
        m->ready = false;
    }
    return rc;
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
    if (render_target(m, params) < 0) {
        return;
    }
    int video_w, video_h;
    if (m->file_loaded && owe_mpv_video_size(m, &video_w, &video_h) == 0) m->ready = true;
}

/* Render one frame into an offscreen framebuffer for the lock feed. The feed
 * reads the pixels back with glReadPixels, so the rows are written top-down
 * and the lock can upload them as a plain QImage. */
int owe_mpv_render_fbo(struct owe_mpv *m, int fbo, int w, int h) {
    owe_app_t *app;
    mpv_opengl_fbo target;
    mpv_render_param params[4];
    int flip = 0;
    int block = 0;
    if (!m || !m->ctx || fbo <= 0 || w <= 0 || h <= 0) {
        return -1;
    }
    app = owe_app_get();
    if (!app || !app->egl) {
        return -1;
    }
    owe_egl_make_current(app->egl);
    target.fbo = fbo;
    target.w = w;
    target.h = h;
    target.internal_format = 0;
    params[0].type = MPV_RENDER_PARAM_OPENGL_FBO;
    params[0].data = &target;
    params[1].type = MPV_RENDER_PARAM_FLIP_Y;
    params[1].data = &flip;
    params[2].type = MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME;
    params[2].data = &block;
    params[3].type = MPV_RENDER_PARAM_INVALID;
    params[3].data = NULL;
    if (render_target(m, params) < 0) {
        return -1;
    }
    if (m->file_loaded) m->ready = true;
    return 0;
}

void owe_mpv_report_swap(struct owe_mpv *m) {
    if (m && m->ctx) {
        mpv_render_context_report_swap(m->ctx);
    }
}

/* Render offscreen for preparation or an outgoing video snapshot. */
static int render_frame(struct owe_mpv *m, int w, int h, FILE *snapshot) {
    GLuint texture = 0, fbo = 0;
    int rc = -1;
    owe_app_t *app = owe_app_get();
    if (!app || !m || !m->file_loaded || w <= 0 || h <= 0 || owe_egl_make_current(app->egl) != 0) return -1;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
        rc = owe_mpv_render_fbo(m, (int)fbo, w, h);
        if (rc == 0 && snapshot) {
            /* OpenGL ES guarantees only RGBA reads, so drop the alpha byte for the PPM. */
            size_t count = (size_t)w * (size_t)h;
            unsigned char *pixels = malloc(count * 4);
            rc = -1;
            if (pixels) {
                glBindFramebuffer(GL_FRAMEBUFFER, fbo);
                glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
                for (size_t i = 0; i < count; i++) memmove(pixels + i * 3, pixels + i * 4, 3);
                if (glGetError() == GL_NO_ERROR && fprintf(snapshot, "P6\n%d %d\n255\n", w, h) > 0 &&
                    fwrite(pixels, 1, count * 3, snapshot) == count * 3) rc = 0;
                free(pixels);
            }
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &texture);
    return rc;
}

int owe_mpv_prepare_frame(struct owe_mpv *m, int w, int h) {
    return render_frame(m, w, h, NULL);
}

int owe_mpv_snapshot(struct owe_mpv *m, const char *path, int w, int h) {
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    FILE *file;
    int rc;
    if (fd < 0) return -1;
    file = fdopen(fd, "wb");
    if (!file) {
        close(fd);
        unlink(path);
        return -1;
    }
    rc = render_frame(m, w, h, file);
    if (fclose(file) != 0) rc = -1;
    if (rc != 0) unlink(path);
    return rc;
}

/* A property read waits for the mpv core. Until the first frame is drawn the
 * core can wait for the render thread, and this read then stalls both for
 * 200 ms. No position exists before that frame. */
double owe_mpv_time_pos(struct owe_mpv *m) {
    double pos = -1.0;
    if (m && m->handle && m->ready) {
        mpv_get_property(m->handle, "time-pos", MPV_FORMAT_DOUBLE, &pos);
        /* A loop plays as copies of the file: report the position within
         * the copy on screen, as a file looped on its own would. */
        if (pos > 0 && m->copy_length > 0) {
            /* mpv sums the copies' starts, so a copy's first frame can sit a
             * hair below k copies and fold to almost a whole copy. */
            pos = fmod(pos, m->copy_length);
            if (m->copy_length - pos < 0.001) pos = 0.0;
        }
    }
    return pos;
}

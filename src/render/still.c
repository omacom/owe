#include "still.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

#include "egl.h"
#include "log.h"
#include "render.h"
#include "wayland.h"

struct still_job {
    pthread_t thread;
    int done_fd[2];
    char path[4096];
    int max_w;
    int max_h;
    uint8_t *rgba;
    int w;
    int h;
    int rc;
    int ready;
};

struct owe_still {
    struct owe_wayland *wl;
    struct owe_egl *egl;
    unsigned int tex;
    int tex_w;
    int tex_h;
    int max_w;
    int max_h;
    char path[4096];
    int fade_ms;
    struct timespec loaded_at;
    struct still_job *job;
};

static long elapsed_ms(const struct timespec *a, const struct timespec *b) {
    return (b->tv_sec - a->tv_sec) * 1000L + (b->tv_nsec - a->tv_nsec) / 1000000L;
}

struct owe_still *owe_still_new(struct owe_wayland *wl, struct owe_egl *egl) {
    struct owe_still *s = calloc(1, sizeof(*s));
    if (!s) {
        return NULL;
    }
    s->wl = wl;
    s->egl = egl;
    s->fade_ms = 250;
    return s;
}

void owe_still_free(struct owe_still *s) {
    if (!s) {
        return;
    }
    owe_still_unload(s);
    free(s);
}

static int decode_first_frame(const char *path, int max_w, int max_h, uint8_t **rgba_out,
                              int *w_out, int *h_out) {
    AVFormatContext *fmt = NULL;
    AVCodecContext *dec = NULL;
    const AVCodec *codec = NULL;
    AVFrame *frame = NULL;
    AVFrame *rgb = NULL;
    AVPacket *pkt = NULL;
    struct SwsContext *sws = NULL;
    int video_stream = -1;
    int rc = -1;
    unsigned int i;
    if (avformat_open_input(&fmt, path, NULL, NULL) < 0) {
        OWE_ERROR("avformat_open_input failed: %s", path);
        return -1;
    }
    if (avformat_find_stream_info(fmt, NULL) < 0) {
        avformat_close_input(&fmt);
        return -1;
    }
    for (i = 0; i < fmt->nb_streams; i++) {
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream = (int)i;
            break;
        }
    }
    if (video_stream < 0) {
        avformat_close_input(&fmt);
        return -1;
    }
    codec = avcodec_find_decoder(fmt->streams[video_stream]->codecpar->codec_id);
    if (!codec) {
        avformat_close_input(&fmt);
        return -1;
    }
    dec = avcodec_alloc_context3(codec);
    if (!dec) {
        avformat_close_input(&fmt);
        return -1;
    }
    if (avcodec_parameters_to_context(dec, fmt->streams[video_stream]->codecpar) < 0) {
        goto done;
    }
    dec->max_pixels = 64 * 1024 * 1024;
    dec->thread_count = 2;
    if (avcodec_open2(dec, codec, NULL) < 0) {
        goto done;
    }
    frame = av_frame_alloc();
    rgb = av_frame_alloc();
    pkt = av_packet_alloc();
    if (!frame || !rgb || !pkt) {
        goto done;
    }
    for (;;) {
        int read_rc = av_read_frame(fmt, pkt);
        int send_rc;
        if (read_rc < 0) {
            break;
        }
        if (pkt->stream_index != video_stream) {
            av_packet_unref(pkt);
            continue;
        }
        send_rc = avcodec_send_packet(dec, pkt);
        av_packet_unref(pkt);
        if (send_rc == AVERROR(EAGAIN)) {
            continue;
        }
        if (send_rc < 0) {
            break;
        }
        if (avcodec_receive_frame(dec, frame) == 0) {
            break;
        }
    }
    if (frame->width <= 0 || frame->height <= 0) {
        /* PNG, AVIF, and other delayed decoders hold the frame until the
         * decoder is flushed with a NULL packet. */
        if (avcodec_send_packet(dec, NULL) == 0) {
            avcodec_receive_frame(dec, frame);
        }
    }
    if (frame->width <= 0 || frame->height <= 0) {
        goto done;
    }
    {
        int tw = frame->width;
        int th = frame->height;
        if (max_w > 0 && max_h > 0 && (tw > max_w || th > max_h)) {
            /* Decode for the cover crop, not for the output bounds. The shader
             * crops the center to cover, so the visible region needs output
             * resolution even when one source dimension is larger than the
             * output. */
            double sx = (double)max_w / (double)tw;
            double sy = (double)max_h / (double)th;
            double s = sx > sy ? sx : sy;
            if (s > 1.0) {
                s = 1.0;
            }
            tw = (int)(tw * s);
            th = (int)(th * s);
            if (tw < 1) {
                tw = 1;
            }
            if (th < 1) {
                th = 1;
            }
        }
        sws = sws_getContext(frame->width, frame->height, frame->format, tw, th, AV_PIX_FMT_RGBA,
                             SWS_BILINEAR, NULL, NULL, NULL);
        if (!sws) {
            goto done;
        }
        rgb->format = AV_PIX_FMT_RGBA;
        rgb->width = tw;
        rgb->height = th;
    }
    if (av_frame_get_buffer(rgb, 0) < 0) {
        goto done;
    }
    sws_scale(sws, (const uint8_t *const *)frame->data, frame->linesize, 0, frame->height,
              rgb->data, rgb->linesize);
    {
        int row_bytes = rgb->width * 4;
        uint8_t *buf = malloc((size_t)row_bytes * (size_t)rgb->height);
        int y;
        if (!buf) {
            goto done;
        }
        /* FFmpeg pads each row to its own alignment. GL expects tightly packed
         * rows, so copy row by row instead of one bulk memcpy. */
        for (y = 0; y < rgb->height; y++) {
            memcpy(buf + (size_t)y * row_bytes, rgb->data[0] + (size_t)y * rgb->linesize[0],
                   (size_t)row_bytes);
        }
        *rgba_out = buf;
        *w_out = rgb->width;
        *h_out = rgb->height;
        rc = 0;
    }
done:
    if (sws) {
        sws_freeContext(sws);
    }
    if (pkt) {
        av_packet_free(&pkt);
    }
    if (frame) {
        av_frame_free(&frame);
    }
    if (rgb) {
        av_frame_free(&rgb);
    }
    if (dec) {
        avcodec_free_context(&dec);
    }
    if (fmt) {
        avformat_close_input(&fmt);
    }
    return rc;
}

static void *still_job_main(void *arg) {
    struct still_job *job = arg;
    job->rc = decode_first_frame(job->path, job->max_w, job->max_h, &job->rgba, &job->w, &job->h);
    {
        char c = 'x';
        ssize_t n = write(job->done_fd[1], &c, 1);
        (void)n;
    }
    return NULL;
}

static void still_job_release(struct owe_still *s) {
    if (!s->job) {
        return;
    }
    close(s->job->done_fd[0]);
    close(s->job->done_fd[1]);
    free(s->job->rgba);
    free(s->job);
    s->job = NULL;
}

static void still_job_join(struct owe_still *s) {
    if (!s->job) {
        return;
    }
    pthread_join(s->job->thread, NULL);
    still_job_release(s);
}

int owe_still_start(struct owe_still *s, const char *path, int max_w, int max_h) {
    struct still_job *job;
    if (!s || !path || !*path) {
        return -1;
    }
    if (s->job) {
        /* A decode cannot be interrupted, so wait for the finished frame
         * before replacing the request. The event loop only blocks for the
         * remainder of that one decode. */
        still_job_join(s);
    }
    job = calloc(1, sizeof(*job));
    if (!job) {
        return -1;
    }
    if (pipe2(job->done_fd, O_CLOEXEC | O_NONBLOCK) != 0) {
        free(job);
        return -1;
    }
    snprintf(job->path, sizeof(job->path), "%s", path);
    job->max_w = max_w;
    job->max_h = max_h;
    if (pthread_create(&job->thread, NULL, still_job_main, job) != 0) {
        close(job->done_fd[0]);
        close(job->done_fd[1]);
        free(job);
        return -1;
    }
    s->job = job;
    return 0;
}

int owe_still_fd(struct owe_still *s) {
    return s && s->job ? s->job->done_fd[0] : -1;
}

bool owe_still_busy(struct owe_still *s) {
    return s && s->job != NULL;
}

int owe_still_decoded_max(struct owe_still *s, int *w, int *h) {
    if (!s) {
        return -1;
    }
    if (w) {
        *w = s->max_w;
    }
    if (h) {
        *h = s->max_h;
    }
    return 0;
}

void owe_still_cancel(struct owe_still *s) {
    if (s) {
        still_job_join(s);
    }
}

int owe_still_poll(struct owe_still *s) {
    struct still_job *job;
    owe_output_t *outs;
    unsigned int tex;
    if (!s || !s->job) {
        return 0;
    }
    job = s->job;
    if (!job->ready) {
        char c;
        ssize_t n = read(job->done_fd[0], &c, 1);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                return 0;
            }
            OWE_ERROR("still job read failed");
            still_job_join(s);
            return -1;
        }
        pthread_join(job->thread, NULL);
        if (job->rc != 0 || !job->rgba) {
            OWE_ERROR("still decode failed: %s", job->path);
            still_job_release(s);
            return -1;
        }
        job->ready = 1;
    }
    outs = owe_wayland_outputs(s->wl);
    if (!outs || !outs->egl_surface) {
        /* Keep the decoded frame until an output can take the upload. */
        return 0;
    }
    if (owe_egl_prepare_output(s->egl, outs) != 0) {
        still_job_release(s);
        return -1;
    }
    tex = owe_egl_tex_from_rgba(s->egl, job->rgba, job->w, job->h);
    if (!tex) {
        OWE_ERROR("still texture upload failed");
        still_job_release(s);
        return -1;
    }
    if (s->tex) {
        owe_egl_tex_free(s->egl, s->tex);
    }
    s->tex = tex;
    s->tex_w = job->w;
    s->tex_h = job->h;
    s->max_w = job->max_w;
    s->max_h = job->max_h;
    snprintf(s->path, sizeof(s->path), "%s", job->path);
    clock_gettime(CLOCK_MONOTONIC, &s->loaded_at);
    OWE_INFO("still loaded %s (%dx%d)", s->path, s->tex_w, s->tex_h);
    still_job_release(s);
    owe_app_request_render();
    return 1;
}

void owe_still_unload(struct owe_still *s) {
    if (!s) {
        return;
    }
    owe_still_cancel(s);
    if (s->tex && s->egl) {
        if (owe_egl_make_current(s->egl) == 0) owe_egl_tex_free(s->egl, s->tex);
    }
    s->tex = 0;
    s->path[0] = '\0';
}

bool owe_still_has_image(struct owe_still *s) {
    return s && s->tex != 0;
}

const char *owe_still_path(struct owe_still *s) {
    return s ? s->path : "";
}

void owe_still_render_output(struct owe_still *s, struct owe_output *out) {
    struct timespec now;
    float alpha = 1.0f;
    if (!s || !s->tex || !out) {
        return;
    }
    if (s->fade_ms > 0) {
        long ms;
        clock_gettime(CLOCK_MONOTONIC, &now);
        ms = elapsed_ms(&s->loaded_at, &now);
        if (ms < s->fade_ms) {
            alpha = (float)ms / (float)s->fade_ms;
        }
    }
    owe_egl_draw_texture(s->egl, out, s->tex, alpha, s->tex_w, s->tex_h);
}

void owe_still_set_fade_ms(struct owe_still *s, int ms) {
    if (s) {
        s->fade_ms = ms;
    }
}

bool owe_still_needs_frames(struct owe_still *s) {
    struct timespec now;
    if (!s || !s->tex || s->fade_ms <= 0) {
        return false;
    }
    clock_gettime(CLOCK_MONOTONIC, &now);
    return elapsed_ms(&s->loaded_at, &now) < s->fade_ms;
}

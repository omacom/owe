#include "still.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

#include "egl.h"
#include "log.h"
#include "render.h"
#include "wayland.h"

struct owe_still {
    struct owe_wayland *wl;
    struct owe_egl *egl;
    unsigned int tex;
    int tex_w;
    int tex_h;
    char path[4096];
    int fade_ms;
    struct timespec loaded_at;
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
    if (avcodec_open2(dec, codec, NULL) < 0) {
        goto done;
    }
    frame = av_frame_alloc();
    rgb = av_frame_alloc();
    pkt = av_packet_alloc();
    if (!frame || !rgb || !pkt) {
        goto done;
    }
    while (av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index != video_stream) {
            av_packet_unref(pkt);
            continue;
        }
        if (avcodec_send_packet(dec, pkt) < 0) {
            av_packet_unref(pkt);
            break;
        }
        av_packet_unref(pkt);
        if (avcodec_receive_frame(dec, frame) == 0) {
            break;
        }
    }
    if (!frame->width) {
        goto done;
    }
    {
        int tw = frame->width;
        int th = frame->height;
        if (max_w > 0 && max_h > 0 && (tw > max_w || th > max_h)) {
            double sx = (double)max_w / (double)tw;
            double sy = (double)max_h / (double)th;
            double s = sx < sy ? sx : sy;
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
    (void)0;
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

int owe_still_load(struct owe_still *s, const char *path) {
    uint8_t *rgba = NULL;
    int w = 0;
    int h = 0;
    unsigned int tex;
    owe_output_t *outs;
    if (!s || !path || !*path) {
        return -1;
    }
    {
        int max_w = 0;
        int max_h = 0;
        owe_wayland_outputs_max_size(s->wl, &max_w, &max_h);
        if (decode_first_frame(path, max_w > 0 ? max_w : 4096, max_h > 0 ? max_h : 4096, &rgba, &w,
                               &h) != 0 ||
            !rgba) {
            OWE_ERROR("still decode failed: %s", path);
            return -1;
        }
    }
    outs = owe_wayland_outputs(s->wl);
    if (!outs || !outs->egl_surface) {
        free(rgba);
        OWE_ERROR("no output for still upload");
        return -1;
    }
    if (owe_egl_prepare_output(s->egl, outs) != 0) {
        free(rgba);
        return -1;
    }
    tex = owe_egl_tex_from_rgba(s->egl, rgba, w, h);
    free(rgba);
    if (!tex) {
        OWE_ERROR("still texture upload failed");
        return -1;
    }
    if (s->tex) {
        owe_egl_tex_free(s->egl, s->tex);
    }
    s->tex = tex;
    s->tex_w = w;
    s->tex_h = h;
    snprintf(s->path, sizeof(s->path), "%s", path);
    clock_gettime(CLOCK_MONOTONIC, &s->loaded_at);
    owe_app_request_render();
    OWE_INFO("still loaded %s (%dx%d)", path, w, h);
    return 0;
}

void owe_still_unload(struct owe_still *s) {
    if (!s) {
        return;
    }
    if (s->tex && s->egl) {
        owe_output_t *outs = s->wl ? owe_wayland_outputs(s->wl) : NULL;
        if (outs && outs->egl_surface) {
            if (owe_egl_prepare_output(s->egl, outs) == 0) {
                owe_egl_tex_free(s->egl, s->tex);
            }
        }
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
            owe_app_request_render();
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

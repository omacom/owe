#include "scene_frame.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <epoxy/gl.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>

#include "egl.h"
#include "log.h"
#include "scene.h"
#include "scene_desc.h"
#include "still.h"
#include "wayland.h"

static void usage(void) {
    fprintf(stderr,
            "Usage: owe-render --scene-frame <scene.jsonc> --out <file or directory> [options]\n"
            "Draws moments of a scene to picture files, with no window.\n"
            "  --part idle|arrive  which part of the scene (default idle)\n"
            "  --at SECONDS        how far into that part (default 0)\n"
            "  --frames N          how many frames, one after another (default 1)\n"
            "  --fps N             frames a second between them (default: the part's own)\n"
            "  --size WxH          the size of the picture (default 1920x1080)\n"
            "  --still PATH        the still the scene belongs to (default: found from the scene)\n"
            "  --settling 0..1     as a paused scene: how far it has settled into its still\n"
            "With one frame, --out is the file, .png or .ppm. With more, it is a directory,\n"
            "and the frames are 0000.png, 0001.png and so on; --ppm writes .ppm instead.\n");
}

static int write_ppm(const char *path, const uint8_t *rgba, int w, int h) {
    FILE *f = fopen(path, "wb");
    int i;
    if (!f) return -1;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (i = 0; i < w * h; i++) fwrite(rgba + (size_t)i * 4, 1, 3, f);
    return fclose(f);
}

static int write_png(const char *path, const uint8_t *rgba, int w, int h) {
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_PNG);
    AVCodecContext *enc = codec ? avcodec_alloc_context3(codec) : NULL;
    AVFrame *frame = av_frame_alloc();
    AVPacket *packet = av_packet_alloc();
    FILE *f = NULL;
    int rc = -1, y;
    if (!enc || !frame || !packet) goto done;
    enc->width = w;
    enc->height = h;
    enc->pix_fmt = AV_PIX_FMT_RGB24;
    enc->time_base = (AVRational){ 1, 25 };
    if (avcodec_open2(enc, codec, NULL) < 0) goto done;
    frame->format = AV_PIX_FMT_RGB24;
    frame->width = w;
    frame->height = h;
    if (av_frame_get_buffer(frame, 0) < 0) goto done;
    for (y = 0; y < h; y++) {
        uint8_t *row = frame->data[0] + (size_t)y * frame->linesize[0];
        int x;
        for (x = 0; x < w; x++) memcpy(row + x * 3, rgba + ((size_t)y * w + x) * 4, 3);
    }
    if (avcodec_send_frame(enc, frame) < 0 || avcodec_receive_packet(enc, packet) < 0) goto done;
    f = fopen(path, "wb");
    if (f && fwrite(packet->data, 1, (size_t)packet->size, f) == (size_t)packet->size) rc = 0;
done:
    if (f && fclose(f) != 0) rc = -1;
    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&enc);
    return rc;
}

static bool ends_with(const char *text, const char *end) {
    size_t n = strlen(text), m = strlen(end);
    return n >= m && strcmp(text + n - m, end) == 0;
}

/* A picture of the scene as a texture, or 0. */
static unsigned int texture(struct owe_egl *egl, const char *path, int max_w, int max_h, int *w, int *h) {
    uint8_t *rgba = NULL;
    int pw = 0, ph = 0;
    unsigned int tex;
    if (!*path) return 0;
    if (owe_still_decode(path, max_w, max_h, &rgba, &pw, &ph) != 0 || !rgba) {
        fprintf(stderr, "owe-render: cannot read %s\n", path);
        return 0;
    }
    tex = owe_egl_tex_from_rgba(egl, rgba, pw, ph);
    free(rgba);
    if (w) *w = pw;
    if (h) *h = ph;
    return tex;
}

int owe_scene_frame_main(int argc, char **argv) {
    /* Too large for the stack. */
    static struct owe_scene_desc desc;
    struct owe_scene_look look = { 0 };
    owe_output_t out = { 0 };
    struct owe_egl *egl;
    const char *scene = NULL, *target = NULL, *still = NULL, *part = "idle";
    char found[OWE_SCENE_PATH], error[512] = "", file[OWE_SCENE_PATH + 32];
    double at = 0.0, settling = 0.0;
    int frames = 1, fps = 0, width = 1920, height = 1080, i, k, status = 1;
    bool ppm = false, arriving;
    uint8_t *pixels = NULL, *upright = NULL;
    void *surface = NULL;

    for (i = 2; i < argc; i++) {
        const char *value = i + 1 < argc ? argv[i + 1] : NULL;
        if (strcmp(argv[i], "--ppm") == 0) {
            ppm = true;
        } else if (argv[i][0] != '-' && !scene) {
            scene = argv[i];
        } else if (!value) {
            usage();
            return 2;
        } else if (strcmp(argv[i], "--out") == 0) {
            target = argv[++i];
        } else if (strcmp(argv[i], "--part") == 0) {
            part = argv[++i];
        } else if (strcmp(argv[i], "--at") == 0) {
            at = atof(argv[++i]);
        } else if (strcmp(argv[i], "--frames") == 0) {
            frames = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--fps") == 0) {
            fps = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--still") == 0) {
            still = argv[++i];
        } else if (strcmp(argv[i], "--settling") == 0) {
            settling = atof(argv[++i]);
        } else if (strcmp(argv[i], "--size") == 0) {
            if (sscanf(argv[++i], "%dx%d", &width, &height) != 2) {
                usage();
                return 2;
            }
        } else {
            usage();
            return 2;
        }
    }
    arriving = strcmp(part, "arrive") == 0;
    if (!scene || !target || (!arriving && strcmp(part, "idle") != 0) || frames < 1 || frames > 100000 || width < 1 ||
        height < 1 || width > 16384 || height > 16384 || at < 0 || fps < 0 || fps > 240) {
        usage();
        return 2;
    }
    owe_log_init("owe-render", OWE_LOG_WARN);
    if (owe_scene_desc_read(scene, &desc, error, sizeof(error)) != 0) {
        fprintf(stderr, "owe-render: %s\n", error);
        return 1;
    }
    if (arriving && desc.arrive_seconds <= 0) {
        fprintf(stderr, "owe-render: the scene has no arrive part\n");
        return 1;
    }
    if (!still) {
        if (owe_scene_find_still(&desc, scene, found, sizeof(found)) != 0) {
            fprintf(stderr, "owe-render: the scene's still, %s, was not found; name it with --still\n", desc.still);
            return 1;
        }
        still = found;
    }
    if (owe_scene_check_still(&desc, still, error, sizeof(error)) != 0) {
        fprintf(stderr, "owe-render: %s\n", error);
        return 1;
    }
    if (!fps) fps = arriving ? desc.arrive_fps : desc.idle_fps;

    egl = owe_egl_new_offscreen(width, height, &surface);
    if (!egl) return 1;
    out.egl_surface = surface;
    out.width = width;
    out.height = height;
    out.scale = 1;
    if (owe_egl_prepare_output(egl, &out) != 0) goto done;

    look.still = texture(egl, still, width, height, &look.still_w, &look.still_h);
    if (!look.still) goto done;
    for (i = 0; i < desc.piece_count; i++) {
        const struct owe_scene_piece *piece = &desc.pieces[i];
        if (!(arriving ? piece->arrive.listed : piece->idle.listed)) continue;
        look.pictures[i] = texture(egl, piece->picture, 16384, 16384, NULL, NULL);
        if (!look.pictures[i]) goto done;
        if (*piece->behind && !(look.behinds[i] = texture(egl, piece->behind, 16384, 16384, NULL, NULL))) goto done;
        if (piece->inside >= 0 && !look.masks[piece->inside]) {
            look.masks[piece->inside] = texture(egl, desc.areas[piece->inside].mask, 16384, 16384, NULL, NULL);
            if (!look.masks[piece->inside]) goto done;
        }
    }

    if (frames > 1 && mkdir(target, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "owe-render: cannot make the directory %s\n", target);
        goto done;
    }
    pixels = malloc((size_t)width * height * 4);
    upright = malloc((size_t)width * height * 4);
    if (!pixels || !upright) goto done;
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    for (k = 0; k < frames; k++) {
        int y, rc;
        owe_scene_draw(egl, &out, &desc, &look, arriving, at + (double)k / fps, settling);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        /* What was read starts at the bottom row. */
        for (y = 0; y < height; y++) {
            memcpy(upright + (size_t)y * width * 4, pixels + (size_t)(height - 1 - y) * width * 4, (size_t)width * 4);
        }
        if (frames > 1) snprintf(file, sizeof(file), "%s/%04d.%s", target, k, ppm ? "ppm" : "png");
        else snprintf(file, sizeof(file), "%s", target);
        rc = ppm || ends_with(file, ".ppm") ? write_ppm(file, upright, width, height) : write_png(file, upright, width, height);
        if (rc != 0) {
            fprintf(stderr, "owe-render: cannot write %s\n", file);
            goto done;
        }
    }
    if (glGetError() == GL_NO_ERROR) status = 0;
done:
    free(pixels);
    free(upright);
    out.egl_surface = NULL;
    owe_egl_free(egl);
    return status;
}

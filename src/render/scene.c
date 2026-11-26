#include "scene.h"

#include <fcntl.h>
#include <libgen.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include <libavutil/mem.h>
#include <libavutil/sha.h>

#include "egl.h"
#include "log.h"
#include "scene_desc.h"
#include "still.h"
#include "wayland.h"

/* Seconds a scene takes to settle into the still: at the end of its arrive
 * part, and when it is paused. */
#define SETTLE 0.4
/* The largest still a scene's checksum is worked out for. */
#define STILL_MAX_BYTES (256 * 1024 * 1024)

enum phase {
    PHASE_NONE = 0,
    PHASE_LOADING,
    PHASE_ARRIVE,
    PHASE_IDLE,
    PHASE_SETTLING, /* still moving as before, fading into the still */
    PHASE_RESTING,  /* the still, and nothing drawn after it */
    PHASE_ENDED,    /* played once as an intro: its last frame, the still, stays */
};

struct owe_scene {
    struct owe_wayland *wl;
    struct owe_egl *egl;
    /* Too large to copy about; read once per load. */
    struct owe_scene_desc *desc;
    struct owe_still *still;
    struct owe_still *pictures[OWE_SCENE_MAX_PIECES];
    struct owe_still *behinds[OWE_SCENE_MAX_PIECES];
    struct owe_still *masks[OWE_SCENE_MAX_AREAS];
    enum phase phase;
    /* The part that plays, or that was playing when it began to settle. */
    bool arriving;
    bool want_arrive;
    bool want_paused;
    /* Played once, as an intro: it ends after its arrive part, and a pause
     * holds it where it is instead of settling it. */
    bool once;
    bool held;
    struct timespec held_at;
    /* The scene's directory, held open while its pictures load; -1 after. */
    int dir_fd;
    struct timespec part_start;
    struct timespec settle_start;
    /* The moment the frame being drawn shows, the same on every output. */
    struct timespec frame_at;
    int timer_fd;
    int timer_fps;
    bool frame_due;
    unsigned long frames;
    char error[256];
};

static double seconds_between(const struct timespec *a, const struct timespec *b) {
    return (double)(b->tv_sec - a->tv_sec) + (double)(b->tv_nsec - a->tv_nsec) / 1e9;
}

static double smooth(double part) {
    if (part <= 0.0) return 0.0;
    if (part >= 1.0) return 1.0;
    return part * part * (3.0 - 2.0 * part);
}

static void set_timer(struct owe_scene *s, int fps) {
    struct itimerspec every = { 0 };
    if (s->timer_fd < 0 || fps == s->timer_fps) return;
    if (fps > 0) {
        every.it_interval.tv_nsec = 1000000000L / fps;
        every.it_value.tv_nsec = 1000000000L / fps;
    }
    timerfd_settime(s->timer_fd, 0, &every, NULL);
    s->timer_fps = fps;
}

struct owe_scene *owe_scene_new(struct owe_wayland *wl, struct owe_egl *egl) {
    struct owe_scene *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->wl = wl;
    s->egl = egl;
    s->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    s->dir_fd = -1;
    return s;
}

static void close_dir(struct owe_scene *s) {
    if (s->dir_fd >= 0) close(s->dir_fd);
    s->dir_fd = -1;
}

void owe_scene_unload(struct owe_scene *s) {
    int i;
    if (!s) return;
    owe_still_free(s->still);
    s->still = NULL;
    for (i = 0; i < OWE_SCENE_MAX_PIECES; i++) {
        owe_still_free(s->pictures[i]);
        owe_still_free(s->behinds[i]);
        s->pictures[i] = NULL;
        s->behinds[i] = NULL;
    }
    for (i = 0; i < OWE_SCENE_MAX_AREAS; i++) {
        owe_still_free(s->masks[i]);
        s->masks[i] = NULL;
    }
    free(s->desc);
    s->desc = NULL;
    close_dir(s);
    s->phase = PHASE_NONE;
    s->frames = 0;
    s->frame_due = false;
    s->error[0] = '\0';
    set_timer(s, 0);
}

void owe_scene_free(struct owe_scene *s) {
    if (!s) return;
    owe_scene_unload(s);
    if (s->timer_fd >= 0) close(s->timer_fd);
    free(s);
}

/* The scene was cut from one picture. Another picture under the same name
 * would show its pieces in the wrong places, so the scene refuses it. */
int owe_scene_check_still(const struct owe_scene_desc *desc, const char *path, char *error, size_t len) {
    struct AVSHA *sha;
    uint8_t digest[32], block[65536];
    char hex[65];
    struct stat st;
    FILE *f;
    size_t n;
    int i;
    if (!*desc->sha256) return 0;
    if (stat(path, &st) != 0 || st.st_size > STILL_MAX_BYTES || !(f = fopen(path, "rb"))) {
        snprintf(error, len, "the scene's still cannot be read: %s", path);
        return -1;
    }
    sha = av_sha_alloc();
    if (!sha) {
        fclose(f);
        return -1;
    }
    av_sha_init(sha, 256);
    while ((n = fread(block, 1, sizeof(block), f)) > 0) av_sha_update(sha, block, n);
    fclose(f);
    av_sha_final(sha, digest);
    av_free(sha);
    for (i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", digest[i]);
    if (strcmp(hex, desc->sha256) != 0) {
        snprintf(error, len, "%s is not the picture the scene was made for", path);
        return -1;
    }
    return 0;
}

/* A scene names its still the way the theme repositories do, <theme>/<name>.
 * Beside a theme's backgrounds the scene sits at scenes/<stem>/, so the still
 * is two directories up; failing that it is where Omarchy installs it. */
int owe_scene_find_still(const struct owe_scene_desc *desc, const char *scene_path, char *out, size_t len) {
    char copy[OWE_SCENE_PATH];
    const char *slash = strrchr(desc->still, '/');
    const char *name = slash ? slash + 1 : desc->still;
    struct stat st;
    snprintf(copy, sizeof(copy), "%s", scene_path);
    if (snprintf(out, len, "%s/../../%s", dirname(copy), name) < (int)len && stat(out, &st) == 0 && S_ISREG(st.st_mode)) {
        return 0;
    }
    if (slash && snprintf(out, len, "/usr/share/omarchy/themes/%.*s/backgrounds/%s", (int)(slash - desc->still),
                          desc->still, name) < (int)len &&
        stat(out, &st) == 0 && S_ISREG(st.st_mode)) {
        return 0;
    }
    return -1;
}

static struct owe_still *start(struct owe_scene *s, const char *path, int max_w, int max_h) {
    struct owe_still *picture = owe_still_new(s->wl, s->egl);
    if (!picture || owe_still_start(picture, path, max_w, max_h) != 0) {
        owe_still_free(picture);
        return NULL;
    }
    return picture;
}

int owe_scene_load(struct owe_scene *s, const char *path, const char *still, bool arrive, bool once, int max_w,
                   int max_h, char *error, size_t error_len) {
    struct owe_scene_desc *desc;
    char found[OWE_SCENE_PATH], copy[OWE_SCENE_PATH], name[OWE_SCENE_PATH], via[OWE_SCENE_PATH];
    bool used[OWE_SCENE_MAX_AREAS] = { false };
    int i, dir_fd;
    if (!s || !path) return -1;
    desc = calloc(1, sizeof(*desc));
    if (!desc) return -1;
    /* A theme being applied is staged in a directory that is renamed into
     * place, and its scene may be asked for just before. Reading through the
     * opened directory finds its pictures wherever it has moved to. */
    snprintf(copy, sizeof(copy), "%s", path);
    snprintf(name, sizeof(name), "%s", basename(copy));
    snprintf(copy, sizeof(copy), "%s", path);
    dir_fd = open(dirname(copy), O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (dir_fd < 0 || snprintf(via, sizeof(via), "/proc/self/fd/%d/%s", dir_fd, name) >= (int)sizeof(via)) {
        snprintf(error, error_len, "%s: its directory cannot be opened", path);
        if (dir_fd >= 0) close(dir_fd);
        free(desc);
        return -1;
    }
    if (owe_scene_desc_read(via, desc, error, error_len) != 0) {
        /* Name the file as it was asked for. */
        if (strncmp(error, via, strlen(via)) == 0) {
            char rest[512];
            snprintf(rest, sizeof(rest), "%s", error + strlen(via));
            snprintf(error, error_len, "%s%s", path, rest);
        }
        goto fail;
    }
    if (!still) {
        if (owe_scene_find_still(desc, via, found, sizeof(found)) != 0) {
            snprintf(error, error_len, "the scene's still, %s, was not found", desc->still);
            goto fail;
        }
        still = found;
    }
    if (owe_scene_check_still(desc, still, error, error_len) != 0) goto fail;
    if (once && desc->arrive_seconds <= 0) {
        snprintf(error, error_len, "the scene has no arrive part to play as an intro");
        goto fail;
    }
    owe_scene_unload(s);
    s->desc = desc;
    s->dir_fd = dir_fd;
    s->want_arrive = (arrive || once) && desc->arrive_seconds > 0;
    s->once = once;
    s->held = false;
    s->still = start(s, still, max_w, max_h);
    if (!s->still) {
        snprintf(error, error_len, "the scene's still cannot be loaded");
        owe_scene_unload(s);
        return -1;
    }
    /* Only what the parts that will play move is loaded. A piece's picture is
     * loaded at its own size: it is small, and is drawn at any size. */
    for (i = 0; i < desc->piece_count; i++) {
        struct owe_scene_piece *piece = &desc->pieces[i];
        if (!piece->idle.listed && !(s->want_arrive && piece->arrive.listed)) continue;
        s->pictures[i] = start(s, piece->picture, 16384, 16384);
        if (*piece->behind) s->behinds[i] = start(s, piece->behind, 16384, 16384);
        if (!s->pictures[i] || (*piece->behind && !s->behinds[i])) {
            snprintf(error, error_len, "a picture of piece \"%s\" cannot be loaded", piece->name);
            owe_scene_unload(s);
            return -1;
        }
        if (piece->inside >= 0) used[piece->inside] = true;
    }
    for (i = 0; i < desc->area_count; i++) {
        if (!used[i]) continue;
        s->masks[i] = start(s, desc->areas[i].mask, 16384, 16384);
        if (!s->masks[i]) {
            snprintf(error, error_len, "the shape of area \"%s\" cannot be loaded", desc->areas[i].name);
            owe_scene_unload(s);
            return -1;
        }
    }
    s->phase = PHASE_LOADING;
    /* Pictures finish loading on worker threads; the timer brings the loop
     * round to pick them up. */
    set_timer(s, 30);
    OWE_INFO("scene %s, %d pieces, idle at %d fps%s", path, desc->piece_count, desc->idle_fps,
             s->want_arrive ? ", arriving" : "");
    return 0;
fail:
    close(dir_fd);
    free(desc);
    return -1;
}

bool owe_scene_loaded(struct owe_scene *s) {
    return s && s->phase != PHASE_NONE;
}

bool owe_scene_ready(struct owe_scene *s) {
    return s && s->phase > PHASE_LOADING;
}

bool owe_scene_shown(struct owe_scene *s) {
    return s && s->frames > 0;
}

const char *owe_scene_error(struct owe_scene *s) {
    return s ? s->error : "";
}

int owe_scene_fd(struct owe_scene *s) {
    return s && s->timer_fps > 0 ? s->timer_fd : -1;
}

bool owe_scene_needs_frames(struct owe_scene *s) {
    return s && !s->held && (s->phase == PHASE_ARRIVE || s->phase == PHASE_IDLE || s->phase == PHASE_SETTLING);
}

bool owe_scene_ended(struct owe_scene *s) {
    return s && s->phase == PHASE_ENDED;
}

bool owe_scene_resting(struct owe_scene *s) {
    /* An intro that has ended is its still too, and draws nothing more. */
    return s && (s->phase == PHASE_RESTING || s->phase == PHASE_ENDED);
}

void owe_scene_rewind(struct owe_scene *s) {
    if (!s || !s->once || s->phase != PHASE_ARRIVE) return;
    clock_gettime(CLOCK_MONOTONIC, &s->part_start);
    s->held_at = s->part_start;
    s->frame_at = s->part_start;
}

double owe_scene_time(struct owe_scene *s) {
    struct timespec now;
    if (!s || (s->phase != PHASE_ARRIVE && s->phase != PHASE_IDLE)) return 0.0;
    if (s->held) return seconds_between(&s->part_start, &s->held_at);
    clock_gettime(CLOCK_MONOTONIC, &now);
    return seconds_between(&s->part_start, &now);
}

/* Returns 1 once the picture is loaded, 0 while it loads, -1 if it failed. */
static int loaded(struct owe_still *picture) {
    if (!picture || owe_still_has_image(picture)) return 1;
    if (owe_still_busy(picture) && owe_still_poll(picture) < 0) return -1;
    if (owe_still_has_image(picture)) return 1;
    return owe_still_busy(picture) ? 0 : -1;
}

static void begin(struct owe_scene *s, enum phase phase, const struct timespec *now) {
    s->phase = phase;
    s->arriving = phase == PHASE_ARRIVE;
    s->part_start = *now;
    set_timer(s, phase == PHASE_ARRIVE ? s->desc->arrive_fps : s->desc->idle_fps);
}

static void rest(struct owe_scene *s) {
    s->phase = PHASE_RESTING;
    set_timer(s, 0);
}

bool owe_scene_tick(struct owe_scene *s) {
    struct timespec now;
    uint64_t fired;
    bool due;
    int i;
    if (!s || s->phase == PHASE_NONE) return false;
    /* Only the timer starts a frame, so a scene draws at its own rate and not
     * at the display's. */
    due = s->timer_fps > 0 && read(s->timer_fd, &fired, sizeof(fired)) == (ssize_t)sizeof(fired);
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (s->phase == PHASE_LOADING) {
        int all = loaded(s->still);
        for (i = 0; all > 0 && i < OWE_SCENE_MAX_PIECES; i++) {
            all = loaded(s->pictures[i]);
            if (all > 0) all = loaded(s->behinds[i]);
        }
        for (i = 0; all > 0 && i < OWE_SCENE_MAX_AREAS; i++) all = loaded(s->masks[i]);
        if (all < 0) {
            close_dir(s);
            snprintf(s->error, sizeof(s->error), "a picture of the scene could not be read");
            OWE_ERROR("scene: %s", s->error);
            s->phase = PHASE_NONE;
            set_timer(s, 0);
            return false;
        }
        if (all == 0) return false;
        close_dir(s);
        if (s->once) {
            /* An intro loaded paused waits on its first frame. */
            begin(s, PHASE_ARRIVE, &now);
            if (s->want_paused) {
                s->held = true;
                s->held_at = now;
                set_timer(s, 0);
            }
        } else if (s->want_paused) {
            rest(s);
        } else {
            begin(s, s->want_arrive ? PHASE_ARRIVE : PHASE_IDLE, &now);
        }
        due = true;
    } else if (s->phase == PHASE_ARRIVE && !s->held && seconds_between(&s->part_start, &now) >= s->desc->arrive_seconds) {
        if (s->once) {
            /* An intro ends on its last frame, which is the still. */
            s->phase = PHASE_ENDED;
            set_timer(s, 0);
        } else {
            /* The arrive part ended at rest, which is where the idle part starts. */
            begin(s, PHASE_IDLE, &now);
        }
        due = true;
    } else if (s->phase == PHASE_SETTLING && seconds_between(&s->settle_start, &now) >= SETTLE) {
        rest(s);
        due = true;
    }
    if (due) {
        s->frame_at = now;
        s->frame_due = true;
    }
    return due;
}

void owe_scene_set_paused(struct owe_scene *s, bool paused) {
    struct timespec now;
    if (!s) return;
    s->want_paused = paused;
    if (s->phase <= PHASE_LOADING) return;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (s->once) {
        /* An intro is held where it is and goes on from there. */
        if (paused && s->phase == PHASE_ARRIVE && !s->held) {
            s->held = true;
            s->held_at = now;
            set_timer(s, 0);
        } else if (!paused && s->held) {
            double waited = seconds_between(&s->held_at, &now);
            s->part_start.tv_sec += (time_t)waited;
            s->part_start.tv_nsec += (long)((waited - (double)(time_t)waited) * 1e9);
            if (s->part_start.tv_nsec >= 1000000000L) {
                s->part_start.tv_sec++;
                s->part_start.tv_nsec -= 1000000000L;
            }
            s->held = false;
            s->frame_at = now;
            set_timer(s, s->desc->arrive_fps);
        }
        return;
    }
    if (paused && (s->phase == PHASE_ARRIVE || s->phase == PHASE_IDLE)) {
        s->phase = PHASE_SETTLING;
        s->settle_start = now;
    } else if (!paused && (s->phase == PHASE_RESTING || s->phase == PHASE_SETTLING)) {
        /* From rest, where the idle part starts. A scene caught settling was
         * not at rest yet, and comes back with a small step. */
        begin(s, PHASE_IDLE, &now);
        s->frame_at = now;
    }
}

void owe_scene_draw(struct owe_egl *egl, struct owe_output *out, const struct owe_scene_desc *desc,
                    const struct owe_scene_look *look, bool arriving, double t, double settling) {
    double into_still = settling;
    int w = look->still_w, h = look->still_h, i;
    float tall;
    if (!look->still || w <= 0 || h <= 0) return;
    owe_egl_draw_texture(egl, out, look->still, 1.0f, w, h);
    if (into_still >= 1.0) return;
    if (t < 0.0) t = 0.0;
    /* The arrive part's last moments settle into the still itself, so nothing
     * shifts when the idle part takes over: a piece drawn in its own colour
     * is never the still to the last pixel. */
    if (arriving && desc->arrive_seconds - t < SETTLE) {
        double ending = smooth(1.0 - (desc->arrive_seconds - t) / SETTLE);
        if (ending > into_still) into_still = ending;
    }
    /* Distances in a scene are shares of the still's height. */
    tall = (float)h / (float)w;
    /* Every patch first, then every piece: pieces that sit close together reach
     * into each other's room, and a patch drawn late would cover a neighbour.
     * A piece that sits on another needs no patch while that other is drawn:
     * what is behind it is then the other piece, not the still. */
    for (i = 0; i < desc->piece_count; i++) {
        const struct owe_scene_piece *piece = &desc->pieces[i];
        const struct owe_scene_part *part = arriving ? &piece->arrive : &piece->idle;
        struct owe_piece_draw patch = { .still_w = w, .still_h = h, .at = piece->at, .size = 1.0f, .fade = 1.0f };
        if (!part->listed || !look->behinds[i]) continue;
        if (piece->on >= 0 && (arriving ? desc->pieces[piece->on].arrive.listed : desc->pieces[piece->on].idle.listed)) {
            continue;
        }
        patch.shape = look->behinds[i];
        owe_egl_draw_piece(egl, out, &patch);
    }
    for (i = 0; i < desc->piece_count; i++) {
        const struct owe_scene_piece *piece = &desc->pieces[i];
        const struct owe_scene_part *part = arriving ? &piece->arrive : &piece->idle;
        struct owe_piece_draw draw = { .still_w = w, .still_h = h, .at = piece->at };
        if (!part->listed || !look->pictures[i]) continue;
        draw.shape = look->pictures[i];
        if (look->behinds[i] && !piece->own_colour) {
            draw.still = look->still;
            draw.behind = look->behinds[i];
        }
        if (piece->inside >= 0 && look->masks[piece->inside]) {
            draw.inside = look->masks[piece->inside];
            draw.inside_at = desc->areas[piece->inside].at;
        }
        draw.dx = (piece->rest_x + owe_scene_offset(&part->x, t)) * tall;
        draw.dy = piece->rest_y + owe_scene_offset(&part->y, t);
        draw.size = owe_scene_level(&part->size, t);
        draw.fade = owe_scene_level(&part->fade, t);
        owe_egl_draw_piece(egl, out, &draw);
    }
    if (into_still > 0.0) owe_egl_draw_texture_overlay(egl, out, look->still, (float)into_still, w, h);
}

void owe_scene_render_output(struct owe_scene *s, struct owe_output *out) {
    struct owe_scene_look look = { 0 };
    double settling = 0.0;
    int i;
    if (!s || !out || s->phase <= PHASE_LOADING) return;
    look.still = owe_still_texture(s->still, &look.still_w, &look.still_h);
    for (i = 0; i < OWE_SCENE_MAX_PIECES; i++) {
        look.pictures[i] = owe_still_texture(s->pictures[i], NULL, NULL);
        look.behinds[i] = owe_still_texture(s->behinds[i], NULL, NULL);
    }
    for (i = 0; i < OWE_SCENE_MAX_AREAS; i++) look.masks[i] = owe_still_texture(s->masks[i], NULL, NULL);
    /* A paused scene settles from wherever its pieces are; at rest it is the still. */
    if (s->phase == PHASE_SETTLING) settling = smooth(seconds_between(&s->settle_start, &s->frame_at) / SETTLE);
    if (s->phase == PHASE_RESTING || s->phase == PHASE_ENDED) settling = 1.0;
    owe_scene_draw(s->egl, out, s->desc, &look, s->arriving,
                   seconds_between(&s->part_start, s->held ? &s->held_at : &s->frame_at), settling);
    s->frames++;
    s->frame_due = false;
}

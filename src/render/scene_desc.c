#include "scene_desc.h"

#include <libgen.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "yyjson.h"

static int fail(char *error, size_t len, const char *fmt, ...) {
    va_list args;
    if (error && len) {
        va_start(args, fmt);
        vsnprintf(error, len, fmt, args);
        va_end(args);
    }
    return -1;
}

static float number(yyjson_val *obj, const char *key, float fallback) {
    yyjson_val *v = yyjson_obj_get(obj, key);
    return v && yyjson_is_num(v) ? (float)yyjson_get_num(v) : fallback;
}

/* A picture a scene names must be one of its own files: a path under the
 * scene's directory, never an absolute one or one that climbs out of it. */
static int own_file(const char *dir, const char *name, char *out, size_t out_len) {
    const char *part = name;
    if (!name || !*name || name[0] == '/') return -1;
    while (part) {
        const char *slash = strchr(part, '/');
        size_t n = slash ? (size_t)(slash - part) : strlen(part);
        if (n == 2 && part[0] == '.' && part[1] == '.') return -1;
        part = slash ? slash + 1 : NULL;
    }
    return snprintf(out, out_len, "%s/%s", dir, name) < (int)out_len ? 0 : -1;
}

static int read_at(yyjson_val *arr, float *at) {
    int i;
    if (!yyjson_is_arr(arr) || yyjson_arr_size(arr) != 4) return -1;
    for (i = 0; i < 4; i++) {
        yyjson_val *v = yyjson_arr_get(arr, (size_t)i);
        if (!yyjson_is_num(v)) return -1;
        at[i] = (float)yyjson_get_num(v);
    }
    /* Somewhere in the still, and not empty. */
    if (at[0] < 0 || at[1] < 0 || at[2] <= 0 || at[3] <= 0 || at[0] + at[2] > 1.0001f || at[1] + at[3] > 1.0001f) return -1;
    return 0;
}

static int read_motion(yyjson_val *obj, struct owe_scene_motion *m) {
    yyjson_val *times;
    size_t i;
    memset(m, 0, sizeof(*m));
    if (!obj) return 0;
    if (!yyjson_is_obj(obj)) return -1;
    if (yyjson_obj_get(obj, "swing")) {
        m->shape = OWE_SCENE_SWING;
        m->amount = number(obj, "swing", 0);
        m->seconds = number(obj, "seconds", 6);
        m->phase = number(obj, "phase", 0);
        return m->seconds > 0 ? 0 : -1;
    }
    if (yyjson_obj_get(obj, "pulse")) {
        m->shape = OWE_SCENE_PULSE;
        m->amount = number(obj, "pulse", 0);
        m->lasts = number(obj, "lasts", 0.3f);
        m->every = number(obj, "every", 8);
        times = yyjson_obj_get(obj, "at");
        if (!yyjson_is_arr(times) || yyjson_arr_size(times) > OWE_SCENE_MAX_TIMES) return -1;
        for (i = 0; i < yyjson_arr_size(times); i++) {
            m->at[m->count++] = (float)yyjson_get_num(yyjson_arr_get(times, i));
        }
        return m->lasts > 0 && m->every > 0 ? 0 : -1;
    }
    if (yyjson_obj_get(obj, "from")) {
        m->shape = OWE_SCENE_FROM;
        m->from = number(obj, "from", 0);
        m->after = number(obj, "after", 0);
        m->over = number(obj, "over", 0);
        m->peak = number(obj, "peak", 0.3f);
        m->takes = number(obj, "for", 0.5f);
        return m->after >= 0 && m->over >= 0 && m->over < 1 && m->peak > 0 && m->takes > 0 ? 0 : -1;
    }
    return -1;
}

static int find_piece(const struct owe_scene_desc *desc, const char *name) {
    int i;
    for (i = 0; name && i < desc->piece_count; i++) {
        if (strcmp(desc->pieces[i].name, name) == 0) return i;
    }
    return -1;
}

/* One part of the scene: what each piece it lists does. */
static int read_part(yyjson_val *section, struct owe_scene_desc *desc, bool arrive, char *error, size_t len) {
    yyjson_obj_iter iter;
    yyjson_val *key;
    if (!section) return 0;
    if (!yyjson_is_obj(section)) return fail(error, len, "\"%s\" is not a list of pieces", arrive ? "arrive" : "idle");
    yyjson_obj_iter_init(section, &iter);
    while ((key = yyjson_obj_iter_next(&iter))) {
        const char *name = yyjson_get_str(key);
        yyjson_val *moves = yyjson_obj_iter_get_val(key);
        struct owe_scene_part *part;
        int index;
        if (strcmp(name, "frames_a_second") == 0 || strcmp(name, "seconds") == 0) continue;
        index = find_piece(desc, name);
        if (index < 0) return fail(error, len, "\"%s\" moves \"%s\", which is not one of the pieces", arrive ? "arrive" : "idle", name);
        part = arrive ? &desc->pieces[index].arrive : &desc->pieces[index].idle;
        part->listed = true;
        if (read_motion(yyjson_obj_get(moves, "x"), &part->x) != 0 || read_motion(yyjson_obj_get(moves, "y"), &part->y) != 0 ||
            read_motion(yyjson_obj_get(moves, "size"), &part->size) != 0 ||
            read_motion(yyjson_obj_get(moves, "fade"), &part->fade) != 0) {
            return fail(error, len, "\"%s\" in \"%s\" has a motion that cannot be read", name, arrive ? "arrive" : "idle");
        }
    }
    return 0;
}

int owe_scene_desc_read(const char *path, struct owe_scene_desc *desc, char *error, size_t error_len) {
    yyjson_read_err problem;
    yyjson_doc *doc;
    yyjson_val *root, *list, *entry, *idle, *arrive;
    yyjson_arr_iter iter;
    char copy[OWE_SCENE_PATH];
    const char *dir, *text;
    int rc = -1, i;
    if (!path || !desc) return fail(error, error_len, "no scene file");
    memset(desc, 0, sizeof(*desc));
    doc = yyjson_read_file(path, YYJSON_READ_ALLOW_COMMENTS | YYJSON_READ_ALLOW_TRAILING_COMMAS, NULL, &problem);
    if (!doc) return fail(error, error_len, "%s: %s", path, problem.msg ? problem.msg : "cannot be read");
    snprintf(copy, sizeof(copy), "%s", path);
    dir = dirname(copy);
    root = yyjson_doc_get_root(doc);
    if (!yyjson_is_obj(root)) {
        fail(error, error_len, "the scene is not an object");
        goto done;
    }
    text = yyjson_get_str(yyjson_obj_get(root, "still"));
    if (!text || strlen(text) >= sizeof(desc->still)) {
        fail(error, error_len, "the scene names no still");
        goto done;
    }
    snprintf(desc->still, sizeof(desc->still), "%s", text);
    text = yyjson_get_str(yyjson_obj_get(root, "sha256"));
    if (text) {
        if (strlen(text) != 64 || strspn(text, "0123456789abcdef") != 64) {
            fail(error, error_len, "\"sha256\" is not 64 lower-case hex digits");
            goto done;
        }
        snprintf(desc->sha256, sizeof(desc->sha256), "%s", text);
    }

    list = yyjson_obj_get(root, "areas");
    yyjson_arr_iter_init(list, &iter);
    while ((entry = yyjson_arr_iter_next(&iter))) {
        struct owe_scene_area *area = &desc->areas[desc->area_count];
        const char *name = yyjson_get_str(yyjson_obj_get(entry, "name"));
        if (desc->area_count == OWE_SCENE_MAX_AREAS) {
            fail(error, error_len, "more than %d areas", OWE_SCENE_MAX_AREAS);
            goto done;
        }
        if (!name || !*name || strlen(name) >= OWE_SCENE_NAME) {
            fail(error, error_len, "an area has no name");
            goto done;
        }
        snprintf(area->name, sizeof(area->name), "%s", name);
        if (own_file(dir, yyjson_get_str(yyjson_obj_get(entry, "mask")), area->mask, sizeof(area->mask)) != 0) {
            fail(error, error_len, "area \"%s\": \"mask\" must be a file of the scene's own", name);
            goto done;
        }
        if (read_at(yyjson_obj_get(entry, "at"), area->at) != 0) {
            fail(error, error_len, "area \"%s\": \"at\" must be left, top, width and height inside the still", name);
            goto done;
        }
        desc->area_count++;
    }

    list = yyjson_obj_get(root, "pieces");
    yyjson_arr_iter_init(list, &iter);
    while ((entry = yyjson_arr_iter_next(&iter))) {
        struct owe_scene_piece *piece = &desc->pieces[desc->piece_count];
        const char *name = yyjson_get_str(yyjson_obj_get(entry, "name"));
        const char *behind = yyjson_get_str(yyjson_obj_get(entry, "behind"));
        const char *inside = yyjson_get_str(yyjson_obj_get(entry, "inside"));
        const char *on = yyjson_get_str(yyjson_obj_get(entry, "on"));
        const char *colour = yyjson_get_str(yyjson_obj_get(entry, "colour"));
        yyjson_val *rest = yyjson_obj_get(entry, "rest");
        if (desc->piece_count == OWE_SCENE_MAX_PIECES) {
            fail(error, error_len, "more than %d pieces", OWE_SCENE_MAX_PIECES);
            goto done;
        }
        if (!name || !*name || strlen(name) >= OWE_SCENE_NAME || find_piece(desc, name) >= 0) {
            fail(error, error_len, "a piece has no name, or two share one");
            goto done;
        }
        snprintf(piece->name, sizeof(piece->name), "%s", name);
        if (own_file(dir, yyjson_get_str(yyjson_obj_get(entry, "picture")), piece->picture, sizeof(piece->picture)) != 0 ||
            (behind && own_file(dir, behind, piece->behind, sizeof(piece->behind)) != 0)) {
            fail(error, error_len, "piece \"%s\": \"picture\" and \"behind\" must be files of the scene's own", name);
            goto done;
        }
        if (read_at(yyjson_obj_get(entry, "at"), piece->at) != 0) {
            fail(error, error_len, "piece \"%s\": \"at\" must be left, top, width and height inside the still", name);
            goto done;
        }
        piece->rest_x = number(rest, "x", 0);
        piece->rest_y = number(rest, "y", 0);
        piece->own_colour = colour && strcmp(colour, "own") == 0;
        piece->inside = -1;
        for (i = 0; inside && i < desc->area_count; i++) {
            if (strcmp(desc->areas[i].name, inside) == 0) piece->inside = i;
        }
        if (inside && piece->inside < 0) {
            fail(error, error_len, "piece \"%s\" is inside \"%s\", which is not one of the areas", name, inside);
            goto done;
        }
        /* A piece sits on one that comes before it, which is drawn first. */
        piece->on = find_piece(desc, on);
        if (on && piece->on < 0) {
            fail(error, error_len, "piece \"%s\" is on \"%s\", which is not a piece before it", name, on);
            goto done;
        }
        desc->piece_count++;
    }

    idle = yyjson_obj_get(root, "idle");
    arrive = yyjson_obj_get(root, "arrive");
    desc->idle_fps = (int)number(idle, "frames_a_second", 24);
    desc->arrive_fps = (int)number(arrive, "frames_a_second", 30);
    desc->arrive_seconds = arrive ? number(arrive, "seconds", 0) : 0;
    if (desc->idle_fps < 1 || desc->idle_fps > 60 || desc->arrive_fps < 1 || desc->arrive_fps > 60) {
        fail(error, error_len, "\"frames_a_second\" must be from 1 to 60");
        goto done;
    }
    if (desc->arrive_seconds < 0 || desc->arrive_seconds > 30) {
        fail(error, error_len, "the arrive part must take from 0 to 30 seconds");
        goto done;
    }
    if (read_part(idle, desc, false, error, error_len) != 0 || read_part(arrive, desc, true, error, error_len) != 0) goto done;
    rc = 0;
done:
    yyjson_doc_free(doc);
    return rc;
}

/* How far a value that starts away from rest has come: 0 at the start, 1 at
 * rest, and past 1 for a moment if it swings over. */
static double arrived(const struct owe_scene_motion *m, double t) {
    double u = t - m->after;
    if (u <= 0.0) return 0.0;
    if (m->over > 0.0f) {
        /* A spring let go: furthest past rest, by the share `over`, `peak`
         * seconds after it sets off. */
        double b = M_PI / m->peak;
        double a = -log(m->over) / m->peak;
        return 1.0 - exp(-a * u) * (cos(b * u) + a / b * sin(b * u));
    }
    if (u >= m->takes) return 1.0;
    return 1.0 - pow(1.0 - u / m->takes, 3.0);
}

float owe_scene_offset(const struct owe_scene_motion *m, double t) {
    switch (m->shape) {
    case OWE_SCENE_SWING: {
        /* Starts at home, wherever in the swing it starts. */
        double start = m->phase * M_PI / 180.0;
        return m->amount * (float)(sin(2.0 * M_PI * t / m->seconds + start) - sin(start));
    }
    case OWE_SCENE_PULSE: {
        double now = fmod(t, m->every);
        int i;
        for (i = 0; i < m->count; i++) {
            double part = (now - m->at[i]) / m->lasts;
            /* Out over the first third, held, back over the last third. */
            if (part >= 0.0 && part < 1.0) return m->amount * (float)fmin(1.0, fmin(part, 1.0 - part) * 3.0);
        }
        return 0.0f;
    }
    case OWE_SCENE_FROM:
        return m->from * (float)(1.0 - arrived(m, t));
    default:
        return 0.0f;
    }
}

float owe_scene_level(const struct owe_scene_motion *m, double t) {
    if (m->shape == OWE_SCENE_FROM) return m->from + (1.0f - m->from) * (float)arrived(m, t);
    return 1.0f;
}

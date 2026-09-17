#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "owe_spawn.h"

#include "../src/render/still.c"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

static char root[512];

static int run(char *const argv[]) {
    char log[8192];
    return owe_spawn_capture(argv[0], argv, log, sizeof(log), 20000);
}

static void path_for(char *out, size_t size, const char *name) {
    CHECK(snprintf(out, size, "%s/%s", root, name) < (int)size);
}

static int generate(const char *name, const char *size) {
    char path[1024];
    char filter[128];
    char *argv[] = {"ffmpeg", "-y", "-v", "error", "-nostdin", "-f", "lavfi", "-i", filter,
                    "-frames:v", "1", "-threads", "1", path, NULL};
    path_for(path, sizeof(path), name);
    snprintf(filter, sizeof(filter), "testsrc=size=%s:duration=0.1", size);
    return run(argv);
}

static void check_decode(const char *name, int *w, int *h) {
    char path[1024];
    uint8_t *rgba = NULL;
    int rc;
    path_for(path, sizeof(path), name);
    rc = decode_first_frame(path, 0, 0, &rgba, w, h);
    CHECK(rc == 0);
    CHECK(rgba != NULL);
    CHECK(*w > 0 && *h > 0);
    free(rgba);
}

int main(void) {
    char log[8192];
    char *version[] = {"ffmpeg", "-version", NULL};
    int skipped = 0;

    CHECK(snprintf(root, sizeof(root), "/tmp/owe-decode-XXXXXX") < (int)sizeof(root));
    CHECK(mkdtemp(root));
    CHECK(owe_spawn_capture("ffmpeg", version, log, sizeof(log), 5000) == 0);

    CHECK(generate("still.png", "64x48") == 0);
    CHECK(generate("still.jpg", "64x48") == 0);
    CHECK(generate("large.jpg", "4000x3000") == 0);
    CHECK(generate("portrait.jpg", "1200x2400") == 0);
    if (generate("still.avif", "64x48") == 0) {
        int w = 0, h = 0;
        check_decode("still.avif", &w, &h);
        printf("avif decoded %dx%d\n", w, h);
    } else {
        skipped++;
        printf("avif fixture unavailable, skip avif\n");
    }

    {
        int w = 0, h = 0;
        check_decode("still.png", &w, &h);
        CHECK(w == 64 && h == 48);
        printf("png decoded %dx%d\n", w, h);
        check_decode("still.jpg", &w, &h);
        CHECK(w == 64 && h == 48);
        printf("jpg decoded %dx%d\n", w, h);
        check_decode("large.jpg", &w, &h);
        CHECK(w == 4000 && h == 3000);
        check_decode("portrait.jpg", &w, &h);
        CHECK(w == 1200 && h == 2400);
    }

    {
        char path[1024];
        uint8_t *rgba = NULL;
        int w = 0, h = 0;
        path_for(path, sizeof(path), "large.jpg");
        CHECK(decode_first_frame(path, 1000, 1000, &rgba, &w, &h) == 0);
        CHECK(w == 1333 && h == 1000);
        printf("cover decode at %dx%d\n", w, h);
        free(rgba);
        path_for(path, sizeof(path), "portrait.jpg");
        CHECK(decode_first_frame(path, 1920, 1080, &rgba, &w, &h) == 0);
        CHECK(w == 1200 && h == 2400);
        free(rgba);
    }

    {
        char *remove[] = {"rm", "-rf", "--", root, NULL};
        CHECK(owe_spawn_capture("rm", remove, log, sizeof(log), 5000) == 0);
    }
    printf("still decode checks passed (%d skipped)\n", skipped);
    return 0;
}

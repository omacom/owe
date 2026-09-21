#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>

#include "owe_spawn.h"

#include "../src/render/still.c"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

static char root[512];

/* Async lifecycle checks stop before GPU upload. */
void owe_app_request_render(void) {}
owe_output_t *owe_wayland_outputs(struct owe_wayland *wl) { (void)wl; return NULL; }
int owe_egl_prepare_output(struct owe_egl *e, owe_output_t *o) { (void)e; (void)o; return 0; }
int owe_egl_make_current(struct owe_egl *e) { (void)e; return 0; }
unsigned int owe_egl_tex_from_rgba(struct owe_egl *e, const uint8_t *p, int w, int h) {
    (void)e; (void)p; (void)w; (void)h; return 1;
}
void owe_egl_tex_free(struct owe_egl *e, unsigned int tex) { (void)e; (void)tex; }

static pthread_mutex_t gate_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cond = PTHREAD_COND_INITIALIZER;
static bool gate_open;

static void *held_decode(void *opaque) {
    struct still_job *job = opaque;
    pthread_mutex_lock(&gate_mutex);
    while (!gate_open) pthread_cond_wait(&gate_cond, &gate_mutex);
    pthread_mutex_unlock(&gate_mutex);
    char done = 'x';
    CHECK(write(job->done_fd[1], &done, 1) == 1);
    return NULL;
}

static void test_async_replace(const char *path) {
    struct owe_still still = {0};
    struct still_job *held = calloc(1, sizeof(*held));
    CHECK(held);
    atomic_init(&held->cancelled, false);
    CHECK(pipe2(held->done_fd, O_CLOEXEC | O_NONBLOCK) == 0);
    CHECK(pthread_create(&held->thread, NULL, held_decode, held) == 0);
    still.job = held;
    /* Joining the worker here deadlocks: the gate opens only after replacing
     * and cancelling, proving these operations never wait for a decoder. */
    CHECK(owe_still_start(&still, "/obsolete.png", 10, 10) == 0);
    owe_still_cancel(&still);
    CHECK(!still.queued && atomic_load(&held->cancelled));
    CHECK(owe_still_start(&still, "/also-obsolete.png", 20, 20) == 0);
    CHECK(owe_still_start(&still, path, 64, 48) == 0);
    pthread_mutex_lock(&gate_mutex);
    gate_open = true;
    pthread_cond_signal(&gate_cond);
    pthread_mutex_unlock(&gate_mutex);
    struct pollfd fd = {.fd = owe_still_fd(&still), .events = POLLIN};
    CHECK(poll(&fd, 1, 5000) == 1);
    CHECK(owe_still_poll(&still) == 0);
    CHECK(still.job && !still.queued && strcmp(still.job->path, path) == 0);
    fd.fd = owe_still_fd(&still);
    CHECK(poll(&fd, 1, 5000) == 1);
    CHECK(owe_still_poll(&still) == 0); /* Keep result until an output exists. */
    CHECK(still.job && still.job->ready && still.job->rgba);
    owe_still_cancel(&still); /* A joined result must not be joined twice. */
    CHECK(!owe_still_busy(&still));
}

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
    rc = decode_first_frame(path, 0, 0, &rgba, w, h, NULL);
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
    {
        char path[1024];
        path_for(path, sizeof(path), "still.png");
        test_async_replace(path);
    }
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
        CHECK(decode_first_frame(path, 1000, 1000, &rgba, &w, &h, NULL) == 0);
        CHECK(w == 1333 && h == 1000);
        printf("cover decode at %dx%d\n", w, h);
        free(rgba);
        path_for(path, sizeof(path), "portrait.jpg");
        CHECK(decode_first_frame(path, 1920, 1080, &rgba, &w, &h, NULL) == 0);
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

#include "../src/render/egl.c"
#include "../src/render/wayland.c"
#include "render_ipc.h"
#include "common_ipc.h"
#include "owe_spawn.h"

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

static owe_app_t app;
static struct owe_wayland wl;
static owe_output_t outputs[2];
static int peer;
static int failures;
static int client;
static char root[] = "/tmp/owe-transition-XXXXXX";
static unsigned char samples[2][4];
static int blended[2];
static int dark_frames[2];
static int total_frames;
static bool capture;
static int capture_stage;
static bool callbacks_enabled = true;
static bool poll_transition = true;

owe_app_t *owe_app_get(void) { return &app; }
void owe_app_request_render(void) { owe_wayland_request_render(&wl); }
void owe_app_on_outputs_changed(void) {}

/* The renderer uses real EGL buffers, libmpv, image decode, and the Wayland draw loop.
 * Only compositor frame callbacks come from the test clock. */
static int init_display(void) {
    EGLDisplay display = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, NULL, NULL)) return 77;
    CHECK(eglBindAPI(EGL_OPENGL_API));
    EGLint attrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                     EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLConfig config;
    EGLint count;
    CHECK(eglChooseConfig(display, attrs, &config, 1, &count) && count == 1);
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, NULL);
    CHECK(context != EGL_NO_CONTEXT);
    app.egl = calloc(1, sizeof(*app.egl));
    CHECK(app.egl);
    *app.egl = (struct owe_egl){.display = display, .context = context, .config = config};
    wl.egl = app.egl;
    wl.outputs = outputs;
    wl.output_count = 2;
    app.wl = &wl;
    for (int i = 0; i < 2; i++) {
        owe_output_t *out = &outputs[i];
        out->width = i ? 96 : 128;
        out->height = 72;
        out->scale = 1;
        out->configured = out->frame_ready = out->frame_pending = 1;
        out->owner = &wl;
        snprintf(out->name, sizeof(out->name), "OWE-TEST-%d", i);
        EGLint size[] = {EGL_WIDTH, out->width, EGL_HEIGHT, out->height, EGL_NONE};
        out->egl_surface = eglCreatePbufferSurface(display, config, size);
        CHECK(out->egl_surface != EGL_NO_SURFACE);
        CHECK(owe_egl_prepare_output(app.egl, out) == 0);
    }
    outputs[0].next = &outputs[1];
    app.mpv = owe_mpv_new(&wl);
    CHECK(app.mpv);
    printf("OpenGL renderer: %s\n", glGetString(GL_RENDERER));
    int sockets[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    peer = sockets[1];
    wl.display = wl_display_connect_to_fd(sockets[0]);
    CHECK(wl.display);
    for (int i = 0; i < 2; i++) {
        outputs[i].surface = (struct wl_surface *)wl_proxy_create((struct wl_proxy *)wl.display, &wl_surface_interface);
        CHECK(outputs[i].surface);
    }
    app.still = owe_still_new(&wl, app.egl);
    app.transition = owe_still_new(&wl, app.egl);
    CHECK(app.still && app.transition);
    char socket_path[256];
    snprintf(socket_path, sizeof(socket_path), "%s/render.sock", root);
    app.ipc = owe_render_ipc_new(socket_path);
    CHECK(app.ipc);
    client = owe_ipc_connect(socket_path);
    CHECK(client >= 0);
    owe_render_ipc_accept(app.ipc);
    return 0;
}

static void tick(void) {
    owe_render_ipc_poll_clients(app.ipc);
    owe_render_ipc_poll_still(app.ipc);
    if (poll_transition && owe_still_busy(app.transition)) {
        owe_still_poll(app.transition);
        owe_app_request_render();
    }
    if (owe_mpv_process_updates(app.mpv)) owe_app_request_render();
    for (int i = 0; i < 2; i++) {
        if (callbacks_enabled && outputs[i].frame_callback) frame_done(&outputs[i], outputs[i].frame_callback, 0);
    }
    owe_wayland_render_pending(&wl);
    for (int i = 0; i < 2; i++) {
        if (!outputs[i].egl_surface) continue;
        CHECK(owe_egl_prepare_output(app.egl, &outputs[i]) == 0);
        glReadPixels(outputs[i].width / 2, outputs[i].height / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, samples[i]);
        CHECK(glGetError() == GL_NO_ERROR);
        if (samples[i][0] > 20 && samples[i][2] > 20 && samples[i][1] < 15) blended[i]++;
        if (samples[i][0] + samples[i][2] < 220) dark_frames[i]++;
        const char *directory = getenv("OWE_TRANSITION_CAPTURE_DIR");
        if (i == 0 && capture && directory && capture_stage < 3 &&
            ((capture_stage == 0 && samples[i][0] > 240 && samples[i][2] < 10) ||
             (capture_stage == 1 && samples[i][0] > 110 && samples[i][0] < 145 && samples[i][2] > 110) ||
             (capture_stage == 2 && samples[i][0] < 10 && samples[i][2] > 240))) {
            unsigned char pixels[128 * 72 * 3];
            glReadPixels(0, 0, 128, 72, GL_RGB, GL_UNSIGNED_BYTE, pixels);
            char path[4096];
            snprintf(path, sizeof(path), "%s/frame-%d.ppm", directory, capture_stage++);
            FILE *file = fopen(path, "wb");
            CHECK(file);
            fprintf(file, "P6\n128 72\n255\n");
            CHECK(fwrite(pixels, 1, sizeof(pixels), file) == sizeof(pixels));
            CHECK(fclose(file) == 0);
            printf("Captured RGB %u,%u,%u to %s\n", samples[i][0], samples[i][1], samples[i][2], path);
        }
    }
    total_frames++;
    usleep(16000);
}

static void run_ms(int ms) {
    int64_t end = now_ms() + ms;
    do { tick(); } while (now_ms() < end);
}

static void command(const char *line) {
    CHECK(owe_ipc_send_line(client, line) == 0);
    struct pollfd pfd = {.fd = client, .events = POLLIN};
    int64_t deadline = now_ms() + 5000;
    while (poll(&pfd, 1, 0) == 0 && now_ms() < deadline) tick();
    char reply[8192];
    CHECK(owe_ipc_recv_line(client, reply, sizeof(reply)) == 0);
    CHECK(strstr(reply, "\"status\":\"ok\""));
}

static void still(void) {
    char line[1024];
    command("{\"cmd\":\"fade\",\"ms\":0}");
    snprintf(line, sizeof(line), "{\"cmd\":\"load\",\"path\":\"%s/red.png\",\"kind\":\"still\"}", root);
    command(line);
    /* Let the outgoing still settle before the next fade setting takes effect. */
    run_ms(300);
    CHECK(samples[0][0] > 240 && samples[0][2] < 10);
    CHECK(samples[1][0] > 240 && samples[1][2] < 10);
}

static void video(int fade) {
    char line[1024];
    snprintf(line, sizeof(line), "{\"cmd\":\"fade\",\"ms\":%d}", fade);
    command(line);
    memset(blended, 0, sizeof(blended));
    memset(dark_frames, 0, sizeof(dark_frames));
    total_frames = 0;
    snprintf(line, sizeof(line), "{\"cmd\":\"load\",\"path\":\"%s/blue.mp4\",\"kind\":\"video\",\"from\":\"%s/red.png\"}", root, root);
    command(line);
}

static void verify(bool condition, const char *description) {
    printf("%s: %s\n", condition ? "PASS" : "FAIL", description);
    if (!condition) failures++;
}

static void make_media(void) {
    char path[256], log[2048];
    snprintf(path, sizeof(path), "%s/red.png", root);
    char *png[] = {"ffmpeg", "-v", "error", "-f", "lavfi", "-i", "color=red:s=128x72",
                   "-frames:v", "1", "-threads", "1", path, NULL};
    CHECK(owe_spawn_capture("ffmpeg", png, log, sizeof(log), 10000) == 0);
    snprintf(path, sizeof(path), "%s/blue.mp4", root);
    char *mp4[] = {"ffmpeg", "-v", "error", "-f", "lavfi", "-i", "color=blue:s=128x72:r=30:d=3",
                   "-c:v", "libx264", "-threads", "1", "-pix_fmt", "yuv420p", path, NULL};
    CHECK(owe_spawn_capture("ffmpeg", mp4, log, sizeof(log), 10000) == 0);
}

int main(void) {
    CHECK(mkdtemp(root));
    make_media();
    int rc = init_display();
    if (rc) return rc;
    char line[1024];
    snprintf(line, sizeof(line), "{\"cmd\":\"load\",\"path\":\"%s/blue.mp4\",\"kind\":\"video\"}", root);
    command(line);
    run_ms(350);
    verify(samples[0][2] > 240 && samples[0][0] < 10 &&
           samples[1][2] > 240 && samples[1][0] < 10,
           "a cold MP4 load displays video without a transition image");

    command("{\"cmd\":\"stop\"}");
    run_ms(50);
    void *unavailable_surface = outputs[0].egl_surface;
    outputs[0].egl_surface = NULL;
    outputs[0].configured = 0;
    video(250);
    run_ms(1400);
    verify(samples[1][2] > 240 && samples[1][0] < 10,
           "an unavailable first output does not block video on another output");
    outputs[0].egl_surface = unavailable_surface;
    outputs[0].configured = 1;
    command("{\"cmd\":\"stop\"}");
    run_ms(50);

    /* Delay transition completion while the MP4 decoder remains available. */
    poll_transition = false;
    video(250);
    run_ms(1400);
    verify(samples[0][2] > 240 && samples[0][0] < 10 &&
           samples[1][2] > 240 && samples[1][0] < 10,
           "a slow transition image cannot leave a playable MP4 black");
    poll_transition = true;
    run_ms(100);
    verify(!owe_still_has_image(app.transition), "a late transition cannot cover recovered video");

    still();
    capture = true;
    video(250);
    run_ms(550);
    capture = false;
    printf("Normal transition: %d and %d blended samples across %d samples\n", blended[0], blended[1], total_frames);
    verify(blended[0] >= 3 && blended[1] >= 3, "both outputs show intermediate colors");
    verify(dark_frames[0] == 0 && dark_frames[1] == 0, "the transition has no dark flash");
    verify(samples[0][2] > 240 && samples[0][0] < 10, "the video replaces the outgoing still");
    verify(!owe_still_has_image(app.transition), "the finished overlay releases its texture");

    still();
    owe_wayland_set_skipped(&wl, "OWE-TEST-0,OWE-TEST-1");
    video(500);
    run_ms(700);
    memset(blended, 0, sizeof(blended));
    owe_wayland_set_skipped(&wl, "");
    run_ms(800);
    printf("Delayed presentation: %d and %d blended samples\n", blended[0], blended[1]);
    verify(blended[0] >= 3 && blended[1] >= 3, "the transition starts when outputs can present");

    still();
    video(2000);
    run_ms(80);
    command("{\"cmd\":\"stop\"}");
    run_ms(100);
    verify(!owe_still_has_image(app.transition) && !owe_still_busy(app.transition), "stop releases the unfinished overlay");

    still();
    video(0);
    run_ms(200);
    verify(!owe_still_has_image(app.transition) && !owe_still_busy(app.transition), "zero fade skips the overlay");
    verify(samples[0][2] > 240 && samples[0][0] < 10, "zero fade displays the video");

    still();
    video(500);
    run_ms(80);
    command("{\"cmd\":\"pause\"}");
    run_ms(650);
    verify(samples[0][2] > 240 && samples[0][0] < 10 && !owe_still_has_image(app.transition),
           "a paused video completes its transition");
    run_ms(80);
    verify(!outputs[0].frame_callback && !outputs[1].frame_callback,
           "the completed transition stops extra frame callbacks");
    command("{\"cmd\":\"resume\"}");

    still();
    video(250);
    run_ms(80);
    command("{\"cmd\":\"pause\"}");
    callbacks_enabled = false;
    run_ms(400);
    callbacks_enabled = true;
    run_ms(150);
    verify(samples[0][2] > 240 && samples[0][0] < 10 &&
           samples[1][2] > 240 && samples[1][0] < 10,
           "delayed callbacks still present the final frame on both outputs");
    command("{\"cmd\":\"resume\"}");

    still();
    video(2000);
    run_ms(80);
    still();
    verify(!owe_still_has_image(app.transition) && !owe_still_busy(app.transition),
           "a still replacement clears the unfinished overlay");

    close(client);
    owe_render_ipc_free(app.ipc);
    owe_still_free(app.transition);
    owe_still_free(app.still);
    owe_mpv_free(app.mpv);
    for (int i = 0; i < 2; i++) {
        if (outputs[i].frame_callback) wl_callback_destroy(outputs[i].frame_callback);
        wl_proxy_destroy((struct wl_proxy *)outputs[i].surface);
        owe_egl_destroy_output(app.egl, &outputs[i]);
    }
    owe_egl_free(app.egl);
    wl_display_disconnect(wl.display);
    close(peer);
    char log[1024];
    char *remove[] = {"rm", "-rf", "--", root, NULL};
    CHECK(owe_spawn_capture("rm", remove, log, sizeof(log), 5000) == 0);
    return failures ? 1 : 0;
}

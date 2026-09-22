#include "../src/render/render_ipc.c"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

struct owe_mpv { bool muted; bool stopped; bool paused; };
struct owe_still { int result; };
struct owe_feed { bool active; };
static owe_app_t app;
static int render_requests;
static bool still_busy = true;
static int reload_width;
static int reload_height;
static bool cancelled;

owe_app_t *owe_app_get(void) { return &app; }
void owe_app_request_render(void) { render_requests++; }
void owe_feed_stop(struct owe_feed *feed) { feed->active = false; }
void owe_mpv_set_muted(struct owe_mpv *mpv, bool muted) { mpv->muted = muted; }
void owe_mpv_set_paused(struct owe_mpv *mpv, bool paused) { mpv->paused = paused; }
void owe_mpv_stop(struct owe_mpv *mpv) { mpv->stopped = true; }
void owe_still_unload(struct owe_still *still) { (void)still; }
void owe_still_cancel(struct owe_still *still) { cancelled = true; still->result = 0; }
bool owe_still_busy(struct owe_still *still) { (void)still; return still_busy; }
int owe_still_poll(struct owe_still *still) { return still->result; }
bool owe_still_has_image(struct owe_still *still) { (void)still; return true; }
const char *owe_still_path(struct owe_still *still) { (void)still; return "/still.png"; }
int owe_still_decoded_max(struct owe_still *still, int *w, int *h) {
    (void)still; *w = 1280; *h = 720; return 0;
}
void owe_wayland_outputs_max_size(struct owe_wayland *wl, int *w, int *h) {
    (void)wl; *w = 3840; *h = 2160;
}
int owe_still_start(struct owe_still *still, const char *path, int w, int h) {
    (void)still; (void)path; reload_width = w; reload_height = h; return 0;
}

int main(void) {
    struct owe_mpv mpv = {.muted = true};
    struct owe_still still = {0};
    struct owe_feed feed = {.active = true};
    struct owe_render_ipc ipc = {.pending_client = -1};
    app.mpv = &mpv;
    app.still = &still;
    app.feed = &feed;
    app.feeding = true;
    strcpy(ipc.pending_path, "/still.png");

    owe_render_ipc_poll_still(&ipc);
    CHECK(app.feeding && feed.active && mpv.muted && !mpv.stopped && render_requests == 0);
    still.result = -1;
    owe_render_ipc_poll_still(&ipc);
    CHECK(app.feeding && feed.active && mpv.muted && !mpv.stopped && render_requests == 0);
    still.result = 1;
    strcpy(ipc.pending_path, "/still.png");
    owe_render_ipc_poll_still(&ipc);
    CHECK(!app.feeding && !feed.active && !mpv.muted && mpv.stopped && render_requests == 1);
    CHECK(strcmp(app.current_path, "/still.png") == 0 && strcmp(app.current_kind, "still") == 0);
    stop_feed(&app);
    CHECK(render_requests == 1);
    puts("renderer feed-to-still transition checks passed");

    /* No feed clients can pause mpv without a desktop pause request. */
    app.feeding = feed.active = true;
    mpv.paused = true;
    stop_feed(&app);
    CHECK(!mpv.paused && !app.feeding && !feed.active);
    app.feeding = feed.active = true;
    app.paused = true;
    stop_feed(&app);
    CHECK(mpv.paused);

    /* Expired asynchronous loads cannot replace the previous image. */
    still.result = 1;
    strcpy(ipc.pending_path, "/late.png");
    ipc.pending_deadline_ms = owe_ipc_now_ms() - 1;
    owe_render_ipc_poll_still(&ipc);
    CHECK(cancelled && !*ipc.pending_path && *ipc.load_error);
    CHECK(strcmp(app.current_path, "/still.png") == 0);

    /* A late decode reply belongs to the original connection only. */
    int first[2], replacement[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, first) == 0);
    ipc.clients[0].fd = -1;
    owe_ipc_client_open(&ipc.clients[0], first[0]);
    ipc.pending_client = 0;
    ipc.pending_generation = ipc.clients[0].generation;
    client_remove(&ipc, 0);
    close(first[1]);
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, replacement) == 0);
    owe_ipc_client_open(&ipc.clients[0], replacement[0]);
    pending_reply(&ipc, 1, NULL);
    char reply[64];
    CHECK(recv(replacement[1], reply, sizeof(reply), MSG_DONTWAIT) < 0 && errno == EAGAIN);
    ipc.pending_client = 0;
    ipc.pending_generation = ipc.clients[0].generation;
    pending_reply(&ipc, 1, NULL);
    CHECK(recv(replacement[1], reply, sizeof(reply), 0) > 0);
    client_remove(&ipc, 0);
    close(replacement[1]);
    puts("still replies follow connection generations");
    CHECK(!owe_render_ipc_reload_still(&ipc));
    CHECK(reload_width == 0);
    still_busy = false;
    CHECK(owe_render_ipc_reload_still(&ipc));
    CHECK(reload_width == 3840 && reload_height == 2160);
    puts("output growth retries after the current still decode");
    return 0;
}

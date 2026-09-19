#include "../src/render/render_ipc.c"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

struct owe_mpv { bool muted; bool stopped; };
struct owe_still { int result; };
struct owe_feed { bool active; };
static owe_app_t app;
static int render_requests;

owe_app_t *owe_app_get(void) { return &app; }
void owe_app_request_render(void) { render_requests++; }
void owe_feed_stop(struct owe_feed *feed) { feed->active = false; }
void owe_mpv_set_muted(struct owe_mpv *mpv, bool muted) { mpv->muted = muted; }
void owe_mpv_stop(struct owe_mpv *mpv) { mpv->stopped = true; }
bool owe_still_busy(struct owe_still *still) { (void)still; return true; }
int owe_still_poll(struct owe_still *still) { return still->result; }

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
    owe_render_ipc_poll_still(&ipc);
    CHECK(!app.feeding && !feed.active && !mpv.muted && mpv.stopped && render_requests == 1);
    CHECK(strcmp(app.current_path, "/still.png") == 0 && strcmp(app.current_kind, "still") == 0);
    stop_feed(&app);
    CHECK(render_requests == 1);
    puts("renderer feed-to-still transition checks passed");
    return 0;
}

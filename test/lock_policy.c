#define main owed_main
#include "../src/daemon/main.c"
#undef main

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

struct owed_power { bool locked; bool sleeping; bool battery; };
struct owed_hypr { bool locked; bool off; };
struct owed_supervisor { int starts; int stops; bool feeding; bool paused; };

bool owed_power_locked(struct owed_power *p) { return p && p->locked; }
bool owed_power_sleeping(struct owed_power *p) { return p && p->sleeping; }
bool owed_power_on_battery(struct owed_power *p) { return p && p->battery; }
bool owed_hypr_locked(struct owed_hypr *h) { return h && h->locked; }
bool owed_hypr_all_monitors_off(struct owed_hypr *h) { return h && h->off; }
bool owed_hypr_any_fullscreen(struct owed_hypr *h) { (void)h; return false; }
bool owed_hypr_any_window_visible(struct owed_hypr *h) { (void)h; return false; }
void owed_ipc_broadcast(struct owed_ipc *ipc, const char *line) { (void)ipc; (void)line; }
int owed_render_is_alive(struct owed_supervisor *s) { return s != NULL; }
int owed_supervisor_feed_start(struct owed_supervisor *s) {
    s->starts++;
    s->feeding = true;
    s->paused = false;
    return 0;
}
int owed_supervisor_feed_stop(struct owed_supervisor *s) {
    s->stops++;
    s->feeding = false;
    return 0;
}
int owed_supervisor_pause(struct owed_supervisor *s) { s->paused = true; return 0; }
int owed_supervisor_resume(struct owed_supervisor *s) { s->paused = false; return 0; }
int owed_supervisor_load(struct owed_supervisor *s, const char *path, const char *kind) {
    (void)s; (void)path; (void)kind;
    return 0;
}

static void apply(void) {
    owed_policy_recompute(g_app.policy);
    apply_playback_state();
}

int main(void) {
    struct owed_power power = {0};
    struct owed_hypr hypr = {.locked = true};
    struct owed_supervisor renderer = {0};
    g_app.engine = OWE_ENGINE_RENDERER;
    g_app.power = &power;
    g_app.hypr = &hypr;
    g_app.supervisor = &renderer;
    g_app.policy = owed_policy_new();
    CHECK(g_app.policy);
    strcpy(g_app.config.renderer_mode, "always");
    strcpy(g_app.loaded_path, "/video.mp4");
    strcpy(g_app.loaded_kind, "video");
    apply();
    CHECK(renderer.feeding && !renderer.paused && renderer.starts == 1);
    apply();
    CHECK(renderer.starts == 1);

    hypr.off = true;
    apply();
    CHECK(!renderer.feeding && renderer.paused && renderer.stops == 1);
    apply();
    CHECK(renderer.starts == 1 && renderer.stops == 1);
    hypr.off = false;
    apply();
    CHECK(renderer.feeding && !renderer.paused && renderer.starts == 2);

    power.sleeping = true;
    apply();
    CHECK(!renderer.feeding && renderer.paused);
    power.sleeping = false;
    apply();
    CHECK(renderer.feeding && !renderer.paused);
    owed_policy_set_manual_pause(g_app.policy, true);
    apply();
    CHECK(!renderer.feeding && renderer.paused);
    owed_policy_set_manual_pause(g_app.policy, false);
    apply();
    CHECK(renderer.feeding && !renderer.paused);

    finish_media("/still.png", "still");
    CHECK(!renderer.feeding && !g_app.render_feeding);
    CHECK(strcmp(g_app.loaded_kind, "still") == 0);
    hypr.locked = false;
    owed_policy_recompute(g_app.policy);
    finish_media("/still.png", "still");
    CHECK(!renderer.feeding && !renderer.paused);

    /* A logind lock follows the same path as a Hyprland lock. */
    power.locked = true;
    owed_policy_recompute(g_app.policy);
    finish_media("/video.mp4", "video");
    CHECK(renderer.feeding);
    power.battery = true;
    strcpy(g_app.config.battery_mode, "poster");
    owed_policy_recompute(g_app.policy);
    finish_media("/poster.png", "still");
    CHECK(!renderer.feeding && !g_app.render_feeding);
    owed_policy_free(g_app.policy);
    puts("lock policy and still transition checks passed");
    return 0;
}

#define main owed_main
#include "../src/daemon/main.c"
#undef main

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

struct owed_power { bool locked; bool sleeping; bool battery; };
struct owed_hypr { bool locked; bool off; bool fullscreen; };
struct owed_supervisor { int starts; int stops; bool feeding; bool paused; bool fail_start; bool fail_load; int loads; };
static char covered_names[1024];
static int skip_sends;
static char supervisor_reply[1024] = "{\"status\":\"ok\"}";
static char supervisor_last[2048];
static bool supervisor_fails;
const char *owed_hypr_covered_names(struct owed_hypr *h) { (void)h; return covered_names; }
int owed_supervisor_send(struct owed_supervisor *s, const char *line, char *reply,
                         unsigned long reply_len) {
    yyjson_doc *doc;
    (void)s;
    snprintf(supervisor_last, sizeof(supervisor_last), "%s", line);
    doc = yyjson_read(line, strlen(line), 0);
    CHECK(doc);
    yyjson_doc_free(doc);
    skip_sends++;
    if (supervisor_fails) {
        return -1;
    }
    if (reply && reply_len > 0) {
        snprintf(reply, reply_len, "%s", supervisor_reply);
    }
    return 0;
}

bool owed_power_locked(struct owed_power *p) { return p && p->locked; }
bool owed_power_sleeping(struct owed_power *p) { return p && p->sleeping; }
bool owed_power_on_battery(struct owed_power *p) { return p && p->battery; }
bool owed_hypr_locked(struct owed_hypr *h) { return h && h->locked; }
bool owed_hypr_all_monitors_off(struct owed_hypr *h) { return h && h->off; }
bool owed_hypr_any_fullscreen(struct owed_hypr *h) { return h && h->fullscreen; }
bool owed_hypr_any_window_visible(struct owed_hypr *h) { (void)h; return false; }
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
int owed_supervisor_load(struct owed_supervisor *s, const char *path, const char *kind,
                         const char *from) {
    s->loads++; (void)path; (void)kind; (void)from;
    return s->fail_load ? -1 : 0;
}
int owed_supervisor_ensure_running(struct owed_supervisor *s) { s->starts++; return s->fail_start ? -1 : 0; }
int owed_supervisor_fade(struct owed_supervisor *s, int ms) { (void)s; (void)ms; return 0; }

static int shell_plugin_calls;
static bool shell_plugin_enabled;
static bool shell_plugin_fail;

int owed_shell_plugin_set(bool enabled) {
    shell_plugin_calls++;
    shell_plugin_enabled = enabled;
    return shell_plugin_fail ? -1 : 0;
}

int owed_transcode_gif_path(const char *gif_path, int fps, int crf, int max_w, int max_h,
                            char *out_mp4, unsigned long out_len) {
    (void)gif_path; (void)fps; (void)crf; (void)max_w; (void)max_h; (void)out_mp4; (void)out_len;
    return -1;
}
static bool poster_path_ok;
int owed_transcode_poster_path(const char *video_path, char *out_png, unsigned long out_len) {
    (void)video_path;
    snprintf(out_png, out_len, "/poster.png");
    return poster_path_ok ? 0 : -1;
}
bool owed_transcode_file_ready(const char *path) { (void)path; return false; }
owed_async_job_t *owed_async_gif(const char *gif_path, int fps, int crf, int max_w, int max_h) {
    (void)gif_path; (void)fps; (void)crf; (void)max_w; (void)max_h;
    return NULL;
}
owed_async_job_t *owed_async_poster(const char *video_path) { (void)video_path; return NULL; }
const char *owed_async_job_input(owed_async_job_t *job) { (void)job; return g_app.source_path; }
int owed_async_job_finish(owed_async_job_t *job, struct owed_async_result *result) {
    (void)job; result->ok = 0; return 0;
}
void owed_async_job_free(owed_async_job_t *job) { (void)job; }

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
    strcpy(g_app.config.battery_mode, "pause");
    apply();
    CHECK(!renderer.feeding && renderer.paused);
    g_app.always_animate = true;
    apply();
    CHECK(renderer.feeding && !renderer.paused);
    g_app.always_animate = false;
    power.battery = false;
    owed_policy_set_idle_pause(g_app.policy, true);
    apply();
    CHECK(!renderer.feeding && renderer.paused);
    owed_policy_set_idle_pause(g_app.policy, false);
    owed_policy_set_blocklisted(g_app.policy, true);
    apply();
    CHECK(!renderer.feeding && renderer.paused);
    owed_policy_set_blocklisted(g_app.policy, false);
    apply();
    CHECK(renderer.feeding && !renderer.paused);
    power.battery = true;
    strcpy(g_app.config.battery_mode, "poster");
    owed_policy_recompute(g_app.policy);
    finish_media("/poster.png", "still");
    CHECK(!renderer.feeding && !g_app.render_feeding);
    power.locked = false;
    owed_policy_recompute(g_app.policy);
    CHECK(owed_policy_should_pause(g_app.policy));
    CHECK(owed_policy_should_poster(g_app.policy));
    power.battery = false;
    owed_policy_recompute(g_app.policy);
    CHECK(!owed_policy_should_poster(g_app.policy));

    /* A still background belongs to the shell and never starts the renderer. */
    renderer.starts = 0;
    renderer.stops = 0;
    renderer.feeding = false;
    renderer.paused = false;
    g_app.engine = OWE_ENGINE_NONE;
    g_app.shell_enabled = 0;
    g_app.render_feeding = 0;
    strcpy(g_app.source_path, "/still.png");
    strcpy(g_app.source_kind, "still");
    g_app.loaded_path[0] = '\0';
    g_app.loaded_kind[0] = '\0';
    shell_plugin_calls = 0;
    owed_app_apply_policy();
    CHECK(g_app.engine == OWE_ENGINE_SHELL);
    CHECK(renderer.starts == 0 && !renderer.feeding);
    CHECK(shell_plugin_calls == 1 && shell_plugin_enabled);

    /* Failed startup retries without marking the media itself as failed. */
    strcpy(g_app.source_path, "/retry.mp4");
    strcpy(g_app.source_kind, "video");
    renderer.fail_start = true;
    owed_app_apply_policy();
    CHECK(!*g_app.fail_path && renderer.starts == 1);
    owed_app_apply_policy();
    CHECK(renderer.starts == 1);
    renderer.fail_start = false;
    g_app.renderer_retry_at_ms = 0;
    owed_app_apply_policy();
    CHECK(renderer.starts == 2 && !strcmp(g_app.loaded_path, "/retry.mp4"));

    /* A failed shell disable keeps the shell state and retries the handoff. */
    g_app.engine = OWE_ENGINE_SHELL;
    g_app.shell_enabled = 1;
    shell_plugin_fail = true;
    int loads_before_handoff = renderer.loads;
    owed_app_apply_policy();
    CHECK(g_app.engine == OWE_ENGINE_SHELL && g_app.shell_enabled == 1);
    CHECK(renderer.loads == loads_before_handoff && g_app.renderer_retry_at_ms > 0);
    shell_plugin_fail = false;
    g_app.renderer_retry_at_ms = 0;
    owed_app_apply_policy();
    CHECK(g_app.engine == OWE_ENGINE_RENDERER && g_app.shell_enabled == 0);

    /* A failed shell command is also rate limited while the still falls back. */
    strcpy(g_app.source_path, "/still.png");
    strcpy(g_app.source_kind, "still");
    shell_plugin_fail = true;
    int calls = shell_plugin_calls;
    owed_app_apply_policy();
    CHECK(shell_plugin_calls == calls + 1 && g_app.shell_retry_at_ms > 0);
    owed_app_apply_policy();
    CHECK(shell_plugin_calls == calls + 1);
    shell_plugin_fail = false;
    g_app.shell_retry_at_ms = 0;
    owed_app_apply_policy();
    CHECK(g_app.engine == OWE_ENGINE_SHELL);

    /* A new renderer receives feed state and media again. */
    g_app.engine = OWE_ENGINE_RENDERER;
    strcpy(g_app.source_path, "/retry.mp4");
    strcpy(g_app.source_kind, "video");
    power.locked = true;
    owed_policy_recompute(g_app.policy);
    g_app.render_feeding = 1;
    strcpy(g_last_skip, "DP-1");
    int starts = renderer.starts, loads = renderer.loads;
    owed_app_on_renderer_restarted();
    CHECK(renderer.starts == starts + 1 && renderer.loads == loads + 1);
    CHECK(g_app.render_feeding && !*g_last_skip && g_app.media_pending);
    /* Poster setup failures must not poison the playable source on AC. */
    power.locked = false;
    for (int path_ok = 0; path_ok <= 1; path_ok++) {
        poster_path_ok = path_ok;
        power.battery = true;
        g_app.loaded_path[0] = '\0';
        g_app.loaded_kind[0] = '\0';
        owed_app_on_policy_changed();
        CHECK(!*g_app.fail_path && !strcmp(g_app.poster_fail_path, g_app.source_path));
        power.battery = false;
        owed_app_on_policy_changed();
        CHECK(!*g_app.poster_fail_path && !strcmp(g_app.loaded_path, g_app.source_path));
        CHECK(!renderer.paused);
    }
    power.battery = true;
    g_app.job_is_poster = true;
    g_app.job = (owed_async_job_t *)(uintptr_t)1;
    owed_app_on_job_done();
    CHECK(!g_app.job && !*g_app.fail_path && *g_app.poster_fail_path);
    power.battery = false;
    owed_app_on_policy_changed();
    CHECK(!*g_app.poster_fail_path && !strcmp(g_app.loaded_path, g_app.source_path));

    /* Escaping monitor names must not overrun the JSON buffer. */
    memset(covered_names, 1, sizeof(covered_names) - 1);
    for (int i = 127; i < 1023; i += 128) covered_names[i] = ',';
    sync_output_skips();
    CHECK(skip_sends == 0 && !*g_last_skip);
    strcpy(covered_names, "DP-1,HDMI-A-1");
    sync_output_skips();
    CHECK(skip_sends == 1 && !strcmp(g_last_skip, covered_names));
    /* A rejected source still is not mistaken for a failed generated poster. */
    power.battery = true;
    renderer.fail_load = true;
    shell_plugin_fail = true;
    g_app.shell_enabled = 0;
    strcpy(g_app.source_path, "/corrupt.png");
    strcpy(g_app.source_kind, "still");
    loads = renderer.loads;
    owed_app_on_policy_changed();
    CHECK(!strcmp(g_app.fail_path, g_app.source_path) && !*g_app.poster_fail_path);
    CHECK(renderer.loads == loads + 1);

    /* A one-shot intro owns the renderer, plays once, and hands the still back. */
    power.locked = false;
    power.sleeping = false;
    power.battery = false;
    hypr.locked = false;
    hypr.off = false;
    hypr.fullscreen = false;
    renderer.fail_start = false;
    renderer.fail_load = false;
    shell_plugin_fail = false;
    g_app.shell_retry_at_ms = 0;
    g_app.renderer_retry_at_ms = 0;
    g_app.engine = OWE_ENGINE_SHELL;
    g_app.shell_enabled = 1;
    shell_plugin_enabled = true;
    shell_plugin_calls = 0;
    renderer.starts = 0;
    strcpy(supervisor_reply, "{\"status\":\"ok\"}");
    CHECK(owed_app_start_intro("/intro.mp4") == 0);
    CHECK(owed_app_intro_active());
    CHECK(strstr(supervisor_last, "\"once\":true") != NULL);
    CHECK(strstr(supervisor_last, "\"mute\":true") != NULL);
    CHECK(g_app.engine == OWE_ENGINE_RENDERER);
    CHECK(renderer.starts == 1);
    CHECK(shell_plugin_calls == 1 && !shell_plugin_enabled);

    strcpy(supervisor_reply, "{\"status\":\"ok\",\"ready\":true,\"error\":\"\",\"eof\":false}");
    owed_app_poll_intro();
    CHECK(owed_app_intro_active());
    CHECK(owed_app_start_intro("/other.mp4") != 0);
    strcpy(supervisor_reply, "{\"status\":\"ok\",\"ready\":true,\"error\":\"\",\"eof\":true}");
    owed_app_poll_intro();
    CHECK(!owed_app_intro_active());
    CHECK(strcmp(owed_app_intro_result(), "ok") == 0);
    CHECK(g_app.engine == OWE_ENGINE_SHELL);
    CHECK(shell_plugin_calls == 2 && shell_plugin_enabled);

    /* Locking interrupts an intro and reports a non-ok result. */
    g_app.engine = OWE_ENGINE_SHELL;
    g_app.shell_enabled = 1;
    shell_plugin_calls = 0;
    strcpy(supervisor_reply, "{\"status\":\"ok\",\"ready\":true,\"error\":\"\",\"eof\":false}");
    CHECK(owed_app_start_intro("/intro.mp4") == 0);
    power.locked = true;
    owed_app_poll_intro();
    CHECK(!owed_app_intro_active());
    CHECK(strcmp(owed_app_intro_result(), "error") == 0);
    CHECK(g_app.engine == OWE_ENGINE_SHELL);
    power.locked = false;

    /* A fullscreen window and an explicit stop also interrupt. */
    strcpy(supervisor_reply, "{\"status\":\"ok\",\"ready\":true,\"error\":\"\",\"eof\":false}");
    CHECK(owed_app_start_intro("/intro.mp4") == 0);
    hypr.fullscreen = true;
    owed_app_poll_intro();
    CHECK(!owed_app_intro_active());
    hypr.fullscreen = false;
    CHECK(owed_app_start_intro("/intro.mp4") == 0);
    owed_app_stop_intro("intro cancelled");
    CHECK(!owed_app_intro_active());
    CHECK(strcmp(owed_app_intro_result(), "error") == 0);

    /* A rejected renderer load leaves the shell drawing the still. */
    g_app.engine = OWE_ENGINE_SHELL;
    g_app.shell_enabled = 1;
    strcpy(supervisor_reply, "{\"status\":\"error\"}");
    CHECK(owed_app_start_intro("/intro.mp4") != 0);
    CHECK(!owed_app_intro_active());
    CHECK(g_app.engine == OWE_ENGINE_SHELL);
    strcpy(supervisor_reply, "{\"status\":\"ok\"}");
    CHECK(owed_app_start_intro("/intro.mp4") == 0);
    hypr.locked = true;
    owed_app_poll_intro();
    CHECK(!owed_app_intro_active() && g_app.engine == OWE_ENGINE_SHELL);
    hypr.locked = false;
    /* An expired poster cancels its load and restores the playable video. */
    g_app.engine = OWE_ENGINE_RENDERER;
    power.battery = true;
    strcpy(g_app.config.battery_mode, "poster");
    strcpy(g_app.source_path, "/video.mp4");
    strcpy(g_app.source_kind, "video");
    strcpy(g_app.loaded_path, "/poster.png");
    strcpy(g_app.loaded_kind, "still");
    strcpy(g_app.last_good_path, "/video.mp4");
    strcpy(g_app.last_good_kind, "video");
    strcpy(g_app.restore_path, "/video.mp4");
    strcpy(g_app.restore_kind, "video");
    g_app.fail_path[0] = g_app.poster_fail_path[0] = '\0';
    g_app.media_pending = true;
    g_app.media_ready = false;
    g_app.media_deadline_ms = monotonic_ms() - 1;
    loads = renderer.loads;
    check_media_ready();
    CHECK(strstr(supervisor_last, "cancel-load") != NULL);
    CHECK(!*g_app.fail_path && !strcmp(g_app.poster_fail_path, "/video.mp4"));
    CHECK(renderer.loads == loads + 1 && !strcmp(g_app.loaded_path, "/video.mp4"));
    CHECK(!g_app.media_ready && g_app.media_pending);
    strcpy(supervisor_reply, "{\"status\":\"ok\",\"path\":\"/wrong.mp4\",\"kind\":\"video\",\"ready\":true}");
    check_media_ready();
    CHECK(!g_app.media_ready && g_app.media_pending);
    strcpy(supervisor_reply, "{\"status\":\"ok\",\"path\":\"/video.mp4\",\"kind\":\"video\",\"ready\":true}");
    check_media_ready();
    CHECK(g_app.media_ready && !g_app.media_pending);
    owed_policy_free(g_app.policy);
    puts("lock policy and still transition checks passed");
    return 0;
}

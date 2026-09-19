#include "daemon.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "yyjson.h"

#include "hypr.h"
#include "daemon_ipc.h"
#include "common_ipc.h"
#include "json.h"
#include "log.h"
#include "policy.h"
#include "power.h"
#include "shell.h"
#include "strutil.h"
#include "supervisor.h"
#include "transcode.h"
#include "watch.h"
#include "xdg.h"

static owed_app_t g_app;
static int g_sigpipe[2];
static int g_sigchld[2];
static int g_lock_fd = -1;

owed_app_t *owed_app_get(void) {
    return &g_app;
}

void owed_app_emit_event(const char *name, const char *detail) {
    char *quoted_name = owe_json_quote(name ? name : "");
    char *quoted_detail = owe_json_quote(detail ? detail : "");
    char *line = NULL;
    if (!quoted_name || !quoted_detail) {
        free(quoted_name);
        free(quoted_detail);
        return;
    }
    if (asprintf(&line, "{\"event\":%s,\"detail\":%s}", quoted_name, quoted_detail) >= 0) {
        owed_ipc_broadcast(g_app.ipc, line);
    }
    free(line);
    free(quoted_name);
    free(quoted_detail);
}

static bool battery_poster_active(void) {
    owed_app_t *app = &g_app;
    return (app->config.battery_poster ||
            strcmp(app->config.battery_mode, "poster") == 0) &&
           app->power && owed_power_on_battery(app->power) &&
           !app->always_animate;
}

#define MEDIA_READY_TIMEOUT_MS 5000

static int64_t monotonic_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void copy_path(char *dst, size_t size, const char *src) {
    size_t len = strlen(src);
    if (len >= size) {
        len = size - 1;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static int load_if_needed(const char *path, const char *kind) {
    owed_app_t *app = &g_app;
    if (!path || !*path || !kind || !*kind) {
        return -1;
    }
    if (strcmp(app->loaded_path, path) == 0 && strcmp(app->loaded_kind, kind) == 0) {
        return 0;
    }
    if (owed_supervisor_load(app->supervisor, path, kind) != 0) {
        OWE_ERROR("renderer load failed: %s", path);
        /* The renderer keeps its previous media after a rejected load, so
         * loaded_path stays as the last successful media to keep policy in
         * control of what is playing. */
        copy_path(app->fail_path, sizeof(app->fail_path), app->source_path);
        return -1;
    }
    /* Snapshot the media that already proved it plays. A video whose decode
     * fails later falls back to it. */
    copy_path(app->restore_path, sizeof(app->restore_path), app->last_good_path);
    copy_path(app->restore_kind, sizeof(app->restore_kind), app->last_good_kind);
    copy_path(app->loaded_path, sizeof(app->loaded_path), path);
    copy_path(app->loaded_kind, sizeof(app->loaded_kind), kind);
    if (strcmp(kind, "video") == 0) {
        app->media_pending = true;
        app->media_deadline_ms = monotonic_ms() + MEDIA_READY_TIMEOUT_MS;
    } else {
        app->media_pending = false;
        copy_path(app->last_good_path, sizeof(app->last_good_path), path);
        copy_path(app->last_good_kind, sizeof(app->last_good_kind), kind);
    }
    return 0;
}

/* A locked session keeps its video: the renderer draws the wallpaper into the
 * lock feed instead of pausing, and the lock screen shows those frames. */
static int apply_feed_state(void) {
    owed_app_t *app = &g_app;
    bool locked;
    bool want_feed;
    if (!app->supervisor || app->engine != OWE_ENGINE_RENDERER) {
        return 0;
    }
    locked = (app->power && owed_power_locked(app->power)) ||
             (app->hypr && owed_hypr_locked(app->hypr));
    want_feed = locked && !owed_policy_manual_pause(app->policy) &&
                !(app->power && owed_power_sleeping(app->power)) &&
                strcmp(app->loaded_kind, "video") == 0 &&
                owed_render_is_alive(app->supervisor);
    if (want_feed) {
        if (!app->render_feeding) {
            if (owed_supervisor_feed_start(app->supervisor) != 0) {
                return 0;
            }
            app->render_feeding = 1;
            app->render_paused = 0;
        }
        return 1;
    }
    if (app->render_feeding) {
        owed_supervisor_feed_stop(app->supervisor);
        app->render_feeding = 0;
        app->render_paused = -1;
    }
    return 0;
}

static void apply_playback_state(void) {
    owed_app_t *app = &g_app;
    int want;
    if (!app->supervisor || app->engine != OWE_ENGINE_RENDERER) {
        return;
    }
    if (apply_feed_state()) {
        return;
    }
    want = owed_policy_should_pause(app->policy) ? 1 : 0;
    if (app->render_paused == want) {
        return;
    }
    if (want) {
        if (owed_supervisor_pause(app->supervisor) == 0) {
            app->render_paused = 1;
        }
    } else {
        if (owed_supervisor_resume(app->supervisor) == 0) {
            app->render_paused = 0;
        }
    }
}

const char *owed_app_engine(void) {
    switch (g_app.engine) {
    case OWE_ENGINE_SHELL:
        return "shell";
    case OWE_ENGINE_RENDERER:
        return "renderer";
    default:
        return "none";
    }
}

bool owed_app_renderer_expected(void) {
    return g_app.engine == OWE_ENGINE_RENDERER;
}

/* Let the shell draw the background and release the renderer, so a still
 * costs only the daemon. */
static int switch_to_shell(void) {
    owed_app_t *app = &g_app;
    if (app->shell_enabled != 1 && owed_shell_plugin_set(true) != 0) {
        return -1;
    }
    app->shell_enabled = 1;
    app->engine = OWE_ENGINE_SHELL;
    app->media_pending = false;
    app->render_paused = -1;
    app->render_feeding = 0;
    app->loaded_path[0] = '\0';
    app->loaded_kind[0] = '\0';
    app->restore_path[0] = '\0';
    app->shell_stop_at_ms = monotonic_ms() + 400;
    return 0;
}

/* Start the renderer. The shell plugin is disabled once the new media is
 * ready, so the handoff never shows a black frame. */
static int switch_to_renderer(void) {
    owed_app_t *app = &g_app;
    int64_t now = monotonic_ms();
    if (app->renderer_retry_at_ms && now < app->renderer_retry_at_ms) {
        return -1;
    }
    if (owed_supervisor_ensure_running(app->supervisor) != 0) {
        app->renderer_retry_at_ms = now + 30000;
        return -1;
    }
    app->renderer_retry_at_ms = 0;
    app->engine = OWE_ENGINE_RENDERER;
    app->shell_stop_at_ms = 0;
    /* Release the layer now. An occluded renderer gets no frame callbacks,
     * so waiting for readiness before the handoff would deadlock. */
    if (app->shell_enabled != 0) {
        owed_shell_plugin_set(false);
    }
    app->shell_enabled = 0;
    owed_supervisor_fade(app->supervisor, app->config.fade_ms);
    return 0;
}

static void process_shell_handoff(void) {
    owed_app_t *app = &g_app;
    if (app->shell_stop_at_ms && monotonic_ms() >= app->shell_stop_at_ms) {
        app->shell_stop_at_ms = 0;
        if (app->engine == OWE_ENGINE_SHELL && owed_render_is_alive(app->supervisor)) {
            OWE_INFO("renderer stopped, the shell draws the still");
            owed_supervisor_stop(app->supervisor);
        }
    }
}

static bool render_status_flags(const char *reply, bool *ready, bool *error) {
    yyjson_doc *doc = reply ? yyjson_read(reply, strlen(reply), 0) : NULL;
    bool ok = false;
    *ready = false;
    *error = false;
    if (doc) {
        yyjson_val *root = yyjson_doc_get_root(doc);
        yyjson_val *status = yyjson_obj_get(root, "status");
        yyjson_val *ready_val = yyjson_obj_get(root, "ready");
        yyjson_val *error_val = yyjson_obj_get(root, "error");
        if (yyjson_is_str(status) && strcmp(yyjson_get_str(status), "ok") == 0) {
            ok = true;
        }
        if (yyjson_is_bool(ready_val)) {
            *ready = yyjson_get_bool(ready_val);
        }
        if (yyjson_is_str(error_val) && yyjson_get_len(error_val) > 0) {
            *error = true;
        }
        yyjson_doc_free(doc);
    }
    return ok;
}

static void media_failed(void) {
    owed_app_t *app = &g_app;
    app->media_pending = false;
    copy_path(app->fail_path, sizeof(app->fail_path), app->source_path);
    OWE_WARN("media did not start: %s", app->source_path);
    if (*app->restore_path && strcmp(app->restore_path, app->source_path) != 0 &&
        strcmp(app->restore_path, app->loaded_path) != 0) {
        if (load_if_needed(app->restore_path, app->restore_kind) == 0) {
            OWE_INFO("recovered to %s", app->restore_path);
            apply_playback_state();
        }
    }
}

/* A video load reply only means the renderer accepted the request. Poll until
 * the first frame presents, or recover when decode fails or stalls. */
static void check_media_ready(void) {
    owed_app_t *app = &g_app;
    char reply[OWE_IPC_MAX_LINE];
    bool ready = false;
    bool error = false;
    if (app->engine != OWE_ENGINE_RENDERER || !app->media_pending) {
        return;
    }
    if (monotonic_ms() >= app->media_deadline_ms) {
        media_failed();
        return;
    }
    if (owed_supervisor_send(app->supervisor, "{\"cmd\":\"status\"}", reply, sizeof(reply)) != 0) {
        return;
    }
    if (!render_status_flags(reply, &ready, &error)) {
        return;
    }
    if (error) {
        media_failed();
        return;
    }
    if (ready) {
        app->media_pending = false;
        copy_path(app->last_good_path, sizeof(app->last_good_path), app->loaded_path);
        copy_path(app->last_good_kind, sizeof(app->last_good_kind), app->loaded_kind);
        OWE_INFO("media ready: %s", app->loaded_path);
    }
}

static char g_last_skip[1024];

/* Publish the monitors covered by a fullscreen window. The renderer stops
 * swapping buffers for those outputs, which avoids holding compositor
 * buffers the compositor is not consuming. */
static void sync_output_skips(void) {
    const char *covered;
    const char *cursor;
    char array[2048];
    char *line = NULL;
    size_t used = 0;
    if (!g_app.hypr || !g_app.supervisor || g_app.engine != OWE_ENGINE_RENDERER) {
        return;
    }
    covered = owed_hypr_covered_names(g_app.hypr);
    if (strcmp(covered, g_last_skip) == 0) {
        return;
    }
    used += (size_t)snprintf(array + used, sizeof(array) - used, "[");
    cursor = covered;
    while (*cursor && used < sizeof(array) - 3) {
        const char *end = strchr(cursor, ',');
        size_t len = end ? (size_t)(end - cursor) : strlen(cursor);
        char name[256];
        char *quoted;
        if (len >= sizeof(name)) {
            break;
        }
        memcpy(name, cursor, len);
        name[len] = '\0';
        quoted = owe_json_quote(name);
        if (quoted) {
            used += (size_t)snprintf(array + used, sizeof(array) - used, "%s%s",
                                     used > 1 ? "," : "", quoted);
            free(quoted);
        }
        cursor = end ? end + 1 : cursor + len;
    }
    snprintf(array + used, sizeof(array) - used, "]");
    if (asprintf(&line, "{\"cmd\":\"skip\",\"outputs\":%s}", array) >= 0 &&
        owed_supervisor_send(g_app.supervisor, line, NULL, 0) == 0) {
        copy_path(g_last_skip, sizeof(g_last_skip), covered);
    }
    free(line);
}

static void finish_media(const char *path, const char *kind, bool is_video) {
    if (load_if_needed(path, kind) != 0) {
        return;
    }
    if (!is_video) {
        return;
    }
    apply_playback_state();
}

void owed_app_apply_policy(void) {
    owed_app_t *app = &g_app;
    char cache[PATH_MAX];
    char video[PATH_MAX];
    bool shell_engine;

    if (!*app->source_path) {
        return;
    }
    /* A still background does not need a renderer. The shell draws it while
     * the renderer stays stopped, unless renderer_mode is "always". */
    shell_engine = strcmp(app->config.renderer_mode, "always") != 0 &&
                   strcmp(app->source_kind, "still") == 0;
    if (shell_engine) {
        if (app->engine != OWE_ENGINE_SHELL) {
            if (switch_to_shell() == 0) {
                OWE_INFO("engine: shell");
                return;
            }
            OWE_WARN("shell handoff failed, keeping the renderer for the still");
        } else {
            return;
        }
    }
    if (app->engine != OWE_ENGINE_RENDERER) {
        if (switch_to_renderer() != 0) {
            OWE_ERROR("renderer start failed");
            copy_path(app->fail_path, sizeof(app->fail_path), app->source_path);
            return;
        }
        OWE_INFO("engine: renderer");
    }
    if (app->job) {
        if (strcmp(app->loaded_kind, "video") == 0 && owed_policy_should_pause(app->policy))
            apply_playback_state();
        return;
    }
    if (strcmp(app->fail_path, app->source_path) == 0) {
        /* The new source failed. The renderer still shows the previous
         * media, so keep pause policy in control of it. */
        apply_playback_state();
        return;
    }
    copy_path(video, sizeof(video), app->source_path);

    if (strcmp(app->source_kind, "gif") == 0) {
        if (owed_transcode_gif_path(app->source_path, app->config.gif_fps, app->config.gif_crf,
                                    app->config.transcode_max_width,
                                    app->config.transcode_max_height, cache, sizeof(cache)) != 0) {
            OWE_ERROR("cannot resolve gif cache path: %s", app->source_path);
            return;
        }
        if (owed_transcode_file_ready(cache)) {
            snprintf(video, sizeof(video), "%s", cache);
        } else {
            app->job = owed_async_gif(app->source_path, app->config.gif_fps, app->config.gif_crf,
                                      app->config.transcode_max_width,
                                      app->config.transcode_max_height);
            if (!app->job) {
                OWE_ERROR("cannot start gif job: %s", app->source_path);
                copy_path(app->fail_path, sizeof(app->fail_path), app->source_path);
            } else {
                OWE_INFO("gif transcode started for %s", app->source_path);
            }
            return;
        }
    }

    if (strcmp(app->source_kind, "video") != 0 && strcmp(app->source_kind, "gif") != 0) {
        finish_media(app->source_path, "still", false);
        return;
    }

    if (battery_poster_active()) {
        if (owed_transcode_poster_path(video, cache, sizeof(cache)) != 0) {
            OWE_ERROR("cannot resolve poster path: %s", app->source_path);
            return;
        }
        if (owed_transcode_file_ready(cache)) {
            app->fail_path[0] = '\0';
            finish_media(cache, "still", false);
            return;
        }
        if (strcmp(app->fail_path, app->source_path) == 0) {
            return;
        }
        if (strcmp(app->loaded_kind, "video") == 0) apply_playback_state();
        app->job = owed_async_poster(video);
        if (!app->job) {
            OWE_ERROR("cannot start poster job: %s", app->source_path);
        } else {
            OWE_INFO("poster extraction started for %s", app->source_path);
        }
        return;
    }

    app->fail_path[0] = '\0';
    finish_media(video, "video", true);
}

void owed_app_on_job_done(void) {
    owed_app_t *app = &g_app;
    struct owed_async_result result;
    char input[PATH_MAX];

    if (!app->job) {
        return;
    }
    snprintf(input, sizeof(input), "%s", owed_async_job_input(app->job));
    if (owed_async_job_finish(app->job, &result) != 0) {
        OWE_WARN("job for %s produced no result", input);
        copy_path(app->fail_path, sizeof(app->fail_path), app->source_path);
    } else if (!result.ok) {
        OWE_ERROR("job failed for %s", input);
        copy_path(app->fail_path, sizeof(app->fail_path), app->source_path);
    } else {
        OWE_INFO("job done for %s", input);
    }
    owed_async_job_free(app->job);
    app->job = NULL;
    owed_app_apply_policy();
}

void owed_app_on_renderer_restarted(void) {
    g_app.loaded_path[0] = '\0';
    g_app.loaded_kind[0] = '\0';
    g_app.render_paused = -1;
    g_app.render_feeding = 0;
    g_app.fail_path[0] = '\0';
    g_app.media_pending = false;
    g_last_skip[0] = '\0';
    owed_supervisor_fade(g_app.supervisor, g_app.config.fade_ms);
    owed_app_apply_policy();
}

void owed_app_on_background_changed(const char *resolved_path) {
    owed_app_t *app = &g_app;
    owe_media_kind_t kind;

    if (!resolved_path || !*resolved_path) {
        return;
    }
    kind = owe_kind_from_path(resolved_path);
    if (kind == OWE_KIND_UNKNOWN) {
        OWE_WARN("unknown media kind: %s", resolved_path);
        owed_app_emit_event("error", "unknown media kind");
        return;
    }
    if (strcmp(app->source_path, resolved_path) == 0) {
        return;
    }
    if (app->job) {
        owed_async_job_free(app->job);
        app->job = NULL;
    }
    app->source_generation++;
    snprintf(app->source_path, sizeof(app->source_path), "%s", resolved_path);
    snprintf(app->source_kind, sizeof(app->source_kind), "%s", owe_kind_to_string(kind));
    app->fail_path[0] = '\0';
    owed_policy_recompute(app->policy);
    owed_app_apply_policy();
    owed_app_emit_event("background", resolved_path);
}

void owed_app_on_policy_changed(void) {
    owed_policy_recompute(g_app.policy);
    owed_app_apply_policy();
}

static void apply_manual_pause(void) {
    owed_app_t *app = &g_app;
    if (strcmp(app->loaded_kind, "video") != 0 || !*app->loaded_path) {
        return;
    }
    apply_playback_state();
}

static void on_signal(int sig) {
    int saved_errno = errno;
    char c = (char)sig;
    ssize_t n = write(g_sigpipe[1], &c, 1);
    (void)n;
    errno = saved_errno;
}

static void on_sigchld(int sig) {
    int saved_errno = errno;
    char c = (char)sig;
    ssize_t n = write(g_sigchld[1], &c, 1);
    (void)n;
    errno = saved_errno;
}

static bool blocklist_active(void) {
    owed_app_t *app = &g_app;
    DIR *dir;
    struct dirent *ent;
    bool found = false;

    if (app->config.blocklist_count <= 0) {
        return false;
    }
    dir = opendir("/proc");
    if (!dir) {
        return false;
    }
    while (!found && (ent = readdir(dir)) != NULL) {
        char path[320];
        char comm[128];
        FILE *f;
        int i;

        if (ent->d_name[0] < '0' || ent->d_name[0] > '9') {
            continue;
        }
        snprintf(path, sizeof(path), "/proc/%s/comm", ent->d_name);
        f = fopen(path, "r");
        if (!f) {
            continue;
        }
        if (!fgets(comm, sizeof(comm), f)) {
            fclose(f);
            continue;
        }
        fclose(f);
        comm[strcspn(comm, "\n")] = '\0';
        for (i = 0; i < app->config.blocklist_count; i++) {
            if (strcmp(comm, app->config.blocklist[i]) == 0) {
                OWE_INFO("blocklist match: %s", comm);
                found = true;
                break;
            }
        }
    }
    closedir(dir);
    return found;
}

static bool every_ms(int64_t *last_ms, int interval_ms) {
    struct timespec now;
    int64_t now_ms;
    clock_gettime(CLOCK_MONOTONIC, &now);
    now_ms = now.tv_sec * 1000L + now.tv_nsec / 1000000L;
    if (*last_ms == 0 || now_ms - *last_ms >= interval_ms) {
        *last_ms = now_ms;
        return true;
    }
    return false;
}

static void usage(const char *argv0) {
    fprintf(stderr,
            "Usage: %s [options]\n"
            "  --socket PATH   IPC socket path (default: $XDG_RUNTIME_DIR/owe/owed.sock)\n"
            "  --verbose       Enable debug logging\n"
            "  --help          Show this help\n",
            argv0);
}

int main(int argc, char **argv) {
    char socket_path[PATH_MAX];
    char config_path[PATH_MAX];
    char lock_path[PATH_MAX + 32];
    char runtime[PATH_MAX];
    bool verbose = false;
    bool blocklisted = false;
    int64_t tick_ms = 0;
    int i;
    char resolved[PATH_MAX];

    if (owe_socket_path_daemon(socket_path, sizeof(socket_path)) != 0) {
        fprintf(stderr, "owed: cannot resolve socket path\n");
        return 1;
    }
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
            snprintf(socket_path, sizeof(socket_path), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--verbose") == 0 || strcmp(argv[i], "-v") == 0) {
            verbose = true;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 1;
        }
    }

    owe_log_init("owed", verbose ? OWE_LOG_DEBUG : OWE_LOG_INFO);

    if (owe_xdg_runtime_dir(runtime, sizeof(runtime)) != 0) {
        return 1;
    }
    snprintf(lock_path, sizeof(lock_path), "%s/owe", runtime);
    owe_mkdir_p(lock_path);
    snprintf(lock_path, sizeof(lock_path), "%s/owe/owed.lock", runtime);
    g_lock_fd = open(lock_path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (g_lock_fd < 0 || flock(g_lock_fd, LOCK_EX | LOCK_NB) != 0) {
        fprintf(stderr, "owed: another instance is already running\n");
        if (g_lock_fd >= 0) {
            close(g_lock_fd);
        }
        return 1;
    }

    if (pipe2(g_sigpipe, O_NONBLOCK | O_CLOEXEC) != 0 ||
        pipe2(g_sigchld, O_NONBLOCK | O_CLOEXEC) != 0) {
        OWE_ERROR("pipe failed");
        return 1;
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, on_sigchld);

    memset(&g_app, 0, sizeof(g_app));
    g_app.running = true;
    g_app.render_paused = -1;
    owe_config_defaults(&g_app.config);
    if (owe_config_path(config_path, sizeof(config_path)) == 0) {
        if (owe_config_load(&g_app.config, config_path) == 0) {
            OWE_INFO("config loaded from %s", config_path);
        } else {
            OWE_INFO("no config at %s, using defaults", config_path);
        }
    }
    owed_transcode_set_cache_limit(g_app.config.cache_max_mb);

    g_app.supervisor = owed_supervisor_new();
    g_app.policy = owed_policy_new();
    g_app.watch = owed_watch_new();
    g_app.hypr = owed_hypr_new();
    g_app.power = owed_power_new();
    if (!g_app.supervisor || !g_app.policy || !g_app.watch) {
        OWE_ERROR("daemon init failed");
        return 1;
    }
    g_app.ipc = owed_ipc_new(socket_path);
    if (!g_app.ipc) {
        OWE_ERROR("ipc init failed");
        return 1;
    }
    g_app.engine = OWE_ENGINE_NONE;
    g_app.shell_enabled = -1;
    owed_policy_recompute(g_app.policy);

    if (owed_watch_resolve_current(resolved, sizeof(resolved)) == 0) {
        owed_app_on_background_changed(resolved);
    } else {
        OWE_WARN("no current background symlink yet");
    }

    OWE_INFO("ready, socket=%s", socket_path);

    while (g_app.running) {
        struct pollfd pfds[10 + OWE_IPC_MAX_CLIENTS];
        int n = 0;
        int rc;
        int have_job = g_app.job != NULL;
        unsigned long job_generation = g_app.source_generation;

        pfds[n].fd = g_sigpipe[0];
        pfds[n].events = POLLIN;
        n++;
        pfds[n].fd = g_sigchld[0];
        pfds[n].events = POLLIN;
        n++;
        if (owed_watch_fd(g_app.watch) >= 0) {
            pfds[n].fd = owed_watch_fd(g_app.watch);
            pfds[n].events = POLLIN;
            n++;
        }
        if (owed_hypr_event_fd(g_app.hypr) >= 0) {
            pfds[n].fd = owed_hypr_event_fd(g_app.hypr);
            pfds[n].events = POLLIN;
            n++;
        }
        if (owed_power_fd_system(g_app.power) >= 0) {
            pfds[n].fd = owed_power_fd_system(g_app.power);
            pfds[n].events = POLLIN;
            n++;
        }
        if (owed_ipc_fd(g_app.ipc) >= 0) {
            pfds[n].fd = owed_ipc_fd(g_app.ipc);
            pfds[n].events = POLLIN;
            n++;
        }
        if (have_job) {
            pfds[n].fd = owed_async_job_fd(g_app.job);
            pfds[n].events = POLLIN;
            n++;
        }
        n += owed_ipc_pollfds(g_app.ipc, &pfds[n]);

        rc = poll(pfds, (nfds_t)n, (have_job || g_app.media_pending) ? 200 : 1000);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            OWE_ERROR("poll failed: %s", strerror(errno));
            break;
        }
        for (i = 0; i < n; i++) {
            if (!(pfds[i].revents & (POLLIN | POLLHUP))) {
                continue;
            }
            if (pfds[i].fd == g_sigpipe[0]) {
                char c;
                while (read(g_sigpipe[0], &c, 1) == 1) {
                }
                g_app.running = false;
                break;
            } else if (pfds[i].fd == g_sigchld[0]) {
                char c;
                while (read(g_sigchld[0], &c, 1) == 1) {
                }
                owed_supervisor_reap(g_app.supervisor);
                if (g_app.running && g_app.engine == OWE_ENGINE_RENDERER &&
                    !owed_render_is_alive(g_app.supervisor)) {
                    OWE_WARN("renderer died, restarting");
                    if (owed_supervisor_ensure_running(g_app.supervisor) == 0) {
                        owed_app_on_renderer_restarted();
                    } else {
                        OWE_ERROR("renderer restart failed");
                    }
                }
            } else if (pfds[i].fd == owed_watch_fd(g_app.watch)) {
                owed_watch_poll(g_app.watch);
            } else if (pfds[i].fd == owed_hypr_event_fd(g_app.hypr)) {
                owed_hypr_poll(g_app.hypr);
            } else if (pfds[i].fd == owed_power_fd_system(g_app.power)) {
                owed_power_poll(g_app.power);
            } else if (pfds[i].fd == owed_ipc_fd(g_app.ipc)) {
                owed_ipc_accept(g_app.ipc);
            } else if (have_job && job_generation == g_app.source_generation &&
                       pfds[i].fd == owed_async_job_fd(g_app.job)) {
                owed_app_on_job_done();
            }
        }
        if (!g_app.running) break;
        owed_ipc_poll_clients(g_app.ipc);
        if (!g_app.running) break;
        check_media_ready();
        sync_output_skips();
        process_shell_handoff();
        if (g_app.engine == OWE_ENGINE_RENDERER &&
            !owed_render_is_alive(g_app.supervisor) &&
            owed_supervisor_ensure_running(g_app.supervisor) == 0)
            owed_app_on_renderer_restarted();
        owed_app_apply_policy();

        if (every_ms(&tick_ms, 2000)) {
            owed_hypr_tick(g_app.hypr);
            if (blocklist_active() != blocklisted) {
                blocklisted = !blocklisted;
                owed_policy_set_blocklisted(g_app.policy, blocklisted);
                apply_manual_pause();
            }
        }
    }

    OWE_INFO("shutdown");
    if (g_app.shell_enabled == 0) {
        owed_shell_plugin_set(true);
    }
    if (g_app.job) {
        owed_async_job_free(g_app.job);
        g_app.job = NULL;
    }
    owed_ipc_free(g_app.ipc);
    owed_supervisor_free(g_app.supervisor);
    owed_power_free(g_app.power);
    owed_hypr_free(g_app.hypr);
    owed_watch_free(g_app.watch);
    owed_policy_free(g_app.policy);
    close(g_sigpipe[0]);
    close(g_sigpipe[1]);
    close(g_sigchld[0]);
    close(g_sigchld[1]);
    if (g_lock_fd >= 0) {
        close(g_lock_fd);
    }
    return 0;
}

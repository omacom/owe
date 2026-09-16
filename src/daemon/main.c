#include "daemon.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "hypr.h"
#include "daemon_ipc.h"
#include "log.h"
#include "policy.h"
#include "power.h"
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
    char line[1024];
    snprintf(line, sizeof(line), "{\"event\":\"%s\",\"detail\":\"%s\"}", name ? name : "",
             detail ? detail : "");
    owed_ipc_broadcast(g_app.ipc, line);
}

static bool battery_poster_active(void) {
    owed_app_t *app = &g_app;
    return app->config.battery_poster && app->power && owed_power_on_battery(app->power) &&
           !app->always_animate;
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
        app->loaded_path[0] = '\0';
        app->loaded_kind[0] = '\0';
        return -1;
    }
    snprintf(app->loaded_path, sizeof(app->loaded_path), "%s", path);
    snprintf(app->loaded_kind, sizeof(app->loaded_kind), "%s", kind);
    return 0;
}

static void apply_playback_state(void) {
    owed_app_t *app = &g_app;
    int want;
    if (!app->supervisor) {
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

    if (app->job) {
        return;
    }
    if (!*app->source_path) {
        return;
    }

    if (strcmp(app->source_kind, "gif") == 0) {
        if (owed_transcode_gif_path(app->source_path, app->config.gif_fps, app->config.gif_crf,
                                    app->config.transcode_max_width,
                                    app->config.transcode_max_height, cache, sizeof(cache)) != 0) {
            OWE_ERROR("cannot resolve gif cache path: %s", app->source_path);
            return;
        }
        if (owed_transcode_file_ready(cache)) {
            app->fail_path[0] = '\0';
            finish_media(cache, "video", true);
            return;
        }
        if (strcmp(app->fail_path, app->source_path) == 0) {
            return;
        }
        app->job = owed_async_gif(app->source_path, app->config.gif_fps, app->config.gif_crf,
                                  app->config.transcode_max_width,
                                  app->config.transcode_max_height);
        if (!app->job) {
            OWE_ERROR("cannot start gif job: %s", app->source_path);
        } else {
            OWE_INFO("gif transcode started for %s", app->source_path);
        }
        return;
    }

    if (strcmp(app->source_kind, "video") != 0) {
        finish_media(app->source_path, "still", false);
        return;
    }

    if (battery_poster_active()) {
        if (owed_transcode_poster_path(app->source_path, cache, sizeof(cache)) != 0) {
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
        app->job = owed_async_poster(app->source_path);
        if (!app->job) {
            OWE_ERROR("cannot start poster job: %s", app->source_path);
        } else {
            OWE_INFO("poster extraction started for %s", app->source_path);
        }
        return;
    }

    app->fail_path[0] = '\0';
    finish_media(app->source_path, "video", true);
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
        if (strcmp(app->source_path, input) == 0) {
            snprintf(app->fail_path, sizeof(app->fail_path), "%s", input);
        }
    } else if (!result.ok) {
        OWE_ERROR("job failed for %s", input);
        if (strcmp(app->source_path, input) == 0) {
            snprintf(app->fail_path, sizeof(app->fail_path), "%s", input);
        }
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
    char c = (char)sig;
    ssize_t n = write(g_sigpipe[1], &c, 1);
    (void)n;
}

static void on_sigchld(int sig) {
    char c = (char)sig;
    ssize_t n = write(g_sigchld[1], &c, 1);
    (void)n;
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

static bool every_ms(int *last_ms, int interval_ms) {
    struct timespec now;
    long now_ms;
    clock_gettime(CLOCK_MONOTONIC, &now);
    now_ms = now.tv_sec * 1000L + now.tv_nsec / 1000000L;
    if (*last_ms == 0 || now_ms - *last_ms >= interval_ms) {
        *last_ms = (int)now_ms;
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
    int tick_ms = 0;
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
    owe_config_defaults(&g_app.config);
    if (owe_config_path(config_path, sizeof(config_path)) == 0) {
        if (owe_config_load(&g_app.config, config_path) == 0) {
            OWE_INFO("config loaded from %s", config_path);
        } else {
            OWE_INFO("no config at %s, using defaults", config_path);
        }
    }

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
    if (owed_supervisor_ensure_running(g_app.supervisor) != 0) {
        OWE_ERROR("renderer start failed");
        return 1;
    }
    owed_supervisor_fade(g_app.supervisor, g_app.config.fade_ms);
    owed_policy_recompute(g_app.policy);

    if (owed_watch_resolve_current(resolved, sizeof(resolved)) == 0) {
        owed_app_on_background_changed(resolved);
    } else {
        OWE_WARN("no current background symlink yet");
    }

    OWE_INFO("ready, socket=%s", socket_path);

    while (g_app.running) {
        struct pollfd pfds[10];
        int n = 0;
        int rc;
        int have_job = g_app.job != NULL;

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
        if (owed_power_fd_session(g_app.power) >= 0) {
            pfds[n].fd = owed_power_fd_session(g_app.power);
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

        rc = poll(pfds, (nfds_t)n, have_job ? 200 : 1000);
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
            } else if (pfds[i].fd == g_sigchld[0]) {
                char c;
                while (read(g_sigchld[0], &c, 1) == 1) {
                }
                owed_supervisor_reap(g_app.supervisor);
                if (!owed_render_is_alive(g_app.supervisor)) {
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
            } else if (pfds[i].fd == owed_power_fd_system(g_app.power) ||
                       pfds[i].fd == owed_power_fd_session(g_app.power)) {
                owed_power_poll(g_app.power);
            } else if (pfds[i].fd == owed_ipc_fd(g_app.ipc)) {
                owed_ipc_accept(g_app.ipc);
            } else if (have_job && pfds[i].fd == owed_async_job_fd(g_app.job)) {
                owed_app_on_job_done();
            }
        }
        owed_ipc_poll_clients(g_app.ipc);
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

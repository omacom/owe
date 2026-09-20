#include "daemon_ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "yyjson.h"

#include "common_ipc.h"
#include "daemon.h"
#include "hypr.h"
#include "common_ipc.h"
#include "log.h"
#include "policy.h"
#include "power.h"
#include "owe_spawn.h"
#include "supervisor.h"
#include "transcode.h"
#include "watch.h"
#include "json.h"
#include "xdg.h"

#define MAX_CLIENTS OWE_IPC_MAX_CLIENTS
#define CLIENT_IDLE_MS 120000

struct owed_client {
    int fd;
    char buf[OWE_IPC_MAX_LINE];
    size_t len;
    int64_t active_ms;
};

struct owed_ipc {
    owe_ipc_server_t *srv;
    char path[PATH_MAX];
    struct owed_client clients[MAX_CLIENTS];
};

static int64_t now_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void client_remove(struct owed_ipc *ipc, int idx) {
    if (ipc->clients[idx].fd >= 0) {
        close(ipc->clients[idx].fd);
    }
    ipc->clients[idx].fd = -1;
    ipc->clients[idx].len = 0;
    ipc->clients[idx].active_ms = 0;
}

static void send_ok(struct owed_client *c, const char *extra) {
    char line[OWE_IPC_MAX_LINE];
    if (extra && *extra) {
        snprintf(line, sizeof(line), "{\"status\":\"ok\",%s}", extra);
    } else {
        snprintf(line, sizeof(line), "{\"status\":\"ok\"}");
    }
    owe_ipc_send_line(c->fd, line);
}

static void send_err(struct owed_client *c, const char *msg) {
    char line[1024];
    snprintf(line, sizeof(line), "{\"status\":\"error\",\"message\":\"%s\"}", msg ? msg : "failed");
    owe_ipc_send_line(c->fd, line);
}

static void handle_status(struct owed_client *c) {
    owed_app_t *app = owed_app_get();
    char *line = NULL;
    char *source = owe_json_quote(app->source_path);
    char *loaded = owe_json_quote(app->loaded_path);
    char *failed = owe_json_quote(app->fail_path);
    if (!source || !loaded || !failed) goto done;
    if (asprintf(&line,
             "{\"status\":\"ok\",\"source_path\":%s,\"source_kind\":\"%s\","
             "\"loaded_path\":%s,\"loaded_kind\":\"%s\","
             "\"job_running\":%s,\"failed_path\":%s,\"media_ready\":%s,\"engine\":\"%s\","
             "\"paused\":%s,\"reason\":\"%s\",\"always_animate\":%s,\"manual_pause\":%s,\"idle_pause\":%s,"
             "\"fullscreen\":%s,\"window_visible\":%s,\"monitors_off\":%s,\"monitor_count\":%d,"
             "\"on_battery\":%s,\"locked\":%s,\"render_alive\":%s}",
             source, app->source_kind, loaded, app->loaded_kind,
             app->job ? "true" : "false", failed, app->media_pending ? "false" : "true",
             owed_app_engine(),
             app->policy && owed_policy_should_pause(app->policy) ? "true" : "false",
             app->policy ? owed_policy_reason(app->policy) : "",
             app->always_animate ? "true" : "false",
             app->policy && owed_policy_manual_pause(app->policy) ? "true" : "false",
             app->policy && owed_policy_idle_pause(app->policy) ? "true" : "false",
             app->hypr && owed_hypr_any_fullscreen(app->hypr) ? "true" : "false",
             app->hypr && owed_hypr_any_window_visible(app->hypr) ? "true" : "false",
             app->hypr && owed_hypr_all_monitors_off(app->hypr) ? "true" : "false",
             app->hypr ? owed_hypr_monitor_count(app->hypr) : 0,
             app->power && owed_power_on_battery(app->power) ? "true" : "false",
             (app->power && owed_power_locked(app->power)) ||
                     (app->hypr && owed_hypr_locked(app->hypr))
                 ? "true"
                 : "false",
             app->supervisor && owed_render_is_alive(app->supervisor) ? "true" : "false") >= 0)
        owe_ipc_send_line(c->fd, line);
done:
    free(line);
    free(source);
    free(loaded);
    free(failed);
}

static void handle_config(struct owed_client *c) {
    owed_app_t *app = owed_app_get();
    char *blocklist = NULL;
    char *line = NULL;
    int i;
    if (asprintf(&blocklist, "[") < 0) {
        return;
    }
    for (i = 0; i < app->config.blocklist_count; i++) {
        char *quoted = owe_json_quote(app->config.blocklist[i]);
        char *next = NULL;
        if (!quoted || asprintf(&next, "%s%s%s", blocklist, i ? "," : "", quoted) < 0) {
            free(quoted);
            free(blocklist);
            free(next);
            return;
        }
        free(quoted);
        free(blocklist);
        blocklist = next;
    }
    {
        char *closed = NULL;
        if (asprintf(&closed, "%s]", blocklist) >= 0) {
            free(blocklist);
            blocklist = closed;
        }
    }
    if (asprintf(&line,
                 "{\"status\":\"ok\",\"pause_fullscreen\":%s,\"pause_occupied_workspace\":%s,"
                 "\"battery_poster\":%s,\"battery_mode\":\"%s\","
                 "\"gif_fps\":%d,\"gif_crf\":%d,"
                 "\"max_width\":%d,\"max_height\":%d,\"cache_max_mb\":%d,"
                 "\"fade_ms\":%d,\"blocklist_count\":%d,\"blocklist\":%s}",
                 app->config.pause_fullscreen ? "true" : "false",
                 app->config.pause_occupied_workspace ? "true" : "false",
                 app->config.battery_poster ? "true" : "false", app->config.battery_mode,
                 app->config.gif_fps,
                 app->config.gif_crf, app->config.transcode_max_width,
                 app->config.transcode_max_height, app->config.cache_max_mb,
                 app->config.fade_ms, app->config.blocklist_count, blocklist) >= 0) {
        owe_ipc_send_line(c->fd, line);
    }
    free(line);
    free(blocklist);
}

static void handle_set(struct owed_client *c, yyjson_val *root) {
    yyjson_val *v = yyjson_obj_get(root, "path");
    const char *path = v && yyjson_is_str(v) ? yyjson_get_str(v) : "";
    if (!owe_json_path(v)) {
        send_err(c, "Invalid path");
        return;
    }
    if (owed_watch_set_current(path) != 0) {
        send_err(c, "Cannot set background");
        return;
    }
    send_ok(c, NULL);
}

static void handle_command(struct owed_ipc *ipc, struct owed_client *c, const char *line) {
    owed_app_t *app = owed_app_get();
    yyjson_doc *doc;
    yyjson_val *root;
    yyjson_val *vcmd;
    const char *cmd;
    (void)ipc;
    doc = yyjson_read(line, strlen(line), 0);
    if (!doc || !yyjson_is_obj(yyjson_doc_get_root(doc))) {
        send_err(c, "bad json");
        yyjson_doc_free(doc);
        return;
    }
    root = yyjson_doc_get_root(doc);
    vcmd = root ? yyjson_obj_get(root, "cmd") : NULL;
    cmd = vcmd && yyjson_is_str(vcmd) ? yyjson_get_str(vcmd) : "";
    if (strcmp(cmd, "hello") == 0) {
        send_ok(c, "\"version\":1");
    } else if (strcmp(cmd, "status") == 0) {
        handle_status(c);
    } else if (strcmp(cmd, "config") == 0) {
        handle_config(c);
    } else if (strcmp(cmd, "set") == 0) {
        handle_set(c, root);
    } else if (strcmp(cmd, "refresh") == 0) {
        char resolved[4096];
        if (owed_watch_resolve_current(resolved, sizeof(resolved)) == 0) {
            app->source_path[0] = '\0';
            app->loaded_path[0] = '\0';
            owed_app_on_background_changed(resolved);
            send_ok(c, NULL);
        } else {
            send_err(c, "symlink unreadable");
        }
    } else if (strcmp(cmd, "pause") == 0) {
        owed_policy_set_manual_pause(app->policy, true);
        owed_app_on_policy_changed();
        send_ok(c, NULL);
    } else if (strcmp(cmd, "resume") == 0) {
        owed_policy_set_manual_pause(app->policy, false);
        owed_app_on_policy_changed();
        send_ok(c, NULL);
    } else if (strcmp(cmd, "idle-pause") == 0) {
        owed_policy_set_idle_pause(app->policy, true);
        owed_app_on_policy_changed();
        send_ok(c, NULL);
    } else if (strcmp(cmd, "idle-resume") == 0) {
        owed_policy_set_idle_pause(app->policy, false);
        owed_app_on_policy_changed();
        send_ok(c, NULL);
    } else if (strcmp(cmd, "always-animate") == 0) {
        yyjson_val *v = yyjson_obj_get(root, "value");
        if (v && yyjson_is_bool(v)) {
            app->always_animate = yyjson_get_bool(v);
            owed_app_on_policy_changed();
            send_ok(c, NULL);
        } else {
            send_err(c, "missing value");
        }
    } else if (strcmp(cmd, "reload-config") == 0) {
        char path[4096];
        owe_config_t next;
        owe_config_defaults(&next);
        if (owe_config_path(path, sizeof(path)) == 0 && owe_config_load(&next, path) == 0) {
            app->config = next;
            owed_transcode_set_cache_limit(app->config.cache_max_mb);
            owed_supervisor_fade(app->supervisor, app->config.fade_ms);
            owed_app_on_policy_changed();
            send_ok(c, NULL);
        } else {
            send_err(c, "config load failed");
        }
    } else if (strcmp(cmd, "render-status") == 0) {
        if (!owed_app_renderer_expected()) {
            char *line = NULL;
            char *path = owe_json_quote(app->source_path);
            char *kind = owe_json_quote(app->source_kind);
            bool video = strcmp(app->source_kind, "video") == 0 ||
                         strcmp(app->source_kind, "gif") == 0;
            if (path && kind &&
                asprintf(&line,
                         "{\"status\":\"ok\",\"engine\":\"shell\",\"path\":%s,\"kind\":%s,"
                         "\"paused\":false,\"ready\":true,\"outputs\":0,"
                         "\"has_video\":%s,\"has_still\":%s,\"time_pos\":-1.000,"
                         "\"hwdec\":\"no\",\"error\":\"\",\"skipped\":\"\"}",
                         path, kind, video ? "true" : "false", video ? "false" : "true") >= 0) {
                owe_ipc_send_line(c->fd, line);
            } else {
                send_err(c, "render unavailable");
            }
            free(line);
            free(path);
            free(kind);
        } else {
            char reply[8192];
            if (owed_supervisor_send(app->supervisor, "{\"cmd\":\"status\"}", reply, sizeof(reply)) == 0) {
                owe_ipc_send_line(c->fd, reply);
            } else {
                send_err(c, "render unreachable");
            }
        }
    } else if (strcmp(cmd, "render-restart") == 0) {
        if (!owed_app_renderer_expected()) {
            OWE_INFO("render-restart ignored while the shell draws the background");
            send_ok(c, "\"engine\":\"shell\"");
            yyjson_doc_free(doc);
            return;
        }
        owed_supervisor_stop(app->supervisor);
        if (owed_supervisor_ensure_running(app->supervisor) == 0) {
            char resolved[4096];
            app->loaded_path[0] = '\0';
            app->loaded_kind[0] = '\0';
            app->render_paused = -1;
            owed_supervisor_fade(app->supervisor, app->config.fade_ms);
            if (owed_watch_resolve_current(resolved, sizeof(resolved)) == 0) {
                app->source_path[0] = '\0';
                owed_app_on_background_changed(resolved);
            }
            send_ok(c, NULL);
        } else {
            send_err(c, "render restart failed");
        }
    } else if (strcmp(cmd, "shutdown") == 0) {
        send_ok(c, NULL);
        app->running = false;
    } else {
        send_err(c, "unknown cmd");
    }
    yyjson_doc_free(doc);
}

struct owed_ipc *owed_ipc_new(const char *socket_path) {
    struct owed_ipc *ipc;
    char dir[PATH_MAX];
    char *slash;
    int i;
    if (!socket_path) {
        return NULL;
    }
    snprintf(dir, sizeof(dir), "%s", socket_path);
    slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        owe_mkdir_p(dir);
    }
    ipc = calloc(1, sizeof(*ipc));
    if (!ipc) {
        return NULL;
    }
    ipc->srv = owe_ipc_server_new(socket_path);
    if (!ipc->srv) {
        free(ipc);
        return NULL;
    }
    snprintf(ipc->path, sizeof(ipc->path), "%s", socket_path);
    for (i = 0; i < MAX_CLIENTS; i++) {
        ipc->clients[i].fd = -1;
    }
    fcntl(owe_ipc_server_fd(ipc->srv), F_SETFL, O_NONBLOCK);
    return ipc;
}

void owed_ipc_free(struct owed_ipc *ipc) {
    int i;
    if (!ipc) {
        return;
    }
    for (i = 0; i < MAX_CLIENTS; i++) {
        client_remove(ipc, i);
    }
    owe_ipc_server_free(ipc->srv);
    free(ipc);
}

int owed_ipc_fd(struct owed_ipc *ipc) {
    return ipc ? owe_ipc_server_fd(ipc->srv) : -1;
}

int owed_ipc_pollfds(struct owed_ipc *ipc, struct pollfd *fds) {
    int n = 0;
    for (int i = 0; ipc && i < MAX_CLIENTS; i++) {
        if (ipc->clients[i].fd >= 0)
            fds[n++] = (struct pollfd){.fd = ipc->clients[i].fd, .events = POLLIN};
    }
    return n;
}

void owed_ipc_accept(struct owed_ipc *ipc) {
    int fd;
    int i;
    if (!ipc) {
        return;
    }
    while ((fd = owe_ipc_server_accept(ipc->srv)) >= 0) {
        for (i = 0; i < MAX_CLIENTS; i++) {
            if (ipc->clients[i].fd < 0) {
                ipc->clients[i].fd = fd;
                ipc->clients[i].len = 0;
                ipc->clients[i].active_ms = now_ms();
                break;
            }
        }
        if (i == MAX_CLIENTS) {
            close(fd);
        }
    }
}

void owed_ipc_poll_clients(struct owed_ipc *ipc) {
    int i;
    if (!ipc) {
        return;
    }
    for (i = 0; i < MAX_CLIENTS; i++) {
        struct owed_client *c = &ipc->clients[i];
        ssize_t n;
        char *nl;
        if (c->fd < 0) {
            continue;
        }
        if (c->len == 0 && now_ms() - c->active_ms > CLIENT_IDLE_MS) {
            client_remove(ipc, i);
            continue;
        }
        n = recv(c->fd, c->buf + c->len, sizeof(c->buf) - c->len - 1, 0);
        if (n == 0) {
            client_remove(ipc, i);
            continue;
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                continue;
            }
            client_remove(ipc, i);
            continue;
        }
        if (memchr(c->buf + c->len, '\0', (size_t)n)) {
            client_remove(ipc, i);
            continue;
        }
        c->len += (size_t)n;
        c->buf[c->len] = '\0';
        c->active_ms = now_ms();
        while ((nl = strchr(c->buf, '\n')) != NULL) {
            *nl = '\0';
            if (nl > c->buf && nl[-1] == '\r') {
                nl[-1] = '\0';
            }
            if (*c->buf) {
                handle_command(ipc, c, c->buf);
            }
            {
                size_t used = (size_t)(nl - c->buf) + 1;
                memmove(c->buf, c->buf + used, c->len - used + 1);
                c->len -= used;
            }
        }
        if (c->len >= sizeof(c->buf) - 1) {
            client_remove(ipc, i);
        }
    }
}

void owed_ipc_broadcast(struct owed_ipc *ipc, const char *line) {
    (void)ipc;
    (void)line;
}

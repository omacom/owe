#include "render_ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "yyjson.h"

#include "common_ipc.h"
#include "json.h"
#include <sys/stat.h>
#include "log.h"
#include "mpv.h"
#include "render.h"
#include "still.h"
#include "strutil.h"
#include "wayland.h"
#include "xdg.h"

#define MAX_CLIENTS OWE_IPC_MAX_CLIENTS

struct owe_client {
    int fd;
    char buf[OWE_IPC_MAX_LINE];
    size_t len;
};

struct owe_render_ipc {
    owe_ipc_server_t *srv;
    char path[PATH_MAX];
    struct owe_client clients[MAX_CLIENTS];
};

static void client_remove(struct owe_render_ipc *ipc, int idx) {
    if (ipc->clients[idx].fd >= 0) {
        close(ipc->clients[idx].fd);
    }
    ipc->clients[idx].fd = -1;
    ipc->clients[idx].len = 0;
}

static void client_send(struct owe_client *c, const char *line) {
    if (c->fd >= 0) {
        owe_ipc_send_line(c->fd, line);
    }
}

static void send_ok(struct owe_client *c, const char *extra) {
    char line[OWE_IPC_MAX_LINE];
    if (extra && *extra) {
        snprintf(line, sizeof(line), "{\"status\":\"ok\",%s}", extra);
    } else {
        snprintf(line, sizeof(line), "{\"status\":\"ok\"}");
    }
    client_send(c, line);
}

static void send_err(struct owe_client *c, const char *msg) {
    char line[1024];
    snprintf(line, sizeof(line), "{\"status\":\"error\",\"message\":\"%s\"}", msg ? msg : "failed");
    client_send(c, line);
}

static void handle_status(struct owe_client *c) {
    owe_app_t *app = owe_app_get();
    char *line = NULL;
    char *path = owe_json_quote(app ? app->current_path : "");
    char *error = owe_json_quote(app ? owe_mpv_error(app->mpv) : "");
    if (!path || !error) { free(path); free(error); return; }
    int mw = 0;
    int mh = 0;
    if (app && app->wl) {
        owe_wayland_outputs_max_size(app->wl, &mw, &mh);
    }
    if (asprintf(&line,
             "{\"status\":\"ok\",\"path\":%s,\"kind\":\"%s\",\"paused\":%s,\"outputs\":%d,"
             "\"max_width\":%d,\"max_height\":%d,\"has_video\":%s,\"has_still\":%s,\"time_pos\":%.3f,\"hwdec\":\"%s\",\"error\":%s}",
             path, app ? app->current_kind : "",
             app && app->paused ? "true" : "false", app && app->wl ? owe_wayland_output_count(app->wl) : 0,
             mw, mh, app && app->mpv && owe_mpv_has_video(app->mpv) ? "true" : "false",
             app && app->still && owe_still_has_image(app->still) ? "true" : "false",
             app && app->mpv ? owe_mpv_time_pos(app->mpv) : -1.0,
             owe_mpv_hwdec(app ? app->mpv : NULL), error) >= 0)
        client_send(c, line);
    free(line);
    free(path);
    free(error);
}

static void handle_load(struct owe_client *c, yyjson_val *root) {
    owe_app_t *app = owe_app_get();
    yyjson_val *vpath;
    yyjson_val *vkind;
    const char *path;
    const char *kind;
    if (!app) {
        send_err(c, "no app");
        return;
    }
    vpath = yyjson_obj_get(root, "path");
    vkind = yyjson_obj_get(root, "kind");
    path = vpath && yyjson_is_str(vpath) ? yyjson_get_str(vpath) : "";
    kind = vkind && yyjson_is_str(vkind) ? yyjson_get_str(vkind) : "";
    struct stat st;
    if (!owe_json_path(vpath) || path[0] != '/' || stat(path, &st) != 0 ||
        !S_ISREG(st.st_mode) || access(path, R_OK) != 0) {
        send_err(c, "Invalid local media path");
        return;
    }
    if (strcmp(kind, "video") == 0) {
        if (owe_mpv_load(app->mpv, path) != 0) {
            send_err(c, "video load failed");
            return;
        }
        owe_still_unload(app->still);
        owe_mpv_set_paused(app->mpv, app->paused);
        snprintf(app->current_path, sizeof(app->current_path), "%s", path);
        snprintf(app->current_kind, sizeof(app->current_kind), "video");
        send_ok(c, NULL);
        return;
    }
    if (strcmp(kind, "still") == 0) {
        if (owe_still_load(app->still, path) != 0) {
            send_err(c, "still load failed");
            return;
        }
        owe_mpv_stop(app->mpv);
        snprintf(app->current_path, sizeof(app->current_path), "%s", path);
        snprintf(app->current_kind, sizeof(app->current_kind), "still");
        send_ok(c, NULL);
        return;
    }
    send_err(c, "unknown kind");
}

static void handle_command(struct owe_render_ipc *ipc, struct owe_client *c, const char *line) {
    owe_app_t *app = owe_app_get();
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
    } else if (strcmp(cmd, "load") == 0) {
        handle_load(c, root);
    } else if (strcmp(cmd, "pause") == 0) {
        if (app) {
            app->paused = true;
            owe_mpv_set_paused(app->mpv, true);
        }
        send_ok(c, NULL);
    } else if (strcmp(cmd, "resume") == 0) {
        if (app) {
            app->paused = false;
            owe_mpv_set_paused(app->mpv, false);
            owe_app_request_render();
        }
        send_ok(c, NULL);
    } else if (strcmp(cmd, "stop") == 0) {
        if (app) {
            owe_mpv_stop(app->mpv);
            owe_still_unload(app->still);
            app->current_path[0] = '\0';
            app->current_kind[0] = '\0';
            owe_app_request_render();
        }
        send_ok(c, NULL);
    } else if (strcmp(cmd, "status") == 0) {
        handle_status(c);
    } else if (strcmp(cmd, "fade") == 0) {
        yyjson_val *vms = yyjson_obj_get(root, "ms");
        if (app && vms && yyjson_is_int(vms) && yyjson_get_sint(vms) >= 0 &&
            yyjson_get_sint(vms) <= 2000) {
            owe_still_set_fade_ms(app->still, (int)yyjson_get_num(vms));
        } else {
            send_err(c, "Fade must be an integer from 0 to 2000");
            yyjson_doc_free(doc);
            return;
        }
        send_ok(c, NULL);
    } else {
        send_err(c, "unknown cmd");
    }
    yyjson_doc_free(doc);
}

struct owe_render_ipc *owe_render_ipc_new(const char *socket_path) {
    struct owe_render_ipc *ipc;
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

void owe_render_ipc_free(struct owe_render_ipc *ipc) {
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

int owe_render_ipc_fd(struct owe_render_ipc *ipc) {
    return ipc ? owe_ipc_server_fd(ipc->srv) : -1;
}

int owe_render_ipc_pollfds(struct owe_render_ipc *ipc, struct pollfd *fds) {
    int n = 0;
    for (int i = 0; ipc && i < MAX_CLIENTS; i++) {
        if (ipc->clients[i].fd >= 0)
            fds[n++] = (struct pollfd){.fd = ipc->clients[i].fd, .events = POLLIN};
    }
    return n;
}

void owe_render_ipc_accept(struct owe_render_ipc *ipc) {
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
                break;
            }
        }
        if (i == MAX_CLIENTS) {
            close(fd);
        }
    }
}

void owe_render_ipc_poll_clients(struct owe_render_ipc *ipc) {
    int i;
    if (!ipc) {
        return;
    }
    for (i = 0; i < MAX_CLIENTS; i++) {
        struct owe_client *c = &ipc->clients[i];
        ssize_t n;
        char *nl;
        if (c->fd < 0) {
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

static void broadcast(struct owe_render_ipc *ipc, const char *line) {
    (void)ipc;
    (void)line;
}

void owe_render_ipc_emit_first_frame(struct owe_render_ipc *ipc, const char *path) {
    char line[8192];
    snprintf(line, sizeof(line), "{\"event\":\"first-frame\",\"path\":\"%s\"}", path ? path : "");
    broadcast(ipc, line);
}

void owe_render_ipc_emit_error(struct owe_render_ipc *ipc, const char *message) {
    char line[2048];
    snprintf(line, sizeof(line), "{\"event\":\"error\",\"message\":\"%s\"}", message ? message : "");
    broadcast(ipc, line);
}

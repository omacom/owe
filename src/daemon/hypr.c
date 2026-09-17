#include "hypr.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include "common_ipc.h"
#include "daemon.h"
#include "display_state.h"
#include "log.h"
#include "yyjson.h"

struct owed_hypr {
    char sock_dir[PATH_MAX];
    int event_fd;
    char event_buf[65536];
    size_t event_len;
    char *clients;
    char *monitors;
    bool any_fullscreen;
    bool any_window_visible;
    bool all_monitors_off;
    int monitor_count;
};

/* The session environment keeps the Hyprland signature from login. A
 * compositor restart starts a new instance with a new signature, so fall
 * back to the newest runtime instance that has an event socket. */
static void resolve_sock_dir(struct owed_hypr *h) {
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    const char *sig = getenv("HYPRLAND_INSTANCE_SIGNATURE");
    char root[PATH_MAX];
    char best[PATH_MAX] = "";
    time_t best_mtime = 0;
    DIR *dir;
    struct dirent *entry;

    h->sock_dir[0] = '\0';
    if (!runtime || !*runtime) {
        return;
    }
    if (sig && *sig) {
        char sock[PATH_MAX + 32];
        snprintf(h->sock_dir, sizeof(h->sock_dir), "%s/hypr/%s", runtime, sig);
        snprintf(sock, sizeof(sock), "%s/.socket2.sock", h->sock_dir);
        if (access(sock, F_OK) == 0) {
            return;
        }
    }
    if (snprintf(root, sizeof(root), "%s/hypr", runtime) >= (int)sizeof(root)) {
        return;
    }
    dir = opendir(root);
    if (!dir) {
        return;
    }
    while ((entry = readdir(dir))) {
        char candidate[PATH_MAX];
        char sock[PATH_MAX + 32];
        struct stat st;
        if (entry->d_name[0] == '.') {
            continue;
        }
        if (snprintf(candidate, sizeof(candidate), "%s/%s", root, entry->d_name) >=
            (int)sizeof(candidate)) {
            continue;
        }
        if (stat(candidate, &st) != 0 || !S_ISDIR(st.st_mode)) {
            continue;
        }
        if (snprintf(sock, sizeof(sock), "%s/.socket2.sock", candidate) >= (int)sizeof(sock)) {
            continue;
        }
        if (access(sock, F_OK) != 0) {
            continue;
        }
        if (!best[0] || st.st_mtime > best_mtime) {
            snprintf(best, sizeof(best), "%s", candidate);
            best_mtime = st.st_mtime;
        }
    }
    closedir(dir);
    if (best[0]) {
        snprintf(h->sock_dir, sizeof(h->sock_dir), "%s", best);
    }
}

static int connect_socket(struct owed_hypr *h, const char *name) {
    char path[PATH_MAX + 32];
    if (!h->sock_dir[0]) return -1;
    snprintf(path, sizeof(path), "%s/%s", h->sock_dir, name);
    int fd = owe_ipc_connect(path);
    if (fd >= 0) owe_ipc_set_timeout(fd, 250);
    return fd;
}

static void reconnect(struct owed_hypr *h) {
    if (h->event_fd >= 0) return;
    resolve_sock_dir(h);
    h->event_fd = connect_socket(h, ".socket2.sock");
    if (h->event_fd >= 0) {
        fcntl(h->event_fd, F_SETFL, O_NONBLOCK);
        OWE_INFO("hyprland connected at %s", h->sock_dir);
    }
}

static int request(struct owed_hypr *h, const char *cmd, char **out) {
    int fd = connect_socket(h, ".socket.sock");
    if (fd < 0) return -1;
    char req[64];
    int bytes = snprintf(req, sizeof(req), "j/%s", cmd);
    char *buf = NULL;
    size_t used = 0;
    bool eof = false;
    if (send(fd, req, (size_t)bytes, MSG_NOSIGNAL) != bytes) goto done;
    for (;;) {
        char tmp[16384];
        ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
        if (n == 0) { eof = true; break; }
        if (n < 0) { if (errno == EINTR) continue; break; }
        if (used + (size_t)n > 4 * 1024 * 1024) break;
        char *next = realloc(buf, used + (size_t)n + 1);
        if (!next) break;
        buf = next;
        memcpy(buf + used, tmp, (size_t)n);
        used += (size_t)n;
        buf[used] = '\0';
    }
done:
    close(fd);
    yyjson_doc *doc = eof && buf ? yyjson_read(buf, used, 0) : NULL;
    bool valid = doc && yyjson_is_arr(yyjson_doc_get_root(doc));
    yyjson_doc_free(doc);
    if (!valid) { free(buf); return -1; }
    free(*out);
    *out = buf;
    return 0;
}

static void recompute(struct owed_hypr *h) {
    bool fs = h->any_fullscreen, vis = h->any_window_visible, off = h->all_monitors_off;
    yyjson_doc *clients = h->clients ? yyjson_read(h->clients, strlen(h->clients), 0) : NULL;
    yyjson_doc *monitors = h->monitors ? yyjson_read(h->monitors, strlen(h->monitors), 0) : NULL;
    h->any_fullscreen = owe_outputs_covered_docs(clients, monitors, true);
    h->any_window_visible = owe_outputs_covered_docs(clients, monitors, false);
    if (monitors) {
        yyjson_val *root = yyjson_doc_get_root(monitors), *m;
        size_t i, n;
        int lit = 0;
        h->monitor_count = (int)yyjson_arr_size(root);
        yyjson_arr_foreach(root, i, n, m) {
            yyjson_val *dpms = yyjson_obj_get(m, "dpmsStatus");
            bool powered = !dpms || (yyjson_is_bool(dpms) ? yyjson_get_bool(dpms) : yyjson_get_int(dpms) != 0);
            if (!yyjson_get_bool(yyjson_obj_get(m, "disabled")) && powered) lit++;
        }
        h->all_monitors_off = h->monitor_count > 0 && lit == 0;
    }
    yyjson_doc_free(clients);
    yyjson_doc_free(monitors);
    int drm = owe_drm_all_off("/sys/class/drm");
    if (drm >= 0) h->all_monitors_off = drm != 0;
    if (fs != h->any_fullscreen || vis != h->any_window_visible || off != h->all_monitors_off)
        owed_app_on_policy_changed();
}

void owed_hypr_refresh_clients(struct owed_hypr *h) {
    if (h && request(h, "clients", &h->clients) == 0) recompute(h);
}

void owed_hypr_refresh_monitors(struct owed_hypr *h) {
    if (h && request(h, "monitors", &h->monitors) == 0) recompute(h);
}

void owed_hypr_refresh(struct owed_hypr *h) {
    if (!h) return;
    request(h, "clients", &h->clients);
    request(h, "monitors", &h->monitors);
    recompute(h);
}

void owed_hypr_tick(struct owed_hypr *h) {
    if (!h) return;
    bool disconnected = h->event_fd < 0;
    reconnect(h);
    if (disconnected && h->event_fd >= 0) owed_hypr_refresh(h);
    else owed_hypr_refresh_monitors(h);
}

struct owed_hypr *owed_hypr_new(void) {
    struct owed_hypr *h = calloc(1, sizeof(*h));
    if (!h) return NULL;
    h->event_fd = -1;
    reconnect(h);
    if (h->event_fd >= 0) {
        owed_hypr_refresh(h);
    }
    return h;
}

void owed_hypr_free(struct owed_hypr *h) {
    if (!h) return;
    if (h->event_fd >= 0) close(h->event_fd);
    free(h->clients);
    free(h->monitors);
    free(h);
}

int owed_hypr_event_fd(struct owed_hypr *h) { return h ? h->event_fd : -1; }

int owed_hypr_poll(struct owed_hypr *h) {
    if (!h || h->event_fd < 0) return 0;
    ssize_t n = recv(h->event_fd, h->event_buf + h->event_len, sizeof(h->event_buf) - h->event_len - 1, 0);
    if (n <= 0) {
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) return 0;
        close(h->event_fd);
        h->event_fd = -1;
        h->event_len = 0;
        return -1;
    }
    h->event_len += (size_t)n;
    h->event_buf[h->event_len] = '\0';
    size_t consumed = 0;
    bool refresh = false;
    char *nl;
    while ((nl = memchr(h->event_buf + consumed, '\n', h->event_len - consumed))) {
        *nl = '\0';
        char *event = h->event_buf + consumed;
        /* Focus and title events do not change wallpaper visibility. */
        if (strncmp(event, "activewindow", 12) && strncmp(event, "windowtitle", 11)) refresh = true;
        consumed = (size_t)(nl - h->event_buf) + 1;
    }
    memmove(h->event_buf, h->event_buf + consumed, h->event_len - consumed);
    h->event_len -= consumed;
    if (h->event_len == sizeof(h->event_buf) - 1) h->event_len = 0;
    if (refresh) owed_hypr_refresh(h);
    return refresh ? 1 : 0;
}

bool owed_hypr_any_fullscreen(struct owed_hypr *h) { return h && h->any_fullscreen; }
bool owed_hypr_any_window_visible(struct owed_hypr *h) { return h && h->any_window_visible; }
bool owed_hypr_all_monitors_off(struct owed_hypr *h) { return h && h->all_monitors_off; }
int owed_hypr_monitor_count(struct owed_hypr *h) { return h ? h->monitor_count : 0; }

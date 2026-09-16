#include "hypr.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include "yyjson.h"

#include "daemon.h"
#include "log.h"

struct owed_hypr {
    char sock_dir[PATH_MAX];
    int event_fd;
    char event_buf[65536];
    size_t event_len;
    bool any_fullscreen;
    bool any_window_visible;
    bool all_monitors_off;
    int monitor_count;
};

static int hypr_sock_dir(char *buf, size_t len) {
    const char *sig = getenv("HYPRLAND_INSTANCE_SIGNATURE");
    const char *rt = getenv("XDG_RUNTIME_DIR");
    char fallback[64];
    if (!sig || !*sig) {
        return -1;
    }
    if (!rt || !*rt) {
        snprintf(fallback, sizeof(fallback), "/run/user/%d", (int)getuid());
        rt = fallback;
    }
    if (snprintf(buf, len, "%s/hypr/%s", rt, sig) >= (int)len) {
        return -1;
    }
    return 0;
}

static int sock_connect(const char *path) {
    struct sockaddr_un addr;
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    int fd;
    if (strlen(path) >= sizeof(addr.sun_path)) {
        return -1;
    }
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    return fd;
}

static int hypr_request(struct owed_hypr *h, const char *cmd, char **out) {
    char path[PATH_MAX + 32];
    char req[256];
    int fd;
    char *buf = NULL;
    size_t cap = 0;
    size_t len = 0;
    snprintf(path, sizeof(path), "%s/.socket.sock", h->sock_dir);
    fd = sock_connect(path);
    if (fd < 0) {
        return -1;
    }
    snprintf(req, sizeof(req), "j/%s", cmd);
    if (send(fd, req, strlen(req), MSG_NOSIGNAL) < 0) {
        close(fd);
        return -1;
    }
    for (;;) {
        char tmp[16384];
        ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
        if (n == 0) {
            break;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (len + (size_t)n + 1 > cap) {
            size_t ncap = cap ? cap * 2 : 16384;
            char *nb;
            while (ncap < len + (size_t)n + 1) {
                ncap *= 2;
            }
            nb = realloc(buf, ncap);
            if (!nb) {
                free(buf);
                close(fd);
                return -1;
            }
            buf = nb;
            cap = ncap;
        }
        memcpy(buf + len, tmp, (size_t)n);
        len += (size_t)n;
    }
    close(fd);
    if (!buf) {
        buf = strdup("[]");
    } else {
        buf[len] = '\0';
    }
    *out = buf;
    return 0;
}

static void recompute_from_clients(struct owed_hypr *h, const char *json) {
    yyjson_doc *doc;
    yyjson_val *root;
    size_t idx;
    size_t max;
    yyjson_val *item;
    bool fs = false;
    bool vis = false;
    if (!json) {
        return;
    }
    doc = yyjson_read(json, strlen(json), 0);
    if (!doc) {
        return;
    }
    root = yyjson_doc_get_root(doc);
    if (!yyjson_is_arr(root)) {
        yyjson_doc_free(doc);
        return;
    }
    yyjson_arr_foreach(root, idx, max, item) {
        yyjson_val *vfs = yyjson_obj_get(item, "fullscreen");
        yyjson_val *vhidden = yyjson_obj_get(item, "hidden");
        bool client_fs = false;
        if (vfs && yyjson_is_bool(vfs)) {
            client_fs = yyjson_get_bool(vfs);
        } else if (vfs && yyjson_is_num(vfs)) {
            client_fs = yyjson_get_num(vfs) != 0;
        }
        if (client_fs) {
            fs = true;
        }
        if (vhidden && yyjson_is_bool(vhidden) && yyjson_get_bool(vhidden)) {
            continue;
        }
        vis = true;
    }
    h->any_fullscreen = fs;
    h->any_window_visible = vis;
    yyjson_doc_free(doc);
}

static void recompute_from_monitors(struct owed_hypr *h, const char *json) {
    yyjson_doc *doc;
    yyjson_val *root;
    size_t idx;
    size_t max;
    yyjson_val *item;
    int count = 0;
    int lit = 0;
    if (!json) {
        return;
    }
    doc = yyjson_read(json, strlen(json), 0);
    if (!doc) {
        return;
    }
    root = yyjson_doc_get_root(doc);
    if (!yyjson_is_arr(root)) {
        yyjson_doc_free(doc);
        return;
    }
    yyjson_arr_foreach(root, idx, max, item) {
        yyjson_val *vdpms = yyjson_obj_get(item, "dpmsStatus");
        yyjson_val *vdis = yyjson_obj_get(item, "disabled");
        bool off = false;
        count++;
        if (vdis && yyjson_is_bool(vdis) && yyjson_get_bool(vdis)) {
            off = true;
        }
        if (vdpms && yyjson_is_bool(vdpms) && !yyjson_get_bool(vdpms)) {
            off = true;
        }
        if (!off) {
            lit++;
        }
    }
    h->monitor_count = count;
    h->all_monitors_off = count > 0 && lit == 0;
    yyjson_doc_free(doc);
}

void owed_hypr_refresh_clients(struct owed_hypr *h) {
    char *out = NULL;
    if (!h) {
        return;
    }
    if (hypr_request(h, "clients", &out) == 0) {
        recompute_from_clients(h, out);
        free(out);
    }
}

void owed_hypr_refresh_monitors(struct owed_hypr *h) {
    char *out = NULL;
    if (!h) {
        return;
    }
    if (hypr_request(h, "monitors", &out) == 0) {
        recompute_from_monitors(h, out);
        free(out);
    }
}

void owed_hypr_refresh(struct owed_hypr *h) {
    owed_hypr_refresh_clients(h);
    owed_hypr_refresh_monitors(h);
}

void owed_hypr_tick(struct owed_hypr *h) {
    bool prev_fs;
    bool prev_vis;
    bool prev_off;
    if (!h) {
        return;
    }
    prev_fs = h->any_fullscreen;
    prev_vis = h->any_window_visible;
    prev_off = h->all_monitors_off;
    owed_hypr_refresh_monitors(h);
    if (prev_fs != h->any_fullscreen || prev_vis != h->any_window_visible ||
        prev_off != h->all_monitors_off) {
        OWE_DEBUG("hypr tick fs=%d vis=%d off=%d", h->any_fullscreen, h->any_window_visible,
                  h->all_monitors_off);
        owed_app_on_policy_changed();
    }
}

struct owed_hypr *owed_hypr_new(void) {
    struct owed_hypr *h = calloc(1, sizeof(*h));
    char path[PATH_MAX + 32];
    if (!h) {
        return NULL;
    }
    if (hypr_sock_dir(h->sock_dir, sizeof(h->sock_dir)) != 0) {
        OWE_WARN("no HYPRLAND_INSTANCE_SIGNATURE, hypr events disabled");
        return h;
    }
    snprintf(path, sizeof(path), "%s/.socket2.sock", h->sock_dir);
    h->event_fd = sock_connect(path);
    if (h->event_fd < 0) {
        OWE_WARN("hypr event socket unavailable: %s", path);
    } else {
        fcntl(h->event_fd, F_SETFL, O_NONBLOCK);
        OWE_INFO("hypr event socket connected");
    }
    owed_hypr_refresh(h);
    return h;
}

void owed_hypr_free(struct owed_hypr *h) {
    if (!h) {
        return;
    }
    if (h->event_fd >= 0) {
        close(h->event_fd);
    }
    free(h);
}

int owed_hypr_event_fd(struct owed_hypr *h) {
    return h ? h->event_fd : -1;
}

static bool event_is_window(const char *line) {
    static const char *prefixes[] = {
        "fullscreen>>",  "openwindow>>",    "closewindow>>", "movewindow>>",
        "workspace>>",   "moveworkspace>>", "changefloatingmode>>", "windowtitle>>",
    };
    size_t i;
    for (i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        if (strncmp(line, prefixes[i], strlen(prefixes[i])) == 0) {
            return true;
        }
    }
    return false;
}

static bool event_is_monitor(const char *line) {
    return strncmp(line, "monitoradded>>", 14) == 0 ||
           strncmp(line, "monitorremoved>>", 16) == 0;
}

int owed_hypr_poll(struct owed_hypr *h) {
    ssize_t n;
    bool changed = false;
    bool prev_fs;
    bool prev_vis;
    bool prev_off;
    size_t consumed = 0;
    bool want_clients = false;
    bool want_monitors = false;
    if (!h || h->event_fd < 0) {
        return 0;
    }
    prev_fs = h->any_fullscreen;
    prev_vis = h->any_window_visible;
    prev_off = h->all_monitors_off;
    n = recv(h->event_fd, h->event_buf + h->event_len, sizeof(h->event_buf) - h->event_len - 1, 0);
    if (n == 0) {
        OWE_WARN("hypr event socket closed, reconnecting");
        close(h->event_fd);
        h->event_fd = -1;
        h->event_len = 0;
        {
            char path[PATH_MAX + 32];
            snprintf(path, sizeof(path), "%s/.socket2.sock", h->sock_dir);
            h->event_fd = sock_connect(path);
            if (h->event_fd >= 0) {
                fcntl(h->event_fd, F_SETFL, O_NONBLOCK);
                OWE_INFO("hypr event socket reconnected");
            }
        }
        return -1;
    }
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return 0;
        }
        return -1;
    }
    h->event_len += (size_t)n;
    while (consumed < h->event_len) {
        char *start = h->event_buf + consumed;
        char *nl = memchr(start, '\n', h->event_len - consumed);
        size_t line_len;
        if (!nl) {
            break;
        }
        line_len = (size_t)(nl - start);
        *nl = '\0';
        if (event_is_window(start)) {
            want_clients = true;
        }
        if (event_is_monitor(start)) {
            want_monitors = true;
        }
        *nl = '\n';
        consumed += line_len + 1;
    }
    if (consumed > 0) {
        memmove(h->event_buf, h->event_buf + consumed, h->event_len - consumed);
        h->event_len -= consumed;
    }
    if (h->event_len >= sizeof(h->event_buf) - 1) {
        OWE_WARN("hypr event buffer overflow, resetting");
        h->event_len = 0;
    }
    if (want_clients && want_monitors) {
        owed_hypr_refresh(h);
        changed = true;
    } else if (want_clients) {
        owed_hypr_refresh_clients(h);
        changed = true;
    } else if (want_monitors) {
        owed_hypr_refresh_monitors(h);
        changed = true;
    }
    if (changed && (prev_fs != h->any_fullscreen || prev_vis != h->any_window_visible ||
                    prev_off != h->all_monitors_off)) {
        OWE_DEBUG("hypr state fs=%d vis=%d off=%d", h->any_fullscreen, h->any_window_visible,
                  h->all_monitors_off);
        owed_app_on_policy_changed();
    }
    return changed ? 1 : 0;
}

bool owed_hypr_any_fullscreen(struct owed_hypr *h) {
    return h && h->any_fullscreen;
}

bool owed_hypr_any_window_visible(struct owed_hypr *h) {
    return h && h->any_window_visible;
}

bool owed_hypr_all_monitors_off(struct owed_hypr *h) {
    return h && h->all_monitors_off;
}

int owed_hypr_monitor_count(struct owed_hypr *h) {
    return h ? h->monitor_count : 0;
}

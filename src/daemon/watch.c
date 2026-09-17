#include "watch.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#include "daemon.h"
#include "log.h"
#include "xdg.h"
#include "strutil.h"

int owed_watch_set_current(const char *path) {
    char resolved[PATH_MAX], link[PATH_MAX], tmp[PATH_MAX + 32];
    struct stat st;
    if (!path || !realpath(path, resolved) || stat(resolved, &st) != 0 ||
        !S_ISREG(st.st_mode) || access(resolved, R_OK) != 0 ||
        owe_kind_from_path(resolved) == OWE_KIND_UNKNOWN) return -1;
    if (owe_omarchy_background_link(link, sizeof(link)) != 0) return -1;
    snprintf(tmp, sizeof(tmp), "%s.owe-XXXXXX", link);
    int fd = mkstemp(tmp);
    if (fd < 0) return -1;
    close(fd);
    unlink(tmp);
    if (symlink(resolved, tmp) != 0) return -1;
    if (rename(tmp, link) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

struct owed_watch {
    int fd;
    int wd;
    char dir[PATH_MAX];
    char link[PATH_MAX];
};

int owed_watch_resolve_current(char *buf, unsigned long len) {
    char link[PATH_MAX];
    char raw[PATH_MAX];
    char resolved[PATH_MAX * 2];
    ssize_t n;
    if (!buf || len < 2) {
        return -1;
    }
    if (owe_omarchy_background_link(link, sizeof(link)) != 0) {
        return -1;
    }
    n = readlink(link, raw, sizeof(raw) - 1);
    if (n < 0) {
        return -1;
    }
    raw[n] = '\0';
    if (raw[0] != '/') {
        char abs[PATH_MAX * 2];
        char *slash = strrchr(link, '/');
        if (!slash) {
            return -1;
        }
        *slash = '\0';
        snprintf(abs, sizeof(abs), "%s/%s", link, raw);
        if (realpath(abs, resolved) == NULL) {
            snprintf(resolved, sizeof(resolved), "%s", abs);
        }
    } else if (realpath(raw, resolved) == NULL) {
        return -1;
    }
    snprintf(buf, len, "%s", resolved);
    return 0;
}

struct owed_watch *owed_watch_new(void) {
    struct owed_watch *w = calloc(1, sizeof(*w));
    char *slash;
    if (!w) {
        return NULL;
    }
    if (owe_omarchy_background_link(w->link, sizeof(w->link)) != 0) {
        free(w);
        return NULL;
    }
    snprintf(w->dir, sizeof(w->dir), "%s", w->link);
    slash = strrchr(w->dir, '/');
    if (!slash) {
        free(w);
        return NULL;
    }
    *slash = '\0';
    owe_mkdir_p(w->dir);
    w->fd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
    if (w->fd < 0) {
        OWE_ERROR("inotify_init1 failed: %s", strerror(errno));
        free(w);
        return NULL;
    }
    w->wd = inotify_add_watch(w->fd, w->dir, IN_CREATE | IN_DELETE | IN_MOVED_TO | IN_MOVED_FROM |
                                                   IN_ATTRIB | IN_CLOSE_WRITE | IN_DONT_FOLLOW);
    if (w->wd < 0) {
        OWE_ERROR("inotify watch failed on %s: %s", w->dir, strerror(errno));
        close(w->fd);
        free(w);
        return NULL;
    }
    OWE_INFO("watching %s", w->dir);
    return w;
}

void owed_watch_free(struct owed_watch *w) {
    if (!w) {
        return;
    }
    if (w->wd >= 0) {
        inotify_rm_watch(w->fd, w->wd);
    }
    if (w->fd >= 0) {
        close(w->fd);
    }
    free(w);
}

int owed_watch_fd(struct owed_watch *w) {
    return w ? w->fd : -1;
}

int owed_watch_poll(struct owed_watch *w) {
    char buf[4096] __attribute__((aligned(8)));
    ssize_t n;
    int changed = 0;
    if (!w) {
        return 0;
    }
    for (;;) {
        const struct inotify_event *ev;
        size_t off = 0;
        n = read(w->fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                break;
            }
            return -1;
        }
        if (n == 0) {
            break;
        }
        while (off + sizeof(*ev) <= (size_t)n) {
            char name[NAME_MAX + 1];
            ev = (const struct inotify_event *)(buf + off);
            off += sizeof(*ev) + ev->len;
            if (ev->len > 0) {
                snprintf(name, sizeof(name), "%s", ev->name);
                if (strcmp(name, "background") == 0) {
                    changed = 1;
                }
            } else {
                changed = 1;
            }
        }
    }
    if (changed) {
        char resolved[PATH_MAX];
        owed_app_t *app = owed_app_get();
        if (owed_watch_resolve_current(resolved, sizeof(resolved)) == 0) {
            if (!app || strcmp(resolved, app->source_path) != 0) {
                OWE_INFO("background symlink now points at %s", resolved);
                owed_app_on_background_changed(resolved);
            }
        } else {
            OWE_WARN("background symlink unreadable");
        }
    }
    return changed;
}

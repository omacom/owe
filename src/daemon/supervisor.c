#include "supervisor.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "common_ipc.h"
#include "log.h"
#include "json.h"
#include "owe_spawn.h"
#include "xdg.h"

/* A renderer on a slow ARM board can need more than one second to load the GPU
 * driver and libmpv. The daemon blocks while it waits, so only a renderer that
 * never creates its socket costs the full wait. With a 1 second stop before
 * and after it, a restart request still answers within the 10 second reply
 * timeout of the owe CLI. */
#ifndef RENDER_START_MS
#define RENDER_START_MS 7000
#endif
/* A renderer without a socket after RENDER_START_MS keeps running, and the
 * main loop takes it when the socket appears. Software GL in a virtual
 * machine needs about 11 seconds. The time counts from the spawn. */
#ifndef RENDER_LATE_MS
#define RENDER_LATE_MS 30000
#endif
/* After a renderer that never created its socket, wait this long before the
 * next automatic start, so the daemon keeps serving its clients. */
#define RENDER_RETRY_MS 30000
/* A stopped renderer gets this long to exit after SIGTERM, then SIGKILL. */
#define RENDER_STOP_MS 1000

struct owed_supervisor {
    pid_t child;
    char socket_path[4096];
    int restarts;
    int64_t next_restart;
    int64_t started;
    /* Set while a renderer that missed RENDER_START_MS can still start. */
    int64_t late_until;
};

static int64_t monotonic_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

struct owed_supervisor *owed_supervisor_new(const char *daemon_socket) {
    struct owed_supervisor *s = calloc(1, sizeof(*s));
    if (!s) {
        return NULL;
    }
    int rc = daemon_socket ? owe_socket_path_sibling(daemon_socket, "render.sock", s->socket_path,
                                                     sizeof(s->socket_path)) :
                             owe_socket_path_render(s->socket_path, sizeof(s->socket_path));
    if (rc != 0) {
        free(s);
        return NULL;
    }
    return s;
}

void owed_supervisor_free(struct owed_supervisor *s) {
    if (!s) {
        return;
    }
    owed_supervisor_stop(s);
    free(s);
}

static int render_socket_ready(const char *path) {
    int fd = owe_ipc_connect(path);
    if (fd < 0) {
        return 0;
    }
    close(fd);
    return 1;
}

static int spawn_render(struct owed_supervisor *s) {
    char self[PATH_MAX];
    char dir[PATH_MAX * 2];
    char cand[PATH_MAX * 2 + 64];
    char *argv[] = { "owe-render", "--socket", s->socket_path, NULL };
    ssize_t n;
    char *slash;
    n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n > 0) {
        self[n] = '\0';
        snprintf(dir, sizeof(dir), "%s", self);
        slash = strrchr(dir, '/');
        if (slash) {
            *slash = '\0';
            snprintf(cand, sizeof(cand), "%s/owe-render", dir);
            if (access(cand, X_OK) == 0) {
                char *sibling[] = {cand, "--socket", s->socket_path, NULL};
                return owe_spawn(cand, sibling, &s->child);
            }
            slash = strrchr(dir, '/');
            if (slash) {
                *slash = '\0';
                snprintf(cand, sizeof(cand), "%s/render/owe-render", dir);
                if (access(cand, X_OK) == 0) {
                    char *argv2[] = { cand, "--socket", s->socket_path, NULL };
                    if (owe_spawn(cand, argv2, &s->child) == 0) {
                        return 0;
                    }
                }
            }
        }
    }
    return owe_spawn("owe-render", argv, &s->child);
}

/* Returns 1 when the late renderer has its socket. A renderer that is still
 * without one at late_until is stopped. */
static int late_start_ready(struct owed_supervisor *s) {
    if (render_socket_ready(s->socket_path)) {
        OWE_INFO("owe-render ready after %lld ms", (long long)(monotonic_ms() - s->started));
        s->late_until = 0;
        return 1;
    }
    if (monotonic_ms() >= s->late_until) {
        OWE_ERROR("owe-render socket never appeared after %d ms", RENDER_LATE_MS);
        owed_supervisor_stop(s);
        s->next_restart = monotonic_ms() + RENDER_RETRY_MS;
    }
    return 0;
}

int owed_supervisor_ensure_running(struct owed_supervisor *s) {
    if (!s) {
        return -1;
    }
    if (s->child > 0) {
        pid_t w = waitpid(s->child, NULL, WNOHANG);
        if (w == 0) {
            if (!s->late_until || late_start_ready(s)) return 0;
            return s->late_until ? 1 : -1;
        }
        if (w < 0 && errno != ECHILD) return -1;
        s->child = 0;
        s->late_until = 0;
    }
    if (monotonic_ms() < s->next_restart) return -1;
    s->next_restart = monotonic_ms() + 2000;
    if (spawn_render(s) != 0) {
        OWE_ERROR("failed to spawn owe-render");
        return -1;
    }
    s->restarts++;
    OWE_INFO("owe-render spawned pid=%d (restart #%d)", (int)s->child, s->restarts);
    s->started = monotonic_ms();
    while (monotonic_ms() < s->started + RENDER_START_MS) {
        if (waitpid(s->child, NULL, WNOHANG) == s->child) {
            s->child = 0;
            return -1;
        }
        if (render_socket_ready(s->socket_path)) {
            OWE_INFO("owe-render ready after %lld ms", (long long)(monotonic_ms() - s->started));
            return 0;
        }
        usleep(50000);
    }
    OWE_WARN("owe-render has no socket after %d ms, the main loop waits for it until %d ms",
             RENDER_START_MS, RENDER_LATE_MS);
    s->late_until = s->started + RENDER_LATE_MS;
    return 1;
}

int owed_supervisor_poll_start(struct owed_supervisor *s) {
    if (!s || !s->late_until) return 0;
    if (!owed_render_is_alive(s)) return 0;
    return late_start_ready(s);
}

bool owed_supervisor_starting(struct owed_supervisor *s) {
    return s && s->late_until;
}

void owed_supervisor_reap(struct owed_supervisor *s) {
    int status = 0;
    pid_t w;
    if (!s) {
        return;
    }
    if (s->child <= 0) return;
    w = waitpid(s->child, &status, WNOHANG);
    if (w == s->child || (w < 0 && errno == ECHILD)) {
        OWE_WARN("owe-render pid=%d exited status=%d", (int)s->child, status);
        s->child = 0;
        s->late_until = 0;
    }
}

void owed_supervisor_stop(struct owed_supervisor *s) {
    if (!s) return;
    s->next_restart = 0;
    s->late_until = 0;
    if (s->child <= 0) return;
    kill(s->child, SIGTERM);
    {
        int64_t deadline = monotonic_ms() + RENDER_STOP_MS;
        bool exited = false;
        while (!exited && monotonic_ms() < deadline) {
            pid_t w = waitpid(s->child, NULL, WNOHANG);
            exited = w == s->child || (w < 0 && errno == ECHILD);
            if (!exited) usleep(50000);
        }
        if (!exited) {
            kill(s->child, SIGKILL);
            while (waitpid(s->child, NULL, 0) < 0 && errno == EINTR) {}
        }
    }
    s->child = 0;
}

int owed_supervisor_send(struct owed_supervisor *s, const char *line, char *reply,
                         unsigned long reply_len) {
    int fd;
    if (!s || !line) {
        return -1;
    }
    if (reply && reply_len) reply[0] = '\0';
    /* Only the daemon's restart path may spawn: it also resets loaded-media
     * and playback state. A send must never silently create an empty renderer.
     * A late renderer gets commands after the main loop has taken it. */
    if (!owed_render_is_alive(s) || s->late_until) {
        return -1;
    }
    fd = owe_ipc_connect(s->socket_path);
    if (fd < 0) {
        return -1;
    }
    owe_ipc_set_timeout(fd, 5000);
    if (owe_ipc_send_line(fd, line) != 0) {
        close(fd);
        return -1;
    }
    if (reply && reply_len > 0) {
        if (owe_ipc_recv_line(fd, reply, reply_len) == 0) {
            close(fd);
            return 0;
        }
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

int owed_supervisor_load(struct owed_supervisor *s, const char *path, const char *kind,
                         const char *from, bool hold) {
    char *line = NULL;
    char reply[8192];
    char *qp = owe_json_quote(path), *qk = owe_json_quote(kind);
    char *qf = from && *from ? owe_json_quote(from) : NULL;
    int rc = -1;
    if (qp && qk) {
        if (qf) {
            if (asprintf(&line, "{\"cmd\":\"load\",\"path\":%s,\"kind\":%s,\"from\":%s,\"hold\":%s}",
                         qp, qk, qf, hold ? "true" : "false") >= 0)
                rc = owed_supervisor_send(s, line, reply, sizeof(reply));
        } else if (asprintf(&line, "{\"cmd\":\"load\",\"path\":%s,\"kind\":%s,\"async\":true}", qp, qk) >= 0) {
            rc = owed_supervisor_send(s, line, reply, sizeof(reply));
        }
    }
    free(qp);
    free(qk);
    free(qf);
    free(line);
    if (rc != 0) return -1;
    if (!owe_json_ok(reply)) {
        OWE_ERROR("renderer load rejected: %s", reply);
        return -1;
    }
    return 0;
}

static int send_ok(struct owed_supervisor *s, const char *line) {
    char reply[1024];
    return owed_supervisor_send(s, line, reply, sizeof(reply)) == 0 && owe_json_ok(reply) ? 0 : -1;
}

int owed_supervisor_pause(struct owed_supervisor *s) {
    return send_ok(s, "{\"cmd\":\"pause\"}");
}

int owed_supervisor_resume(struct owed_supervisor *s) {
    return send_ok(s, "{\"cmd\":\"resume\"}");
}

int owed_supervisor_feed_start(struct owed_supervisor *s) {
    return send_ok(s, "{\"cmd\":\"feed\"}");
}

int owed_supervisor_feed_stop(struct owed_supervisor *s) {
    return send_ok(s, "{\"cmd\":\"feed-stop\"}");
}

int owed_supervisor_stop_render(struct owed_supervisor *s) {
    return send_ok(s, "{\"cmd\":\"stop\"}");
}

int owed_supervisor_intro_show(struct owed_supervisor *s) {
    return send_ok(s, "{\"cmd\":\"intro-show\"}");
}

int owed_supervisor_fade(struct owed_supervisor *s, int ms) {
    char line[128];
    snprintf(line, sizeof(line), "{\"cmd\":\"fade\",\"ms\":%d}", ms);
    return send_ok(s, line);
}

int owed_render_is_alive(struct owed_supervisor *s) {
    if (!s || s->child <= 0) {
        return 0;
    }
    owed_supervisor_reap(s);
    return s->child > 0;
}

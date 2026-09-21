#include "common_ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#include <time.h>

int64_t owe_ipc_now_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

void owe_ipc_client_close(struct owe_ipc_client *c) {
    if (c->fd >= 0) close(c->fd);
    c->fd = -1;
    c->len = 0;
    c->request_ms = 0;
    free(c->output);
    c->output = NULL;
    c->output_len = c->output_sent = 0;
}

void owe_ipc_client_open(struct owe_ipc_client *c, int fd) {
    owe_ipc_client_close(c);
    c->fd = fd;
    c->generation++;
    c->active_ms = owe_ipc_now_ms();
}

short owe_ipc_client_events(const struct owe_ipc_client *c) {
    return POLLIN | (c->output_len > c->output_sent ? POLLOUT : 0);
}

static void client_flush(struct owe_ipc_client *c) {
    while (c->fd >= 0 && c->output_sent < c->output_len) {
        ssize_t n = send(c->fd, c->output + c->output_sent,
                         c->output_len - c->output_sent, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        if (n <= 0) {
            owe_ipc_client_close(c);
            return;
        }
        c->output_sent += (size_t)n;
    }
    free(c->output);
    c->output = NULL;
    c->output_len = c->output_sent = 0;
}

void owe_ipc_client_send(struct owe_ipc_client *c, const char *line) {
    if (c->fd < 0 || !line) return;
    size_t len = strlen(line);
    size_t pending = c->output_len - c->output_sent;
    if (len >= OWE_IPC_MAX_LINE || pending + len + 1 > 4 * OWE_IPC_MAX_LINE) {
        owe_ipc_client_close(c);
        return;
    }
    if (pending) memmove(c->output, c->output + c->output_sent, pending);
    char *output = realloc(c->output, pending + len + 1);
    if (!output) {
        owe_ipc_client_close(c);
        return;
    }
    c->output = output;
    memcpy(output + pending, line, len);
    output[pending + len] = '\n';
    c->output_len = pending + len + 1;
    c->output_sent = 0;
    client_flush(c);
}

void owe_ipc_client_poll(struct owe_ipc_client *c, int64_t now, owe_ipc_handler handler,
                         void *context) {
    if (c->fd < 0) return;
    if (now - c->active_ms >= OWE_IPC_CLIENT_IDLE_MS ||
        (c->len && now - c->request_ms >= OWE_IPC_CLIENT_IDLE_MS)) {
        owe_ipc_client_close(c);
        return;
    }
    client_flush(c);
    if (c->fd < 0) return;
    ssize_t n = recv(c->fd, c->buf + c->len, sizeof(c->buf) - c->len - 1, MSG_DONTWAIT);
    if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) return;
    if (n <= 0 || memchr(c->buf + c->len, '\0', (size_t)n)) {
        owe_ipc_client_close(c);
        return;
    }
    if (!c->len) c->request_ms = now;
    c->active_ms = now;
    c->len += (size_t)n;
    c->buf[c->len] = '\0';
    char *nl;
    while ((nl = strchr(c->buf, '\n'))) {
        *nl = '\0';
        if (nl > c->buf && nl[-1] == '\r') nl[-1] = '\0';
        if (*c->buf) handler(context, c, c->buf);
        if (c->fd < 0) return;
        size_t used = (size_t)(nl - c->buf) + 1;
        memmove(c->buf, c->buf + used, c->len - used + 1);
        c->len -= used;
        c->request_ms = now;
    }
    if (c->len == sizeof(c->buf) - 1) owe_ipc_client_close(c);
}

struct owe_ipc_server {
    int fd;
    int lock_fd;
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
};

int owe_ipc_listen(const char *path) {
    struct sockaddr_un addr;
    int fd;
    if (!path || strlen(path) >= sizeof(addr.sun_path)) {
        return -1;
    }
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    unlink(path);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    chmod(path, 0600);
    if (listen(fd, 16) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int owe_ipc_connect(const char *path) {
    struct sockaddr_un addr;
    int fd;
    if (!path || strlen(path) >= sizeof(addr.sun_path)) {
        return -1;
    }
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    owe_ipc_set_timeout(fd, 5000);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int owe_ipc_set_timeout(int fd, int ms) {
    struct timeval tv;
    if (fd < 0 || ms <= 0) {
        return -1;
    }
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
        return -1;
    }
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0) {
        return -1;
    }
    return 0;
}

int owe_ipc_send_line(int fd, const char *line) {
    size_t len;
    size_t off = 0;
    if (!line) {
        return -1;
    }
    len = strlen(line);
    while (off < len) {
        ssize_t n = send(fd, line + off, len - off, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) return -1;
        off += (size_t)n;
    }
    ssize_t n;
    do { n = send(fd, "\n", 1, MSG_NOSIGNAL); } while (n < 0 && errno == EINTR);
    if (n != 1) return -1;
    return 0;
}

int owe_ipc_recv_line(int fd, char *buf, size_t len) {
    size_t off = 0;
    if (!buf || len < 2) {
        return -1;
    }
    for (;;) {
        char chunk[512];
        ssize_t peek = recv(fd, chunk, sizeof(chunk), MSG_PEEK);
        char *nl;
        size_t take;
        ssize_t got;
        size_t i;
        if (peek == 0) {
            return -1;
        }
        if (peek < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        /* Consume only up to the newline. A peek does not disturb bytes
         * that belong to a later line on the same connection. */
        nl = memchr(chunk, '\n', (size_t)peek);
        take = nl ? (size_t)(nl - chunk) + 1 : (size_t)peek;
        got = recv(fd, chunk, take, 0);
        if (got <= 0) {
            if (got < 0 && errno == EINTR) {
                continue;
            }
            return -1;
        }
        for (i = 0; i < (size_t)got; i++) {
            char c = chunk[i];
            if (c == '\n') {
                buf[off] = '\0';
                return off ? 0 : -1;
            }
            if (c == '\0') {
                return -1;
            }
            if (c != '\r') {
                if (off + 1 >= len) {
                    buf[off] = '\0';
                    errno = EMSGSIZE;
                    return -1;
                }
                buf[off++] = c;
            }
        }
    }
}

owe_ipc_server_t *owe_ipc_server_new(const char *path) {
    owe_ipc_server_t *srv;
    char lock_path[sizeof(((struct sockaddr_un *)0)->sun_path) + 8];
    if (!path || strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) return NULL;
    snprintf(lock_path, sizeof(lock_path), "%s.lock", path);
    int lock_fd = open(lock_path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (lock_fd < 0) return NULL;
    if (flock(lock_fd, LOCK_EX | LOCK_NB) < 0) {
        close(lock_fd);
        return NULL;
    }
    int fd = owe_ipc_listen(path);
    if (fd < 0) {
        close(lock_fd);
        return NULL;
    }
    srv = calloc(1, sizeof(*srv));
    if (!srv) {
        close(fd);
        unlink(path);
        close(lock_fd);
        return NULL;
    }
    srv->fd = fd;
    srv->lock_fd = lock_fd;
    snprintf(srv->path, sizeof(srv->path), "%s", path);
    return srv;
}

void owe_ipc_server_free(owe_ipc_server_t *srv) {
    if (!srv) {
        return;
    }
    close(srv->fd);
    unlink(srv->path);
    close(srv->lock_fd);
    free(srv);
}

int owe_ipc_server_fd(owe_ipc_server_t *srv) {
    return srv ? srv->fd : -1;
}

int owe_ipc_server_accept(owe_ipc_server_t *srv) {
    if (!srv) {
        return -1;
    }
    return accept4(srv->fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
}

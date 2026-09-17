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
    while (off + 1 < len) {
        char c;
        ssize_t n = recv(fd, &c, 1, 0);
        if (n == 0) return -1;
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (c == '\n') {
            buf[off] = '\0';
            return off ? 0 : -1;
        }
        if (c == '\0') return -1;
        if (c != '\r') {
            buf[off++] = c;
        }
    }
    buf[off] = '\0';
    errno = EMSGSIZE;
    return -1;
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

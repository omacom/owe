#include "feed.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <epoxy/gl.h>

#include "common_ipc.h"
#include "egl.h"
#include "log.h"
#include "mpv.h"
#include "xdg.h"

#define FEED_MAGIC 0x4645574fu /* "OWEF" */
#define FEED_VERSION 1
#define FEED_MAX_WIDTH 3840
#define FEED_MAX_HEIGHT 2160
#define FEED_FORMAT_RGBA8888 0
#define FEED_ACK_TIMEOUT_MS 1000

enum {
    FEED_MSG_HELLO = 1,
    FEED_MSG_FRAME = 2,
    FEED_MSG_ACK = 3,
};

struct feed_msg {
    uint32_t magic;
    uint32_t version;
    uint32_t type;
    uint32_t slot; /* slot count in HELLO, slot index in FRAME */
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
    uint64_t seq;
};

struct feed_slot {
    int fd;
    uint8_t *map;
    size_t size;
    unsigned int tex;
    unsigned int fbo;
    int w;
    int h;
    int stride;
    uint64_t seq;
    uint32_t pending; /* One bit per client that must acknowledge this frame. */
    int64_t published_ms;
};

struct feed_client {
    int fd;
    bool mapped;
    struct feed_msg msg;
    size_t len;
};

struct owe_feed {
    owe_ipc_server_t *srv;
    struct owe_egl *egl;
    struct feed_client clients[OWE_FEED_MAX_CLIENTS];
    struct feed_slot slots[OWE_FEED_SLOTS];
    int width;
    int height;
    int target_width;
    int target_height;
    uint64_t seq;
    bool active;
};

_Static_assert(OWE_FEED_MAX_CLIENTS <= 32, "feed client mask size");

static void slot_reset(struct feed_slot *s) {
    if (s->tex) {
        glDeleteTextures(1, &s->tex);
        s->tex = 0;
    }
    if (s->fbo) {
        glDeleteFramebuffers(1, &s->fbo);
        s->fbo = 0;
    }
    if (s->map) {
        munmap(s->map, s->size);
        s->map = NULL;
    }
    if (s->fd >= 0) {
        close(s->fd);
        s->fd = -1;
    }
    s->size = 0;
    s->seq = 0;
    s->pending = 0;
}

static int slot_alloc(struct feed_slot *s, int w, int h) {
    GLenum status;
    s->fd = memfd_create("owe-lock-feed", MFD_CLOEXEC);
    if (s->fd < 0) {
        OWE_ERROR("feed memfd failed: %s", strerror(errno));
        return -1;
    }
    s->w = w;
    s->h = h;
    s->stride = w * 4;
    s->size = (size_t)s->stride * (size_t)h;
    if (ftruncate(s->fd, (off_t)s->size) != 0) {
        OWE_ERROR("feed ftruncate failed: %s", strerror(errno));
        slot_reset(s);
        return -1;
    }
    s->map = mmap(NULL, s->size, PROT_READ | PROT_WRITE, MAP_SHARED, s->fd, 0);
    if (s->map == MAP_FAILED) {
        s->map = NULL;
        OWE_ERROR("feed mmap failed: %s", strerror(errno));
        slot_reset(s);
        return -1;
    }
    glGenTextures(1, &s->tex);
    glBindTexture(GL_TEXTURE_2D, s->tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glGenFramebuffers(1, &s->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, s->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s->tex, 0);
    status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        OWE_ERROR("feed framebuffer incomplete: 0x%x", (unsigned)status);
        slot_reset(s);
        return -1;
    }
    return 0;
}

static int send_msg(int fd, const struct feed_msg *msg, const int *fds, int nfds) {
    struct iovec iov;
    struct msghdr hdr;
    char control[CMSG_SPACE(sizeof(int) * OWE_FEED_SLOTS)];
    ssize_t n;
    iov.iov_base = (void *)msg;
    iov.iov_len = sizeof(*msg);
    memset(&hdr, 0, sizeof(hdr));
    hdr.msg_iov = &iov;
    hdr.msg_iovlen = 1;
    if (nfds > 0) {
        struct cmsghdr *cmsg;
        hdr.msg_control = control;
        hdr.msg_controllen = CMSG_SPACE(sizeof(int) * (size_t)nfds);
        cmsg = CMSG_FIRSTHDR(&hdr);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int) * (size_t)nfds);
        memcpy(CMSG_DATA(cmsg), fds, sizeof(int) * (size_t)nfds);
    }
    n = sendmsg(fd, &hdr, MSG_NOSIGNAL);
    return n == (ssize_t)sizeof(*msg) ? 0 : -1;
}

static int mapped_count(struct owe_feed *f) {
    int i;
    int count = 0;
    for (i = 0; i < OWE_FEED_MAX_CLIENTS; i++) {
        if (f->clients[i].fd >= 0 && f->clients[i].mapped) {
            count++;
        }
    }
    return count;
}

static int client_count(struct owe_feed *f) {
    int i;
    int count = 0;
    for (i = 0; i < OWE_FEED_MAX_CLIENTS; i++) {
        if (f->clients[i].fd >= 0) {
            count++;
        }
    }
    return count;
}

static void client_remove(struct owe_feed *f, int idx) {
    struct feed_client *c = &f->clients[idx];
    int i;
    if (c->fd < 0) {
        return;
    }
    for (i = 0; i < OWE_FEED_SLOTS; i++) {
        f->slots[i].pending &= ~(UINT32_C(1) << idx);
    }
    close(c->fd);
    c->fd = -1;
    c->mapped = false;
    c->len = 0;
}

static void send_hello(struct owe_feed *f, struct feed_client *c) {
    struct feed_msg hello;
    int fds[OWE_FEED_SLOTS];
    int i;
    for (i = 0; i < OWE_FEED_SLOTS; i++) {
        fds[i] = f->slots[i].fd;
    }
    memset(&hello, 0, sizeof(hello));
    hello.magic = FEED_MAGIC;
    hello.version = FEED_VERSION;
    hello.type = FEED_MSG_HELLO;
    hello.slot = OWE_FEED_SLOTS;
    hello.width = (uint32_t)f->width;
    hello.height = (uint32_t)f->height;
    hello.stride = (uint32_t)(f->width * 4);
    hello.format = FEED_FORMAT_RGBA8888;
    if (send_msg(c->fd, &hello, fds, OWE_FEED_SLOTS) != 0) {
        client_remove(f, (int)(c - f->clients));
        return;
    }
    c->mapped = true;
}

static int ensure_slots(struct owe_feed *f, int w, int h) {
    int i;
    if (f->width == w && f->height == h && f->slots[0].fd >= 0) {
        return 0;
    }
    for (i = 0; i < OWE_FEED_SLOTS; i++) {
        slot_reset(&f->slots[i]);
    }
    f->width = w;
    f->height = h;
    /* Keep frame sequences unique across buffer changes. Old ACKs can arrive later. */
    for (i = 0; i < OWE_FEED_SLOTS; i++) {
        if (slot_alloc(&f->slots[i], w, h) != 0) {
            int j;
            for (j = 0; j <= i; j++) {
                slot_reset(&f->slots[j]);
            }
            f->width = 0;
            f->height = 0;
            return -1;
        }
    }
    for (i = 0; i < OWE_FEED_MAX_CLIENTS; i++) {
        struct feed_client *c = &f->clients[i];
        if (c->fd >= 0) {
            c->mapped = false;
            send_hello(f, c);
        }
    }
    OWE_INFO("feed slots %dx%d", w, h);
    return 0;
}

struct owe_feed *owe_feed_new(const char *socket_path, struct owe_egl *egl) {
    struct owe_feed *f;
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
    f = calloc(1, sizeof(*f));
    if (!f) {
        return NULL;
    }
    for (i = 0; i < OWE_FEED_MAX_CLIENTS; i++) {
        f->clients[i].fd = -1;
    }
    for (i = 0; i < OWE_FEED_SLOTS; i++) {
        f->slots[i].fd = -1;
    }
    f->egl = egl;
    f->srv = owe_ipc_server_new(socket_path);
    if (!f->srv) {
        free(f);
        return NULL;
    }
    fcntl(owe_ipc_server_fd(f->srv), F_SETFL, O_NONBLOCK);
    return f;
}

void owe_feed_free(struct owe_feed *f) {
    int i;
    if (!f) {
        return;
    }
    if (f->egl) {
        owe_egl_make_current(f->egl);
    }
    for (i = 0; i < OWE_FEED_MAX_CLIENTS; i++) {
        client_remove(f, i);
    }
    for (i = 0; i < OWE_FEED_SLOTS; i++) {
        slot_reset(&f->slots[i]);
    }
    owe_ipc_server_free(f->srv);
    free(f);
}

int owe_feed_fd(struct owe_feed *f) {
    return f ? owe_ipc_server_fd(f->srv) : -1;
}

void owe_feed_accept(struct owe_feed *f) {
    int fd;
    int i;
    if (!f) {
        return;
    }
    while ((fd = owe_ipc_server_accept(f->srv)) >= 0) {
        fcntl(fd, F_SETFL, O_NONBLOCK);
        for (i = 0; i < OWE_FEED_MAX_CLIENTS; i++) {
            if (f->clients[i].fd < 0) {
                f->clients[i].fd = fd;
                f->clients[i].mapped = false;
                f->clients[i].len = 0;
                break;
            }
        }
        if (i == OWE_FEED_MAX_CLIENTS) {
            close(fd);
            continue;
        }
        if (f->width > 0 && f->height > 0 && f->slots[0].fd >= 0 && f->active) {
            send_hello(f, &f->clients[i]);
        }
    }
}

int owe_feed_pollfds(struct owe_feed *f, struct pollfd *fds) {
    int n = 0;
    int i;
    for (i = 0; f && i < OWE_FEED_MAX_CLIENTS; i++) {
        if (f->clients[i].fd >= 0) {
            fds[n].fd = f->clients[i].fd;
            fds[n].events = POLLIN;
            n++;
        }
    }
    return n;
}

static void handle_ack(struct owe_feed *f, int client, uint32_t slot, uint64_t seq) {
    if (slot < OWE_FEED_SLOTS && f->slots[slot].seq == seq) {
        f->slots[slot].pending &= ~(UINT32_C(1) << client);
    }
}

static void poll_client(struct owe_feed *f, int idx) {
    struct feed_client *c = &f->clients[idx];
    for (int messages = 0; messages < 64; messages++) {
        ssize_t n = recv(c->fd, (char *)&c->msg + c->len, sizeof(c->msg) - c->len, 0);
        if (n == 0) {
            client_remove(f, idx);
            return;
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                return;
            }
            client_remove(f, idx);
            return;
        }
        c->len += (size_t)n;
        if (c->len < sizeof(c->msg)) {
            return;
        }
        if (c->msg.magic == FEED_MAGIC && c->msg.version == FEED_VERSION &&
            c->msg.type == FEED_MSG_ACK) {
            handle_ack(f, idx, c->msg.slot, c->msg.seq);
        }
        c->len = 0;
    }
}

void owe_feed_poll_clients(struct owe_feed *f, struct owe_mpv *m) {
    int i;
    if (!f) {
        return;
    }
    for (i = 0; i < OWE_FEED_MAX_CLIENTS; i++) {
        if (f->clients[i].fd >= 0) {
            poll_client(f, i);
        }
    }
    int64_t now = owe_ipc_now_ms();
    for (i = 0; i < OWE_FEED_SLOTS; i++) {
        struct feed_slot *slot = &f->slots[i];
        if (slot->pending && now - slot->published_ms >= FEED_ACK_TIMEOUT_MS) {
            for (int client = 0; client < OWE_FEED_MAX_CLIENTS; client++) {
                if (slot->pending & (UINT32_C(1) << client)) {
                    OWE_WARN("feed client %d missed its acknowledgement deadline", client);
                    client_remove(f, client);
                }
            }
        }
    }
    if (f->active && m) {
        /* A new client needs decode to start before the first buffers exist. */
        owe_mpv_set_paused(m, client_count(f) == 0);
    }
}

void owe_feed_start(struct owe_feed *f) {
    int i;
    if (!f) {
        return;
    }
    f->active = true;
    if (f->width > 0 && f->height > 0 && f->slots[0].fd >= 0) {
        for (i = 0; i < OWE_FEED_MAX_CLIENTS; i++) {
            if (f->clients[i].fd >= 0 && !f->clients[i].mapped) {
                send_hello(f, &f->clients[i]);
            }
        }
    }
    OWE_INFO("feed started");
}

void owe_feed_stop(struct owe_feed *f) {
    int i;
    if (!f) {
        return;
    }
    f->active = false;
    for (i = 0; i < OWE_FEED_MAX_CLIENTS; i++) {
        client_remove(f, i);
    }
    if (f->egl) owe_egl_make_current(f->egl);
    for (i = 0; i < OWE_FEED_SLOTS; i++) slot_reset(&f->slots[i]);
    f->width = f->height = 0;
    OWE_INFO("feed stopped");
}

bool owe_feed_running(struct owe_feed *f) {
    return f && f->active;
}

void owe_feed_set_target_size(struct owe_feed *f, int width, int height) {
    if (!f) return;
    f->target_width = width;
    f->target_height = height;
}

static void frame_size(struct owe_feed *f, int *w, int *h) {
    double scale = 1.0;
    if (f->target_width > 0 && f->target_height > 0) {
        double sx = (double)f->target_width / *w;
        double sy = (double)f->target_height / *h;
        double cover = sx > sy ? sx : sy;
        if (cover < scale) scale = cover;
    }
    double sx = (double)FEED_MAX_WIDTH / *w;
    double sy = (double)FEED_MAX_HEIGHT / *h;
    if (sx < scale) scale = sx;
    if (sy < scale) scale = sy;
    if (scale < 1.0) {
        *w = (int)(*w * scale);
        *h = (int)(*h * scale);
        if (*w < 1) *w = 1;
        if (*h < 1) *h = 1;
    }
}

int owe_feed_publish(struct owe_feed *f, struct owe_mpv *m) {
    struct feed_slot *s;
    struct feed_msg frame;
    int w = 0;
    int h = 0;
    int slot = -1;
    int i;
    if (!f || !f->active || !m || client_count(f) == 0) {
        return -1;
    }
    if (!owe_mpv_has_video(m) || owe_mpv_video_size(m, &w, &h) != 0) {
        return -1;
    }
    if (w <= 0 || h <= 0) {
        return -1;
    }
    frame_size(f, &w, &h);
    owe_egl_make_current(f->egl);
    if (ensure_slots(f, w, h) != 0) {
        return -1;
    }
    if (mapped_count(f) == 0) {
        return -1;
    }
    for (i = 0; i < OWE_FEED_SLOTS; i++) {
        if (!f->slots[i].pending) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return -1;
    }
    s = &f->slots[slot];
    if (owe_mpv_render_fbo(m, (int)s->fbo, s->w, s->h) != 0) {
        return -1;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, s->fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, s->w, s->h, GL_RGBA, GL_UNSIGNED_BYTE, s->map);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    s->seq = ++f->seq;
    s->published_ms = owe_ipc_now_ms();
    s->pending = 0;
    memset(&frame, 0, sizeof(frame));
    frame.magic = FEED_MAGIC;
    frame.version = FEED_VERSION;
    frame.type = FEED_MSG_FRAME;
    frame.slot = (uint32_t)slot;
    frame.width = (uint32_t)s->w;
    frame.height = (uint32_t)s->h;
    frame.stride = (uint32_t)s->stride;
    frame.format = FEED_FORMAT_RGBA8888;
    frame.seq = s->seq;
    for (i = OWE_FEED_MAX_CLIENTS - 1; i >= 0; i--) {
        struct feed_client *c = &f->clients[i];
        if (c->fd < 0 || !c->mapped) {
            continue;
        }
        if (send_msg(c->fd, &frame, NULL, 0) == 0) {
            s->pending |= UINT32_C(1) << i;
        } else {
            client_remove(f, i);
        }
    }
    return s->pending ? 0 : -1;
}

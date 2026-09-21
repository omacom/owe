#include "../src/render/feed.c"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

struct owe_mpv { bool paused; };
static char socket_path[108];

void owe_mpv_set_paused(struct owe_mpv *m, bool paused) { m->paused = paused; }
bool owe_mpv_has_video(struct owe_mpv *m) { return m != NULL; }
int owe_mpv_video_size(struct owe_mpv *m, int *w, int *h) {
    (void)m;
    *w = *h = 1;
    return 0;
}
int owe_mpv_render_fbo(struct owe_mpv *m, int fbo, int w, int h) {
    (void)m; (void)fbo; (void)w; (void)h;
    return 0;
}
int owe_egl_make_current(struct owe_egl *egl) { (void)egl; return 0; }

/* Replace only GPU calls. The tests use real sockets, memfds, and protocol messages. */
static void bind_framebuffer(GLenum target, GLuint fbo) { (void)target; (void)fbo; }
static void pixel_store(GLenum name, GLint value) { (void)name; (void)value; }
static void read_pixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format,
                        GLenum type, void *data) {
    (void)x; (void)y; (void)format; (void)type;
    memset(data, 0xff, (size_t)w * (size_t)h * 4);
}

static struct owe_feed *new_feed(void) {
    struct owe_feed *f = owe_feed_new(socket_path, NULL);
    CHECK(f);
    return f;
}

static void prepare_slots(struct owe_feed *f) {
    f->width = f->height = 1;
    for (int i = 0; i < OWE_FEED_SLOTS; i++) {
        struct feed_slot *s = &f->slots[i];
        s->fd = memfd_create("owe-feed-test", MFD_CLOEXEC);
        CHECK(s->fd >= 0);
        s->w = s->h = 1;
        s->stride = s->size = 4;
        CHECK(ftruncate(s->fd, (off_t)s->size) == 0);
        s->map = mmap(NULL, s->size, PROT_READ | PROT_WRITE, MAP_SHARED, s->fd, 0);
        CHECK(s->map != MAP_FAILED);
    }
}

static int connect_client(struct owe_feed *f) {
    int fd = owe_ipc_connect(socket_path);
    CHECK(fd >= 0);
    owe_feed_accept(f);
    return fd;
}

static struct feed_msg receive(int fd, uint32_t type) {
    struct feed_msg message;
    struct iovec iov = {.iov_base = &message, .iov_len = sizeof(message)};
    union {
        struct cmsghdr align;
        char bytes[CMSG_SPACE(sizeof(int) * OWE_FEED_SLOTS)];
    } control;
    struct msghdr header = {.msg_iov = &iov, .msg_iovlen = 1,
                           .msg_control = control.bytes, .msg_controllen = sizeof(control.bytes)};
    CHECK(recvmsg(fd, &header, 0) == sizeof(message));
    CHECK(!(header.msg_flags & MSG_CTRUNC));
    CHECK(message.magic == FEED_MAGIC && message.version == FEED_VERSION && message.type == type);
    int count = 0;
    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&header); cmsg; cmsg = CMSG_NXTHDR(&header, cmsg)) {
        CHECK(cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS);
        int *fds = (int *)CMSG_DATA(cmsg);
        size_t n = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        for (size_t i = 0; i < n; i++) {
            struct stat st;
            CHECK(fstat(fds[i], &st) == 0 && st.st_size == 4);
            close(fds[i]);
            count++;
        }
    }
    CHECK(count == (type == FEED_MSG_HELLO ? OWE_FEED_SLOTS : 0));
    return message;
}

static void ack(int fd, struct feed_msg frame) {
    frame.type = FEED_MSG_ACK;
    CHECK(send(fd, &frame, sizeof(frame), MSG_NOSIGNAL) == sizeof(frame));
}

static void check_no_message(int fd) {
    char byte;
    CHECK(recv(fd, &byte, 1, MSG_DONTWAIT) < 0 && errno == EAGAIN);
}

static void test_restart(void) {
    struct owe_feed *f = new_feed();
    struct owe_mpv mpv = {0};
    uint64_t previous_seq = 0;
    for (int i = 0; i < 2; i++) {
        prepare_slots(f);
        int fd = connect_client(f);
        check_no_message(fd);
        owe_feed_start(f);
        receive(fd, FEED_MSG_HELLO);
        CHECK(owe_feed_publish(f, &mpv) == 0);
        struct feed_msg frame = receive(fd, FEED_MSG_FRAME);
        CHECK(frame.seq > previous_seq);
        previous_seq = frame.seq;
        owe_feed_stop(f);
        CHECK(f->width == 0 && f->height == 0);
        for (int slot = 0; slot < OWE_FEED_SLOTS; slot++) {
            CHECK(f->slots[slot].fd == -1 && f->slots[slot].map == NULL);
        }
        char byte;
        CHECK(recv(fd, &byte, 1, 0) == 0);
        close(fd);
    }
    owe_feed_free(f);
}

static void test_client_pause(void) {
    struct owe_feed *f = new_feed();
    struct owe_mpv mpv = {0};
    owe_feed_start(f);
    owe_feed_poll_clients(f, &mpv);
    CHECK(mpv.paused);
    int fd = connect_client(f);
    owe_feed_poll_clients(f, &mpv);
    CHECK(!mpv.paused); /* Decode must start before the first buffers exist. */
    close(fd);
    owe_feed_poll_clients(f, &mpv);
    CHECK(mpv.paused);
    prepare_slots(f);
    fd = connect_client(f);
    receive(fd, FEED_MSG_HELLO);
    owe_feed_poll_clients(f, &mpv);
    CHECK(!mpv.paused);
    close(fd);
    owe_feed_poll_clients(f, &mpv);
    CHECK(mpv.paused);
    fd = connect_client(f);
    receive(fd, FEED_MSG_HELLO);
    owe_feed_poll_clients(f, &mpv);
    CHECK(!mpv.paused);
    owe_feed_stop(f);
    owe_feed_poll_clients(f, &mpv);
    CHECK(!mpv.paused); /* The daemon owns pause state outside the feed. */
    close(fd);
    owe_feed_free(f);
}

static void test_frame_ownership(void) {
    struct owe_feed *f = new_feed();
    struct owe_mpv mpv = {0};
    struct feed_msg frames[OWE_FEED_SLOTS];
    prepare_slots(f);
    owe_feed_start(f);
    int first = connect_client(f), second = connect_client(f);
    receive(first, FEED_MSG_HELLO);
    receive(second, FEED_MSG_HELLO);
    for (int i = 0; i < OWE_FEED_SLOTS; i++) {
        CHECK(owe_feed_publish(f, &mpv) == 0);
        frames[i] = receive(first, FEED_MSG_FRAME);
        struct feed_msg other = receive(second, FEED_MSG_FRAME);
        CHECK(frames[i].slot == other.slot && frames[i].seq == other.seq);
    }
    CHECK(owe_feed_publish(f, &mpv) == -1);
    ack(first, frames[0]);
    ack(first, frames[0]);
    owe_feed_poll_clients(f, &mpv);
    CHECK(owe_feed_publish(f, &mpv) == -1);

    int late = connect_client(f);
    receive(late, FEED_MSG_HELLO);
    ack(late, frames[0]);
    owe_feed_poll_clients(f, &mpv);
    CHECK(owe_feed_publish(f, &mpv) == -1);
    close(late);
    owe_feed_poll_clients(f, &mpv);
    CHECK(owe_feed_publish(f, &mpv) == -1);

    ack(second, frames[0]);
    owe_feed_poll_clients(f, &mpv);
    CHECK(owe_feed_publish(f, &mpv) == 0);
    struct feed_msg replacement = receive(first, FEED_MSG_FRAME);
    receive(second, FEED_MSG_FRAME);
    CHECK(replacement.slot == frames[0].slot && replacement.seq > frames[0].seq);
    ack(first, frames[0]);
    ack(second, frames[0]);
    owe_feed_poll_clients(f, &mpv);
    CHECK(owe_feed_publish(f, &mpv) == -1);
    close(first);
    owe_feed_poll_clients(f, &mpv);
    CHECK(owe_feed_publish(f, &mpv) == -1);
    ack(second, replacement);
    owe_feed_poll_clients(f, &mpv);
    CHECK(owe_feed_publish(f, &mpv) == 0);
    close(second);
    owe_feed_free(f);
}

static void test_send_failure(void) {
    struct owe_feed *f = new_feed();
    struct owe_mpv mpv = {0};
    prepare_slots(f);
    owe_feed_start(f);
    int failed = connect_client(f), healthy = connect_client(f);
    receive(failed, FEED_MSG_HELLO);
    receive(healthy, FEED_MSG_HELLO);
    close(failed);
    for (int i = 0; i < OWE_FEED_SLOTS; i++) {
        CHECK(owe_feed_publish(f, &mpv) == 0);
        receive(healthy, FEED_MSG_FRAME);
    }
    CHECK(owe_feed_publish(f, &mpv) == -1);
    close(healthy);
    owe_feed_free(f);
}

static void test_stalled_client(void) {
    struct owe_feed *f = new_feed();
    struct owe_mpv mpv = {0};
    prepare_slots(f);
    owe_feed_start(f);
    int stalled = connect_client(f), healthy = connect_client(f);
    receive(stalled, FEED_MSG_HELLO);
    receive(healthy, FEED_MSG_HELLO);
    for (int i = 0; i < OWE_FEED_SLOTS; i++) {
        CHECK(owe_feed_publish(f, &mpv) == 0);
        receive(stalled, FEED_MSG_FRAME);
        ack(healthy, receive(healthy, FEED_MSG_FRAME));
        owe_feed_poll_clients(f, &mpv);
    }
    CHECK(owe_feed_publish(f, &mpv) == -1);
    f->slots[0].published_ms -= FEED_ACK_TIMEOUT_MS;
    owe_feed_poll_clients(f, &mpv);
    char byte;
    CHECK(recv(stalled, &byte, 1, 0) == 0);
    CHECK(!mpv.paused);
    CHECK(owe_feed_publish(f, &mpv) == 0);
    receive(healthy, FEED_MSG_FRAME);
    close(stalled);
    close(healthy);
    owe_feed_free(f);
}

int main(void) {
    char root[] = "/tmp/owe-feed-XXXXXX";
    CHECK(mkdtemp(root));
    CHECK(snprintf(socket_path, sizeof(socket_path), "%s/feed.sock", root) < (int)sizeof(socket_path));
    epoxy_glBindFramebuffer = bind_framebuffer;
    epoxy_glPixelStorei = pixel_store;
    epoxy_glReadPixels = read_pixels;
    struct owe_feed dimensions = {.target_width = 1920, .target_height = 1080};
    int w = 3840, h = 2160;
    frame_size(&dimensions, &w, &h);
    CHECK(w == 1920 && h == 1080);
    w = 1200; h = 2400;
    frame_size(&dimensions, &w, &h);
    CHECK(w == 1080 && h == 2160); /* Protocol cap, with aspect preserved. */
    w = 640; h = 360;
    frame_size(&dimensions, &w, &h);
    CHECK(w == 640 && h == 360); /* Never upscale readback. */
    test_restart();
    test_client_pause();
    test_frame_ownership();
    test_send_failure();
    test_stalled_client();
    char lock[128];
    snprintf(lock, sizeof(lock), "%s.lock", socket_path);
    unlink(lock);
    CHECK(rmdir(root) == 0);
    puts("feed lifecycle and frame ownership checks passed");
    return 0;
}

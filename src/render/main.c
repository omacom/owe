#include "render.h"
#include "log.h"
#include "xdg.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifdef __GLIBC__
#include <malloc.h>
#endif

#include "egl.h"
#include "feed.h"
#include "render_ipc.h"
#include "common_ipc.h"
#include "mpv.h"
#include "still.h"
#include "wayland.h"

static owe_app_t g_app;
static int g_sigpipe[2];
static int g_last_max_w = -1;
static int g_last_max_h = -1;

owe_app_t *owe_app_get(void) {
    return &g_app;
}

void owe_app_request_render(void) {
    owe_wayland_request_render(g_app.wl);
}

void owe_app_on_outputs_changed(void) {
    if (g_app.wl && g_app.egl) {
        owe_wayland_attach_egl(g_app.wl, g_app.egl);
    }
}

static int fail_init(void) {
    close(g_sigpipe[0]);
    close(g_sigpipe[1]);
    return 1;
}

/* Media buffers are large and long-lived, and glibc keeps freed blocks
 * resident by default. Returning them to the OS trims roughly 30 MiB of
 * RSS for a 4K video with no measurable CPU cost. */
static void tune_allocator(void) {
#ifdef __GLIBC__
    mallopt(M_TRIM_THRESHOLD, 0);
    mallopt(M_MMAP_THRESHOLD, 128 * 1024);
#endif
}

static void on_signal(int sig) {
    int saved_errno = errno;
    (void)sig;
    if (g_app.running) {
        char c = 'q';
        ssize_t n = write(g_sigpipe[1], &c, 1);
        (void)n;
    }
    errno = saved_errno;
}

static void cleanup(void) {
    owe_render_ipc_free(g_app.ipc);
    owe_feed_free(g_app.feed);
    owe_still_free(g_app.still);
    owe_mpv_free(g_app.mpv);
    owe_wayland_destroy_outputs(g_app.wl);
    owe_egl_free(g_app.egl);
    g_app.egl = NULL;
    owe_wayland_free(g_app.wl);
}

/* The feed socket sits next to the render socket, so a test renderer with a
 * private socket gets its own feed socket too. */
static void feed_socket_path(const char *render_socket, char *buf, size_t len) {
    char *slash;
    snprintf(buf, len, "%s", render_socket);
    slash = strrchr(buf, '/');
    if (slash) {
        snprintf(slash + 1, len - (size_t)(slash - buf + 1), "lock-feed.sock");
    }
}

static void usage(const char *argv0) {
    fprintf(stderr,
            "Usage: %s [options]\n"
            "  --socket PATH   IPC socket path (default: $XDG_RUNTIME_DIR/owe/render.sock)\n"
            "  --verbose       Enable debug logging\n"
            "  --help          Show this help\n",
            argv0);
}

int main(int argc, char **argv) {
    char socket_path[4096];
    char feed_path[4096];
    bool verbose = false;
    int i;

    if (owe_socket_path_render(socket_path, sizeof(socket_path)) != 0) {
        fprintf(stderr, "owe-render: cannot resolve socket path\n");
        return 1;
    }
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
            snprintf(socket_path, sizeof(socket_path), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--verbose") == 0 || strcmp(argv[i], "-v") == 0) {
            verbose = true;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 1;
        }
    }

    owe_log_init("owe-render", verbose ? OWE_LOG_DEBUG : OWE_LOG_INFO);
    tune_allocator();

    if (pipe2(g_sigpipe, O_NONBLOCK | O_CLOEXEC) != 0) {
        OWE_ERROR("pipe failed");
        return 1;
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    memset(&g_app, 0, sizeof(g_app));
    g_app.running = true;

    g_app.wl = owe_wayland_new();
    if (!g_app.wl) {
        OWE_ERROR("wayland init failed");
        return fail_init();
    }
    g_app.egl = owe_egl_new(g_app.wl);
    if (!g_app.egl) {
        OWE_ERROR("egl init failed");
        owe_wayland_free(g_app.wl);
        return fail_init();
    }
    owe_wayland_set_egl(g_app.wl, g_app.egl);
    owe_app_on_outputs_changed();
    g_app.mpv = owe_mpv_new(g_app.wl);
    if (!g_app.mpv) {
        OWE_ERROR("libmpv init failed");
        cleanup();
        return fail_init();
    }
    g_app.still = owe_still_new(g_app.wl, g_app.egl);
    if (!g_app.still) {
        OWE_ERROR("still init failed");
        cleanup();
        return fail_init();
    }
    g_app.ipc = owe_render_ipc_new(socket_path);
    if (!g_app.ipc) {
        OWE_ERROR("ipc init failed");
        cleanup();
        return fail_init();
    }
    feed_socket_path(socket_path, feed_path, sizeof(feed_path));
    g_app.feed = owe_feed_new(feed_path, g_app.egl);
    if (!g_app.feed) {
        OWE_ERROR("feed init failed");
        cleanup();
        return fail_init();
    }

    OWE_INFO("ready, socket=%s", socket_path);

    while (g_app.running) {
        int max_w = 0;
        int max_h = 0;
        if (owe_wayland_dispatch_pending(g_app.wl) < 0) break;
        if (!g_app.feeding) {
            owe_wayland_render_pending(g_app.wl);
        }
        owe_wayland_outputs_max_size(g_app.wl, &max_w, &max_h);
        owe_feed_set_target_size(g_app.feed, max_w, max_h);
        if (max_w != g_last_max_w || max_h != g_last_max_h) {
            /* A still texture is cut for the output that decoded it. Give a
             * larger output the extra detail. */
            if (owe_render_ipc_reload_still(g_app.ipc)) {
                g_last_max_w = max_w;
                g_last_max_h = max_h;
            }
        }
        int wlfd = owe_wayland_fd(g_app.wl);
        int ipcfd = owe_render_ipc_fd(g_app.ipc);
        int feedfd = owe_feed_fd(g_app.feed);
        int mpvfd = owe_mpv_fd(g_app.mpv);
        int stillfd = owe_still_fd(g_app.still);
        struct pollfd pfds[7 + OWE_IPC_MAX_CLIENTS + OWE_FEED_MAX_CLIENTS];
        int n = 0;
        int rc;
        bool mpv_ready = false;
        pfds[n].fd = g_sigpipe[0];
        pfds[n].events = POLLIN;
        n++;
        if (wlfd >= 0) {
            pfds[n].fd = wlfd;
            pfds[n].events = POLLIN;
            n++;
        }
        if (ipcfd >= 0) {
            pfds[n].fd = ipcfd;
            pfds[n].events = POLLIN;
            n++;
        }
        if (feedfd >= 0) {
            pfds[n].fd = feedfd;
            pfds[n].events = POLLIN;
            n++;
        }
        if (mpvfd >= 0) {
            pfds[n].fd = mpvfd;
            pfds[n].events = POLLIN;
            n++;
        }
        if (stillfd >= 0) {
            pfds[n].fd = stillfd;
            pfds[n].events = POLLIN;
            n++;
        }
        n += owe_render_ipc_pollfds(g_app.ipc, &pfds[n]);
        n += owe_feed_pollfds(g_app.feed, &pfds[n]);
        rc = poll(pfds, (nfds_t)n, owe_still_busy(g_app.still) ? 200 : 1000);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            OWE_ERROR("poll failed: %s", strerror(errno));
            break;
        }
        for (i = 0; i < n; i++) {
            if (!(pfds[i].revents & (POLLIN | POLLHUP))) {
                continue;
            }
            if (pfds[i].fd == g_sigpipe[0]) {
                char c;
                while (read(g_sigpipe[0], &c, 1) == 1) {
                }
                g_app.running = false;
                break;
            } else if (pfds[i].fd == wlfd) {
                if (owe_wayland_dispatch(g_app.wl) != 0) {
                    OWE_ERROR("wayland dispatch failed");
                    g_app.running = false;
                }
            } else if (pfds[i].fd == ipcfd) {
                owe_render_ipc_accept(g_app.ipc);
            } else if (pfds[i].fd == feedfd) {
                owe_feed_accept(g_app.feed);
            } else if (pfds[i].fd == mpvfd) {
                mpv_ready = true;
            }
        }
        if (!g_app.running) break;
        owe_render_ipc_poll_clients(g_app.ipc);
        owe_render_ipc_poll_still(g_app.ipc);
        owe_feed_poll_clients(g_app.feed, g_app.mpv);
        if (!g_app.feeding) {
            owe_wayland_render_pending(g_app.wl);
        }
        if (mpvfd >= 0 && mpv_ready && owe_mpv_process_updates(g_app.mpv)) {
            if (g_app.feeding) {
                if (owe_feed_publish(g_app.feed, g_app.mpv) == 0) {
                    owe_mpv_report_swap(g_app.mpv);
                }
            } else {
                owe_app_request_render();
                owe_wayland_render_pending(g_app.wl);
            }
        }
    }

    OWE_INFO("shutdown");
    cleanup();
    close(g_sigpipe[0]);
    close(g_sigpipe[1]);
    return 0;
}

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

#include "egl.h"
#include "render_ipc.h"
#include "mpv.h"
#include "still.h"
#include "wayland.h"

static owe_app_t g_app;
static int g_sigpipe[2];

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

void owe_app_emit_first_frame(const char *path) {
    owe_render_ipc_emit_first_frame(g_app.ipc, path ? path : "");
}

void owe_app_emit_error(const char *message) {
    owe_render_ipc_emit_error(g_app.ipc, message ? message : "unknown error");
}

static void on_signal(int sig) {
    (void)sig;
    if (g_app.running) {
        char c = 'q';
        ssize_t n = write(g_sigpipe[1], &c, 1);
        (void)n;
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
        return 1;
    }
    g_app.egl = owe_egl_new(g_app.wl);
    if (!g_app.egl) {
        OWE_ERROR("egl init failed");
        owe_wayland_free(g_app.wl);
        return 1;
    }
    owe_wayland_set_egl(g_app.wl, g_app.egl);
    owe_app_on_outputs_changed();
    g_app.mpv = owe_mpv_new(g_app.wl);
    if (!g_app.mpv) {
        OWE_ERROR("libmpv init failed");
        owe_egl_free(g_app.egl);
        owe_wayland_free(g_app.wl);
        return 1;
    }
    g_app.still = owe_still_new(g_app.wl, g_app.egl);
    if (!g_app.still) {
        OWE_ERROR("still init failed");
        owe_mpv_free(g_app.mpv);
        owe_egl_free(g_app.egl);
        owe_wayland_free(g_app.wl);
        return 1;
    }
    g_app.ipc = owe_render_ipc_new(socket_path);
    if (!g_app.ipc) {
        OWE_ERROR("ipc init failed");
        owe_still_free(g_app.still);
        owe_mpv_free(g_app.mpv);
        owe_egl_free(g_app.egl);
        owe_wayland_free(g_app.wl);
        return 1;
    }

    OWE_INFO("ready, socket=%s", socket_path);

    while (g_app.running) {
        int wlfd = owe_wayland_fd(g_app.wl);
        int ipcfd = owe_render_ipc_fd(g_app.ipc);
        int mpvfd = owe_mpv_fd(g_app.mpv);
        struct pollfd pfds[4];
        int n = 0;
        int rc;
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
        if (mpvfd >= 0) {
            pfds[n].fd = mpvfd;
            pfds[n].events = POLLIN;
            n++;
        }
        rc = poll(pfds, (nfds_t)n, 1000);
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
            } else if (pfds[i].fd == wlfd) {
                if (owe_wayland_dispatch(g_app.wl) != 0) {
                    OWE_ERROR("wayland dispatch failed");
                    g_app.running = false;
                }
            } else if (pfds[i].fd == ipcfd) {
                owe_render_ipc_accept(g_app.ipc);
            } else if (pfds[i].fd == mpvfd) {
                if (owe_mpv_process_updates(g_app.mpv)) {
                    owe_app_request_render();
                }
            }
        }
        owe_render_ipc_poll_clients(g_app.ipc);
        owe_wayland_render_pending(g_app.wl);
        if (mpvfd >= 0 && owe_mpv_process_updates(g_app.mpv)) {
            owe_app_request_render();
            owe_wayland_render_pending(g_app.wl);
        }
    }

    OWE_INFO("shutdown");
    owe_render_ipc_free(g_app.ipc);
    owe_still_free(g_app.still);
    owe_mpv_free(g_app.mpv);
    owe_egl_free(g_app.egl);
    owe_wayland_free(g_app.wl);
    close(g_sigpipe[0]);
    close(g_sigpipe[1]);
    return 0;
}

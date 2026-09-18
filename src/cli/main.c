#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <spawn.h>  /* system */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "common_ipc.h"
#include "json.h"
#include "xdg.h"

static int daemon_call(const char *line, int print_reply) {
    char path[PATH_MAX];
    char reply[OWE_IPC_MAX_LINE];
    int fd;
    if (owe_socket_path_daemon(path, sizeof(path)) != 0) {
        fprintf(stderr, "owe: cannot resolve daemon socket\n");
        return 1;
    }
    fd = owe_ipc_connect(path);
    if (fd < 0) {
        fprintf(stderr, "owe: daemon not running (no socket at %s)\n", path);
        return 1;
    }
    if (owe_ipc_send_line(fd, line) != 0) {
        fprintf(stderr, "owe: send failed\n");
        close(fd);
        return 1;
    }
    {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int rc = poll(&pfd, 1, 10000);
        if (rc > 0 && owe_ipc_recv_line(fd, reply, sizeof(reply)) == 0) {
            close(fd);
            if (print_reply) {
                printf("%s\n", reply);
            }
            return owe_json_ok(reply) ? 0 : 1;
        }
    }
    close(fd);
    fprintf(stderr, "owe: no reply from daemon\n");
    return 1;
}

static int render_call(const char *line) {
    char path[PATH_MAX];
    char reply[OWE_IPC_MAX_LINE];
    int fd;
    if (owe_socket_path_render(path, sizeof(path)) != 0) {
        fprintf(stderr, "owe: cannot resolve render socket\n");
        return 1;
    }
    fd = owe_ipc_connect(path);
    if (fd < 0) {
        fprintf(stderr, "owe: renderer not running (no socket at %s)\n", path);
        return 1;
    }
    if (owe_ipc_send_line(fd, line) != 0) {
        fprintf(stderr, "owe: send failed\n");
        close(fd);
        return 1;
    }
    {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int rc = poll(&pfd, 1, 10000);
        if (rc > 0 && owe_ipc_recv_line(fd, reply, sizeof(reply)) == 0) {
            close(fd);
            printf("%s\n", reply);
            return owe_json_ok(reply) ? 0 : 1;
        }
    }
    close(fd);
    fprintf(stderr, "owe: no reply from renderer\n");
    return 1;
}

static void usage(const char *argv0) {
    fprintf(stderr,
            "Usage: %s <command> [args]\n"
            "\n"
            "Background:\n"
            "  set <path>              Update the Omarchy background symlink\n"
            "  next                    Cycle to next theme background\n"
            "  current                 Show current background name\n"
            "  refresh                 Re-read the background symlink now\n"
            "\n"
            "Playback:\n"
            "  pause                   Pause video manually\n"
            "  resume                  Clear manual pause\n"
            "  always-animate on|off   Force animation regardless of policy\n"
            "\n"
            "State:\n"
            "  status                  Daemon status as JSON\n"
            "  config                  Effective config as JSON\n"
            "  render-status           Renderer status as JSON\n"
            "  render CMD...           Send raw JSON command to renderer\n"
            "\n"
            "Control:\n"
            "  reload-config           Reload config.toml\n"
            "  render-restart          Restart the renderer process\n"
            "  raw <json>              Send raw JSON to the daemon\n"
            "  shutdown                Stop the daemon\n",
            argv0);
}

static int cmd_next(void) {
    pid_t pid;
    char *argv[] = { "omarchy-theme-bg-next", NULL };
    extern char **environ;
    int status = 0;
    if (posix_spawnp(&pid, "omarchy-theme-bg-next", NULL, NULL, argv, environ) != 0) {
        fprintf(stderr, "owe: omarchy-theme-bg-next failed\n");
        return 1;
    }
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : 1;
}

static int cmd_current(void) {
    char link[PATH_MAX];
    char target[PATH_MAX];
    ssize_t n;
    const char *base;
    if (owe_omarchy_background_link(link, sizeof(link)) != 0) {
        fprintf(stderr, "owe: cannot resolve background link\n");
        return 1;
    }
    n = readlink(link, target, sizeof(target) - 1);
    if (n < 0) {
        fprintf(stderr, "owe: no current background\n");
        return 1;
    }
    target[n] = '\0';
    base = strrchr(target, '/');
    printf("%s\n", base ? base + 1 : target);
    return 0;
}

int main(int argc, char **argv) {
    char line[OWE_IPC_MAX_LINE];
    const char *cmd;
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }
    cmd = argv[1];
    if (strcmp(cmd, "status") == 0) {
        return daemon_call("{\"cmd\":\"status\"}", 1);
    }
    if (strcmp(cmd, "config") == 0) {
        return daemon_call("{\"cmd\":\"config\"}", 1);
    }
    if (strcmp(cmd, "render-status") == 0) {
        return daemon_call("{\"cmd\":\"render-status\"}", 1);
    }
    if (strcmp(cmd, "set") == 0) {
        char abs[PATH_MAX];
        if (argc < 3) {
            fprintf(stderr, "owe set <path>\n");
            return 1;
        }
        if (!realpath(argv[2], abs)) {
            fprintf(stderr, "owe: file not found: %s\n", argv[2]);
            return 1;
        }
        char *quoted = owe_json_quote(abs);
        if (!quoted) return 1;
        int n = snprintf(line, sizeof(line), "{\"cmd\":\"set\",\"path\":%s}", quoted);
        free(quoted);
        if (n >= (int)sizeof(line)) return 1;
        return daemon_call(line, 1);
    }
    if (strcmp(cmd, "next") == 0) {
        return cmd_next();
    }
    if (strcmp(cmd, "current") == 0) {
        return cmd_current();
    }
    if (strcmp(cmd, "refresh") == 0) {
        return daemon_call("{\"cmd\":\"refresh\"}", 1);
    }
    if (strcmp(cmd, "pause") == 0) {
        return daemon_call("{\"cmd\":\"pause\"}", 1);
    }
    if (strcmp(cmd, "resume") == 0) {
        return daemon_call("{\"cmd\":\"resume\"}", 1);
    }
    if (strcmp(cmd, "always-animate") == 0) {
        if (argc < 3) {
            fprintf(stderr, "owe always-animate on|off\n");
            return 1;
        }
        if (strcmp(argv[2], "on") == 0) {
            return daemon_call("{\"cmd\":\"always-animate\",\"value\":true}", 1);
        }
        if (strcmp(argv[2], "off") == 0) {
            return daemon_call("{\"cmd\":\"always-animate\",\"value\":false}", 1);
        }
        fprintf(stderr, "owe always-animate on|off\n");
        return 1;
    }
    if (strcmp(cmd, "reload-config") == 0) {
        return daemon_call("{\"cmd\":\"reload-config\"}", 1);
    }
    if (strcmp(cmd, "render-restart") == 0) {
        return daemon_call("{\"cmd\":\"render-restart\"}", 1);
    }
    if (strcmp(cmd, "render") == 0) {
        size_t off = 0;
        int i;
        line[0] = '\0';
        for (i = 2; i < argc; i++) {
            off += snprintf(line + off, sizeof(line) - off, "%s%s", i > 2 ? " " : "", argv[i]);
            if (off >= sizeof(line) - 1) {
                fprintf(stderr, "owe: Request exceeds the IPC limit\n");
                return 1;
            }
        }
        if (off == 0) {
            fprintf(stderr, "owe render <json>\n");
            return 1;
        }
        return render_call(line);
    }
    if (strcmp(cmd, "raw") == 0) {
        size_t off = 0;
        int i;
        line[0] = '\0';
        for (i = 2; i < argc; i++) {
            off += snprintf(line + off, sizeof(line) - off, "%s%s", i > 2 ? " " : "", argv[i]);
            if (off >= sizeof(line) - 1) {
                fprintf(stderr, "owe: Request exceeds the IPC limit\n");
                return 1;
            }
        }
        if (off == 0) {
            fprintf(stderr, "owe raw <json>\n");
            return 1;
        }
        return daemon_call(line, 1);
    }
    if (strcmp(cmd, "shutdown") == 0) {
        return daemon_call("{\"cmd\":\"shutdown\"}", 1);
    }
    if (strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0 || strcmp(cmd, "help") == 0) {
        usage(argv[0]);
        return 0;
    }
    usage(argv[0]);
    return 1;
}

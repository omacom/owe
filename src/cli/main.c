#include <errno.h>
#include <limits.h>
#include <spawn.h>  /* system */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "common_ipc.h"
#include "json.h"
#include "xdg.h"

static const char *socket_override;

static int socket_path(char *path, size_t len, bool renderer) {
    if (!socket_override) {
        return renderer ? owe_socket_path_render(path, len) : owe_socket_path_daemon(path, len);
    }
    if (renderer) return owe_socket_path_sibling(socket_override, "render.sock", path, len);
    return snprintf(path, len, "%s", socket_override) < (int)len ? 0 : -1;
}

static int daemon_call(const char *line, int print_reply) {
    char path[PATH_MAX];
    char reply[OWE_IPC_MAX_LINE];
    int fd;
    if (socket_path(path, sizeof(path), false) != 0) {
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
        if (owe_ipc_recv_line_timeout(fd, reply, sizeof(reply), 10000) == 0) {
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
    if (socket_path(path, sizeof(path), true) != 0) {
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
        if (owe_ipc_recv_line_timeout(fd, reply, sizeof(reply), 10000) == 0) {
            close(fd);
            printf("%s\n", reply);
            return owe_json_ok(reply) ? 0 : 1;
        }
    }
    close(fd);
    fprintf(stderr, "owe: no reply from renderer\n");
    return 1;
}

static int reply_line(int fd, char *reply, size_t len, int timeout_ms) {
    return owe_ipc_recv_line_timeout(fd, reply, len, timeout_ms);
}

/* Play a one-shot intro video and block until it ends. The shell starts this
 * when a still background has a matching boot intro and reveals the still
 * after the process exits. */
static int cmd_intro(const char *path) {
    char sock[PATH_MAX];
    char resolved[PATH_MAX];
    char reply[OWE_IPC_MAX_LINE];
    char line[OWE_IPC_MAX_LINE];
    char *quoted;
    int fd;
    if (!realpath(path, resolved)) {
        fprintf(stderr, "owe: file not found: %s\n", path);
        return 1;
    }
    if (socket_path(sock, sizeof(sock), false) != 0) {
        fprintf(stderr, "owe: cannot resolve daemon socket\n");
        return 1;
    }
    fd = owe_ipc_connect(sock);
    if (fd < 0) {
        fprintf(stderr, "owe: daemon not running at %s\n", sock);
        return 1;
    }
    quoted = owe_json_quote(resolved);
    if (!quoted || snprintf(line, sizeof(line), "{\"cmd\":\"intro\",\"path\":%s}", quoted) >=
                       (int)sizeof(line)) {
        free(quoted);
        close(fd);
        return 1;
    }
    free(quoted);
    if (owe_ipc_send_line(fd, line) != 0 || reply_line(fd, reply, sizeof(reply), 10000) != 0) {
        fprintf(stderr, "owe: no reply from daemon\n");
        close(fd);
        return 1;
    }
    if (!owe_json_ok(reply)) {
        fprintf(stderr, "owe: intro rejected: %s\n", reply);
        close(fd);
        return 1;
    }
    int64_t deadline = owe_ipc_now_ms() + 35000;
    while (owe_ipc_now_ms() < deadline) {
        int64_t remaining = deadline - owe_ipc_now_ms();
        int timeout = remaining < 5000 ? (int)remaining : 5000;
        if (owe_ipc_send_line(fd, "{\"cmd\":\"intro-status\"}") != 0 ||
            reply_line(fd, reply, sizeof(reply), timeout) != 0) {
            fprintf(stderr, "owe: intro status unavailable\n");
            close(fd);
            return 1;
        }
        yyjson_doc *doc = yyjson_read(reply, strlen(reply), 0);
        yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
        yyjson_val *running = yyjson_obj_get(root, "running");
        bool valid = yyjson_equals_str(yyjson_obj_get(root, "status"), "ok") && yyjson_is_bool(running);
        bool finished = valid && !yyjson_get_bool(running);
        bool ok = yyjson_equals_str(yyjson_obj_get(root, "result"), "ok");
        yyjson_doc_free(doc);
        if (!valid) {
            fprintf(stderr, "owe: invalid intro status: %s\n", reply);
            close(fd);
            return 1;
        }
        if (finished) {
            if (!ok) {
                fprintf(stderr, "owe: intro ended early: %s\n", reply);
            }
            close(fd);
            return ok ? 0 : 1;
        }
        usleep(200000);
    }
    owe_ipc_send_line(fd, "{\"cmd\":\"intro-stop\"}");
    close(fd);
    fprintf(stderr, "owe: intro timed out\n");
    return 1;
}

static void usage(const char *argv0) {
    fprintf(stderr,
            "Usage: %s [--socket PATH] <command> [args]\n"
            "  --socket PATH           Override the daemon socket before the command\n"
            "  --version               Show the version\n"
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
            "  intro <video>           Play a one-shot intro video and wait\n"
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
    const char *program = argv[0];
    while (argc > 1 && strcmp(argv[1], "--socket") == 0) {
        if (argc < 4) {
            usage(program);
            return 1;
        }
        socket_override = argv[2];
        argv += 2;
        argc -= 2;
    }
    if (argc < 2) {
        usage(program);
        return 1;
    }
    cmd = argv[1];
    if (strcmp(cmd, "--version") == 0) {
        printf("owe %s\n", OWE_VERSION);
        return 0;
    }
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
    if (strcmp(cmd, "intro") == 0) {
        if (argc < 3) {
            fprintf(stderr, "owe intro <video>\n");
            return 1;
        }
        return cmd_intro(argv[2]);
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
        usage(program);
        return 0;
    }
    usage(program);
    return 1;
}

#include "../src/daemon/supervisor.c"
#include <stdio.h>
#include <sys/socket.h>
#include <sys/un.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
static int spawns;
/* When set, a spawn starts a fake renderer that creates its socket late. */
static int socket_after_ms = -1;
int owe_spawn(const char *file, char *const argv[], pid_t *pid) {
    (void)file;
    spawns++;
    if (socket_after_ms < 0) return -1;
    pid_t child = fork();
    if (child < 0) return -1;
    if (child == 0) {
        struct sockaddr_un addr = {.sun_family = AF_UNIX};
        int fd;
        usleep((useconds_t)socket_after_ms * 1000);
        snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", argv[2]);
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0 || bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(fd, 1) != 0) {
            _exit(1);
        }
        pause();
        _exit(0);
    }
    *pid = child;
    return 0;
}

int main(void) {
    struct owed_supervisor supervisor = {0};
    CHECK(owed_supervisor_send(&supervisor, "{}", NULL, 0) < 0);
    CHECK(spawns == 0);
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) _exit(0);
    siginfo_t info;
    CHECK(waitid(P_PID, (id_t)child, &info, WEXITED | WNOWAIT) == 0);
    supervisor.child = child;
    CHECK(owed_supervisor_send(&supervisor, "{}", NULL, 0) < 0);
    CHECK(supervisor.child == 0 && spawns == 0);
    supervisor.next_restart = monotonic_ms() + 2000;
    owed_supervisor_stop(&supervisor);
    CHECK(supervisor.next_restart == 0);
    puts("only explicit supervisor restarts spawn renderers");

    /* A renderer that needs more than one second to start, as on a slow ARM
     * board, must not be stopped and started again. */
    char dir[] = "/tmp/owe-supervisor-XXXXXX";
    CHECK(mkdtemp(dir));
    struct owed_supervisor slow = {0};
    snprintf(slow.socket_path, sizeof(slow.socket_path), "%s/render.sock", dir);
    socket_after_ms = 1500;
    int before = spawns;
    CHECK(owed_supervisor_ensure_running(&slow) == 0);
    CHECK(slow.child > 0 && spawns == before + 1 && owed_render_is_alive(&slow));
    owed_supervisor_stop(&slow);
    CHECK(slow.child == 0);
    unlink(slow.socket_path);
    puts("a renderer that starts in 1.5 seconds is accepted");

    /* A renderer that never creates its socket blocks the daemon for the
     * full wait. The next automatic start must wait, so the daemon can serve
     * its clients in between. */
    struct owed_supervisor hung = {0};
    snprintf(hung.socket_path, sizeof(hung.socket_path), "%s/render.sock", dir);
    socket_after_ms = 60000;
    before = spawns;
    CHECK(owed_supervisor_ensure_running(&hung) != 0);
    CHECK(hung.child == 0 && spawns == before + 1);
    CHECK(hung.next_restart >= monotonic_ms() + RENDER_RETRY_MS - 1000);
    CHECK(owed_supervisor_ensure_running(&hung) != 0 && spawns == before + 1);
    CHECK(rmdir(dir) == 0);
    puts("a renderer that never starts delays the next start");
}

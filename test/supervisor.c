#include "../src/daemon/supervisor.c"
#include <stdio.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
static int spawns;
int owe_spawn(const char *file, char *const argv[], pid_t *pid) {
    (void)file; (void)argv; (void)pid;
    spawns++;
    return -1;
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
}

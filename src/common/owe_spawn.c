#include "owe_spawn.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h> /* system spawn.h, not this module's header */
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

int owe_spawn(const char *file, char *const argv[], pid_t *pid_out) {
    pid_t pid;
    int rc = posix_spawnp(&pid, file, NULL, NULL, argv, environ);
    if (rc != 0) {
        return -1;
    }
    if (pid_out) {
        *pid_out = pid;
    }
    return 0;
}

static long ms_since(const struct timespec *start) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - start->tv_sec) * 1000L + (now.tv_nsec - start->tv_nsec) / 1000000L;
}

int owe_spawn_capture(const char *file, char *const argv[], char *out, unsigned long out_len,
                      int timeout_ms) {
    int pipefd[2] = { -1, -1 };
    posix_spawn_file_actions_t fa;
    pid_t pid;
    int status = -1;
    unsigned long off = 0;
    int rc;
    struct timespec start;
    int exited = 0;

    if (!out || out_len < 2) {
        return -1;
    }
    out[0] = '\0';
    if (pipe2(pipefd, O_CLOEXEC) != 0) {
        return -1;
    }
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, pipefd[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&fa, pipefd[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&fa, pipefd[0]);
    posix_spawn_file_actions_addclose(&fa, pipefd[1]);
    rc = posix_spawnp(&pid, file, &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(pipefd[1]);
    if (rc != 0) {
        close(pipefd[0]);
        return -1;
    }
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    clock_gettime(CLOCK_MONOTONIC, &start);
    while (!exited) {
        struct pollfd pfd = { .fd = pipefd[0], .events = POLLIN };
        int pr = poll(&pfd, 1, 100);
        if (pr > 0 && (pfd.revents & (POLLIN | POLLHUP))) {
            char buf[4096];
            ssize_t n;
            while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
                unsigned long copy = (unsigned long)n;
                if (off + copy >= out_len) {
                    copy = out_len - 1 - off;
                }
                if (copy > 0) {
                    memcpy(out + off, buf, copy);
                    off += copy;
                }
            }
        }
        if (waitpid(pid, &status, WNOHANG) == pid) {
            exited = 1;
            break;
        }
        if (ms_since(&start) >= timeout_ms) {
            break;
        }
    }
    if (!exited) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        close(pipefd[0]);
        out[off] = '\0';
        return -1;
    }
    {
        char buf[4096];
        ssize_t n;
        while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
            unsigned long copy = (unsigned long)n;
            if (off + copy >= out_len) {
                copy = out_len - 1 - off;
            }
            if (copy > 0) {
                memcpy(out + off, buf, copy);
                off += copy;
            }
        }
    }
    close(pipefd[0]);
    out[off] = '\0';
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return -1;
    }
    return 0;
}

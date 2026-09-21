#include "common_ipc.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

static void echo(void *context, struct owe_ipc_client *c, const char *line) {
    int *commands = context;
    (*commands)++;
    owe_ipc_client_send(c, line);
}

int main(void) {
    struct owe_ipc_client c = {.fd = -1};
    int pair[2], commands = 0;
    CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pair) == 0);
    owe_ipc_client_open(&c, pair[0]);
    int64_t start = c.active_ms;
    CHECK(send(pair[1], "x", 1, 0) == 1);
    owe_ipc_client_poll(&c, start, echo, &commands);
    CHECK(c.len == 1 && commands == 0);
    /* Dribbling bytes extends activity, but not an incomplete request's deadline. */
    CHECK(send(pair[1], "y", 1, 0) == 1);
    owe_ipc_client_poll(&c, start + OWE_IPC_CLIENT_IDLE_MS - 1, echo, &commands);
    CHECK(c.fd >= 0);
    owe_ipc_client_poll(&c, start + OWE_IPC_CLIENT_IDLE_MS, echo, &commands);
    CHECK(c.fd < 0);
    close(pair[1]);

    CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pair) == 0);
    owe_ipc_client_open(&c, pair[0]);
    CHECK(send(pair[1], "first\r\nsecond\n", 14, 0) == 14);
    owe_ipc_client_poll(&c, c.active_ms, echo, &commands);
    CHECK(commands == 2);
    char small[64] = {0};
    CHECK(recv(pair[1], small, sizeof(small), 0) == 13);
    CHECK(strcmp(small, "first\nsecond\n") == 0);

    /* Force EAGAIN and partial writes, then drain and verify the complete reply. */
    int size = 1024;
    CHECK(setsockopt(c.fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) == 0);
    char *line = malloc(OWE_IPC_MAX_LINE - 1);
    CHECK(line);
    memset(line, 'a', OWE_IPC_MAX_LINE - 2);
    line[OWE_IPC_MAX_LINE - 2] = '\0';
    owe_ipc_client_send(&c, line);
    CHECK(c.fd >= 0 && (owe_ipc_client_events(&c) & POLLOUT));
    size_t received = 0;
    for (int attempts = 0; attempts < 1000 && received < OWE_IPC_MAX_LINE - 1; attempts++) {
        char buf[8192];
        ssize_t n = recv(pair[1], buf, sizeof(buf), 0);
        if (n > 0) {
            for (ssize_t i = 0; i < n; i++, received++)
                CHECK(buf[i] == (received == OWE_IPC_MAX_LINE - 2 ? '\n' : 'a'));
        } else CHECK(errno == EAGAIN);
        owe_ipc_client_poll(&c, owe_ipc_now_ms(), echo, &commands);
    }
    CHECK(received == OWE_IPC_MAX_LINE - 1);
    CHECK(!(owe_ipc_client_events(&c) & POLLOUT));
    /* A reader that never drains cannot grow our response queue indefinitely. */
    for (int i = 0; i < 10 && c.fd >= 0; i++) owe_ipc_client_send(&c, line);
    CHECK(c.fd < 0 && c.output == NULL);
    close(pair[1]);
    free(line);
    puts("IPC deadlines, framing and backpressure passed");
}

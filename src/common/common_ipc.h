#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <poll.h>
#include <stdint.h>

#define OWE_IPC_MAX_LINE 65536
#define OWE_IPC_VERSION 1
#define OWE_IPC_MAX_CLIENTS 16
#define OWE_IPC_CLIENT_IDLE_MS 120000

/* Shared, bounded JSON-line transport for the daemon and renderer. */
struct owe_ipc_client {
    int fd;
    uint64_t generation;
    char buf[OWE_IPC_MAX_LINE];
    size_t len;
    int64_t active_ms;
    int64_t request_ms;
    char *output;
    size_t output_len;
    size_t output_sent;
};

typedef void (*owe_ipc_handler)(void *context, struct owe_ipc_client *client, const char *line);
int64_t owe_ipc_now_ms(void);
void owe_ipc_client_open(struct owe_ipc_client *c, int fd);
void owe_ipc_client_close(struct owe_ipc_client *c);
short owe_ipc_client_events(const struct owe_ipc_client *c);
void owe_ipc_client_send(struct owe_ipc_client *c, const char *line);
void owe_ipc_client_poll(struct owe_ipc_client *c, int64_t now, owe_ipc_handler handler,
                         void *context);

int owe_ipc_listen(const char *path);
int owe_ipc_connect(const char *path);
int owe_ipc_set_timeout(int fd, int ms);

int owe_ipc_send_line(int fd, const char *line);
int owe_ipc_recv_line(int fd, char *buf, size_t len);

typedef struct owe_ipc_server owe_ipc_server_t;

owe_ipc_server_t *owe_ipc_server_new(const char *path);
void owe_ipc_server_free(owe_ipc_server_t *srv);
int owe_ipc_server_fd(owe_ipc_server_t *srv);
int owe_ipc_server_accept(owe_ipc_server_t *srv);

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <poll.h>

#define OWE_IPC_MAX_LINE 65536
#define OWE_IPC_VERSION 1
#define OWE_IPC_MAX_CLIENTS 16

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

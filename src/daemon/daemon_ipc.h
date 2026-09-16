#pragma once

struct owed_ipc;

struct owed_ipc *owed_ipc_new(const char *socket_path);
void owed_ipc_free(struct owed_ipc *ipc);
int owed_ipc_fd(struct owed_ipc *ipc);
void owed_ipc_accept(struct owed_ipc *ipc);
void owed_ipc_poll_clients(struct owed_ipc *ipc);
void owed_ipc_broadcast(struct owed_ipc *ipc, const char *line);

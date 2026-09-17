#pragma once
#include <poll.h>

struct owe_render_ipc;

struct owe_render_ipc *owe_render_ipc_new(const char *socket_path);
void owe_render_ipc_free(struct owe_render_ipc *ipc);
int owe_render_ipc_fd(struct owe_render_ipc *ipc);
void owe_render_ipc_accept(struct owe_render_ipc *ipc);
void owe_render_ipc_poll_clients(struct owe_render_ipc *ipc);
int owe_render_ipc_pollfds(struct owe_render_ipc *ipc, struct pollfd *fds);
void owe_render_ipc_poll_still(struct owe_render_ipc *ipc);
void owe_render_ipc_reload_still(struct owe_render_ipc *ipc);

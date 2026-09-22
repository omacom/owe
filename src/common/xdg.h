#pragma once

#include <stddef.h>

int owe_xdg_runtime_dir(char *buf, size_t len);
int owe_xdg_config_home(char *buf, size_t len);
int owe_xdg_cache_home(char *buf, size_t len);
int owe_xdg_state_home(char *buf, size_t len);
int owe_home(char *buf, size_t len);

int owe_socket_path_render(char *buf, size_t len);
int owe_socket_path_daemon(char *buf, size_t len);
int owe_socket_path_feed(char *buf, size_t len);
int owe_socket_path_sibling(const char *socket_path, const char *name, char *buf, size_t len);
int owe_config_path(char *buf, size_t len);
int owe_transcode_cache_dir(char *buf, size_t len);
int owe_omarchy_background_link(char *buf, size_t len);

int owe_mkdir_p(const char *path);

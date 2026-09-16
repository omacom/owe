#pragma once

#include <sys/types.h>

int owe_spawn(const char *file, char *const argv[], pid_t *pid_out);
int owe_spawn_capture(const char *file, char *const argv[], char *out, unsigned long out_len,
                      int timeout_ms);

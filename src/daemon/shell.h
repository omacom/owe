#pragma once

#include <stdbool.h>

/* Enable or disable the Omarchy shell background plugin. Returns 0 when the
 * shell accepted the change. */
int owed_shell_plugin_set(bool enabled);

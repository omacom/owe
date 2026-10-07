#include "shell.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "log.h"
#include "owe_spawn.h"

/* Prefer the first omarchy-shell on PATH, then the session Omarchy tree. */
static const char *shell_command(void) {
    static char resolved[PATH_MAX];
    const char *path = getenv("PATH");
    if (path) {
        const char *cursor = path;
        while (*cursor) {
            const char *end = strchr(cursor, ':');
            size_t len = end ? (size_t)(end - cursor) : strlen(cursor);
            char candidate[PATH_MAX];
            if (len > 0 && len < sizeof(candidate)) {
                snprintf(candidate, sizeof(candidate), "%.*s/omarchy-shell", (int)len, cursor);
                if (access(candidate, X_OK) == 0) {
                    snprintf(resolved, sizeof(resolved), "%s", candidate);
                    return resolved;
                }
            }
            cursor = end ? end + 1 : cursor + len;
        }
    }
    {
        const char *omarchy = getenv("OMARCHY_PATH");
        if (omarchy && *omarchy) {
            snprintf(resolved, sizeof(resolved), "%s/bin/omarchy-shell", omarchy);
            if (access(resolved, X_OK) == 0) {
                return resolved;
            }
        }
    }
    return "omarchy-shell";
}

int owed_shell_plugin_set(bool enabled) {
    char log[4096];
    const char *command = shell_command();
    char *argv[] = { (char *)command, "shell", "setPluginEnabled", "omarchy.background",
                     (char *)(enabled ? "true" : "false"), NULL };
    char *suspend[] = { (char *)command, "background", "setSuspended",
                        (char *)(enabled ? "false" : "true"), NULL };
    /* omarchy-shell gives the shell 2 seconds to answer by default. A shell
     * that is still starting, as at login, can take longer to switch a
     * plugin, so let it use most of the time this call waits for it. */
    setenv("OMARCHY_SHELL_IPC_TIMEOUT", "4s", 0);
    if (owe_spawn_capture(command, suspend, log, sizeof(log), 5000) != 0 &&
        owe_spawn_capture(command, argv, log, sizeof(log), 5000) != 0) {
        OWE_WARN("shell background %s failed: %s", enabled ? "enable" : "disable",
                 log[0] ? log : "command failed");
        return -1;
    }
    OWE_INFO("shell background %s", enabled ? "enabled" : "disabled");
    return 0;
}

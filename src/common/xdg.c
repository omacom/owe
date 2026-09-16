#include "xdg.h"

#include <errno.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static int join_env(const char *env, const char *fallback, char *buf, size_t len) {
    const char *v = getenv(env);
    if (!v || !*v) {
        v = fallback;
    }
    if (!v) {
        return -1;
    }
    if (snprintf(buf, len, "%s", v) >= (int)len) {
        return -1;
    }
    return 0;
}

int owe_home(char *buf, size_t len) {
    const char *h = getenv("HOME");
    struct passwd *pw;
    if (h && *h) {
        if (snprintf(buf, len, "%s", h) >= (int)len) {
            return -1;
        }
        return 0;
    }
    pw = getpwuid(getuid());
    if (!pw || !pw->pw_dir) {
        return -1;
    }
    if (snprintf(buf, len, "%s", pw->pw_dir) >= (int)len) {
        return -1;
    }
    return 0;
}

int owe_xdg_runtime_dir(char *buf, size_t len) {
    char fallback[64];
    const char *v = getenv("XDG_RUNTIME_DIR");
    if (v && *v) {
        if (snprintf(buf, len, "%s", v) >= (int)len) {
            return -1;
        }
        return 0;
    }
    snprintf(fallback, sizeof(fallback), "/run/user/%d", (int)getuid());
    return join_env("XDG_RUNTIME_DIR", fallback, buf, len);
}

int owe_xdg_config_home(char *buf, size_t len) {
    char home[PATH_MAX];
    char fallback[PATH_MAX * 2];
    const char *v = getenv("XDG_CONFIG_HOME");
    if (v && *v) {
        if (snprintf(buf, len, "%s", v) >= (int)len) {
            return -1;
        }
        return 0;
    }
    if (owe_home(home, sizeof(home)) != 0) {
        return -1;
    }
    snprintf(fallback, sizeof(fallback), "%s/.config", home);
    if (snprintf(buf, len, "%s", fallback) >= (int)len) {
        return -1;
    }
    return 0;
}

int owe_xdg_cache_home(char *buf, size_t len) {
    char home[PATH_MAX];
    char fallback[PATH_MAX * 2];
    const char *v = getenv("XDG_CACHE_HOME");
    if (v && *v) {
        if (snprintf(buf, len, "%s", v) >= (int)len) {
            return -1;
        }
        return 0;
    }
    if (owe_home(home, sizeof(home)) != 0) {
        return -1;
    }
    snprintf(fallback, sizeof(fallback), "%s/.cache", home);
    if (snprintf(buf, len, "%s", fallback) >= (int)len) {
        return -1;
    }
    return 0;
}

int owe_xdg_state_home(char *buf, size_t len) {
    char home[PATH_MAX];
    char fallback[PATH_MAX * 2];
    const char *v = getenv("XDG_STATE_HOME");
    if (v && *v) {
        if (snprintf(buf, len, "%s", v) >= (int)len) {
            return -1;
        }
        return 0;
    }
    if (owe_home(home, sizeof(home)) != 0) {
        return -1;
    }
    snprintf(fallback, sizeof(fallback), "%s/.local/state", home);
    if (snprintf(buf, len, "%s", fallback) >= (int)len) {
        return -1;
    }
    return 0;
}

int owe_socket_path_render(char *buf, size_t len) {
    char rt[PATH_MAX];
    if (owe_xdg_runtime_dir(rt, sizeof(rt)) != 0) {
        return -1;
    }
    if (snprintf(buf, len, "%s/owe/render.sock", rt) >= (int)len) {
        return -1;
    }
    return 0;
}

int owe_socket_path_daemon(char *buf, size_t len) {
    char rt[PATH_MAX];
    if (owe_xdg_runtime_dir(rt, sizeof(rt)) != 0) {
        return -1;
    }
    if (snprintf(buf, len, "%s/owe/owed.sock", rt) >= (int)len) {
        return -1;
    }
    return 0;
}

int owe_config_path(char *buf, size_t len) {
    char cfg[PATH_MAX];
    if (owe_xdg_config_home(cfg, sizeof(cfg)) != 0) {
        return -1;
    }
    if (snprintf(buf, len, "%s/owe/config.toml", cfg) >= (int)len) {
        return -1;
    }
    return 0;
}

int owe_transcode_cache_dir(char *buf, size_t len) {
    char cache[PATH_MAX];
    if (owe_xdg_cache_home(cache, sizeof(cache)) != 0) {
        return -1;
    }
    if (snprintf(buf, len, "%s/owe/gif", cache) >= (int)len) {
        return -1;
    }
    return 0;
}

int owe_omarchy_background_link(char *buf, size_t len) {
    char state[PATH_MAX];
    if (owe_xdg_state_home(state, sizeof(state)) != 0) {
        return -1;
    }
    if (snprintf(buf, len, "%s/omarchy/current/background", state) >= (int)len) {
        return -1;
    }
    return 0;
}

int owe_mkdir_p(const char *path) {
    char tmp[PATH_MAX];
    char *p;
    size_t len;
    if (!path || !*path) {
        return -1;
    }
    len = strlen(path);
    if (len >= sizeof(tmp)) {
        return -1;
    }
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                return -1;
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

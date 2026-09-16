#include "config.h"
#include "strutil.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void owe_config_defaults(owe_config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->pause_fullscreen = true;
    cfg->pause_occupied_workspace = false;
    cfg->battery_poster = false;
    cfg->gif_fps = 20;
    cfg->gif_crf = 20;
    cfg->transcode_max_width = 2560;
    cfg->transcode_max_height = 1440;
    cfg->fade_ms = 250;
}

static bool parse_bool(const char *v, bool *out) {
    if (strcmp(v, "true") == 0 || strcmp(v, "yes") == 0 || strcmp(v, "1") == 0) {
        *out = true;
        return true;
    }
    if (strcmp(v, "false") == 0 || strcmp(v, "no") == 0 || strcmp(v, "0") == 0) {
        *out = false;
        return true;
    }
    return false;
}

static void strip_quotes(char *v) {
    size_t n = strlen(v);
    if (n >= 2 && ((v[0] == '"' && v[n - 1] == '"') || (v[0] == '\'' && v[n - 1] == '\''))) {
        v[n - 1] = '\0';
        memmove(v, v + 1, n - 1);
    }
}

static void parse_blocklist(owe_config_t *cfg, char *v) {
    owe_trim(v);
    if (*v == '[') {
        v++;
    }
    for (char *tok = strtok(v, ",]"); tok; tok = strtok(NULL, ",]")) {
        owe_trim(tok);
        strip_quotes(tok);
        if (!*tok || cfg->blocklist_count >= 16) {
            continue;
        }
        snprintf(cfg->blocklist[cfg->blocklist_count], sizeof(cfg->blocklist[0]), "%s", tok);
        cfg->blocklist_count++;
    }
}

int owe_config_load(owe_config_t *cfg, const char *path) {
    FILE *f;
    char line[512];
    char section[64] = "";
    if (!path) {
        return -1;
    }
    f = fopen(path, "r");
    if (!f) {
        return -1;
    }
    while (fgets(line, sizeof(line), f)) {
        char *hash = strchr(line, '#');
        char *eq;
        char *k;
        char *v;
        if (hash) {
            *hash = '\0';
        }
        owe_trim(line);
        if (!*line) {
            continue;
        }
        if (line[0] == '[') {
            char *end = strchr(line, ']');
            if (end) {
                *end = '\0';
                snprintf(section, sizeof(section), "%s", line + 1);
            }
            continue;
        }
        eq = strchr(line, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        k = line;
        v = eq + 1;
        owe_trim(k);
        owe_trim(v);
        strip_quotes(v);
        if (strcmp(section, "pause") == 0) {
            if (strcmp(k, "fullscreen") == 0) {
                parse_bool(v, &cfg->pause_fullscreen);
            } else if (strcmp(k, "occupied_workspace") == 0) {
                parse_bool(v, &cfg->pause_occupied_workspace);
            } else if (strcmp(k, "battery_poster") == 0) {
                parse_bool(v, &cfg->battery_poster);
            } else if (strcmp(k, "blocklist") == 0) {
                cfg->blocklist_count = 0;
                parse_blocklist(cfg, v);
            }
        } else if (strcmp(section, "transcode") == 0) {
            if (strcmp(k, "gif_fps") == 0) {
                cfg->gif_fps = atoi(v);
            } else if (strcmp(k, "gif_crf") == 0) {
                cfg->gif_crf = atoi(v);
            } else if (strcmp(k, "max_width") == 0) {
                cfg->transcode_max_width = atoi(v);
            } else if (strcmp(k, "max_height") == 0) {
                cfg->transcode_max_height = atoi(v);
            }
        } else if (strcmp(section, "render") == 0) {
            if (strcmp(k, "fade_ms") == 0) {
                cfg->fade_ms = atoi(v);
            }
        }
    }
    fclose(f);
    if (cfg->gif_fps < 5) {
        cfg->gif_fps = 5;
    }
    if (cfg->gif_fps > 50) {
        cfg->gif_fps = 50;
    }
    if (cfg->transcode_max_width < 320) {
        cfg->transcode_max_width = 320;
    }
    if (cfg->transcode_max_height < 200) {
        cfg->transcode_max_height = 200;
    }
    if (cfg->fade_ms < 0) {
        cfg->fade_ms = 0;
    }
    if (cfg->fade_ms > 2000) {
        cfg->fade_ms = 2000;
    }
    return 0;
}

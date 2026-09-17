#include "config.h"
#include "strutil.h"

#include <stdio.h>
#include <errno.h>
#include <limits.h>
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

static bool parse_int(const char *value, int *out) {
    char *end;
    errno = 0;
    long result = strtol(value, &end, 10);
    if (errno || end == value || *end || result < INT_MIN || result > INT_MAX) return false;
    *out = (int)result;
    return true;
}

int owe_config_load(owe_config_t *cfg, const char *path) {
    owe_config_t *destination = cfg;
    owe_config_t parsed = *cfg;
    cfg = &parsed;
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
                if (!parse_bool(v, &cfg->pause_fullscreen)) goto invalid;
            } else if (strcmp(k, "occupied_workspace") == 0) {
                if (!parse_bool(v, &cfg->pause_occupied_workspace)) goto invalid;
            } else if (strcmp(k, "battery_poster") == 0) {
                if (!parse_bool(v, &cfg->battery_poster)) goto invalid;
            } else if (strcmp(k, "blocklist") == 0) {
                cfg->blocklist_count = 0;
                parse_blocklist(cfg, v);
            }
        } else if (strcmp(section, "transcode") == 0) {
            if (strcmp(k, "gif_fps") == 0) {
                if (!parse_int(v, &cfg->gif_fps)) goto invalid;
            } else if (strcmp(k, "gif_crf") == 0) {
                if (!parse_int(v, &cfg->gif_crf)) goto invalid;
            } else if (strcmp(k, "max_width") == 0) {
                if (!parse_int(v, &cfg->transcode_max_width)) goto invalid;
            } else if (strcmp(k, "max_height") == 0) {
                if (!parse_int(v, &cfg->transcode_max_height)) goto invalid;
            }
        } else if (strcmp(section, "render") == 0) {
            if (strcmp(k, "fade_ms") == 0) {
                if (!parse_int(v, &cfg->fade_ms)) goto invalid;
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
    if (cfg->gif_crf < 0) cfg->gif_crf = 0;
    if (cfg->gif_crf > 51) cfg->gif_crf = 51;
    if (cfg->transcode_max_width > 16384) cfg->transcode_max_width = 16384;
    if (cfg->transcode_max_height > 16384) cfg->transcode_max_height = 16384;
    *destination = *cfg;
    return 0;
invalid:
    fclose(f);
    return -1;
}

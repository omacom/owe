#include "config.h"
#include "strutil.h"
#include "log.h"

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
    snprintf(cfg->battery_mode, sizeof(cfg->battery_mode), "play");
    snprintf(cfg->renderer_mode, sizeof(cfg->renderer_mode), "lazy");
    cfg->gif_fps = 20;
    cfg->gif_crf = 20;
    cfg->transcode_max_width = 2560;
    cfg->transcode_max_height = 1440;
    cfg->cache_max_mb = 512;
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

static bool apply_value(owe_config_t *cfg, const char *section, const char *key, char *value) {
    if (strcmp(section, "pause") == 0) {
        if (strcmp(key, "fullscreen") == 0) {
            return parse_bool(value, &cfg->pause_fullscreen);
        }
        if (strcmp(key, "occupied_workspace") == 0) {
            return parse_bool(value, &cfg->pause_occupied_workspace);
        }
        if (strcmp(key, "battery_poster") == 0) {
            return parse_bool(value, &cfg->battery_poster);
        }
        if (strcmp(key, "battery_mode") == 0) {
            if (strcmp(value, "play") != 0 && strcmp(value, "pause") != 0 &&
                strcmp(value, "poster") != 0) {
                return false;
            }
            snprintf(cfg->battery_mode, sizeof(cfg->battery_mode), "%s", value);
            return true;
        }
        if (strcmp(key, "blocklist") == 0) {
            cfg->blocklist_count = 0;
            parse_blocklist(cfg, value);
            return true;
        }
    } else if (strcmp(section, "transcode") == 0) {
        if (strcmp(key, "gif_fps") == 0) {
            return parse_int(value, &cfg->gif_fps);
        }
        if (strcmp(key, "gif_crf") == 0) {
            return parse_int(value, &cfg->gif_crf);
        }
        if (strcmp(key, "max_width") == 0) {
            return parse_int(value, &cfg->transcode_max_width);
        }
        if (strcmp(key, "max_height") == 0) {
            return parse_int(value, &cfg->transcode_max_height);
        }
        if (strcmp(key, "cache_max_mb") == 0) {
            return parse_int(value, &cfg->cache_max_mb);
        }
    } else if (strcmp(section, "render") == 0) {
        if (strcmp(key, "fade_ms") == 0) {
            return parse_int(value, &cfg->fade_ms);
        }
        if (strcmp(key, "renderer_mode") == 0) {
            if (strcmp(value, "lazy") != 0 && strcmp(value, "always") != 0) {
                return false;
            }
            snprintf(cfg->renderer_mode, sizeof(cfg->renderer_mode), "%s", value);
            return true;
        }
    }
    OWE_WARN("unknown config key %s.%s", section[0] ? section : "(top)", key);
    return true;
}

int owe_config_load(owe_config_t *cfg, const char *path) {
    owe_config_t *destination = cfg;
    owe_config_t parsed = *cfg;
    FILE *f;
    char line[512];
    char section[512] = "";
    char pending_key[512] = "";
    char pending_value[4096] = "";
    cfg = &parsed;
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
        if (hash) {
            *hash = '\0';
        }
        owe_trim(line);
        if (pending_key[0]) {
            size_t used = strlen(pending_value);
            if (used < sizeof(pending_value) - 2) {
                snprintf(pending_value + used, sizeof(pending_value) - used, " %s", line);
            }
            if (strchr(line, ']')) {
                char key[sizeof(pending_key)];
                char value[sizeof(pending_value)];
                snprintf(key, sizeof(key), "%s", pending_key);
                snprintf(value, sizeof(value), "%s", pending_value);
                pending_key[0] = '\0';
                pending_value[0] = '\0';
                if (!apply_value(cfg, section, key, value)) {
                    goto invalid;
                }
            }
            continue;
        }
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
        {
            char *key = line;
            char *value = eq + 1;
            owe_trim(key);
            owe_trim(value);
            strip_quotes(value);
            if (value[0] == '[' && !strchr(value, ']')) {
                snprintf(pending_key, sizeof(pending_key), "%s", key);
                snprintf(pending_value, sizeof(pending_value), "%s", value);
                continue;
            }
            if (!apply_value(cfg, section, key, value)) {
                goto invalid;
            }
        }
    }
    if (pending_key[0]) {
        fclose(f);
        return -1;
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
    if (cfg->cache_max_mb < 0) cfg->cache_max_mb = 0;
    if (cfg->cache_max_mb > 65536) cfg->cache_max_mb = 65536;
    *destination = *cfg;
    return 0;
invalid:
    fclose(f);
    return -1;
}

#include "config.h"
#include "strutil.h"
#include "log.h"

#include <stdio.h>
#include <ctype.h>
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

static char *unquoted(char *text, char needle) {
    char quote = 0;
    for (char *p = text; *p; p++) {
        if (quote) {
            if (*p == '\\' && quote == '"' && p[1]) p++;
            else if (*p == quote) quote = 0;
        } else if (*p == needle) {
            return p;
        } else if (*p == '"' || *p == '\'') {
            quote = *p;
        }
    }
    return NULL;
}

static bool parse_blocklist(owe_config_t *cfg, char *v) {
    owe_trim(v);
    size_t len = strlen(v);
    if (len < 2 || *v != '[' || v[len - 1] != ']') return false;
    v[len - 1] = '\0';
    v++;
    cfg->blocklist_count = 0;
    for (;;) {
        while (isspace((unsigned char)*v)) v++;
        if (!*v) return true;
        char name[sizeof(cfg->blocklist[0])];
        size_t used = 0;
        char quote = (*v == '"' || *v == '\'') ? *v++ : 0;
        while (*v && (quote ? *v != quote : *v != ',')) {
            char c = *v++;
            if (quote == '"' && c == '\\') {
                c = *v++;
                if (c != '\\' && c != '"') return false;
            } else if (!quote && (c == '[' || c == ']' || c == '"' || c == '\'')) {
                return false;
            }
            if (used + 1 >= sizeof(name)) return false;
            name[used++] = c;
        }
        if (quote && *v++ != quote) return false;
        name[used] = '\0';
        if (!quote) owe_trim(name);
        if (!*name || cfg->blocklist_count >= 16) return false;
        snprintf(cfg->blocklist[cfg->blocklist_count], sizeof(cfg->blocklist[0]), "%s", name);
        cfg->blocklist_count++;
        while (isspace((unsigned char)*v)) v++;
        if (!*v) return true;
        if (*v++ != ',') return false;
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
            return parse_blocklist(cfg, value);
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
    unsigned int line_number = 0;
    cfg = &parsed;
    if (!path) {
        return -1;
    }
    f = fopen(path, "r");
    if (!f) {
        return -1;
    }
    while (fgets(line, sizeof(line), f)) {
        line_number++;
        if (strlen(line) == sizeof(line) - 1 && !strchr(line, '\n')) goto invalid;
        char *hash = unquoted(line, '#');
        char *eq;
        if (hash) {
            *hash = '\0';
        }
        owe_trim(line);
        if (pending_key[0]) {
            size_t used = strlen(pending_value);
            if (snprintf(pending_value + used, sizeof(pending_value) - used, " %s", line) >=
                (int)(sizeof(pending_value) - used)) goto invalid;
            if (unquoted(line, ']')) {
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
                if (end[1]) goto invalid;
                *end = '\0';
                snprintf(section, sizeof(section), "%s", line + 1);
            } else goto invalid;
            continue;
        }
        eq = strchr(line, '=');
        if (!eq) {
            goto invalid;
        }
        *eq = '\0';
        {
            char *key = line;
            char *value = eq + 1;
            owe_trim(key);
            owe_trim(value);
            strip_quotes(value);
            if (value[0] == '[' && !unquoted(value, ']')) {
                snprintf(pending_key, sizeof(pending_key), "%s", key);
                snprintf(pending_value, sizeof(pending_value), "%s", value);
                continue;
            }
            if (!apply_value(cfg, section, key, value)) {
                goto invalid;
            }
        }
    }
    if (pending_key[0] || ferror(f)) goto invalid;
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
    OWE_ERROR("Invalid config at %s:%u", path, line_number);
    fclose(f);
    errno = EINVAL;
    return -1;
}

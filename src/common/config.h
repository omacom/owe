#pragma once

#include <stdbool.h>

typedef struct owe_config {
    bool pause_fullscreen;
    bool pause_occupied_workspace;
    bool battery_poster;
    char battery_mode[16];
    char renderer_mode[16];
    int gif_fps;
    int gif_crf;
    int transcode_max_width;
    int transcode_max_height;
    int cache_max_mb;
    int fade_ms;
    char blocklist[16][64];
    int blocklist_count;
} owe_config_t;

void owe_config_defaults(owe_config_t *cfg);
int owe_config_load(owe_config_t *cfg, const char *path);

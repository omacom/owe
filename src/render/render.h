#pragma once

#include <stdbool.h>

struct owe_wayland;
struct owe_egl;
struct owe_mpv;
struct owe_still;
struct owe_render_ipc;
struct owe_feed;

typedef struct owe_app {
    struct owe_wayland *wl;
    struct owe_egl *egl;
    struct owe_mpv *mpv;
    struct owe_still *still;
    struct owe_render_ipc *ipc;
    struct owe_feed *feed;
    bool running;
    bool paused;
    bool feeding;
    bool intro;
    char current_path[4096];
    char current_kind[16];
} owe_app_t;

owe_app_t *owe_app_get(void);

void owe_app_request_render(void);
void owe_app_on_outputs_changed(void);

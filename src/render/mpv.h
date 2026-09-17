#pragma once

#include <stdbool.h>

struct owe_mpv;
struct owe_wayland;
struct owe_output;

struct owe_mpv *owe_mpv_new(struct owe_wayland *wl);
void owe_mpv_free(struct owe_mpv *m);

int owe_mpv_load(struct owe_mpv *m, const char *path);
void owe_mpv_stop(struct owe_mpv *m);
void owe_mpv_set_paused(struct owe_mpv *m, bool paused);

bool owe_mpv_has_video(struct owe_mpv *m);
bool owe_mpv_is_paused(struct owe_mpv *m);
bool owe_mpv_ready(struct owe_mpv *m);
const char *owe_mpv_path(struct owe_mpv *m);

int owe_mpv_fd(struct owe_mpv *m);
bool owe_mpv_process_updates(struct owe_mpv *m);
void owe_mpv_render_output(struct owe_mpv *m, struct owe_output *out);
void owe_mpv_report_swap(struct owe_mpv *m);
double owe_mpv_time_pos(struct owe_mpv *m);
const char *owe_mpv_error(struct owe_mpv *m);
const char *owe_mpv_hwdec(struct owe_mpv *m);

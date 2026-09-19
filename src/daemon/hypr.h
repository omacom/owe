#pragma once

#include <stdbool.h>

struct owed_hypr;

struct owed_hypr *owed_hypr_new(void);
void owed_hypr_free(struct owed_hypr *h);
int owed_hypr_event_fd(struct owed_hypr *h);
int owed_hypr_poll(struct owed_hypr *h);
void owed_hypr_tick(struct owed_hypr *h);

bool owed_hypr_any_fullscreen(struct owed_hypr *h);
bool owed_hypr_any_window_visible(struct owed_hypr *h);
bool owed_hypr_all_monitors_off(struct owed_hypr *h);
bool owed_hypr_locked(struct owed_hypr *h);
int owed_hypr_monitor_count(struct owed_hypr *h);
const char *owed_hypr_covered_names(struct owed_hypr *h);
void owed_hypr_refresh(struct owed_hypr *h);
void owed_hypr_refresh_clients(struct owed_hypr *h);
void owed_hypr_refresh_monitors(struct owed_hypr *h);

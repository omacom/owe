#pragma once

#include <stdbool.h>

#include "config.h"
#include "transcode.h"

struct owed_watch;
struct owed_hypr;
struct owed_power;
struct owed_policy;
struct owed_ipc;
struct owed_supervisor;

typedef struct owed_app {
    owe_config_t config;
    struct owed_watch *watch;
    struct owed_hypr *hypr;
    struct owed_power *power;
    struct owed_policy *policy;
    struct owed_ipc *ipc;
    struct owed_supervisor *supervisor;
    bool running;
    bool always_animate;

    char source_path[4096];
    char source_kind[16];

    char loaded_path[4096];
    char loaded_kind[16];

    owed_async_job_t *job;
    char fail_path[4096];
    int job_restarts;
    int render_paused; /* -1 unknown, 0 playing, 1 paused */
    unsigned long source_generation;
} owed_app_t;

owed_app_t *owed_app_get(void);

void owed_app_on_background_changed(const char *resolved_path);
void owed_app_on_policy_changed(void);
void owed_app_on_job_done(void);
void owed_app_on_renderer_restarted(void);
void owed_app_apply_policy(void);
void owed_app_emit_event(const char *name, const char *detail);

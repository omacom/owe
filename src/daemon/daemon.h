#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "config.h"
#include "transcode.h"

struct owed_watch;
struct owed_hypr;
struct owed_power;
struct owed_policy;
struct owed_ipc;
struct owed_supervisor;

enum owed_engine {
    OWE_ENGINE_NONE = 0,
    OWE_ENGINE_SHELL = 1,
    OWE_ENGINE_RENDERER = 2,
};

enum owed_intro_phase {
    OWE_INTRO_IDLE = 0,
    OWE_INTRO_PREPARING = 1,
    OWE_INTRO_PLAYING = 2,
    OWE_INTRO_FINISHING = 3,
};

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

    int engine;
    int shell_enabled; /* -1 unknown, 0 disabled, 1 enabled */
    int64_t shell_stop_at_ms;
    int64_t shell_retry_at_ms;
    int64_t renderer_retry_at_ms;

    char source_path[4096];
    char source_kind[16];

    char loaded_path[4096];
    char loaded_kind[16];

    /* The media that proved it plays, and the previous media captured when a
     * new load starts. A video that never presents a frame falls back to it. */
    char last_good_path[4096];
    char last_good_kind[16];
    char restore_path[4096];
    char restore_kind[16];
    /* The still on screen when a video load starts, so the renderer can fade
     * the new video in over it. */
    char transition_from[4096];
    bool media_pending;
    bool media_ready;
    int64_t media_deadline_ms;

    owed_async_job_t *job;
    bool job_is_poster;
    char fail_path[4096];
    char poster_fail_path[4096];
    int render_paused; /* -1 unknown, 0 playing, 1 paused */
    int render_feeding; /* 1 while the lock feed owns the renderer */
    int intro_active; /* 1 while a one-shot intro video owns the renderer */
    int intro_phase;
    bool intro_committed;
    char intro_path[4096];
    char intro_result[16];
    int64_t intro_deadline_ms;
    int64_t intro_phase_deadline_ms;
    unsigned long source_generation;
} owed_app_t;

owed_app_t *owed_app_get(void);

void owed_app_on_background_changed(const char *resolved_path);
void owed_app_on_policy_changed(void);
void owed_app_on_job_done(void);
void owed_app_on_renderer_restarted(void);
void owed_app_apply_policy(void);
const char *owed_app_engine(void);
bool owed_app_renderer_expected(void);

int owed_app_start_intro(const char *path);
int owed_app_commit_intro(void);
void owed_app_poll_intro(void);
void owed_app_stop_intro(const char *reason);
bool owed_app_intro_active(void);
const char *owed_app_intro_result(void);

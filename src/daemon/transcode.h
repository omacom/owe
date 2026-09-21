#pragma once

#include <stdbool.h>

typedef struct owed_async_job owed_async_job_t;

struct owed_async_result {
    int ok;
    char out[4096];
};

int owed_transcode_gif(const char *gif_path, int fps, int crf, int max_w, int max_h,
                       char *out_mp4, unsigned long out_len);
int owed_transcode_poster(const char *video_path, char *out_png, unsigned long out_len);

int owed_transcode_gif_path(const char *gif_path, int fps, int crf, int max_w, int max_h,
                            char *out_mp4, unsigned long out_len);
int owed_transcode_poster_path(const char *video_path, char *out_png, unsigned long out_len);
bool owed_transcode_file_ready(const char *path);

void owed_transcode_set_cache_limit(int max_mb);
void owed_transcode_prune_cache(void);
void owed_transcode_cleanup_cache(void);

owed_async_job_t *owed_async_gif(const char *gif_path, int fps, int crf, int max_w, int max_h);
owed_async_job_t *owed_async_poster(const char *video_path);
int owed_async_job_fd(owed_async_job_t *job);
const char *owed_async_job_input(owed_async_job_t *job);
int owed_async_job_finish(owed_async_job_t *job, struct owed_async_result *result);
void owed_async_job_free(owed_async_job_t *job);

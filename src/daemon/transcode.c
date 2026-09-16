#include "transcode.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "log.h"
#include "owe_spawn.h"
#include "xdg.h"

struct owed_async_job {
    pthread_t thread;
    int read_fd;
    int write_fd;
    char input[PATH_MAX];
    int is_gif;
    int fps;
    int crf;
    int max_w;
    int max_h;
};

static unsigned long fnv1a(const char *s) {
    unsigned long h = 1469598103934665603UL;
    while (*s) {
        h ^= (unsigned long)(unsigned char)*s++;
        h *= 1099511628211UL;
    }
    return h;
}

bool owed_transcode_file_ready(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0 && st.st_size > 0;
}

static int gif_cache_path(const char *gif_path, int fps, int crf, int max_w, int max_h, char *out,
                          unsigned long out_len) {
    char dir[PATH_MAX];
    char key[PATH_MAX + 64];
    struct stat st;
    if (owe_transcode_cache_dir(dir, sizeof(dir)) != 0) {
        return -1;
    }
    if (owe_mkdir_p(dir) != 0) {
        return -1;
    }
    if (stat(gif_path, &st) != 0) {
        return -1;
    }
    snprintf(key, sizeof(key), "%s|%lld|%lld|%d|%d|%d|%d", gif_path, (long long)st.st_size,
             (long long)st.st_mtime, fps, crf, max_w, max_h);
    snprintf(out, out_len, "%s/%016lx.mp4", dir, fnv1a(key));
    return 0;
}

static int poster_cache_path(const char *video_path, char *out, unsigned long out_len) {
    char dir[PATH_MAX];
    char key[PATH_MAX + 32];
    struct stat st;
    if (owe_transcode_cache_dir(dir, sizeof(dir)) != 0) {
        return -1;
    }
    if (owe_mkdir_p(dir) != 0) {
        return -1;
    }
    if (stat(video_path, &st) != 0) {
        return -1;
    }
    snprintf(key, sizeof(key), "poster|%s|%lld|%lld", video_path, (long long)st.st_size,
             (long long)st.st_mtime);
    snprintf(out, out_len, "%s/poster-%016lx.png", dir, fnv1a(key));
    return 0;
}

int owed_transcode_gif_path(const char *gif_path, int fps, int crf, int max_w, int max_h,
                            char *out_mp4, unsigned long out_len) {
    return gif_cache_path(gif_path, fps, crf, max_w, max_h, out_mp4, out_len);
}

int owed_transcode_poster_path(const char *video_path, char *out_png, unsigned long out_len) {
    return poster_cache_path(video_path, out_png, out_len);
}

static int run_gif(const owed_async_job_t *job, char *out, unsigned long out_len) {
    char cached[PATH_MAX];
    char vf[256];
    char fps_s[16];
    char crf_s[16];
    char *argv[32];
    int ai = 0;
    if (gif_cache_path(job->input, job->fps, job->crf, job->max_w, job->max_h, cached,
                       sizeof(cached)) != 0) {
        return -1;
    }
    if (owed_transcode_file_ready(cached)) {
        snprintf(out, out_len, "%s", cached);
        return 0;
    }
    snprintf(fps_s, sizeof(fps_s), "%d", job->fps);
    snprintf(crf_s, sizeof(crf_s), "%d", job->crf);
    snprintf(vf, sizeof(vf),
             "fps=%d,scale=%d:%d:force_original_aspect_ratio=decrease:flags=lanczos,"
             "scale=trunc(iw/2)*2:trunc(ih/2)*2,format=yuv420p",
             job->fps, job->max_w, job->max_h);
    argv[ai++] = "ffmpeg";
    argv[ai++] = "-y";
    argv[ai++] = "-v";
    argv[ai++] = "error";
    argv[ai++] = "-i";
    argv[ai++] = (char *)job->input;
    argv[ai++] = "-vf";
    argv[ai++] = vf;
    argv[ai++] = "-an";
    argv[ai++] = "-c:v";
    argv[ai++] = "libx264";
    argv[ai++] = "-preset";
    argv[ai++] = "veryfast";
    argv[ai++] = "-crf";
    argv[ai++] = crf_s;
    argv[ai++] = "-movflags";
    argv[ai++] = "+faststart";
    argv[ai++] = "-r";
    argv[ai++] = fps_s;
    argv[ai++] = cached;
    argv[ai] = NULL;
    OWE_INFO("transcoding gif %s -> %s", job->input, cached);
    {
        char log[4096];
        if (owe_spawn_capture("ffmpeg", argv, log, sizeof(log), 300000) != 0) {
            OWE_ERROR("ffmpeg gif failed: %s", log);
            unlink(cached);
            return -1;
        }
    }
    if (!owed_transcode_file_ready(cached)) {
        unlink(cached);
        return -1;
    }
    snprintf(out, out_len, "%s", cached);
    OWE_INFO("gif cached %s", cached);
    return 0;
}

static int run_poster(const owed_async_job_t *job, char *out, unsigned long out_len) {
    char poster[PATH_MAX];
    char *argv[20];
    int ai = 0;
    if (poster_cache_path(job->input, poster, sizeof(poster)) != 0) {
        return -1;
    }
    if (owed_transcode_file_ready(poster)) {
        snprintf(out, out_len, "%s", poster);
        return 0;
    }
    argv[ai++] = "ffmpeg";
    argv[ai++] = "-y";
    argv[ai++] = "-v";
    argv[ai++] = "error";
    argv[ai++] = "-i";
    argv[ai++] = (char *)job->input;
    argv[ai++] = "-frames:v";
    argv[ai++] = "1";
    argv[ai++] = "-vf";
    argv[ai++] = "scale=w='min(2560,iw)':h=-2";
    argv[ai++] = poster;
    argv[ai] = NULL;
    {
        char log[4096];
        if (owe_spawn_capture("ffmpeg", argv, log, sizeof(log), 120000) != 0) {
            OWE_ERROR("poster extract failed: %s", log);
            unlink(poster);
            return -1;
        }
    }
    if (!owed_transcode_file_ready(poster)) {
        unlink(poster);
        return -1;
    }
    snprintf(out, out_len, "%s", poster);
    return 0;
}

int owed_transcode_gif(const char *gif_path, int fps, int crf, int max_w, int max_h,
                       char *out_mp4, unsigned long out_len) {
    owed_async_job_t job;
    memset(&job, 0, sizeof(job));
    job.is_gif = 1;
    job.fps = fps;
    job.crf = crf;
    job.max_w = max_w;
    job.max_h = max_h;
    snprintf(job.input, sizeof(job.input), "%s", gif_path);
    return run_gif(&job, out_mp4, out_len);
}

int owed_transcode_poster(const char *video_path, char *out_png, unsigned long out_len) {
    owed_async_job_t job;
    memset(&job, 0, sizeof(job));
    snprintf(job.input, sizeof(job.input), "%s", video_path);
    return run_poster(&job, out_png, out_len);
}

static void *job_main(void *arg) {
    owed_async_job_t *job = arg;
    struct owed_async_result result;
    ssize_t n;
    memset(&result, 0, sizeof(result));
    if (job->is_gif) {
        result.ok = run_gif(job, result.out, sizeof(result.out)) == 0;
    } else {
        result.ok = run_poster(job, result.out, sizeof(result.out)) == 0;
    }
    n = write(job->write_fd, &result, sizeof(result));
    if (n < 0) {
        OWE_WARN("async job completion write failed");
    }
    close(job->write_fd);
    job->write_fd = -1;
    return NULL;
}

owed_async_job_t *owed_async_gif(const char *gif_path, int fps, int crf, int max_w, int max_h) {
    owed_async_job_t *job = calloc(1, sizeof(*job));
    int fds[2];
    if (!job) {
        return NULL;
    }
    if (pipe2(fds, O_CLOEXEC) != 0) {
        free(job);
        return NULL;
    }
    job->read_fd = fds[0];
    job->write_fd = fds[1];
    fcntl(job->read_fd, F_SETFL, O_NONBLOCK);
    job->is_gif = 1;
    job->fps = fps;
    job->crf = crf;
    job->max_w = max_w;
    job->max_h = max_h;
    snprintf(job->input, sizeof(job->input), "%s", gif_path);
    if (pthread_create(&job->thread, NULL, job_main, job) != 0) {
        close(job->read_fd);
        close(job->write_fd);
        free(job);
        return NULL;
    }
    return job;
}

owed_async_job_t *owed_async_poster(const char *video_path) {
    owed_async_job_t *job = calloc(1, sizeof(*job));
    int fds[2];
    if (!job) {
        return NULL;
    }
    if (pipe2(fds, O_CLOEXEC) != 0) {
        free(job);
        return NULL;
    }
    job->read_fd = fds[0];
    job->write_fd = fds[1];
    fcntl(job->read_fd, F_SETFL, O_NONBLOCK);
    job->is_gif = 0;
    snprintf(job->input, sizeof(job->input), "%s", video_path);
    if (pthread_create(&job->thread, NULL, job_main, job) != 0) {
        close(job->read_fd);
        close(job->write_fd);
        free(job);
        return NULL;
    }
    return job;
}

int owed_async_job_fd(owed_async_job_t *job) {
    return job ? job->read_fd : -1;
}

const char *owed_async_job_input(owed_async_job_t *job) {
    return job ? job->input : "";
}

int owed_async_job_finish(owed_async_job_t *job, struct owed_async_result *result) {
    ssize_t n;
    if (!job || !result) {
        return -1;
    }
    n = read(job->read_fd, result, sizeof(*result));
    pthread_join(job->thread, NULL);
    if (n != (ssize_t)sizeof(*result)) {
        result->ok = 0;
        result->out[0] = '\0';
        return -1;
    }
    return 0;
}

void owed_async_job_free(owed_async_job_t *job) {
    if (!job) {
        return;
    }
    if (job->read_fd >= 0) {
        close(job->read_fd);
    }
    if (job->write_fd >= 0) {
        close(job->write_fd);
    }
    free(job);
}

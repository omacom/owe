#include "transcode.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

#include "log.h"
#include "owe_spawn.h"
#include "xdg.h"

struct owed_async_job {
    pthread_t thread;
    int event_fd;
    bool joined;
    atomic_bool cancel;
    struct owed_async_result result;
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
    return path && stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

static int g_cache_max_mb = 512;

void owed_transcode_set_cache_limit(int max_mb) {
    g_cache_max_mb = max_mb;
}

struct cache_entry {
    char path[PATH_MAX];
    time_t mtime;
    off_t size;
};

static int cache_entry_cmp(const void *a, const void *b) {
    const struct cache_entry *left = a;
    const struct cache_entry *right = b;
    if (left->mtime < right->mtime) return -1;
    if (left->mtime > right->mtime) return 1;
    return 0;
}

/* Remove the oldest cache files until the budget is met. A limit of 0 or
 * less disables eviction. */
void owed_transcode_prune_cache(void) {
    char dir[PATH_MAX];
    DIR *handle;
    struct dirent *entry;
    struct cache_entry *files = NULL;
    size_t count = 0;
    size_t capacity = 0;
    long long total = 0;
    long long budget;
    if (g_cache_max_mb <= 0) {
        return;
    }
    if (owe_transcode_cache_dir(dir, sizeof(dir)) != 0) {
        return;
    }
    handle = opendir(dir);
    if (!handle) {
        return;
    }
    while ((entry = readdir(handle))) {
        char path[PATH_MAX];
        struct stat st;
        if (entry->d_name[0] == '.') {
            continue;
        }
        if (snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name) >= (int)sizeof(path)) {
            continue;
        }
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }
        if (count == capacity) {
            size_t next = capacity ? capacity * 2 : 32;
            struct cache_entry *grown = realloc(files, next * sizeof(*files));
            if (!grown) {
                break;
            }
            files = grown;
            capacity = next;
        }
        snprintf(files[count].path, sizeof(files[count].path), "%s", path);
        files[count].mtime = st.st_mtime;
        files[count].size = st.st_size;
        total += st.st_size;
        count++;
    }
    closedir(handle);
    budget = (long long)g_cache_max_mb * 1024 * 1024;
    if (total > budget && count > 0) {
        size_t i;
        qsort(files, count, sizeof(*files), cache_entry_cmp);
        for (i = 0; i < count && total > budget; i++) {
            if (unlink(files[i].path) == 0) {
                total -= files[i].size;
                OWE_INFO("cache pruned %s", files[i].path);
            }
        }
    }
    free(files);
}

static int gif_cache_path(const char *gif_path, int fps, int crf, int max_w, int max_h, char *out,
                          unsigned long out_len) {
    char dir[PATH_MAX];
    char key[PATH_MAX + 256];
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
    snprintf(key, sizeof(key), "gif-v2|%s|%lld|%lld|%ld|%lld|%ld|%d|%d|%d|%d", gif_path,
             (long long)st.st_size, (long long)st.st_mtim.tv_sec, st.st_mtim.tv_nsec,
             (long long)st.st_ctim.tv_sec, st.st_ctim.tv_nsec, fps, crf, max_w, max_h);
    return snprintf(out, out_len, "%s/%016lx.mp4", dir, fnv1a(key)) < (int)out_len ? 0 : -1;
}

static int poster_cache_path(const char *video_path, char *out, unsigned long out_len) {
    char dir[PATH_MAX];
    char key[PATH_MAX + 256];
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
    snprintf(key, sizeof(key), "poster-v2|%s|%lld|%lld|%ld|%lld|%ld", video_path,
             (long long)st.st_size, (long long)st.st_mtim.tv_sec, st.st_mtim.tv_nsec,
             (long long)st.st_ctim.tv_sec, st.st_ctim.tv_nsec);
    return snprintf(out, out_len, "%s/poster-%016lx.png", dir, fnv1a(key)) < (int)out_len ? 0 : -1;
}

static int temp_output(const char *target, const char *suffix, char *tmp, size_t len) {
    if (snprintf(tmp, len, "%s.part-XXXXXX%s", target, suffix) >= (int)len) return -1;
    int fd = mkstemps(tmp, (int)strlen(suffix));
    if (fd < 0) return -1;
    close(fd);
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
    char tmp[PATH_MAX + 32];
    char vf[256];
    char fps_s[16];
    char crf_s[16];
    char threads_s[16];
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
    {
        long cores = sysconf(_SC_NPROCESSORS_ONLN);
        int threads = cores > 4 ? 4 : (int)(cores > 1 ? cores : 1);
        snprintf(threads_s, sizeof(threads_s), "%d", threads);
    }
    snprintf(vf, sizeof(vf),
             "fps=%d,scale='min(iw,%d)':'min(ih,%d)':force_original_aspect_ratio=decrease:flags=lanczos,"
             "scale='max(2,trunc(iw/2)*2)':'max(2,trunc(ih/2)*2)',format=yuv420p",
             job->fps, job->max_w, job->max_h);
    argv[ai++] = "ffmpeg";
    argv[ai++] = "-y";
    argv[ai++] = "-v";
    argv[ai++] = "error";
    argv[ai++] = "-nostdin";
    argv[ai++] = "-i";
    argv[ai++] = (char *)job->input;
    argv[ai++] = "-map";
    argv[ai++] = "0:v:0";
    argv[ai++] = "-vf";
    argv[ai++] = vf;
    argv[ai++] = "-an";
    argv[ai++] = "-c:v";
    argv[ai++] = "libx264";
    argv[ai++] = "-threads";
    argv[ai++] = threads_s;
    argv[ai++] = "-preset";
    argv[ai++] = "veryfast";
    argv[ai++] = "-crf";
    argv[ai++] = crf_s;
    argv[ai++] = "-movflags";
    argv[ai++] = "+faststart";
    argv[ai++] = "-r";
    argv[ai++] = fps_s;
    if (temp_output(cached, ".mp4", tmp, sizeof(tmp)) != 0) return -1;
    argv[ai++] = tmp;
    argv[ai] = NULL;
    OWE_INFO("transcoding gif %s -> %s", job->input, cached);
    {
        char log[4096];
        if (owe_spawn_capture_cancel("ffmpeg", argv, log, sizeof(log), 300000, &job->cancel) != 0) {
            OWE_ERROR("ffmpeg gif failed: %s", log);
            unlink(tmp);
            return -1;
        }
    }
    if (atomic_load(&job->cancel) || !owed_transcode_file_ready(tmp) || rename(tmp, cached) != 0) {
        unlink(tmp);
        return -1;
    }
    snprintf(out, out_len, "%s", cached);
    OWE_INFO("gif cached %s", cached);
    owed_transcode_prune_cache();
    return 0;
}

static int run_poster(const owed_async_job_t *job, char *out, unsigned long out_len) {
    char poster[PATH_MAX];
    char tmp[PATH_MAX + 32];
    char *argv[24];
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
    argv[ai++] = "-nostdin";
    argv[ai++] = "-i";
    argv[ai++] = (char *)job->input;
    argv[ai++] = "-an";
    argv[ai++] = "-map";
    argv[ai++] = "0:v:0";
    argv[ai++] = "-frames:v";
    argv[ai++] = "1";
    argv[ai++] = "-vf";
    argv[ai++] = "scale=w='min(2560,iw)':h=-2";
    if (temp_output(poster, ".png", tmp, sizeof(tmp)) != 0) return -1;
    argv[ai++] = tmp;
    argv[ai] = NULL;
    {
        char log[4096];
        if (owe_spawn_capture_cancel("ffmpeg", argv, log, sizeof(log), 120000, &job->cancel) != 0) {
            OWE_ERROR("poster extract failed: %s", log);
            unlink(tmp);
            return -1;
        }
    }
    if (atomic_load(&job->cancel) || !owed_transcode_file_ready(tmp) || rename(tmp, poster) != 0) {
        unlink(tmp);
        return -1;
    }
    snprintf(out, out_len, "%s", poster);
    owed_transcode_prune_cache();
    return 0;
}

int owed_transcode_gif(const char *gif_path, int fps, int crf, int max_w, int max_h,
                       char *out_mp4, unsigned long out_len) {
    owed_async_job_t job;
    memset(&job, 0, sizeof(job));
    atomic_init(&job.cancel, false);
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
    atomic_init(&job.cancel, false);
    snprintf(job.input, sizeof(job.input), "%s", video_path);
    return run_poster(&job, out_png, out_len);
}

static void *job_main(void *arg) {
    owed_async_job_t *job = arg;
    if (job->is_gif) {
        job->result.ok = run_gif(job, job->result.out, sizeof(job->result.out)) == 0;
    } else {
        job->result.ok = run_poster(job, job->result.out, sizeof(job->result.out)) == 0;
    }
    uint64_t done = 1;
    while (write(job->event_fd, &done, sizeof(done)) < 0 && errno == EINTR) {}
    return NULL;
}

owed_async_job_t *owed_async_gif(const char *gif_path, int fps, int crf, int max_w, int max_h) {
    owed_async_job_t *job = calloc(1, sizeof(*job));
    if (!job) {
        return NULL;
    }
    job->event_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (job->event_fd < 0) {
        free(job);
        return NULL;
    }
    atomic_init(&job->cancel, false);
    job->is_gif = 1;
    job->fps = fps;
    job->crf = crf;
    job->max_w = max_w;
    job->max_h = max_h;
    snprintf(job->input, sizeof(job->input), "%s", gif_path);
    if (pthread_create(&job->thread, NULL, job_main, job) != 0) {
        close(job->event_fd);
        free(job);
        return NULL;
    }
    return job;
}

owed_async_job_t *owed_async_poster(const char *video_path) {
    owed_async_job_t *job = calloc(1, sizeof(*job));
    if (!job) {
        return NULL;
    }
    job->event_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (job->event_fd < 0) {
        free(job);
        return NULL;
    }
    atomic_init(&job->cancel, false);
    job->is_gif = 0;
    snprintf(job->input, sizeof(job->input), "%s", video_path);
    if (pthread_create(&job->thread, NULL, job_main, job) != 0) {
        close(job->event_fd);
        free(job);
        return NULL;
    }
    return job;
}

int owed_async_job_fd(owed_async_job_t *job) {
    return job ? job->event_fd : -1;
}

const char *owed_async_job_input(owed_async_job_t *job) {
    return job ? job->input : "";
}

int owed_async_job_finish(owed_async_job_t *job, struct owed_async_result *result) {
    uint64_t done;
    if (!job || !result) {
        return -1;
    }
    if (read(job->event_fd, &done, sizeof(done)) != sizeof(done)) return -1;
    pthread_join(job->thread, NULL);
    job->joined = true;
    *result = job->result;
    return 0;
}

void owed_async_job_free(owed_async_job_t *job) {
    if (!job) {
        return;
    }
    if (!job->joined) {
        atomic_store(&job->cancel, true);
        pthread_join(job->thread, NULL);
    }
    close(job->event_fd);
    free(job);
}

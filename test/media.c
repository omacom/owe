#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "owe_spawn.h"
#include "transcode.h"
#include "xdg.h"
#include "yyjson.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

int main(void) {
    char root[] = "/tmp/owe-media-XXXXXX";
    CHECK(mkdtemp(root));
    CHECK(setenv("XDG_CACHE_HOME", root, 1) == 0);
    char input[512], log[8192];
    snprintf(input, sizeof(input), "%s/tiny \"quote\".gif", root);
    char *generate[] = {"ffmpeg", "-v", "error", "-nostdin", "-f", "lavfi", "-i",
                        "testsrc=size=31x23:rate=10:duration=1", "-threads", "1", input, NULL};
    CHECK(owe_spawn_capture("ffmpeg", generate, log, sizeof(log), 15000) == 0);
    owed_async_job_t *job = owed_async_gif(input, 20, 20, 640, 360);
    CHECK(job);
    struct pollfd fd = {.fd = owed_async_job_fd(job), .events = POLLIN};
    CHECK(poll(&fd, 1, 15000) == 1);
    struct owed_async_result result;
    CHECK(owed_async_job_finish(job, &result) == 0 && result.ok);
    owed_async_job_free(job);
    char *probe[] = {"ffprobe", "-v", "error", "-show_entries", "stream=codec_name,codec_type,width,height",
                    "-of", "json", result.out, NULL};
    CHECK(owe_spawn_capture("ffprobe", probe, log, sizeof(log), 5000) == 0);
    yyjson_doc *doc = yyjson_read(log, strlen(log), 0);
    CHECK(doc);
    yyjson_val *streams = yyjson_obj_get(yyjson_doc_get_root(doc), "streams");
    CHECK(yyjson_arr_size(streams) == 1);
    yyjson_val *video = yyjson_arr_get(streams, 0);
    CHECK(yyjson_equals_str(yyjson_obj_get(video, "codec_name"), "h264"));
    CHECK(yyjson_get_int(yyjson_obj_get(video, "width")) == 30);
    CHECK(yyjson_get_int(yyjson_obj_get(video, "height")) == 22);
    yyjson_doc_free(doc);
    job = owed_async_poster(result.out);
    CHECK(job);
    fd = (struct pollfd){.fd = owed_async_job_fd(job), .events = POLLIN};
    CHECK(poll(&fd, 1, 15000) == 1);
    CHECK(owed_async_job_finish(job, &result) == 0 && result.ok);
    owed_async_job_free(job);
    CHECK(owed_transcode_file_ready(result.out));
    {
        char cache[512];
        char big[1024];
        char small[1024];
        struct timespec times[2];
        int big_fd;
        int small_fd;
        CHECK(owe_transcode_cache_dir(cache, sizeof(cache)) == 0);
        CHECK(owe_mkdir_p(cache) == 0);
        snprintf(big, sizeof(big), "%s/prune-big.mp4", cache);
        snprintf(small, sizeof(small), "%s/prune-small.mp4", cache);
        big_fd = open(big, O_CREAT | O_WRONLY | O_TRUNC, 0600);
        small_fd = open(small, O_CREAT | O_WRONLY | O_TRUNC, 0600);
        CHECK(big_fd >= 0 && small_fd >= 0);
        CHECK(ftruncate(big_fd, 2 * 1024 * 1024) == 0);
        CHECK(ftruncate(small_fd, 4096) == 0);
        close(big_fd);
        close(small_fd);
        times[0].tv_sec = time(NULL) - 3600;
        times[0].tv_nsec = 0;
        times[1] = times[0];
        CHECK(utimensat(AT_FDCWD, big, times, 0) == 0);
        owed_transcode_set_cache_limit(1);
        owed_transcode_prune_cache();
        CHECK(!owed_transcode_file_ready(big));
        CHECK(owed_transcode_file_ready(small));
        unlink(small);
        puts("Cache eviction passed");
    }
    char *remove[] = {"rm", "-rf", "--", root, NULL};
    CHECK(owe_spawn_capture("rm", remove, log, sizeof(log), 2000) == 0);
    puts("Real GIF and poster conversion passed");
    return 0;
}

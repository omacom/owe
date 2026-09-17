#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <unistd.h>
#include "owe_spawn.h"
#include "transcode.h"
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
    char *remove[] = {"rm", "-rf", "--", root, NULL};
    CHECK(owe_spawn_capture("rm", remove, log, sizeof(log), 2000) == 0);
    puts("Real GIF and poster conversion passed");
    return 0;
}

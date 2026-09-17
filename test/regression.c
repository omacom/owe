#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "common_ipc.h"
#include "config.h"
#include "display_state.h"
#include "json.h"
#include "owe_spawn.h"
#include "daemon.h"
#include "supervisor.h"
#include "transcode.h"
#include "watch.h"
#include "xdg.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

/* File-watch callbacks are not part of these isolated lifecycle tests. */
owed_app_t *owed_app_get(void) { return NULL; }
void owed_app_on_background_changed(const char *path) { (void)path; }

static char root[PATH_MAX];
static int groups;

static void path_for(char *out, size_t n, const char *relative) {
    CHECK(snprintf(out, n, "%s/%s", root, relative) < (int)n);
}

static void put(const char *relative, const char *text) {
    char path[PATH_MAX];
    path_for(path, sizeof(path), relative);
    FILE *f = fopen(path, "w");
    CHECK(f);
    CHECK(fputs(text, f) >= 0);
    CHECK(fclose(f) == 0);
}

static void directory(const char *relative) {
    char path[PATH_MAX];
    path_for(path, sizeof(path), relative);
    CHECK(owe_mkdir_p(path) == 0);
}

static double seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void test_json(void) {
    const char *filename = "/tmp/a\"b\\c\n雪.mp4";
    char *quoted = owe_json_quote(filename);
    CHECK(quoted);
    yyjson_doc *doc = yyjson_read(quoted, strlen(quoted), 0);
    CHECK(doc);
    CHECK(yyjson_equals_str(yyjson_doc_get_root(doc), filename));
    CHECK(owe_json_path(yyjson_doc_get_root(doc)));
    yyjson_doc_free(doc);
    free(quoted);
    CHECK(owe_json_ok("{\"status\":\"ok\",\"path\":\"error\"}"));
    CHECK(!owe_json_ok("{\"status\":\"error\",\"message\":\"ok\"}"));
    CHECK(!owe_json_ok("[]"));
    doc = yyjson_read("\"x\\u0000.png\"", strlen("\"x\\u0000.png\""), 0);
    CHECK(doc && !owe_json_path(yyjson_doc_get_root(doc)));
    yyjson_doc_free(doc);
    groups++;
}

static void test_ipc(void) {
    int fds[2];
    char line[32];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    CHECK(owe_ipc_send_line(fds[0], "{\"cmd\":\"status\"}") == 0);
    CHECK(owe_ipc_recv_line(fds[1], line, sizeof(line)) == 0);
    CHECK(strcmp(line, "{\"cmd\":\"status\"}") == 0);
    CHECK(write(fds[0], "truncated", 9) == 9);
    shutdown(fds[0], SHUT_WR);
    CHECK(owe_ipc_recv_line(fds[1], line, sizeof(line)) < 0);
    close(fds[0]); close(fds[1]);
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    CHECK(owe_ipc_send_line(fds[0], "12345678") == 0);
    CHECK(owe_ipc_recv_line(fds[1], line, 4) < 0);
    close(fds[0]); close(fds[1]);

    char sock[PATH_MAX];
    path_for(sock, sizeof(sock), "test.sock");
    owe_ipc_server_t *srv = owe_ipc_server_new(sock);
    CHECK(srv);
    CHECK(!owe_ipc_server_new(sock));
    int client = owe_ipc_connect(sock);
    CHECK(client >= 0);
    int peer = owe_ipc_server_accept(srv);
    CHECK(peer >= 0);
    CHECK(owe_ipc_send_line(client, "hello") == 0);
    CHECK(owe_ipc_recv_line(peer, line, sizeof(line)) == 0);
    close(peer); close(client);
    owe_ipc_server_free(srv);
    CHECK(access(sock, F_OK) < 0);
    groups++;
}

static void test_display_state(void) {
    directory("drm/card0-eDP-1");
    char drm[PATH_MAX];
    path_for(drm, sizeof(drm), "drm");
    CHECK(owe_drm_all_off(drm) == -1);
    put("drm/card0-eDP-1/status", "connected\n");
    CHECK(owe_drm_all_off(drm) == -1);
    put("drm/card0-eDP-1/dpms", "On\n");
    CHECK(owe_drm_all_off(drm) == 0);
    put("drm/card0-eDP-1/dpms", "Off\n");
    CHECK(owe_drm_all_off(drm) == 1);
    directory("drm/card0-DP-1");
    put("drm/card0-DP-1/status", "connected\n");
    CHECK(owe_drm_all_off(drm) == -1);
    put("drm/card0-DP-1/dpms", "On\n");
    CHECK(owe_drm_all_off(drm) == 0);
    put("drm/card0-DP-1/status", "disconnected\n");
    CHECK(owe_drm_all_off(drm) == 1);

    const char *monitors = "[{\"activeWorkspace\":{\"id\":1},\"dpmsStatus\":true}]";
    CHECK(!owe_outputs_covered("[{\"workspace\":{\"id\":2},\"fullscreen\":2}]", monitors, true));
    CHECK(owe_outputs_covered("[{\"workspace\":{\"id\":1},\"fullscreen\":2}]", monitors, true));
    CHECK(!owe_outputs_covered("[{\"workspace\":{\"id\":1},\"fullscreen\":1}]", monitors, true));
    CHECK(!owe_outputs_covered("[{\"workspace\":{\"id\":1},\"hidden\":true}]", monitors, false));
    CHECK(owe_outputs_covered("[{\"workspace\":{\"id\":1}}]", monitors, false));
    monitors = "[{\"activeWorkspace\":{\"id\":1}},{\"activeWorkspace\":{\"id\":2}}]";
    CHECK(!owe_outputs_covered("[{\"workspace\":{\"id\":1},\"fullscreen\":2}]", monitors, true));
    CHECK(!owe_outputs_covered("[]", "[]", false));
    groups++;
}

static void test_symlink(void) {
    char input[PATH_MAX], resolved[PATH_MAX];
    directory("omarchy/current");
    put("file\"\\\n.png", "image");
    path_for(input, sizeof(input), "file\"\\\n.png");
    CHECK(owed_watch_set_current(input) == 0);
    CHECK(owed_watch_resolve_current(resolved, sizeof(resolved)) == 0);
    CHECK(strcmp(input, resolved) == 0);
    CHECK(owed_watch_set_current("/does/not/exist.mp4") < 0);
    CHECK(owed_watch_resolve_current(resolved, sizeof(resolved)) == 0);
    CHECK(strcmp(input, resolved) == 0);
    CHECK(owed_watch_set_current(root) < 0);
    groups++;
}

static void await_start(const char *marker) {
    double deadline = seconds() + 5;
    while (access(marker, F_OK) != 0 && seconds() < deadline) usleep(1000);
    CHECK(access(marker, F_OK) == 0);
}

static void no_parts(void) {
    char cache[PATH_MAX];
    CHECK(owe_transcode_cache_dir(cache, sizeof(cache)) == 0);
    DIR *dir = opendir(cache);
    CHECK(dir);
    struct dirent *ent;
    while ((ent = readdir(dir))) CHECK(!strstr(ent->d_name, ".part-"));
    closedir(dir);
}

static void test_jobs(void) {
    char input[PATH_MAX], target[PATH_MAX], marker[PATH_MAX];
    put("input.gif", "source");
    path_for(input, sizeof(input), "input.gif");
    path_for(marker, sizeof(marker), "started");
    CHECK(setenv("OWE_TEST_FFMPEG_STARTED", marker, 1) == 0);
    CHECK(owed_transcode_gif_path(input, 20, 20, 640, 360, target, sizeof(target)) == 0);
    CHECK(setenv("OWE_TEST_FFMPEG_MODE", "wait", 1) == 0);
    owed_async_job_t *job = owed_async_gif(input, 20, 20, 640, 360);
    CHECK(job);
    await_start(marker);
    CHECK(!owed_transcode_file_ready(target));
    double start = seconds();
    owed_async_job_free(job);
    CHECK(seconds() - start < 2);
    CHECK(!owed_transcode_file_ready(target));
    no_parts();

    unlink(marker);
    CHECK(setenv("OWE_TEST_FFMPEG_MODE", "ok", 1) == 0);
    job = owed_async_gif(input, 20, 20, 640, 360);
    CHECK(job);
    struct owed_supervisor *supervisor = owed_supervisor_new();
    CHECK(supervisor);
    struct pollfd pfd = {.fd = owed_async_job_fd(job), .events = POLLIN};
    double deadline = seconds() + 5;
    while (poll(&pfd, 1, 1) == 0 && seconds() < deadline) owed_supervisor_reap(supervisor);
    CHECK(pfd.revents & POLLIN);
    struct owed_async_result result;
    CHECK(owed_async_job_finish(job, &result) == 0 && result.ok);
    CHECK(strcmp(result.out, target) == 0 && owed_transcode_file_ready(target));
    owed_async_job_free(job);
    owed_supervisor_free(supervisor);
    no_parts();

    CHECK(unlink(target) == 0);
    CHECK(setenv("OWE_TEST_FFMPEG_MODE", "fail", 1) == 0);
    job = owed_async_gif(input, 20, 20, 640, 360);
    CHECK(job);
    pfd = (struct pollfd){.fd = owed_async_job_fd(job), .events = POLLIN};
    CHECK(poll(&pfd, 1, 5000) == 1);
    CHECK(owed_async_job_finish(job, &result) == 0 && !result.ok);
    owed_async_job_free(job);
    CHECK(!owed_transcode_file_ready(target));
    no_parts();
    groups++;
}

static void test_spawn(void) {
    char out[64];
    char *argv[] = {"sh", "-c", "exec 1>&- 2>&-; sleep 0.2; exit 7", NULL};
    double start = seconds();
    CHECK(owe_spawn_capture("sh", argv, out, sizeof(out), 2000) < 0);
    CHECK(seconds() - start < 1);
    char *slow[] = {"sleep", "5", NULL};
    start = seconds();
    CHECK(owe_spawn_capture("sleep", slow, out, sizeof(out), 100) < 0);
    CHECK(seconds() - start < 1);
    groups++;
}

static void test_config_errors(void) {
    put("config.toml", "[pause]\nfullscreen=false\n[transcode]\ngif_fps=99999999999999999999999999999\n");
    char path[PATH_MAX];
    path_for(path, sizeof(path), "config.toml");
    owe_config_t config;
    owe_config_defaults(&config);
    CHECK(owe_config_load(&config, path) < 0);
    CHECK(config.pause_fullscreen);
    put("config.toml", "[pause]\nfullscreen=flase\n");
    CHECK(owe_config_load(&config, path) < 0);
    CHECK(config.pause_fullscreen);
    groups++;
}

int main(int argc, char **argv) {
    CHECK(argc == 2);
    snprintf(root, sizeof(root), "/tmp/owe-regression-XXXXXX");
    CHECK(mkdtemp(root));
    CHECK(setenv("XDG_CACHE_HOME", root, 1) == 0);
    CHECK(setenv("XDG_STATE_HOME", root, 1) == 0);
    directory("bin");
    char binary[PATH_MAX], fake[PATH_MAX], *path = NULL;
    CHECK(realpath(argv[1], binary));
    path_for(fake, sizeof(fake), "bin/ffmpeg");
    CHECK(symlink(binary, fake) == 0);
    CHECK(asprintf(&path, "%s/bin:%s", root, getenv("PATH")) > 0);
    CHECK(setenv("PATH", path, 1) == 0);
    free(path);
    test_json();
    test_ipc();
    test_display_state();
    test_symlink();
    test_jobs();
    test_spawn();
    test_config_errors();
    char *remove[] = {"rm", "-rf", "--", root, NULL}, log[256];
    CHECK(owe_spawn_capture("rm", remove, log, sizeof(log), 2000) == 0);
    printf("%d regression groups passed\n", groups);
    return 0;
}

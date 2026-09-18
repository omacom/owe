#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "strutil.h"

static int failures = 0;

#define CHECK(cond, msg)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);                                    \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

static void test_kind(void) {
    CHECK(owe_kind_from_path("a.mp4") == OWE_KIND_VIDEO, "mp4 is video");
    CHECK(owe_kind_from_path("A.MP4") == OWE_KIND_VIDEO, "MP4 is video");
    CHECK(owe_kind_from_path("a.webm") == OWE_KIND_VIDEO, "webm is video");
    CHECK(owe_kind_from_path("a.mkv") == OWE_KIND_VIDEO, "mkv is video");
    CHECK(owe_kind_from_path("a.mov") == OWE_KIND_VIDEO, "mov is video");
    CHECK(owe_kind_from_path("a.avi") == OWE_KIND_VIDEO, "avi is video");
    CHECK(owe_kind_from_path("a.m4v") == OWE_KIND_VIDEO, "m4v is video");
    CHECK(owe_kind_from_path("a.gif") == OWE_KIND_GIF, "gif is gif");
    CHECK(owe_kind_from_path("a.GIF") == OWE_KIND_GIF, "GIF is gif");
    CHECK(owe_kind_from_path("a.png") == OWE_KIND_STILL, "png is still");
    CHECK(owe_kind_from_path("a.jpg") == OWE_KIND_STILL, "jpg is still");
    CHECK(owe_kind_from_path("a.webp") == OWE_KIND_STILL, "webp is still");
    CHECK(owe_kind_from_path("a.avif") == OWE_KIND_STILL, "avif is still");
    CHECK(owe_kind_from_path("a.txt") == OWE_KIND_UNKNOWN, "txt is unknown");
    CHECK(owe_kind_from_path("") == OWE_KIND_UNKNOWN, "empty is unknown");
    CHECK(owe_kind_from_path(NULL) == OWE_KIND_UNKNOWN, "null is unknown");
    CHECK(strcmp(owe_kind_to_string(OWE_KIND_VIDEO), "video") == 0, "kind video string");
    CHECK(strcmp(owe_kind_to_string(OWE_KIND_GIF), "gif") == 0, "kind gif string");
    CHECK(strcmp(owe_kind_to_string(OWE_KIND_STILL), "still") == 0, "kind still string");
}

static void test_trim(void) {
    char a[] = "  hello  ";
    char b[] = "x";
    char c[] = "   ";
    owe_trim(a);
    owe_trim(b);
    owe_trim(c);
    CHECK(strcmp(a, "hello") == 0, "trim spaces");
    CHECK(strcmp(b, "x") == 0, "trim single");
    CHECK(strcmp(c, "") == 0, "trim all");
}

static void test_config_defaults(void) {
    owe_config_t cfg;
    owe_config_defaults(&cfg);
    CHECK(cfg.pause_fullscreen, "default pause fullscreen");
    CHECK(!cfg.pause_occupied_workspace, "default do not pause occupied");
    CHECK(!cfg.battery_poster, "default no battery poster");
    CHECK(strcmp(cfg.battery_mode, "play") == 0, "default battery mode");
    CHECK(strcmp(cfg.renderer_mode, "lazy") == 0, "default renderer mode");
    CHECK(cfg.gif_fps == 20, "default gif fps");
    CHECK(cfg.gif_crf == 20, "default gif crf");
    CHECK(cfg.cache_max_mb == 512, "default cache budget");
    CHECK(cfg.fade_ms == 250, "default fade");
}

static void test_config_multiline(void) {
    char path[] = "/tmp/owe-config-multiline-XXXXXX";
    int fd = mkstemp(path);
    owe_config_t cfg;
    FILE *f;
    CHECK(fd >= 0, "mkstemp");
    f = fdopen(fd, "w");
    fprintf(f, "[pause]\nblocklist = [\n  \"obs\",\n  'steam',\n]\n"
               "[transcode]\ncache_max_mb = 128\n"
               "[unknown]\nkey = 1\n");
    fclose(f);
    owe_config_defaults(&cfg);
    CHECK(owe_config_load(&cfg, path) == 0, "multiline config loads");
    CHECK(cfg.blocklist_count == 2, "multiline blocklist count");
    CHECK(strcmp(cfg.blocklist[0], "obs") == 0, "multiline blocklist 0");
    CHECK(strcmp(cfg.blocklist[1], "steam") == 0, "multiline blocklist 1");
    CHECK(cfg.cache_max_mb == 128, "cache budget parsed");
    unlink(path);
}

static void test_config_load(void) {
    char path[] = "/tmp/owe-config-test-XXXXXX";
    int fd = mkstemp(path);
    owe_config_t cfg;
    FILE *f;
    CHECK(fd >= 0, "mkstemp");
    f = fdopen(fd, "w");
    fprintf(f, "# comment\n[pause]\nfullscreen = false\noccupied_workspace = no\n"
               "battery_poster = false\nbattery_mode = \"pause\"\n"
               "blocklist = [obs, steam]\n[transcode]\ngif_fps = 15\ngif_crf = 23\n"
               "[render]\nfade_ms = 100\nrenderer_mode = \"always\"\n");
    fclose(f);
    owe_config_defaults(&cfg);
    CHECK(owe_config_load(&cfg, path) == 0, "config load");
    CHECK(!cfg.pause_fullscreen, "parse fullscreen false");
    CHECK(!cfg.pause_occupied_workspace, "parse occupied false");
    CHECK(!cfg.battery_poster, "parse battery poster false");
    CHECK(strcmp(cfg.battery_mode, "pause") == 0, "parse battery mode");
    CHECK(strcmp(cfg.renderer_mode, "always") == 0, "parse renderer mode");
    CHECK(cfg.gif_fps == 15, "parse gif fps");
    CHECK(cfg.gif_crf == 23, "parse gif crf");
    CHECK(cfg.fade_ms == 100, "parse fade");
    CHECK(cfg.blocklist_count == 2, "parse blocklist count");
    CHECK(strcmp(cfg.blocklist[0], "obs") == 0, "parse blocklist 0");
    CHECK(strcmp(cfg.blocklist[1], "steam") == 0, "parse blocklist 1");
    unlink(path);
}

int main(void) {
    test_kind();
    test_trim();
    test_config_defaults();
    test_config_load();
    test_config_multiline();
    if (failures == 0) {
        printf("all unit tests passed\n");
        return 0;
    }
    printf("%d failures\n", failures);
    return 1;
}

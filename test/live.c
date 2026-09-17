/* Run explicitly against a test daemon in an active Wayland session. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include "common_ipc.h"
#include "json.h"
#include "xdg.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
static char daemon_socket[4096], render_socket[4096];

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static yyjson_doc *call(const char *socket, const char *request) {
    char reply[OWE_IPC_MAX_LINE];
    int fd = owe_ipc_connect(socket);
    CHECK(fd >= 0);
    CHECK(owe_ipc_send_line(fd, request) == 0);
    CHECK(owe_ipc_recv_line(fd, reply, sizeof(reply)) == 0);
    close(fd);
    yyjson_doc *doc = yyjson_read(reply, strlen(reply), 0);
    CHECK(doc);
    return doc;
}

static double position(void) {
    yyjson_doc *doc = call(render_socket, "{\"cmd\":\"status\"}");
    double pos = yyjson_get_num(yyjson_obj_get(yyjson_doc_get_root(doc), "time_pos"));
    yyjson_doc_free(doc);
    return pos;
}

static void ok(const char *request) {
    yyjson_doc *doc = call(daemon_socket, request);
    CHECK(yyjson_equals_str(yyjson_obj_get(yyjson_doc_get_root(doc), "status"), "ok"));
    yyjson_doc_free(doc);
}

int main(void) {
    CHECK(owe_socket_path_daemon(daemon_socket, sizeof(daemon_socket)) == 0);
    CHECK(owe_socket_path_render(render_socket, sizeof(render_socket)) == 0);
    yyjson_doc *doc = call(daemon_socket, "{\"cmd\":\"status\"}");
    yyjson_val *state = yyjson_doc_get_root(doc);
    const char *source = yyjson_get_str(yyjson_obj_get(state, "source_path"));
    CHECK(source);
    char *quoted = owe_json_quote(source), *request = NULL;
    CHECK(quoted && asprintf(&request, "{\"cmd\":\"set\",\"path\":%s}", quoted) > 0);
    bool paused = yyjson_get_bool(yyjson_obj_get(state, "paused"));
    bool manual = yyjson_get_bool(yyjson_obj_get(state, "manual_pause"));
    yyjson_doc_free(doc);
    ok(request);
    free(request); free(quoted);
    /* A client that sends after connect must wake the poll loop. */
    int fd = owe_ipc_connect(daemon_socket);
    CHECK(fd >= 0);
    CHECK(send(fd, "{\"cmd\":", 7, 0) == 7);
    usleep(100000);
    double start = now();
    CHECK(send(fd, "\"status\"}\n", 10, 0) == 10);
    char reply[OWE_IPC_MAX_LINE];
    CHECK(owe_ipc_recv_line(fd, reply, sizeof(reply)) == 0 && owe_json_ok(reply));
    double latency = now() - start;
    CHECK(latency < 0.5);
    close(fd);
    printf("Fragmented IPC response: %.2f ms\n", latency * 1000);
    for (int i = 0; i < 20; i++) ok("{\"cmd\":\"status\"}");

    char bad[] = "/tmp/owe-invalid-XXXXXX.png";
    fd = mkstemps(bad, 4);
    CHECK(fd >= 0 && write(fd, "not an image", 12) == 12);
    close(fd);
    CHECK(asprintf(&request, "{\"cmd\":\"load\",\"path\":\"%s\",\"kind\":\"still\"}", bad) > 0);
    doc = call(render_socket, request);
    CHECK(yyjson_equals_str(yyjson_obj_get(yyjson_doc_get_root(doc), "status"), "error"));
    yyjson_doc_free(doc);
    free(request);
    unlink(bad);
    doc = call(render_socket, "{\"cmd\":\"status\"}");
    printf("Renderer state: %s\n", yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(doc), "hwdec")));
    CHECK(yyjson_get_bool(yyjson_obj_get(yyjson_doc_get_root(doc), "has_video")));
    yyjson_doc_free(doc);
    if (!paused) {
        double a = position();
        usleep(400000);
        double b = position();
        CHECK(b != a);
        ok("{\"cmd\":\"pause\"}");
        usleep(200000);
        a = position();
        usleep(250000);
        b = position();
        CHECK(a == b);
        if (!manual) ok("{\"cmd\":\"resume\"}");
    }
    puts("Live IPC, playback, pause, and invalid-image checks passed");
    return 0;
}

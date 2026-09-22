#include "../src/render/wayland.c"
#include <stdio.h>
#include <stdint.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

struct proxy { bool alive; };
static struct proxy proxies[32];
static int proxy_count, windows, surfaces, failure;
static owe_app_t app;

static struct wl_proxy *new_proxy(void) {
    CHECK(proxy_count < 32);
    proxies[proxy_count].alive = true;
    return (struct wl_proxy *)&proxies[proxy_count++];
}

void wl_proxy_destroy(struct wl_proxy *proxy) {
    struct proxy *p = (struct proxy *)proxy;
    CHECK(p->alive);
    p->alive = false;
}
uint32_t wl_proxy_get_version(struct wl_proxy *proxy) { (void)proxy; return 4; }
int wl_proxy_add_listener(struct wl_proxy *proxy, void (**listener)(void), void *data) {
    (void)proxy; (void)listener; (void)data; return 0;
}
struct wl_proxy *wl_proxy_marshal_flags(struct wl_proxy *proxy, uint32_t opcode,
    const struct wl_interface *interface, uint32_t version, uint32_t flags, ...) {
    (void)opcode; (void)version;
    if (flags & WL_MARSHAL_FLAG_DESTROY) wl_proxy_destroy(proxy);
    if (!interface) return NULL;
    if (failure == 1 && interface == &zwlr_layer_surface_v1_interface) return NULL;
    return new_proxy();
}
struct wl_egl_window *wl_egl_window_create(struct wl_surface *surface, int width, int height) {
    (void)surface; (void)width; (void)height;
    if (failure == 2) return NULL;
    windows++;
    return (struct wl_egl_window *)(uintptr_t)1;
}
void wl_egl_window_destroy(struct wl_egl_window *window) { (void)window; windows--; }
void *owe_egl_create_window_surface(struct owe_egl *egl, void *window) {
    (void)egl; (void)window;
    if (failure == 3) return NULL;
    surfaces++;
    return (void *)(uintptr_t)2;
}
void owe_egl_destroy_output(struct owe_egl *egl, struct owe_output *out) {
    (void)egl;
    if (out->egl_surface) { surfaces--; out->egl_surface = NULL; }
}
owe_app_t *owe_app_get(void) { return &app; }
void owe_app_request_render(void) {}
bool owe_still_has_image(struct owe_still *still) { (void)still; return false; }

int main(void) {
    app.egl = (struct owe_egl *)(uintptr_t)3;
    for (failure = 0; failure <= 3; failure++) {
        memset(proxies, 0, sizeof(proxies));
        proxy_count = windows = surfaces = 0;
        struct owe_wayland wl = {0};
        wl.compositor = (struct wl_compositor *)(uintptr_t)4;
        wl.layer_shell = (struct zwlr_layer_shell_v1 *)(uintptr_t)5;
        wl.viewporter = (struct wp_viewporter *)(uintptr_t)6;
        wl.fractional_scale = (struct wp_fractional_scale_manager_v1 *)(uintptr_t)7;
        owe_output_t *out = output_new(&wl, (struct wl_output *)new_proxy(), 1);
        if (failure) {
            CHECK(out == NULL);
        } else {
            CHECK(out && wl.output_count == 1);
            output_free(&wl, out);
        }
        CHECK(wl.output_count == 0 && wl.outputs == NULL && windows == 0 && surfaces == 0);
        for (int i = 0; i < proxy_count; i++) CHECK(!proxies[i].alive);
    }
    puts("Wayland creation failures release every owned proxy");
    failure = 0;
    proxy_count = 0;
    struct owe_wayland wl = {0};
    registry_global(&wl, NULL, 1, "wl_output", 4);
    CHECK(wl.pending_outputs && !wl.output_count);
    registry_global(&wl, NULL, 2, "wl_output", 4);
    registry_global_remove(&wl, NULL, 2);
    registry_global(&wl, NULL, 3, "wl_compositor", 4);
    registry_global(&wl, NULL, 4, "zwlr_layer_shell_v1", 1);
    registry_global(&wl, NULL, 5, "wp_fractional_scale_manager_v1", 1);
    registry_global(&wl, NULL, 6, "wp_viewporter", 1);
    create_pending_outputs(&wl);
    CHECK(wl.output_count == 1 && !wl.pending_outputs);
    CHECK(wl.outputs->fractional && wl.outputs->viewport);
    registry_global(&wl, NULL, 7, "wl_output", 4);
    CHECK(wl.output_count == 2);
    registry_global_remove(&wl, NULL, 1);
    CHECK(wl.output_count == 1 && wl.outputs->registry_name == 7);
    owe_wayland_destroy_outputs(&wl);
    wl_compositor_destroy(wl.compositor);
    zwlr_layer_shell_v1_destroy(wl.layer_shell);
    wp_fractional_scale_manager_v1_destroy(wl.fractional_scale);
    wp_viewporter_destroy(wl.viewporter);
    CHECK(windows == 0 && surfaces == 0);
    for (int i = 0; i < proxy_count; i++) CHECK(!proxies[i].alive);
    puts("Output discovery tolerates registry order and output removal");
}

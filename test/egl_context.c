#include <stdbool.h>
#include <epoxy/gl.h>
#include <EGL/egl.h>
#include <wayland-egl.h>

/* Record whether a window is given its size before or after its surface is
 * first made current. */
static int window_resizes;
static int resized_w;
static int resized_h;
static bool current_before_resize;
static EGLSurface watched_surface;
static EGLBoolean test_make_current(EGLDisplay display, EGLSurface draw, EGLSurface read, EGLContext context) {
    if (draw == watched_surface && draw != EGL_NO_SURFACE && !window_resizes) current_before_resize = true;
    return eglMakeCurrent(display, draw, read, context);
}
static void test_window_resize(struct wl_egl_window *window, int width, int height, int dx, int dy) {
    (void)window;
    (void)dx;
    (void)dy;
    window_resizes++;
    resized_w = width;
    resized_h = height;
}

#define eglMakeCurrent test_make_current
#define wl_egl_window_resize test_window_resize
#include "../src/render/egl.c"
#undef eglMakeCurrent
#undef wl_egl_window_resize
#include <stdio.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

void owe_output_buffer_size(const owe_output_t *out, int *w, int *h) { *w = out->width; *h = out->height; }

/* Desktop OpenGL and OpenGL ES must pass the same checks, because drivers on
 * ARM SoCs can expose only OpenGL ES. */
static void check_api(EGLDisplay display, bool gles) {
    CHECK(eglBindAPI(gles ? EGL_OPENGL_ES_API : EGL_OPENGL_API));
    EGLint attrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE,
                      gles ? EGL_OPENGL_ES3_BIT : EGL_OPENGL_BIT, EGL_NONE};
    EGLint es_attrs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE};
    EGLConfig config;
    EGLint count;
    CHECK(eglChooseConfig(display, attrs, &config, 1, &count) && count == 1);
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, gles ? es_attrs : NULL);
    CHECK(context != EGL_NO_CONTEXT);
    EGLint size[] = {EGL_WIDTH, 2, EGL_HEIGHT, 2, EGL_NONE};
    EGLSurface surface = eglCreatePbufferSurface(display, config, size);
    CHECK(surface != EGL_NO_SURFACE);
    CHECK(eglMakeCurrent(display, surface, surface, context));
    struct owe_egl egl = {.display = display, .context = context, .current = surface, .gles = gles};
    CHECK(owe_egl_make_current(&egl) == 0);
    CHECK(eglGetCurrentSurface(EGL_DRAW) == surface && egl.current == surface);
    owe_output_t output = {.egl_surface = surface};
    owe_egl_destroy_output(&egl, &output);
    CHECK(!output.egl_surface && egl.current == EGL_NO_SURFACE);
    CHECK(eglGetCurrentSurface(EGL_DRAW) == EGL_NO_SURFACE);
    CHECK(eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT));
    CHECK(owe_egl_make_current(&egl) == 0);
    CHECK(eglGetCurrentContext() == context);
    CHECK(compile_shader(GL_FRAGMENT_SHADER, gles, "invalid shader") == 0);
    CHECK(blit_init(&egl.blit, gles) == 0);
    GLint max_size = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_size);
    const uint8_t rgba[] = {255, 0, 0, 255};
    CHECK(owe_egl_tex_from_rgba(&egl, rgba, max_size + 1, 1) == 0);
    GLuint texture = owe_egl_tex_from_rgba(&egl, rgba, 1, 1);
    CHECK(texture && glIsTexture(texture));
    owe_egl_tex_free(&egl, texture);
    /* A context first made current without a surface can keep GL_NONE as the
     * default framebuffer's draw and read buffers, as NVIDIA does. Binding an
     * output must still draw to its back buffer. */
    const GLenum none = GL_NONE;
    if (gles) glDrawBuffers(1, &none);
    else glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    EGLSurface window = eglCreatePbufferSurface(display, config, size);
    CHECK(window != EGL_NO_SURFACE);
    owe_output_t drawn = {.egl_surface = window, .width = 2, .height = 2, .scale = 1};
    CHECK(owe_egl_prepare_output(&egl, &drawn) == 0);
    GLint draw_buffer = 0;
    GLint read_buffer = 0;
    glGetIntegerv(GL_DRAW_BUFFER0, &draw_buffer);
    glGetIntegerv(GL_READ_BUFFER, &read_buffer);
    CHECK(draw_buffer == GL_BACK && read_buffer == GL_BACK);
    /* The blit shader must draw the texture, not only compile. */
    texture = owe_egl_tex_from_rgba(&egl, rgba, 1, 1);
    CHECK(texture);
    owe_egl_draw_texture(&egl, &drawn, texture, 1.0f, 1, 1);
    uint8_t pixel[4] = {0};
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    CHECK(pixel[0] == 255 && pixel[1] == 0 && pixel[2] == 0 && pixel[3] == 255);
    owe_egl_tex_free(&egl, texture);
    CHECK(glGetError() == GL_NO_ERROR);
    owe_egl_destroy_output(&egl, &drawn);
    /* A new output's window must have its size before its surface is first made
     * current. Mesa takes the surface's first buffer at the size the window has
     * then, so a later resize leaves the first frame in the 64x64 the window is
     * created with. */
    EGLSurface fresh = eglCreatePbufferSurface(display, config, size);
    CHECK(fresh != EGL_NO_SURFACE);
    owe_output_t sized = {.egl_surface = fresh, .egl_window = (struct wl_egl_window *)&sized,
                          .egl_w = 64, .egl_h = 64, .width = 1920, .height = 1080, .scale = 1};
    watched_surface = fresh;
    window_resizes = 0;
    current_before_resize = false;
    CHECK(owe_egl_prepare_output(&egl, &sized) == 0);
    CHECK(window_resizes == 1 && resized_w == 1920 && resized_h == 1080);
    CHECK(!current_before_resize);
    CHECK(owe_egl_prepare_output(&egl, &sized) == 0);
    CHECK(window_resizes == 1);
    watched_surface = EGL_NO_SURFACE;
    sized.egl_window = NULL;
    owe_egl_destroy_output(&egl, &sized);
    glDeleteBuffers(1, &egl.blit.vbo);
    glDeleteVertexArrays(1, &egl.blit.vao);
    glDeleteProgram(egl.blit.prog);
    CHECK(eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT));
    CHECK(eglDestroyContext(display, context));
    printf("%s context reuse and output release passed\n", gles ? "OpenGL ES" : "OpenGL");
}

int main(void) {
    EGLDisplay display = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, NULL, NULL)) return 77;
    check_api(display, false);
    check_api(display, true);
    CHECK(eglTerminate(display));
    eglReleaseThread();
    puts("EGL context reuse and output release passed");
}

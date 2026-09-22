#include "../src/render/egl.c"
#include <stdio.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

int main(void) {
    EGLDisplay display = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, NULL, NULL)) return 77;
    CHECK(eglBindAPI(EGL_OPENGL_API));
    EGLint attrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE};
    EGLConfig config;
    EGLint count;
    CHECK(eglChooseConfig(display, attrs, &config, 1, &count) && count == 1);
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, NULL);
    CHECK(context != EGL_NO_CONTEXT);
    EGLint size[] = {EGL_WIDTH, 2, EGL_HEIGHT, 2, EGL_NONE};
    EGLSurface surface = eglCreatePbufferSurface(display, config, size);
    CHECK(surface != EGL_NO_SURFACE);
    CHECK(eglMakeCurrent(display, surface, surface, context));
    struct owe_egl egl = {.display = display, .context = context, .current = surface};
    CHECK(owe_egl_make_current(&egl) == 0);
    CHECK(eglGetCurrentSurface(EGL_DRAW) == surface && egl.current == surface);
    owe_output_t output = {.egl_surface = surface};
    owe_egl_destroy_output(&egl, &output);
    CHECK(!output.egl_surface && egl.current == EGL_NO_SURFACE);
    CHECK(eglGetCurrentSurface(EGL_DRAW) == EGL_NO_SURFACE);
    CHECK(eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT));
    CHECK(owe_egl_make_current(&egl) == 0);
    CHECK(eglGetCurrentContext() == context);
    CHECK(compile_shader(GL_FRAGMENT_SHADER, "invalid shader") == 0);
    CHECK(blit_init(&egl.blit) == 0);
    GLint max_size = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_size);
    const uint8_t rgba[] = {255, 0, 0, 255};
    CHECK(owe_egl_tex_from_rgba(&egl, rgba, max_size + 1, 1) == 0);
    GLuint texture = owe_egl_tex_from_rgba(&egl, rgba, 1, 1);
    CHECK(texture && glIsTexture(texture));
    owe_egl_tex_free(&egl, texture);
    glDeleteBuffers(1, &egl.blit.vbo);
    glDeleteVertexArrays(1, &egl.blit.vao);
    glDeleteProgram(egl.blit.prog);
    CHECK(eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT));
    CHECK(eglDestroyContext(display, context));
    CHECK(eglTerminate(display));
    eglReleaseThread();
    puts("EGL context reuse and output release passed");
}

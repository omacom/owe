#include "egl.h"

#include <stdlib.h>
#include <string.h>

#include <epoxy/gl.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <wayland-egl.h>

#include "log.h"
#include "wayland.h"

struct owe_gl_prog {
    GLuint prog;
    GLint pos_loc;
    GLint uv_loc;
    GLint tex_loc;
    GLint alpha_loc;
    GLint uv_offset_loc;
    GLint uv_scale_loc;
    GLuint vbo;
    GLuint vao;
    int ready;
};

struct owe_egl {
    EGLDisplay display;
    EGLContext context;
    EGLConfig config;
    EGLSurface current;
    struct owe_gl_prog blit;
};

static const char *vs_src = "#version 150\n"
                            "in vec2 pos;\n"
                            "in vec2 uv;\n"
                            "out vec2 vuv;\n"
                            "void main() {\n"
                            "  vuv = uv;\n"
                            "  gl_Position = vec4(pos, 0.0, 1.0);\n"
                            "}\n";

static const char *fs_src = "#version 150\n"
                            "in vec2 vuv;\n"
                            "uniform sampler2D tex;\n"
                            "uniform float alpha;\n"
                            "uniform vec2 uv_offset;\n"
                            "uniform vec2 uv_scale;\n"
                            "out vec4 frag;\n"
                            "void main() {\n"
                            "  vec2 uv = vuv * uv_scale + uv_offset;\n"
                            "  vec4 c = texture(tex, uv);\n"
                            "  float a = c.a * alpha;\n"
                            "  frag = vec4(c.rgb * a, a);\n"
                            "}\n";

static GLuint compile_shader(GLenum type, const char *src) {
    GLuint sh = glCreateShader(type);
    GLint ok = 0;
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        GLsizei n = 0;
        glGetShaderInfoLog(sh, sizeof(log) - 1, &n, log);
        log[n] = '\0';
        OWE_ERROR("shader compile failed: %s", log);
    }
    return sh;
}

static int blit_init(struct owe_gl_prog *b) {
    /* Image row 0 is the top row, which GL treats as v=0. Map v=0 to the top
     * of the quad (y=+1) so stills are not flipped vertically. */
    static const float verts[] = {
        -1.0f, -1.0f, 0.0f, 1.0f, 1.0f, -1.0f, 1.0f, 1.0f,
        -1.0f, 1.0f,  0.0f, 0.0f, 1.0f, 1.0f,  1.0f, 0.0f,
    };
    GLuint vs;
    GLuint fs;
    vs = compile_shader(GL_VERTEX_SHADER, vs_src);
    fs = compile_shader(GL_FRAGMENT_SHADER, fs_src);
    b->prog = glCreateProgram();
    glAttachShader(b->prog, vs);
    glAttachShader(b->prog, fs);
    glBindAttribLocation(b->prog, 0, "pos");
    glBindAttribLocation(b->prog, 1, "uv");
    glLinkProgram(b->prog);
    {
        GLint ok = 0;
        glGetProgramiv(b->prog, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[2048];
            GLsizei n = 0;
            glGetProgramInfoLog(b->prog, sizeof(log) - 1, &n, log);
            log[n] = '\0';
            OWE_ERROR("program link failed: %s", log);
        }
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    b->pos_loc = glGetAttribLocation(b->prog, "pos");
    b->uv_loc = glGetAttribLocation(b->prog, "uv");
    b->tex_loc = glGetUniformLocation(b->prog, "tex");
    b->alpha_loc = glGetUniformLocation(b->prog, "alpha");
    b->uv_offset_loc = glGetUniformLocation(b->prog, "uv_offset");
    b->uv_scale_loc = glGetUniformLocation(b->prog, "uv_scale");
    glGenVertexArrays(1, &b->vao);
    glBindVertexArray(b->vao);
    glGenBuffers(1, &b->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, b->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          (void *)(2 * sizeof(float)));
    b->ready = 1;
    return 0;
}

struct owe_egl *owe_egl_new(struct owe_wayland *wl) {
    struct owe_egl *e;
    struct wl_display *dpy;
    EGLint major = 0;
    EGLint minor = 0;
    EGLint ncfg = 0;
    EGLint cfg_attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE,
    };
    static const struct { int major, minor; } gl_versions[] = {
        { 4, 6 }, { 4, 5 }, { 4, 4 }, { 4, 3 }, { 4, 2 }, { 4, 1 }, { 4, 0 },
        { 3, 3 }, { 3, 2 }, { 3, 1 }, { 3, 0 }, { 0, 0 },
    };
    size_t vi;
    if (!wl) {
        return NULL;
    }
    dpy = owe_wayland_display(wl);
    if (!dpy) {
        return NULL;
    }
    e = calloc(1, sizeof(*e));
    if (!e) {
        return NULL;
    }
    e->display = eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, dpy, NULL);
    if (e->display == EGL_NO_DISPLAY) {
        OWE_ERROR("eglGetDisplay failed");
        free(e);
        return NULL;
    }
    if (!eglInitialize(e->display, &major, &minor)) {
        OWE_ERROR("eglInitialize failed");
        free(e);
        return NULL;
    }
    eglBindAPI(EGL_OPENGL_API);
    if (!eglChooseConfig(e->display, cfg_attribs, &e->config, 1, &ncfg) || ncfg < 1) {
        OWE_ERROR("eglChooseConfig failed");
        eglTerminate(e->display);
        free(e);
        return NULL;
    }
    e->context = EGL_NO_CONTEXT;
    for (vi = 0; gl_versions[vi].major > 0; vi++) {
        EGLint ctx_attribs[] = {
            EGL_CONTEXT_MAJOR_VERSION, gl_versions[vi].major,
            EGL_CONTEXT_MINOR_VERSION, gl_versions[vi].minor,
            EGL_NONE,
        };
        e->context = eglCreateContext(e->display, e->config, EGL_NO_CONTEXT, ctx_attribs);
        if (e->context != EGL_NO_CONTEXT) {
            break;
        }
    }
    if (e->context == EGL_NO_CONTEXT) {
        OWE_ERROR("eglCreateContext failed");
        eglTerminate(e->display);
        free(e);
        return NULL;
    }
    if (!eglMakeCurrent(e->display, EGL_NO_SURFACE, EGL_NO_SURFACE, e->context)) {
        OWE_ERROR("eglMakeCurrent failed");
        eglDestroyContext(e->display, e->context);
        eglTerminate(e->display);
        free(e);
        return NULL;
    }
    OWE_INFO("egl ready version %d.%d", (int)major, (int)minor);
    return e;
}

void owe_egl_free(struct owe_egl *egl) {
    if (!egl) {
        return;
    }
    owe_egl_make_current(egl);
    if (egl->blit.ready) {
        glDeleteBuffers(1, &egl->blit.vbo);
        glDeleteVertexArrays(1, &egl->blit.vao);
        glDeleteProgram(egl->blit.prog);
    }
    /* EGL defers deletion while a context remains current on a thread. */
    eglMakeCurrent(egl->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (egl->context != EGL_NO_CONTEXT) {
        eglDestroyContext(egl->display, egl->context);
    }
    if (egl->display != EGL_NO_DISPLAY) {
        eglTerminate(egl->display);
    }
    eglReleaseThread();
    free(egl);
}

int owe_egl_make_current(struct owe_egl *egl) {
    if (!egl) {
        return -1;
    }
    if (eglMakeCurrent(egl->display, EGL_NO_SURFACE, EGL_NO_SURFACE, egl->context)) {
        egl->current = EGL_NO_SURFACE;
        return 0;
    }
    return -1;
}

void *owe_egl_display(struct owe_egl *egl) {
    return egl ? egl->display : NULL;
}

void *owe_egl_context(struct owe_egl *egl) {
    return egl ? egl->context : NULL;
}

void *owe_egl_config(struct owe_egl *egl) {
    return egl ? egl->config : NULL;
}

void *owe_egl_create_window_surface(struct owe_egl *egl, void *egl_window) {
    EGLSurface s;
    if (!egl || !egl_window) {
        return NULL;
    }
    s = eglCreateWindowSurface(egl->display, egl->config, (EGLNativeWindowType)egl_window, NULL);
    if (s == EGL_NO_SURFACE) {
        OWE_ERROR("eglCreateWindowSurface failed: 0x%x", eglGetError());
        return NULL;
    }
    return s;
}

void owe_egl_destroy_output(struct owe_egl *egl, struct owe_output *out) {
    if (!egl || !out) {
        return;
    }
    if (out->egl_surface) {
        owe_egl_make_current(egl);
        eglDestroySurface(egl->display, (EGLSurface)out->egl_surface);
        out->egl_surface = NULL;
    }
}

int owe_egl_prepare_output(struct owe_egl *egl, struct owe_output *out) {
    int w;
    int h;
    if (!egl || !out || !out->egl_surface) {
        return -1;
    }
    if (egl->current != (EGLSurface)out->egl_surface) {
        if (!eglMakeCurrent(egl->display, (EGLSurface)out->egl_surface,
                            (EGLSurface)out->egl_surface, egl->context)) {
            OWE_ERROR("eglMakeCurrent failed: 0x%x", eglGetError());
            return -1;
        }
        egl->current = (EGLSurface)out->egl_surface;
    }
    if (!egl->blit.ready) {
        blit_init(&egl->blit);
    }
    owe_output_buffer_size(out, &w, &h);
    if (out->egl_window && (out->egl_w != w || out->egl_h != h)) {
        wl_egl_window_resize((struct wl_egl_window *)out->egl_window, w, h, 0, 0);
        out->egl_w = w;
        out->egl_h = h;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, w, h);
    return 0;
}

void owe_egl_swap_output(struct owe_egl *egl, struct owe_output *out) {
    if (!egl || !out || !out->egl_surface) {
        return;
    }
    if (!eglSwapBuffers(egl->display, (EGLSurface)out->egl_surface)) {
        OWE_ERROR("eglSwapBuffers failed: 0x%x", eglGetError());
    }
}

void owe_egl_clear_output(struct owe_egl *egl, struct owe_output *out, float r, float g, float b,
                          float a) {
    if (owe_egl_prepare_output(egl, out) != 0) {
        return;
    }
    glClearColor(r, g, b, a);
    glClear(GL_COLOR_BUFFER_BIT);
}

unsigned int owe_egl_tex_from_rgba(struct owe_egl *egl, const uint8_t *rgba, int w, int h) {
    GLuint tex = 0;
    if (!egl || !rgba || w <= 0 || h <= 0) {
        return 0;
    }
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return tex;
}

void owe_egl_tex_free(struct owe_egl *egl, unsigned int tex) {
    GLuint t = tex;
    (void)egl;
    if (t) {
        glDeleteTextures(1, &t);
    }
}

void owe_egl_draw_texture(struct owe_egl *egl, struct owe_output *out, unsigned int tex,
                          float alpha, int tex_w, int tex_h) {
    struct owe_gl_prog *b;
    int out_w;
    int out_h;
    float src_aspect;
    float dst_aspect;
    float scale_u = 1.0f;
    float scale_v = 1.0f;
    float off_u = 0.0f;
    float off_v = 0.0f;
    if (!egl || !out || !tex) {
        return;
    }
    if (owe_egl_prepare_output(egl, out) != 0) {
        return;
    }
    owe_output_buffer_size(out, &out_w, &out_h);
    if (tex_w > 0 && tex_h > 0 && out_w > 0 && out_h > 0) {
        src_aspect = (float)tex_w / (float)tex_h;
        dst_aspect = (float)out_w / (float)out_h;
        if (dst_aspect > src_aspect) {
            scale_v = src_aspect / dst_aspect;
        } else {
            scale_u = dst_aspect / src_aspect;
        }
        off_u = (1.0f - scale_u) * 0.5f;
        off_v = (1.0f - scale_v) * 0.5f;
    }
    b = &egl->blit;
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glUseProgram(b->prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glUniform1i(b->tex_loc, 0);
    glUniform1f(b->alpha_loc, alpha);
    glUniform2f(b->uv_offset_loc, off_u, off_v);
    glUniform2f(b->uv_scale_loc, scale_u, scale_v);
    glBindVertexArray(b->vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    glUseProgram(0);
}

#include "../src/render/egl.c"
#include "../src/render/scene_effects.h"
#include <stdio.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

void owe_output_buffer_size(const owe_output_t *out, int *w, int *h) { *w = out->width; *h = out->height; }

enum { SIDE = 64, CUT = 16 };

static uint8_t still_px[SIDE * SIDE * 4];
static uint8_t screen[SIDE * SIDE * 4];

/* The pixel `x` across and `y` down from the top left of what was drawn. */
static const uint8_t *seen(int x, int y) {
    return screen + ((SIDE - 1 - y) * SIDE + x) * 4;
}

static bool is(const uint8_t *px, int r, int g, int b) {
    return abs(px[0] - r) <= 1 && abs(px[1] - g) <= 1 && abs(px[2] - b) <= 1;
}

static void read_screen(void) {
    glReadPixels(0, 0, SIDE, SIDE, GL_RGBA, GL_UNSIGNED_BYTE, screen);
}

/* Desktop OpenGL and OpenGL ES must pass the same checks, as for the rest of the renderer. */
static void check_api(EGLDisplay display, bool gles) {
    CHECK(eglBindAPI(gles ? EGL_OPENGL_ES_API : EGL_OPENGL_API));
    EGLint attrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE,
                      gles ? EGL_OPENGL_ES3_BIT : EGL_OPENGL_BIT, EGL_RED_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLint es_attrs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE};
    EGLConfig config;
    EGLint count;
    CHECK(eglChooseConfig(display, attrs, &config, 1, &count) && count == 1);
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, gles ? es_attrs : NULL);
    CHECK(context != EGL_NO_CONTEXT);
    EGLint size[] = {EGL_WIDTH, SIDE, EGL_HEIGHT, SIDE, EGL_NONE};
    EGLSurface surface = eglCreatePbufferSurface(display, config, size);
    CHECK(surface != EGL_NO_SURFACE);
    CHECK(eglMakeCurrent(display, surface, surface, context));
    struct owe_egl egl = {.display = display, .context = context, .current = EGL_NO_SURFACE, .gles = gles};
    owe_output_t out = {.egl_surface = surface, .width = SIDE, .height = SIDE, .scale = 1};

    /* A still: a plain ground with a red square on it, and a green mark inside the square. */
    static uint8_t shape_px[CUT * CUT * 4], behind_px[CUT * CUT * 4], own_px[CUT * CUT * 4], left_px[CUT * CUT * 4];
    for (int y = 0; y < SIDE; y++) for (int x = 0; x < SIDE; x++) {
        uint8_t *p = still_px + (y * SIDE + x) * 4;
        bool square = x >= 20 && x < 28 && y >= 30 && y < 38;
        bool mark = x == 22 && y == 32;
        p[0] = mark ? 0 : square ? 200 : 40;
        p[1] = mark ? 250 : square ? 10 : 60;
        p[2] = square || mark ? 10 : 80;
        p[3] = 255;
    }
    /* The square as a piece: its shape in a cut 16 pixels wide, and the ground that is behind it. */
    const float at[4] = {16.0f / SIDE, 26.0f / SIDE, (float)CUT / SIDE, (float)CUT / SIDE};
    for (int y = 0; y < CUT; y++) for (int x = 0; x < CUT; x++) {
        bool square = x >= 4 && x < 12 && y >= 4 && y < 12;
        uint8_t *s = shape_px + (y * CUT + x) * 4, *b = behind_px + (y * CUT + x) * 4;
        uint8_t *o = own_px + (y * CUT + x) * 4, *l = left_px + (y * CUT + x) * 4;
        s[0] = s[1] = s[2] = 255; s[3] = square ? 255 : 0;
        b[0] = 40; b[1] = 60; b[2] = 80; b[3] = square ? 255 : 0;
        o[0] = 10; o[1] = 20; o[2] = 240; o[3] = square ? 255 : 0;
        l[0] = l[1] = l[2] = 255; l[3] = x < 8 ? 255 : 0;
    }
    GLuint still = owe_egl_tex_from_rgba(&egl, still_px, SIDE, SIDE);
    GLuint shape = owe_egl_tex_from_rgba(&egl, shape_px, CUT, CUT);
    GLuint behind = owe_egl_tex_from_rgba(&egl, behind_px, CUT, CUT);
    GLuint own = owe_egl_tex_from_rgba(&egl, own_px, CUT, CUT);
    GLuint left = owe_egl_tex_from_rgba(&egl, left_px, CUT, CUT);
    CHECK(still && shape && behind && own && left);
    /* Texels land on pixels one for one here; nearest keeps the comparison exact. */
    GLuint all[] = {still, shape, behind, own, left};
    for (size_t i = 0; i < sizeof(all) / sizeof(*all); i++) {
        glBindTexture(GL_TEXTURE_2D, all[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    struct owe_piece_draw patch = {.shape = behind, .still_w = SIDE, .still_h = SIDE, .at = at, .size = 1, .fade = 1};
    struct owe_piece_draw piece = {.shape = shape, .still = still, .behind = behind, .still_w = SIDE, .still_h = SIDE,
                                   .at = at, .size = 1, .fade = 1};

    /* At home over its patch, the piece is the still again, to the pixel. */
    owe_egl_draw_texture(&egl, &out, still, 1.0f, SIDE, SIDE);
    owe_egl_draw_piece(&egl, &out, &patch);
    owe_egl_draw_piece(&egl, &out, &piece);
    read_screen();
    for (int y = 0; y < SIDE; y++) for (int x = 0; x < SIDE; x++) {
        const uint8_t *want = still_px + (y * SIDE + x) * 4;
        CHECK(is(seen(x, y), want[0], want[1], want[2]));
    }

    /* Moved four pixels across, it takes its colours with it and leaves the ground behind. */
    struct owe_piece_draw moved = piece;
    moved.dx = 4.0f / SIDE;
    owe_egl_draw_texture(&egl, &out, still, 1.0f, SIDE, SIDE);
    owe_egl_draw_piece(&egl, &out, &patch);
    owe_egl_draw_piece(&egl, &out, &moved);
    read_screen();
    CHECK(is(seen(21, 33), 40, 60, 80));   /* where it was */
    CHECK(is(seen(30, 33), 200, 10, 10));  /* where it is, over the ground */
    CHECK(is(seen(25, 33), 200, 10, 10));  /* where it is, over its own old place */
    CHECK(is(seen(26, 32), 0, 250, 10));   /* its mark came along */
    CHECK(is(seen(22, 32), 40, 60, 80));   /* and is not left where it was */
    CHECK(is(seen(32, 33), 40, 60, 80));   /* just past it */

    /* Twice the size, about its middle. */
    struct owe_piece_draw large = piece;
    large.size = 2.0f;
    owe_egl_draw_texture(&egl, &out, still, 1.0f, SIDE, SIDE);
    owe_egl_draw_piece(&egl, &out, &patch);
    owe_egl_draw_piece(&egl, &out, &large);
    read_screen();
    CHECK(is(seen(17, 27), 200, 10, 10) && is(seen(30, 40), 200, 10, 10));
    CHECK(is(seen(15, 27), 40, 60, 80) && is(seen(32, 40), 40, 60, 80));

    /* Half faded, it is halfway between itself and what is behind it. */
    struct owe_piece_draw faint = piece;
    faint.fade = 0.5f;
    owe_egl_draw_texture(&egl, &out, still, 1.0f, SIDE, SIDE);
    owe_egl_draw_piece(&egl, &out, &patch);
    owe_egl_draw_piece(&egl, &out, &faint);
    read_screen();
    CHECK(abs(seen(24, 34)[0] - 120) <= 2 && abs(seen(24, 34)[1] - 35) <= 2);

    /* Kept inside a shape, only the part of it in the shape shows. */
    struct owe_piece_draw kept = piece;
    kept.inside = left;
    kept.inside_at = at;
    owe_egl_draw_texture(&egl, &out, still, 1.0f, SIDE, SIDE);
    owe_egl_draw_piece(&egl, &out, &patch);
    owe_egl_draw_piece(&egl, &out, &kept);
    read_screen();
    CHECK(is(seen(21, 33), 200, 10, 10) && is(seen(26, 33), 40, 60, 80));

    /* A piece with a colour of its own shows that colour, not the still's. */
    struct owe_piece_draw painted = {.shape = own, .still_w = SIDE, .still_h = SIDE, .at = at, .size = 1, .fade = 1,
                                     .dy = -10.0f / SIDE};
    owe_egl_draw_texture(&egl, &out, still, 1.0f, SIDE, SIDE);
    owe_egl_draw_piece(&egl, &out, &painted);
    read_screen();
    CHECK(is(seen(24, 24), 10, 20, 240) && is(seen(24, 34), 200, 10, 10));

    /* Nothing is drawn of a piece with no size or wholly faded, and nothing beyond its reach. */
    struct owe_piece_draw gone = piece;
    gone.size = 0.0f;
    owe_egl_draw_texture(&egl, &out, still, 1.0f, SIDE, SIDE);
    owe_egl_draw_piece(&egl, &out, &gone);
    gone.size = 1.0f;
    gone.fade = 0.0f;
    owe_egl_draw_piece(&egl, &out, &gone);
    read_screen();
    CHECK(is(seen(24, 34), 200, 10, 10) && is(seen(2, 2), 40, 60, 80));
    CHECK(glGetError() == GL_NO_ERROR);

    /* Every effect OWE has compiles under both, and takes the values it says it takes. */
    const struct owe_scene_builtin *builtin;
    char said[512];
    for (int i = 0; (builtin = owe_scene_builtin_at(i)); i++) {
        struct owe_effect *fx = owe_egl_effect_new(&egl, builtin->source, said, sizeof(said));
        if (!fx) fprintf(stderr, "%s: %s\n", builtin->name, said);
        CHECK(fx);
        for (int k = 0; k < builtin->value_count; k++) {
            CHECK(owe_egl_effect_value(fx, builtin->values[k].name) == builtin->values[k].count);
        }
        owe_egl_effect_free(fx);
    }
    /* An effect redraws its area by the area's shape: the left half of the piece's place here, at half strength. */
    struct owe_effect *fill = owe_egl_effect_new(&egl, "uniform vec3 colour;\nvec4 effect(vec2 uv) { return vec4(colour, 1.0); }\n", said, sizeof(said));
    CHECK(fill && owe_egl_effect_value(fill, "colour") == 3 && owe_egl_effect_value(fill, "color") == 0);
    const float blue[] = {0, 0, 1};
    struct owe_effect_value values[] = {{"colour", blue, 3}};
    struct owe_effect_draw redraw = {.still = still, .mask = left, .still_w = SIDE, .still_h = SIDE, .at = at, .strength = 0.5f,
                                     .seconds = 1, .values = values, .value_count = 1};
    owe_egl_draw_texture(&egl, &out, still, 1.0f, SIDE, SIDE);
    owe_egl_draw_effect(&egl, &out, fill, &redraw);
    read_screen();
    CHECK(abs(seen(18, 28)[0] - 20) <= 1 && abs(seen(18, 28)[1] - 30) <= 1 && abs(seen(18, 28)[2] - 168) <= 1);
    CHECK(is(seen(26, 28), 40, 60, 80) && is(seen(10, 28), 40, 60, 80));
    /* With no strength it draws nothing, and a shader that is wrong is refused with the compiler's words. */
    redraw.strength = 0.0f;
    owe_egl_draw_texture(&egl, &out, still, 1.0f, SIDE, SIDE);
    owe_egl_draw_effect(&egl, &out, fill, &redraw);
    read_screen();
    CHECK(is(seen(18, 28), 40, 60, 80));
    owe_egl_effect_free(fill);
    CHECK(!owe_egl_effect_new(&egl, "vec4 effect(vec2 uv) { return 1; }\n", said, sizeof(said)) && *said);
    CHECK(glGetError() == GL_NO_ERROR);

    for (size_t i = 0; i < sizeof(all) / sizeof(*all); i++) owe_egl_tex_free(&egl, all[i]);
    owe_egl_destroy_output(&egl, &out);
    glDeleteBuffers(1, &egl.blit.vbo);
    glDeleteVertexArrays(1, &egl.blit.vao);
    glDeleteProgram(egl.blit.prog);
    glDeleteProgram(egl.piece.prog);
    CHECK(eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT));
    CHECK(eglDestroyContext(display, context));
    printf("%s scene pieces passed\n", gles ? "OpenGL ES" : "OpenGL");
}

int main(void) {
    EGLDisplay display = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, NULL, NULL)) return 77;
    check_api(display, false);
    check_api(display, true);
    CHECK(eglTerminate(display));
    eglReleaseThread();
    puts("scene pieces passed");
}

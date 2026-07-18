/* Unit tests for the Phase 4 OpenGL compatibility entry points. */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <GL/gl.h>

#include "fbdev.h"
#include "l10gl.h"

#define EPSILON 1.0e-5f

struct capture_state {
    struct l10gl_vertex triangle[3];
    int triangles;
    int color_clears;
    int depth_clears;
    int waits;
    int swaps;
};

static struct capture_state capture;
static int failures;

static void expect_int(const char *label, int actual, int expected)
{
    if (actual == expected)
        return;
    fprintf(stderr, "test-gl: %s is %d, expected %d\n",
            label, actual, expected);
    failures++;
}

static void expect_float(const char *label, float actual, float expected)
{
    if (fabsf(actual - expected) <= EPSILON)
        return;
    fprintf(stderr, "test-gl: %s is %.9g, expected %.9g\n",
            label, actual, expected);
    failures++;
}

static int capture_init(struct l10gl_ctx *ctx, int width, int height, int bpp)
{
    l10gl_mode_set_linear(ctx, width, height, bpp * 8,
                          (uint32_t)width * (uint32_t)bpp, NULL);
    return 0;
}

static void capture_triangle(struct l10gl_ctx *ctx,
                             struct l10gl_vertex v0,
                             struct l10gl_vertex v1,
                             struct l10gl_vertex v2)
{
    (void)ctx;
    capture.triangle[0] = v0;
    capture.triangle[1] = v1;
    capture.triangle[2] = v2;
    capture.triangles++;
}

static void capture_clear_color(struct l10gl_ctx *ctx,
                                float red, float green, float blue)
{
    (void)ctx;
    expect_float("clear callback red", red, 1.0f);
    expect_float("clear callback green", green, 0.0f);
    expect_float("clear callback blue", blue, 0.25f);
    capture.color_clears++;
}

static void capture_clear_depth(struct l10gl_ctx *ctx, float depth)
{
    (void)ctx;
    expect_float("clear callback depth", depth, 0.0f);
    capture.depth_clears++;
}

static void capture_wait(struct l10gl_ctx *ctx)
{
    (void)ctx;
    capture.waits++;
}

static void capture_swap(struct l10gl_ctx *ctx)
{
    (void)ctx;
    capture.swaps++;
}

static const struct l10gl_backend capture_backend = {
    .name = "gl-capture",
    .init = capture_init,
    .clear_color = capture_clear_color,
    .clear_depth = capture_clear_depth,
    .draw_triangle = capture_triangle,
    .wait_engine = capture_wait,
    .swap_buffers = capture_swap,
    .caps = L10GL_CAP_GOURAUD | L10GL_CAP_ZBUFFER,
};

static void test_no_context(void)
{
    glVertex3f(0, 0, 0);
    expect_int("no-context error", glGetError(), GL_INVALID_OPERATION);
    expect_int("error latch clears", glGetError(), GL_NO_ERROR);
}

static void test_immediate_and_matrices(struct l10gl_ctx *ctx)
{
    memset(&capture, 0, sizeof(capture));
    glViewport(0, 0, 100, 80);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslatef(0.25f, 0, 0);

    glBegin(GL_TRIANGLES);
    glColor4f(0.1f, 0.2f, 0.3f, 0.4f);
    glNormal3f(0, 0, 1);
    glTexCoord2f(0.6f, 0.7f);
    glVertex3f(-0.5f, -0.5f, 0);
    glVertex3f(0.5f, -0.5f, 0);
    glVertex2f(0, 0.5f);
    glEnd();

    expect_int("one GL triangle", capture.triangles, 1);
    expect_float("translated left X", capture.triangle[0].x, 37.5f);
    expect_float("right X", capture.triangle[1].x, 87.5f);
    expect_float("top Y", capture.triangle[2].y, 20.0f);
    expect_float("captured red", capture.triangle[0].r, 0.1f);
    expect_float("captured alpha", capture.triangle[1].a, 0.4f);
    expect_float("captured U", capture.triangle[2].u, 0.6f);
    expect_int("immediate-mode error", glGetError(), GL_NO_ERROR);

    glBegin(GL_QUADS);
    expect_int("unsupported primitive", glGetError(), GL_INVALID_ENUM);
    glEnd();
    expect_int("end after rejected begin", glGetError(), GL_INVALID_OPERATION);

    glMatrixMode(0xdead);
    expect_int("bad matrix target", glGetError(), GL_INVALID_ENUM);

    (void)ctx;
}

static void test_state(struct l10gl_ctx *ctx)
{
    glEnable(GL_DEPTH_TEST);
    expect_int("depth enabled", ctx->depth_test_enabled, 1);
    glDisable(GL_DEPTH_TEST);
    expect_int("depth disabled", ctx->depth_test_enabled, 0);

    glDepthFunc(GL_GEQUAL);
    expect_int("depth function", ctx->depth_func_val, L10GL_GEQUAL);
    glDepthMask(GL_FALSE);
    expect_int("depth writes disabled", ctx->depth_writes_enabled, 0);

    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    expect_int("source blend factor", ctx->blend_sfactor, L10GL_SRC_ALPHA);
    expect_int("destination blend factor", ctx->blend_dfactor,
               L10GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_BLEND);
    expect_int("blend enabled", ctx->blend_enabled, 1);

    glCullFace(GL_FRONT);
    glEnable(GL_CULL_FACE);
    expect_int("front culling", ctx->cull_mode_val, L10GL_CULL_FRONT);
    glDisable(GL_CULL_FACE);
    expect_int("culling disabled", ctx->cull_mode_val, L10GL_CULL_NONE);

    glEnable(GL_LIGHTING);
    expect_int("light0 still gates lighting", ctx->lighting_enabled, 0);
    glEnable(GL_LIGHT0);
    expect_int("lighting enabled", ctx->lighting_enabled, 1);
    glDisable(GL_LIGHT0);
    expect_int("lighting disabled with light0", ctx->lighting_enabled, 0);

    glEnable(0xbeef);
    expect_int("bad capability", glGetError(), GL_INVALID_ENUM);
    glDepthFunc(0xbeef);
    glBlendFunc(GL_SRC_ALPHA, 0xbeef);
    expect_int("first error remains latched", glGetError(), GL_INVALID_ENUM);
    expect_int("second error consumed with latch", glGetError(), GL_NO_ERROR);
}

static void test_clear_and_sync(struct l10gl_ctx *ctx)
{
    memset(&capture, 0, sizeof(capture));
    glClearColor(2.0f, -1.0f, 0.25f, 0.5f);
    glClearDepth(-4.0);
    expect_float("clamped clear red", ctx->clear_r, 1.0f);
    expect_float("clamped clear green", ctx->clear_g, 0.0f);
    expect_float("clamped clear depth", ctx->clear_z, 0.0f);

    glClear(GL_COLOR_BUFFER_BIT);
    expect_int("color-only clear", capture.color_clears, 1);
    expect_int("no depth clear", capture.depth_clears, 0);
    glClear(GL_DEPTH_BUFFER_BIT);
    expect_int("depth-only clear", capture.depth_clears, 1);

    glFinish();
    l10glSwapBuffers();
    expect_int("finish waits", capture.waits, 1);
    expect_int("swap dispatch", capture.swaps, 1);
}

static void test_stack_errors(void)
{
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glPopMatrix();
    expect_int("projection underflow", glGetError(), GL_STACK_UNDERFLOW);
    glPushMatrix();
    glPushMatrix();
    glPushMatrix();
    glPushMatrix();
    expect_int("projection overflow", glGetError(), GL_STACK_OVERFLOW);
}

int main(void)
{
    struct l10gl_ctx ctx;

    test_no_context();
    if (l10gl_create(&ctx, &capture_backend, 100, 80, 4) != 0) {
        fprintf(stderr, "test-gl: failed to create capture context\n");
        return 1;
    }
    l10glMakeCurrent(&ctx);
    expect_int("current context installed",
               l10glGetCurrentContext() == &ctx, 1);

    test_immediate_and_matrices(&ctx);
    test_state(&ctx);
    test_clear_and_sync(&ctx);
    test_stack_errors();

    l10glMakeCurrent(NULL);
    l10gl_destroy(&ctx);

    if (failures) {
        fprintf(stderr, "test-gl: %d failure(s)\n", failures);
        return 1;
    }
    printf("test-gl: all tests passed\n");
    return 0;
}

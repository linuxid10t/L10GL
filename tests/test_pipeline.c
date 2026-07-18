/* Capture-backend regression tests for X2 immediate primitive assembly. */

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "l10gl.h"

#define MAX_CAPTURED 16
#define EPSILON 1.0e-5f

struct captured_triangle {
    struct l10gl_vertex v[3];
    int textured;
};

struct captured_line {
    struct l10gl_vertex v[2];
};

struct capture_state {
    struct captured_triangle triangles[MAX_CAPTURED];
    struct captured_line lines[MAX_CAPTURED];
    int triangle_count;
    int line_count;
};

static struct capture_state capture;
static int failures;

static void reset_capture(void)
{
    memset(&capture, 0, sizeof(capture));
}

static void expect_int(const char *label, int actual, int expected)
{
    if (actual == expected)
        return;
    fprintf(stderr, "test-pipeline: %s is %d, expected %d\n",
            label, actual, expected);
    failures++;
}

static void expect_float(const char *label, float actual, float expected)
{
    if (fabsf(actual - expected) <= EPSILON)
        return;
    fprintf(stderr, "test-pipeline: %s is %.9g, expected %.9g\n",
            label, actual, expected);
    failures++;
}

static int capture_init(struct l10gl_ctx *ctx, int width, int height, int bpp)
{
    (void)bpp;
    ctx->width = width;
    ctx->height = height;
    ctx->backend_data = &capture;
    return 0;
}

static void capture_cleanup(struct l10gl_ctx *ctx)
{
    ctx->backend_data = NULL;
}

static void capture_triangle(struct l10gl_ctx *ctx,
                             struct l10gl_vertex v0,
                             struct l10gl_vertex v1,
                             struct l10gl_vertex v2)
{
    struct captured_triangle *triangle;
    (void)ctx;

    if (capture.triangle_count >= MAX_CAPTURED)
        return;
    triangle = &capture.triangles[capture.triangle_count++];
    triangle->v[0] = v0;
    triangle->v[1] = v1;
    triangle->v[2] = v2;
    triangle->textured = 0;
}

static void capture_textured_triangle(struct l10gl_ctx *ctx,
                                      struct l10gl_vertex v0,
                                      struct l10gl_vertex v1,
                                      struct l10gl_vertex v2)
{
    int before = capture.triangle_count;

    capture_triangle(ctx, v0, v1, v2);
    if (capture.triangle_count > before)
        capture.triangles[capture.triangle_count - 1].textured = 1;
}

static void capture_line(struct l10gl_ctx *ctx,
                         struct l10gl_vertex v0,
                         struct l10gl_vertex v1)
{
    struct captured_line *line;
    (void)ctx;

    if (capture.line_count >= MAX_CAPTURED)
        return;
    line = &capture.lines[capture.line_count++];
    line->v[0] = v0;
    line->v[1] = v1;
}

static const struct l10gl_backend capture_backend = {
    .name = "capture",
    .init = capture_init,
    .cleanup = capture_cleanup,
    .draw_triangle = capture_triangle,
    .draw_textured_triangle = capture_textured_triangle,
    .draw_line = capture_line,
    .caps = L10GL_CAP_GOURAUD | L10GL_CAP_LINES | L10GL_CAP_TEXTURE,
};

static void submit_colored_vertex(struct l10gl_ctx *ctx, float x, float y,
                                  float z, float id)
{
    l10gl_color4f(ctx, id, id + .01f, id + .02f, id + .03f);
    l10gl_texcoord2f(ctx, id + .04f, id + .05f);
    expect_int("vertex submission", l10gl_vertex3f(ctx, x, y, z), 0);
}

static void test_errors_and_defaults(struct l10gl_ctx *ctx)
{
    expect_float("default red", ctx->current_r, 1);
    expect_float("default green", ctx->current_g, 1);
    expect_float("default blue", ctx->current_b, 1);
    expect_float("default alpha", ctx->current_a, 1);
    expect_float("default normal X", ctx->current_nx, 0);
    expect_float("default normal Y", ctx->current_ny, 0);
    expect_float("default normal Z", ctx->current_nz, 1);
    expect_int("default culling", ctx->cull_mode_val, L10GL_CULL_NONE);

    expect_int("vertex outside begin", l10gl_vertex3f(ctx, 0, 0, 0), -EPERM);
    expect_int("end outside begin", l10gl_end(ctx), -EPERM);
    expect_int("points unsupported", l10gl_begin(ctx, L10GL_POINTS), -ENOTSUP);
    expect_int("invalid cull mode",
               l10gl_cull_face(ctx, (enum l10gl_cull_mode)99), -EINVAL);

    reset_capture();
    expect_int("begin triangles", l10gl_begin(ctx, L10GL_TRIANGLES), 0);
    expect_int("nested begin", l10gl_begin(ctx, L10GL_LINES), -EBUSY);
    l10gl_vertex3f(ctx, 0, 0, 0);
    l10gl_vertex3f(ctx, .5f, 0, 0);
    expect_int("end incomplete triangle", l10gl_end(ctx), 0);
    expect_int("incomplete triangle ignored", capture.triangle_count, 0);

    l10gl_normal3f(ctx, 2, 3, 4);
    expect_float("current normal X", ctx->current_nx, 2);
    expect_float("current normal Y", ctx->current_ny, 3);
    expect_float("current normal Z", ctx->current_nz, 4);
}

static void test_triangle_transform_and_attributes(struct l10gl_ctx *ctx)
{
    const struct captured_triangle *triangle;

    reset_capture();
    l10gl_matrix_mode(ctx, L10GL_MATRIX_MODELVIEW);
    l10gl_load_identity(ctx);
    l10gl_matrix_mode(ctx, L10GL_MATRIX_PROJECTION);
    l10gl_load_identity(ctx);
    l10gl_viewport(ctx, 0, 0, 100, 80);
    l10gl_depth_range(ctx, 0, 1);
    l10gl_cull_face(ctx, L10GL_CULL_NONE);
    l10gl_bind_texture(ctx, NULL);

    l10gl_begin(ctx, L10GL_TRIANGLES);
    submit_colored_vertex(ctx, -.5f, -.5f, 0, .1f);
    submit_colored_vertex(ctx,  .5f, -.5f, 0, .2f);
    submit_colored_vertex(ctx,  0.0f, .5f, 0, .3f);
    l10gl_end(ctx);

    expect_int("one transformed triangle", capture.triangle_count, 1);
    triangle = &capture.triangles[0];
    expect_int("plain triangle dispatch", triangle->textured, 0);
    expect_float("v0 screen x", triangle->v[0].x, 25);
    expect_float("v0 screen y", triangle->v[0].y, 60);
    expect_float("v1 screen x", triangle->v[1].x, 75);
    expect_float("v1 screen y", triangle->v[1].y, 60);
    expect_float("v2 screen x", triangle->v[2].x, 50);
    expect_float("v2 screen y", triangle->v[2].y, 20);
    expect_float("window depth", triangle->v[0].z, .5f);
    expect_float("X2 affine W", triangle->v[0].w, 1);
    expect_float("captured v0 red", triangle->v[0].r, .1f);
    expect_float("captured v1 green", triangle->v[1].g, .21f);
    expect_float("captured v2 alpha", triangle->v[2].a, .33f);
    expect_float("captured v0 U", triangle->v[0].u, .14f);
    expect_float("captured v2 V", triangle->v[2].v, .35f);
}

static void test_textured_dispatch(struct l10gl_ctx *ctx)
{
    struct l10gl_texture texture = { .width = 1, .height = 1 };

    reset_capture();
    l10gl_bind_texture(ctx, &texture);
    l10gl_begin(ctx, L10GL_TRIANGLES);
    submit_colored_vertex(ctx, -.5f, -.5f, 0, .1f);
    submit_colored_vertex(ctx,  .5f, -.5f, 0, .2f);
    submit_colored_vertex(ctx,  0.0f, .5f, 0, .3f);
    l10gl_end(ctx);
    expect_int("one textured triangle", capture.triangle_count, 1);
    expect_int("textured triangle dispatch", capture.triangles[0].textured, 1);
    l10gl_bind_texture(ctx, NULL);
}

static void test_modelview_connection(struct l10gl_ctx *ctx)
{
    reset_capture();
    l10gl_matrix_mode(ctx, L10GL_MATRIX_MODELVIEW);
    l10gl_load_identity(ctx);
    l10gl_translatef(ctx, .25f, 0, 0);
    l10gl_scalef(ctx, .5f, .5f, 1);
    l10gl_matrix_mode(ctx, L10GL_MATRIX_PROJECTION);
    l10gl_load_identity(ctx);

    l10gl_begin(ctx, L10GL_TRIANGLES);
    l10gl_vertex3f(ctx, -1, -1, 0);
    l10gl_vertex3f(ctx,  1, -1, 0);
    l10gl_vertex3f(ctx,  0,  1, 0);
    l10gl_end(ctx);
    expect_int("modelview triangle count", capture.triangle_count, 1);
    expect_float("modelview transformed left X",
                 capture.triangles[0].v[0].x, 37.5f);
    expect_float("modelview transformed right X",
                 capture.triangles[0].v[1].x, 87.5f);
    expect_float("modelview transformed bottom Y",
                 capture.triangles[0].v[0].y, 60);
    expect_float("modelview transformed top Y",
                 capture.triangles[0].v[2].y, 20);

    l10gl_matrix_mode(ctx, L10GL_MATRIX_MODELVIEW);
    l10gl_load_identity(ctx);
}

static void test_strip_and_fan_assembly(struct l10gl_ctx *ctx)
{
    reset_capture();
    l10gl_cull_face(ctx, L10GL_CULL_BACK);
    l10gl_begin(ctx, L10GL_TRIANGLE_STRIP);
    submit_colored_vertex(ctx, -.5f, -.5f, 0, .1f);
    submit_colored_vertex(ctx,  .5f, -.5f, 0, .2f);
    submit_colored_vertex(ctx, -.5f,  .5f, 0, .3f);
    submit_colored_vertex(ctx,  .5f,  .5f, 0, .4f);
    l10gl_end(ctx);
    expect_int("strip triangle count", capture.triangle_count, 2);
    expect_float("strip t0 order 0", capture.triangles[0].v[0].r, .1f);
    expect_float("strip t0 order 1", capture.triangles[0].v[1].r, .2f);
    expect_float("strip t0 order 2", capture.triangles[0].v[2].r, .3f);
    expect_float("strip t1 alternating 0", capture.triangles[1].v[0].r, .3f);
    expect_float("strip t1 alternating 1", capture.triangles[1].v[1].r, .2f);
    expect_float("strip t1 alternating 2", capture.triangles[1].v[2].r, .4f);

    reset_capture();
    l10gl_begin(ctx, L10GL_TRIANGLE_FAN);
    submit_colored_vertex(ctx,  0.0f, 0.0f, 0, .1f);
    submit_colored_vertex(ctx,  .5f, 0.0f, 0, .2f);
    submit_colored_vertex(ctx,  0.0f, .5f, 0, .3f);
    submit_colored_vertex(ctx, -.5f, 0.0f, 0, .4f);
    l10gl_end(ctx);
    expect_int("fan triangle count", capture.triangle_count, 2);
    expect_float("fan fixed origin t0", capture.triangles[0].v[0].r, .1f);
    expect_float("fan fixed origin t1", capture.triangles[1].v[0].r, .1f);
    expect_float("fan t1 previous", capture.triangles[1].v[1].r, .3f);
    expect_float("fan t1 new", capture.triangles[1].v[2].r, .4f);
    l10gl_cull_face(ctx, L10GL_CULL_NONE);
}

static void test_line_assembly(struct l10gl_ctx *ctx)
{
    reset_capture();
    l10gl_begin(ctx, L10GL_LINES);
    for (int i = 0; i < 5; i++)
        submit_colored_vertex(ctx, -.8f + i * .3f, 0, 0, .1f * (i + 1));
    l10gl_end(ctx);
    expect_int("independent line count", capture.line_count, 2);
    expect_float("line 0 start", capture.lines[0].v[0].r, .1f);
    expect_float("line 0 end", capture.lines[0].v[1].r, .2f);
    expect_float("line 1 start", capture.lines[1].v[0].r, .3f);
    expect_float("line 1 end", capture.lines[1].v[1].r, .4f);

    reset_capture();
    l10gl_begin(ctx, L10GL_LINE_STRIP);
    for (int i = 0; i < 4; i++)
        submit_colored_vertex(ctx, -.6f + i * .4f, 0, 0, .1f * (i + 1));
    l10gl_end(ctx);
    expect_int("line strip count", capture.line_count, 3);
    expect_float("line strip shared end", capture.lines[1].v[0].r, .2f);
    expect_float("line strip shared start", capture.lines[1].v[1].r, .3f);
}

static void submit_test_triangle(struct l10gl_ctx *ctx, int clockwise,
                                 float z)
{
    l10gl_begin(ctx, L10GL_TRIANGLES);
    if (!clockwise) {
        l10gl_vertex3f(ctx, -.5f, -.5f, z);
        l10gl_vertex3f(ctx,  .5f, -.5f, z);
    } else {
        l10gl_vertex3f(ctx,  .5f, -.5f, z);
        l10gl_vertex3f(ctx, -.5f, -.5f, z);
    }
    l10gl_vertex3f(ctx, 0, .5f, z);
    l10gl_end(ctx);
}

static void test_culling_and_clip_rejection(struct l10gl_ctx *ctx)
{
    reset_capture();
    l10gl_cull_face(ctx, L10GL_CULL_BACK);
    submit_test_triangle(ctx, 0, 0);
    submit_test_triangle(ctx, 1, 0);
    expect_int("back-face culling", capture.triangle_count, 1);

    reset_capture();
    l10gl_cull_face(ctx, L10GL_CULL_FRONT);
    submit_test_triangle(ctx, 0, 0);
    submit_test_triangle(ctx, 1, 0);
    expect_int("front-face culling", capture.triangle_count, 1);
    /* The surviving clockwise triangle becomes clockwise only after the
     * backend-coordinate Y flip; culling was correctly done in NDC. */
    expect_float("front cull surviving first x",
                 capture.triangles[0].v[0].x, 75);

    reset_capture();
    l10gl_cull_face(ctx, L10GL_CULL_NONE);
    submit_test_triangle(ctx, 0, -2);
    submit_test_triangle(ctx, 0, 2);
    expect_int("clip-depth rejection", capture.triangle_count, 0);

    reset_capture();
    submit_test_triangle(ctx, 0, 0);
    expect_int("valid triangle after rejection", capture.triangle_count, 1);
}

int main(void)
{
    struct l10gl_ctx ctx;

    reset_capture();
    if (l10gl_create(&ctx, &capture_backend, 100, 80, 3) < 0) {
        fprintf(stderr, "test-pipeline: failed to create capture context\n");
        return 1;
    }

    test_errors_and_defaults(&ctx);
    test_triangle_transform_and_attributes(&ctx);
    test_textured_dispatch(&ctx);
    test_modelview_connection(&ctx);
    test_strip_and_fan_assembly(&ctx);
    test_line_assembly(&ctx);
    test_culling_and_clip_rejection(&ctx);
    l10gl_destroy(&ctx);

    if (failures) {
        fprintf(stderr, "test-pipeline: FAILED (%d checks)\n", failures);
        return 1;
    }
    printf("test-pipeline: PASS (attributes, transforms, triangles, strips, "
           "fans, lines, textures, culling, clip rejection)\n");
    return 0;
}

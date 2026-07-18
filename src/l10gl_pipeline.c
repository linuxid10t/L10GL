/*
 * l10gl_pipeline.c - X2 immediate-mode capture and primitive assembly.
 *
 * This layer accepts model-space vertices, applies the X1 MODELVIEW,
 * PROJECTION, viewport, and depth-range state, then emits the existing
 * screen-space backend primitives.  It streams at most three captured
 * vertices and performs no allocation.
 */

#include <errno.h>
#include <math.h>

#include "l10gl.h"
#include "l10gl_pipeline.h"

struct projected_vertex {
    struct l10gl_vertex screen;
    float ndc_x;
    float ndc_y;
};

void l10gl_pipeline_init(struct l10gl_ctx *ctx)
{
    ctx->current_r = 1.0f;
    ctx->current_g = 1.0f;
    ctx->current_b = 1.0f;
    ctx->current_a = 1.0f;
    ctx->current_nx = 0.0f;
    ctx->current_ny = 0.0f;
    ctx->current_nz = 1.0f;
    ctx->current_u = 0.0f;
    ctx->current_v = 0.0f;
    ctx->cull_mode_val = L10GL_CULL_NONE;
    ctx->immediate_active = 0;
    ctx->immediate_vertex_count = 0;
}

static int primitive_supported(enum l10gl_primitive primitive)
{
    return primitive == L10GL_TRIANGLES ||
           primitive == L10GL_TRIANGLE_STRIP ||
           primitive == L10GL_TRIANGLE_FAN ||
           primitive == L10GL_LINES ||
           primitive == L10GL_LINE_STRIP;
}

static int project_vertex(const struct l10gl_ctx *ctx,
                          const struct l10gl_immediate_vertex *input,
                          struct projected_vertex *output)
{
    const float object[4] = { input->x, input->y, input->z, 1.0f };
    float clip[4];
    float ndc[3];
    float window[3];
    float z_slop;

    l10gl_object_to_clip(ctx, object, clip);
    if (!isfinite(clip[0]) || !isfinite(clip[1]) ||
        !isfinite(clip[2]) || !isfinite(clip[3]) ||
        !(clip[3] > 1.0e-20f))
        return 0;

    /* X3 will clip primitives crossing the near plane. Until then, reject a
     * primitive with any vertex outside clip-depth bounds so negative or
     * wrapped Z values never reach vintage hardware. X/Y remain available to
     * the backend's existing clip rectangle. */
    z_slop = clip[3] * 1.0e-6f;
    if (clip[2] < -clip[3] - z_slop || clip[2] > clip[3] + z_slop)
        return 0;

    ndc[0] = clip[0] / clip[3];
    ndc[1] = clip[1] / clip[3];
    ndc[2] = clip[2] / clip[3];
    l10gl_ndc_to_window(ctx, ndc, window);

    output->screen.x = window[0];
    output->screen.y = window[1];
    output->screen.z = window[2];
    /* X5 will replace this affine value with reciprocal eye-space depth. */
    output->screen.w = 1.0f;
    output->screen.r = input->r;
    output->screen.g = input->g;
    output->screen.b = input->b;
    output->screen.a = input->a;
    output->screen.u = input->u;
    output->screen.v = input->v;
    output->ndc_x = ndc[0];
    output->ndc_y = ndc[1];
    return 1;
}

static int triangle_is_culled(const struct l10gl_ctx *ctx,
                              const struct projected_vertex *v0,
                              const struct projected_vertex *v1,
                              const struct projected_vertex *v2)
{
    float area = (v1->ndc_x - v0->ndc_x) * (v2->ndc_y - v0->ndc_y)
               - (v1->ndc_y - v0->ndc_y) * (v2->ndc_x - v0->ndc_x);

    if (area == 0.0f)
        return 1;
    if (ctx->cull_mode_val == L10GL_CULL_BACK)
        return area < 0.0f;
    if (ctx->cull_mode_val == L10GL_CULL_FRONT)
        return area > 0.0f;
    return 0;
}

static void emit_triangle(struct l10gl_ctx *ctx,
                          const struct l10gl_immediate_vertex *a,
                          const struct l10gl_immediate_vertex *b,
                          const struct l10gl_immediate_vertex *c)
{
    struct projected_vertex projected[3];

    if (!project_vertex(ctx, a, &projected[0]) ||
        !project_vertex(ctx, b, &projected[1]) ||
        !project_vertex(ctx, c, &projected[2]) ||
        triangle_is_culled(ctx, &projected[0], &projected[1], &projected[2]))
        return;

    /* Binding NULL selects untextured emission. Backends without a textured
     * hook still receive the frontend's established Gouraud fallback. */
    if (ctx->current_texture) {
        l10gl_draw_textured_triangle(ctx, projected[0].screen,
                                     projected[1].screen,
                                     projected[2].screen);
    } else {
        l10gl_draw_triangle(ctx, projected[0].screen,
                            projected[1].screen,
                            projected[2].screen);
    }
}

static void emit_line(struct l10gl_ctx *ctx,
                      const struct l10gl_immediate_vertex *a,
                      const struct l10gl_immediate_vertex *b)
{
    struct projected_vertex projected[2];

    if (!project_vertex(ctx, a, &projected[0]) ||
        !project_vertex(ctx, b, &projected[1]))
        return;
    l10gl_draw_line(ctx, projected[0].screen, projected[1].screen);
}

static void assemble_vertex(struct l10gl_ctx *ctx,
                            struct l10gl_immediate_vertex vertex)
{
    struct l10gl_immediate_vertex *slots = ctx->immediate_vertices;
    unsigned long count = ctx->immediate_vertex_count;

    switch (ctx->immediate_primitive) {
    case L10GL_TRIANGLES:
        slots[count % 3] = vertex;
        count++;
        if (count % 3 == 0)
            emit_triangle(ctx, &slots[0], &slots[1], &slots[2]);
        break;

    case L10GL_TRIANGLE_STRIP:
        if (count < 2) {
            slots[count] = vertex;
        } else {
            /* Alternating source order preserves a consistent CCW/CW facing
             * convention for every triangle in the strip. */
            if ((count - 2) & 1)
                emit_triangle(ctx, &slots[1], &slots[0], &vertex);
            else
                emit_triangle(ctx, &slots[0], &slots[1], &vertex);
            slots[0] = slots[1];
            slots[1] = vertex;
        }
        count++;
        break;

    case L10GL_TRIANGLE_FAN:
        if (count == 0)
            slots[0] = vertex;
        else if (count == 1)
            slots[1] = vertex;
        else {
            emit_triangle(ctx, &slots[0], &slots[1], &vertex);
            slots[1] = vertex;
        }
        count++;
        break;

    case L10GL_LINES:
        if ((count & 1) == 0)
            slots[0] = vertex;
        else
            emit_line(ctx, &slots[0], &vertex);
        count++;
        break;

    case L10GL_LINE_STRIP:
        if (count != 0)
            emit_line(ctx, &slots[0], &vertex);
        slots[0] = vertex;
        count++;
        break;

    case L10GL_POINTS:
        /* Rejected by l10gl_begin; retained for exhaustive enum handling. */
        break;
    }
    ctx->immediate_vertex_count = count;
}

int l10gl_begin(struct l10gl_ctx *ctx, enum l10gl_primitive primitive)
{
    if (!ctx)
        return -EINVAL;
    if (ctx->immediate_active)
        return -EBUSY;
    if (!primitive_supported(primitive))
        return -ENOTSUP;

    ctx->immediate_active = 1;
    ctx->immediate_primitive = primitive;
    ctx->immediate_vertex_count = 0;
    return 0;
}

int l10gl_end(struct l10gl_ctx *ctx)
{
    if (!ctx)
        return -EINVAL;
    if (!ctx->immediate_active)
        return -EPERM;
    ctx->immediate_active = 0;
    ctx->immediate_vertex_count = 0;
    return 0;
}

int l10gl_vertex3f(struct l10gl_ctx *ctx, float x, float y, float z)
{
    struct l10gl_immediate_vertex vertex;

    if (!ctx)
        return -EINVAL;
    if (!ctx->immediate_active)
        return -EPERM;

    vertex.x = x;
    vertex.y = y;
    vertex.z = z;
    vertex.r = ctx->current_r;
    vertex.g = ctx->current_g;
    vertex.b = ctx->current_b;
    vertex.a = ctx->current_a;
    vertex.nx = ctx->current_nx;
    vertex.ny = ctx->current_ny;
    vertex.nz = ctx->current_nz;
    vertex.u = ctx->current_u;
    vertex.v = ctx->current_v;
    assemble_vertex(ctx, vertex);
    return 0;
}

void l10gl_color4f(struct l10gl_ctx *ctx,
                   float r, float g, float b, float a)
{
    ctx->current_r = r;
    ctx->current_g = g;
    ctx->current_b = b;
    ctx->current_a = a;
}

void l10gl_normal3f(struct l10gl_ctx *ctx, float x, float y, float z)
{
    ctx->current_nx = x;
    ctx->current_ny = y;
    ctx->current_nz = z;
}

void l10gl_texcoord2f(struct l10gl_ctx *ctx, float u, float v)
{
    ctx->current_u = u;
    ctx->current_v = v;
}

int l10gl_cull_face(struct l10gl_ctx *ctx, enum l10gl_cull_mode mode)
{
    if (!ctx || (mode != L10GL_CULL_NONE && mode != L10GL_CULL_FRONT &&
                 mode != L10GL_CULL_BACK))
        return -EINVAL;
    ctx->cull_mode_val = mode;
    return 0;
}

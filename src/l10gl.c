/*
 * l10gl.c - L10GL frontend: thin dispatch layer over backend vtable.
 *
 * Applications link against this and a backend (e.g. mga1064).
 * This layer handles state caching and dispatch.
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#include "console.h"
#include "l10gl.h"
#include "l10gl_pipeline.h"
#include "l10gl_xform.h"

static int l10gl_mode_valid(const struct l10gl_ctx *ctx)
{
    const struct l10gl_pixel_format *format = &ctx->pixel_format;
    const struct l10gl_color_channel *channels[] = {
        &format->red, &format->green, &format->blue, &format->alpha,
    };
    uint64_t minimum_stride;
    size_t i;

    if (ctx->width <= 0 || ctx->height <= 0 || ctx->bpp <= 0 ||
        ctx->bpp > 4 || !format->bits_per_pixel ||
        format->bits_per_pixel > ctx->bpp * 8)
        return 0;
    minimum_stride = (uint64_t)(uint32_t)ctx->width * (uint32_t)ctx->bpp;
    if (!ctx->stride || ctx->stride < minimum_stride)
        return 0;
    for (i = 0; i < sizeof(channels) / sizeof(channels[0]); i++) {
        if ((unsigned int)channels[i]->offset + channels[i]->length >
            (unsigned int)ctx->bpp * 8u)
            return 0;
    }
    return 1;
}

/* ========================================================================
 * Lifecycle
 * ======================================================================== */

/* Priority order matters when a machine contains more than one supported
 * adapter: the ViRGE is L10GL's primary, silicon-tested target. */
static const struct l10gl_backend *const l10gl_backends[] = {
    &virge_backend,
    &mga1064_backend,
    &swrast_backend,
};

const struct l10gl_backend *l10gl_autodetect(void)
{
    const char *forced = getenv("L10GL_BACKEND");
    size_t i;

    if (forced && forced[0]) {
        for (i = 0; i < sizeof(l10gl_backends) / sizeof(l10gl_backends[0]); i++) {
            if (strcmp(forced, l10gl_backends[i]->name) == 0) {
                printf("L10GL: forcing backend '%s' via L10GL_BACKEND\n",
                       forced);
                return l10gl_backends[i];
            }
        }

        fprintf(stderr, "L10GL: unknown backend '%s' (available:", forced);
        for (i = 0; i < sizeof(l10gl_backends) / sizeof(l10gl_backends[0]); i++)
            fprintf(stderr, " %s", l10gl_backends[i]->name);
        fprintf(stderr, ")\n");
        return NULL;
    }

    for (i = 0; i < sizeof(l10gl_backends) / sizeof(l10gl_backends[0]); i++) {
        const struct l10gl_backend *backend = l10gl_backends[i];

        if (backend->probe && backend->probe() > 0) {
            printf("L10GL: detected backend '%s'\n", backend->name);
            return backend;
        }
    }

    fprintf(stderr, "L10GL: no supported graphics adapter detected\n");
    return NULL;
}

int l10gl_create_auto(struct l10gl_ctx *ctx, int width, int height, int bpp)
{
    const struct l10gl_backend *backend = l10gl_autodetect();

    if (!backend)
        return -ENODEV;
    return l10gl_create(ctx, backend, width, height, bpp);
}

int l10gl_create(struct l10gl_ctx *ctx,
                 const struct l10gl_backend *backend,
                 int width, int height, int bpp)
{
    int ret;

    if (!ctx || !backend)
        return -EINVAL;

    memset(ctx, 0, sizeof(*ctx));
    ctx->backend = backend;

    /* Set default state before init so backend sees it */
    ctx->width = width;
    ctx->height = height;
    ctx->bpp = bpp;

    ctx->clear_r = 0.0f;
    ctx->clear_g = 0.0f;
    ctx->clear_b = 0.0f;
    ctx->clear_z = 1.0f;

    ctx->depth_func_val = L10GL_LESS;
    ctx->depth_test_enabled = 1;
    ctx->depth_writes_enabled = 1;
    ctx->alpha_test_enabled = 0;
    ctx->alpha_func_val = L10GL_ALWAYS;
    ctx->alpha_ref = 0.0f;
    ctx->blend_enabled = 0;
    ctx->blend_sfactor = L10GL_SRC_ALPHA;
    ctx->blend_dfactor = L10GL_ONE_MINUS_SRC_ALPHA;
    ctx->current_texture = NULL;

    /* P2 must snapshot the original fbdev mode before P1 backend init has an
     * opportunity to negotiate a different one. Offscreen and native-only
     * paths do not declare an available fbdev target and remain untouched. */
    ret = l10gl_console_acquire(ctx, backend);
    if (ret)
        return ret;

    if (backend->init) {
        ret = backend->init(ctx, width, height, bpp);
        if (ret) {
            l10gl_console_release(ctx);
            return ret;
        }
    }

    if (!l10gl_mode_valid(ctx)) {
        fprintf(stderr,
                "L10GL: backend '%s' returned an invalid mode "
                "(%dx%d, %d storage bytes, %ubpp, stride %u)\n",
                backend->name, ctx->width, ctx->height, ctx->bpp,
                ctx->pixel_format.bits_per_pixel, ctx->stride);
        if (backend->cleanup)
            backend->cleanup(ctx);
        l10gl_console_release(ctx);
        return -EINVAL;
    }

    /* Transform state depends on the actual raster adopted by the backend. */
    l10gl_xform_init(ctx);
    l10gl_pipeline_init(ctx);

    printf("L10GL: Backend '%s' initialized\n", backend->name);
    printf("  Requested: %dx%d @ %dbpp\n", width, height, bpp * 8);
    printf("  Actual:    %dx%d @ %ubpp (%d storage bytes), stride %u\n",
           ctx->width, ctx->height, ctx->pixel_format.bits_per_pixel,
           ctx->bpp, ctx->stride);
    printf("  Channels:  R%u:%u G%u:%u B%u:%u A%u:%u\n",
           ctx->pixel_format.red.offset, ctx->pixel_format.red.length,
           ctx->pixel_format.green.offset, ctx->pixel_format.green.length,
           ctx->pixel_format.blue.offset, ctx->pixel_format.blue.length,
           ctx->pixel_format.alpha.offset, ctx->pixel_format.alpha.length);
    printf("  Caps: %s%s%s%s%s%s%s%s%s%s\n",
           ctx->backend->caps & L10GL_CAP_GOURAUD     ? "Gouraud " : "",
           ctx->backend->caps & L10GL_CAP_ZBUFFER     ? "Z-buffer " : "",
           ctx->backend->caps & L10GL_CAP_LINES       ? "Lines " : "",
           ctx->backend->caps & L10GL_CAP_TEXTURE     ? "Texture " : "",
           ctx->backend->caps & L10GL_CAP_BLEND       ? "Blend " : "",
           ctx->backend->caps & L10GL_CAP_DITHER      ? "Dither " : "",
           ctx->backend->caps & L10GL_CAP_BILINEAR    ? "Bilinear " : "",
           ctx->backend->caps & L10GL_CAP_TRILINEAR   ? "Trilinear " : "",
           ctx->backend->caps & L10GL_CAP_PERSPECTIVE ? "Perspective " : "",
           ctx->backend->caps & L10GL_CAP_ALPHA_TEST  ? "AlphaTest " : "");

    return 0;
}

void l10gl_destroy(struct l10gl_ctx *ctx)
{
    if (ctx->backend && ctx->backend->cleanup)
        ctx->backend->cleanup(ctx);
    l10gl_console_release(ctx);
}

/* ========================================================================
 * Clearing
 * ======================================================================== */

void l10gl_clear_color(struct l10gl_ctx *ctx, float r, float g, float b)
{
    ctx->clear_r = r;
    ctx->clear_g = g;
    ctx->clear_b = b;
}

void l10gl_clear_depth(struct l10gl_ctx *ctx, float z)
{
    ctx->clear_z = z;
}

void l10gl_clear(struct l10gl_ctx *ctx)
{
    /* Clear depth buffer if backend supports it */
    if (ctx->backend->clear_depth && (ctx->backend->caps & L10GL_CAP_ZBUFFER))
        ctx->backend->clear_depth(ctx, ctx->clear_z);

    /* Clear color buffer */
    if (ctx->backend->clear_color)
        ctx->backend->clear_color(ctx, ctx->clear_r, ctx->clear_g, ctx->clear_b);
}

/* ========================================================================
 * State
 * ======================================================================== */

void l10gl_depth_func(struct l10gl_ctx *ctx, enum l10gl_depth_func func)
{
    ctx->depth_func_val = func;
    if (ctx->backend->depth_func)
        ctx->backend->depth_func(ctx, func);
}

void l10gl_depth_mask(struct l10gl_ctx *ctx, int enable)
{
    ctx->depth_writes_enabled = enable;
    if (ctx->backend->depth_mask)
        ctx->backend->depth_mask(ctx, enable);
}

void l10gl_enable_depth_test(struct l10gl_ctx *ctx, int enable)
{
    ctx->depth_test_enabled = enable;
    if (ctx->backend->depth_test)
        ctx->backend->depth_test(ctx, enable);
}

/* Alpha test has no backend hook: swrast reads these ctx fields directly in
 * its fragment stage, and ViRGE has no silicon alpha test (Q5 approximates
 * it via texture-alpha blend in Stage 3). */
void l10gl_enable_alpha_test(struct l10gl_ctx *ctx, int enable)
{
    ctx->alpha_test_enabled = enable;
}

void l10gl_alpha_func(struct l10gl_ctx *ctx, enum l10gl_depth_func func,
                      float ref)
{
    ctx->alpha_func_val = func;
    ctx->alpha_ref = ref < 0.0f ? 0.0f : (ref > 1.0f ? 1.0f : ref);
}

void l10gl_enable_blend(struct l10gl_ctx *ctx, int enable)
{
    ctx->blend_enabled = enable;
    if (ctx->backend->blend_enable)
        ctx->backend->blend_enable(ctx, enable);
}

void l10gl_blend_func(struct l10gl_ctx *ctx,
                      enum l10gl_blend_func sfactor,
                      enum l10gl_blend_func dfactor)
{
    ctx->blend_sfactor = sfactor;
    ctx->blend_dfactor = dfactor;
    if (ctx->backend->blend_func)
        ctx->backend->blend_func(ctx, sfactor, dfactor);
}

/* ========================================================================
 * Drawing
 *
 * If a textured triangle is requested and the backend has a texture bound,
 * dispatch through draw_textured_triangle. If the backend doesn't support
 * textures, fall back to draw_triangle (texture is silently ignored).
 * ======================================================================== */

void l10gl_draw_triangle(struct l10gl_ctx *ctx,
                          struct l10gl_vertex v0,
                          struct l10gl_vertex v1,
                          struct l10gl_vertex v2)
{
    if (ctx->backend->draw_triangle)
        ctx->backend->draw_triangle(ctx, v0, v1, v2);
}

void l10gl_draw_textured_triangle(struct l10gl_ctx *ctx,
                                   struct l10gl_vertex v0,
                                   struct l10gl_vertex v1,
                                   struct l10gl_vertex v2)
{
    /* If backend can't do textured triangles, fall back to plain */
    if (!ctx->backend->draw_textured_triangle) {
        if (ctx->backend->draw_triangle)
            ctx->backend->draw_triangle(ctx, v0, v1, v2);
        return;
    }

    ctx->backend->draw_textured_triangle(ctx, v0, v1, v2);
}

void l10gl_draw_line(struct l10gl_ctx *ctx,
                      struct l10gl_vertex v0,
                      struct l10gl_vertex v1)
{
    if (ctx->backend->draw_line)
        ctx->backend->draw_line(ctx, v0, v1);
}

void l10gl_draw_rect(struct l10gl_ctx *ctx,
                     int x, int y, int w, int h, uint32_t color)
{
    if (ctx->backend->fill_rect)
        ctx->backend->fill_rect(ctx, x, y, w, h, color);
}

/* ========================================================================
 * Texture management
 * ======================================================================== */

int l10gl_tex_image_2d(struct l10gl_ctx *ctx, struct l10gl_texture *tex,
                        int width, int height,
                        enum l10gl_tex_format format,
                        const void *data)
{
    if (!ctx->backend->tex_image_2d)
        return -1;  /* backend doesn't support textures */

    return ctx->backend->tex_image_2d(ctx, tex, width, height, format, data);
}

void l10gl_bind_texture(struct l10gl_ctx *ctx, struct l10gl_texture *tex)
{
    ctx->current_texture = tex;
    if (ctx->backend->bind_texture)
        ctx->backend->bind_texture(ctx, tex);
}

void l10gl_tex_parameter(struct l10gl_ctx *ctx,
                         enum l10gl_tex_filter filter,
                         enum l10gl_tex_wrap wrap)
{
    if (ctx->backend->tex_parameter)
        ctx->backend->tex_parameter(ctx, filter, wrap);
}

/* ========================================================================
 * Sync
 * ======================================================================== */

void l10gl_wait_engine(struct l10gl_ctx *ctx)
{
    if (ctx->backend->wait_engine)
        ctx->backend->wait_engine(ctx);
}

void l10gl_wait_vsync(struct l10gl_ctx *ctx)
{
    if (ctx->backend->wait_vsync)
        ctx->backend->wait_vsync(ctx);
}

void l10gl_swap_buffers(struct l10gl_ctx *ctx)
{
    /* NULL-safe: backends without double-buffer (e.g. mga1064) leave the
     * slot unset, so this degrades to a single-buffer no-op. */
    if (ctx->backend->swap_buffers)
        ctx->backend->swap_buffers(ctx);
}

/* ========================================================================
 * Capabilities
 * ======================================================================== */

int l10gl_has_cap(struct l10gl_ctx *ctx, unsigned int cap)
{
    return (ctx->backend->caps & cap) ? 1 : 0;
}

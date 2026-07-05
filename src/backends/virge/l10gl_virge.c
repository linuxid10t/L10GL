/*
 * l10gl_virge.c - L10GL backend glue for the S3 ViRGE.
 *
 * Adapts the virge hardware driver to the l10gl_backend vtable.
 * Converts between generic l10gl_vertex/l10gl types and the
 * hardware-specific virge_vertex format.
 *
 * The ViRGE backend exposes the full capability set that the S3d Engine
 * supports: Gouraud shading, Z-buffering, texture mapping, bilinear/
 * trilinear filtering, alpha blending, and fogging. (Texture mapping
 * and fogging paths are defined in the hardware driver but not yet
 * exposed through the vtable — those require extending the frontend API.)
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "../../l10gl.h"
#include "virge.h"

/* ========================================================================
 * Color conversion helpers
 * ======================================================================== */

/* Convert float RGB (0..1) to 16bpp 5:6:5 */
static uint16_t rgb_to_565(float r, float g, float b)
{
    int ri = (int)(r * 31.0f + 0.5f) & 0x1F;
    int gi = (int)(g * 63.0f + 0.5f) & 0x3F;
    int bi = (int)(b * 31.0f + 0.5f) & 0x1F;
    return (ri << 11) | (gi << 5) | bi;
}

/* Convert float RGB (0..1) to 24bpp RGB888 packed in uint32 */
static uint32_t rgb_to_888(float r, float g, float b)
{
    return ((int)(r * 255) << 16) | ((int)(g * 255) << 8) | (int)(b * 255);
}

/* ========================================================================
 * Backend-private data
 * ======================================================================== */

struct virge_private {
    struct virge_ctx hw;          /* low-level hardware context */
    int depth_test_enabled;
    int depth_writes_enabled;
    enum l10gl_depth_func depth_func_val;
};

static inline struct virge_private *VIRGE_PRIV(struct l10gl_ctx *ctx)
{
    return (struct virge_private *)ctx->backend_data;
}

/* ========================================================================
 * Lifecycle
 * ======================================================================== */

static int virge_be_init(struct l10gl_ctx *ctx, int w, int h, int bpp)
{
    struct virge_private *priv = calloc(1, sizeof(*priv));
    if (!priv)
        return -1;
    ctx->backend_data = priv;

    int ret = virge_init(&priv->hw, w, h, bpp);
    if (ret) {
        free(priv);
        ctx->backend_data = NULL;
        return ret;
    }

    /* Defaults */
    priv->depth_test_enabled = 1;
    priv->depth_writes_enabled = 1;
    priv->depth_func_val = L10GL_LESS;

    return 0;
}

static void virge_be_cleanup(struct l10gl_ctx *ctx)
{
    struct virge_private *priv = VIRGE_PRIV(ctx);
    if (priv) {
        virge_cleanup(&priv->hw);
        free(priv);
        ctx->backend_data = NULL;
    }
}

/* ========================================================================
 * Buffer clearing
 * ======================================================================== */

static void virge_be_clear_color(struct l10gl_ctx *ctx, float r, float g, float b)
{
    struct virge_private *priv = VIRGE_PRIV(ctx);
    uint32_t color;

    if (priv->hw.bpp == 2)
        color = rgb_to_565(r, g, b);
    else
        color = rgb_to_888(r, g, b);

    virge_fill_rect(&priv->hw, 0, 0, priv->hw.width, priv->hw.height, color);
}

static void virge_be_clear_depth(struct l10gl_ctx *ctx, float z)
{
    struct virge_private *priv = VIRGE_PRIV(ctx);
    virge_clear_z(&priv->hw, z);
}

/* ========================================================================
 * State
 *
 * The ViRGE backend caches depth function and write mask state.
 * These are currently cached but not yet wired into the CMD_SET
 * programming in virge_draw_triangle_gouraud — the hardware driver
 * uses a fixed ZBC_LEQUAL + ZUP_ENABLE. A TODO marks where the
 * cached values would be applied.
 * ======================================================================== */

static void virge_be_depth_func(struct l10gl_ctx *ctx, enum l10gl_depth_func func)
{
    VIRGE_PRIV(ctx)->depth_func_val = func;
}

static void virge_be_depth_mask(struct l10gl_ctx *ctx, int enable)
{
    VIRGE_PRIV(ctx)->depth_writes_enabled = enable;
}

static void virge_be_depth_test(struct l10gl_ctx *ctx, int enable)
{
    VIRGE_PRIV(ctx)->depth_test_enabled = enable;
}

/* ========================================================================
 * Drawing
 * ======================================================================== */

static void virge_be_draw_triangle(struct l10gl_ctx *ctx,
                                    struct l10gl_vertex v0,
                                    struct l10gl_vertex v1,
                                    struct l10gl_vertex v2)
{
    struct virge_private *priv = VIRGE_PRIV(ctx);

    /* l10gl_vertex and virge_vertex have the same layout (x,y,z,r,g,b,a) */
    struct virge_vertex vs0 = { v0.x, v0.y, v0.z, v0.r, v0.g, v0.b, v0.a };
    struct virge_vertex vs1 = { v1.x, v1.y, v1.z, v1.r, v1.g, v1.b, v1.a };
    struct virge_vertex vs2 = { v2.x, v2.y, v2.z, v2.r, v2.g, v2.b, v2.a };

    virge_draw_triangle_gouraud(&priv->hw, vs0, vs1, vs2);
}

static void virge_be_draw_line(struct l10gl_ctx *ctx,
                                struct l10gl_vertex v0,
                                struct l10gl_vertex v1)
{
    struct virge_private *priv = VIRGE_PRIV(ctx);
    uint32_t color;

    if (priv->hw.bpp == 2)
        color = rgb_to_565(v0.r, v0.g, v0.b);
    else
        color = rgb_to_888(v0.r, v0.g, v0.b);

    virge_draw_line(&priv->hw,
                    (int)v0.x, (int)v0.y, (int)v1.x, (int)v1.y, color);
}

static void virge_be_fill_rect(struct l10gl_ctx *ctx,
                                int x, int y, int w, int h, uint32_t color)
{
    struct virge_private *priv = VIRGE_PRIV(ctx);
    virge_fill_rect(&priv->hw, x, y, w, h, color);
}

/* ========================================================================
 * Sync
 * ======================================================================== */

static void virge_be_wait_engine(struct l10gl_ctx *ctx)
{
    virge_wait_engine(&VIRGE_PRIV(ctx)->hw);
}

static void virge_be_wait_vsync(struct l10gl_ctx *ctx)
{
    virge_wait_vsync(&VIRGE_PRIV(ctx)->hw);
}

/* ========================================================================
 * Backend Vtable
 *
 * The ViRGE supports nearly all L10GL capability bits:
 *   Gouraud shading, Z-buffering, lines, texture mapping, alpha blending,
 *   dithering, bilinear and trilinear filtering.
 *
 * Texture and blend vtable entries are NULL for now since the frontend
 * API doesn't expose them yet. When the API is extended, the ViRGE can
 * implement them in hardware — no software fallback needed.
 * ======================================================================== */

const struct l10gl_backend virge_backend = {
    .name          = "virge",
    .init          = virge_be_init,
    .cleanup       = virge_be_cleanup,
    .clear_color   = virge_be_clear_color,
    .clear_depth   = virge_be_clear_depth,
    .clear         = NULL,  /* frontend handles color+depth dispatch */
    .depth_func    = virge_be_depth_func,
    .depth_mask    = virge_be_depth_mask,
    .depth_test    = virge_be_depth_test,
    .blend_func    = NULL,  /* hardware supports it, frontend API doesn't expose yet */
    .blend_enable  = NULL,
    .draw_triangle = virge_be_draw_triangle,
    .draw_line     = virge_be_draw_line,
    .fill_rect     = virge_be_fill_rect,
    .wait_engine   = virge_be_wait_engine,
    .wait_vsync    = virge_be_wait_vsync,
    .caps = L10GL_CAP_GOURAUD | L10GL_CAP_ZBUFFER | L10GL_CAP_LINES |
            L10GL_CAP_TEXTURE | L10GL_CAP_BLEND | L10GL_CAP_DITHER |
            L10GL_CAP_BILINEAR | L10GL_CAP_TRILINEAR,
};

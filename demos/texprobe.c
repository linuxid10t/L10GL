/*
 * texprobe.c - readback probe for the textured-triangle path.
 *
 * textured_cube was the one demo never silicon-verified (HANDOFF only ever
 * confirmed it animates/swaps, not that texture mapping is correct). The
 * texture-coordinate pipeline -- UV fixed-point scaling, perspective W,
 * format/upload -- is untested, and virge.c's tex_coord_fixed() does NOT
 * multiply by (tex_width-1) the way its own doc-comment claims, so the UV
 * may collapse to a single texel. This probe decides that on real hardware.
 *
 * METHOD: render a FACE-ON quad (no rotation, constant w => no perspective
 * confound) textured with a UV-ENCODING 64x64 ARGB1555 texture whose
 * texel(x,y) color is R = x>>1, G = y>>1 (B held at 1 so every rendered
 * texel is non-zero and distinguishable from the black background). Then
 * CPU-read the framebuffer: the sampled color at each screen pixel IS the
 * (u,v) the engine actually computed.
 *
 *   CORRECT mapping  => R rises 0..31 left->right, G rises 0..31 top->bottom,
 *                       matching the expected column (model A: normalized UV).
 *   COLLAPSED UV     => R,G stuck near 0 everywhere (texel 0/1 only sampled)
 *                       -- model B, the tex_coord_fixed scale bug.
 *
 * Drives the REAL frontend API (upload/bind/parameter/draw = the exact
 * textured_cube path), and reaches struct virge_ctx via ctx.backend_data
 * (hw is the first field of virge_private) for the framebuffer readback.
 *
 * Build: make -B BACKEND=virge texprobe     Run: sudo ./texprobe
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>

#include "l10gl.h"
#include "backends/virge/virge.h"   /* struct virge_ctx, for the readback */

#define TEX 64

static volatile sig_atomic_t running = 1;
static void sighandler(int s) { (void)s; running = 0; }

/* Expected 5-bit channel value at a screen coordinate under model A
 * (normalized UV): texcol = floor(u*TEX) clamped to [0,TEX-1]; chan = texcol>>1. */
static int exp_chan(int screen, int lo, int hi)
{
    int c = (int)((float)(screen - lo) / (float)(hi - lo) * TEX);
    if (c >= TEX) c = TEX - 1;
    if (c < 0) c = 0;
    return c >> 1;
}

/* texel(x,y) = opaque ARGB1555, R=x>>1, G=y>>1, B=1 (non-zero floor so a
 * rendered texel(0,0) is 0x8001, never confused with the 0x0000 background). */
static void gen_uv_gradient(uint16_t *tex, int size)
{
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            int r = x >> 1;          /* 0..31 */
            int g = y >> 1;          /* 0..31 */
            tex[y * size + x] = (uint16_t)(0x8000 | (r << 10) | (g << 5) | 1);
        }
    }
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    struct l10gl_ctx ctx;

#ifdef BACKEND_VIRGE
    const struct l10gl_backend *backend = &virge_backend;
#else
    fprintf(stderr, "texprobe requires BACKEND=virge\n");
    return 1;
#endif
    if (!(backend->caps & L10GL_CAP_TEXTURE)) {
        fprintf(stderr, "backend advertises no texture cap\n");
        return 1;
    }

    if (l10gl_create(&ctx, backend, 800, 600, 2) < 0) {
        fprintf(stderr, "l10gl_create failed\n");
        return 1;
    }
    int W = ctx.width, H = ctx.height;

    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = sighandler;
    sigaction(SIGINT, &sa, NULL); sigaction(SIGTERM, &sa, NULL);

    /* UV-encoding texture, real upload + bind path. */
    struct l10gl_texture tex;
    uint16_t texmem[TEX * TEX];
    gen_uv_gradient(texmem, TEX);
    if (l10gl_tex_image_2d(&ctx, &tex, TEX, TEX, L10GL_TEX_FMT_ARGB1555, texmem) < 0) {
        fprintf(stderr, "texture upload failed\n");
        return 1;
    }
    l10gl_bind_texture(&ctx, &tex);
    /* NEAREST so readback is an exact texel (no bilinear blur); CLAMP keeps
     * the far edge at texel 63. tex_parameter re-binds, refreshing the bits. */
    l10gl_tex_parameter(&ctx, L10GL_FILTER_NEAREST, L10GL_WRAP_CLAMP);

    l10gl_clear_color(&ctx, 0.0f, 0.0f, 0.0f);
    l10gl_clear_depth(&ctx, 1.0f);
    l10gl_depth_func(&ctx, L10GL_LESS);
    l10gl_clear(&ctx);

    /* Face-on quad, constant w => affine, isolates pure UV scaling/orientation.
     * UV (0,0) top-left .. (1,1) bottom-right; matches the cube's face_uvs. */
    int x0 = 200, y0 = 150, x1 = 600, y1 = 450;
    float zv = 0.5f, w = 1.0f;
    struct l10gl_vertex TL = { (float)x0,(float)y0, zv,w, 1,1,1,1, 0,0 };
    struct l10gl_vertex TR = { (float)x1,(float)y0, zv,w, 1,1,1,1, 1,0 };
    struct l10gl_vertex BR = { (float)x1,(float)y1, zv,w, 1,1,1,1, 1,1 };
    struct l10gl_vertex BL = { (float)x0,(float)y1, zv,w, 1,1,1,1, 0,1 };
    l10gl_draw_textured_triangle(&ctx, TL, TR, BR);
    l10gl_draw_textured_triangle(&ctx, TL, BR, BL);
    l10gl_wait_engine(&ctx);

    /* Readback: hw is the first field of virge_private, so backend_data is
     * directly the virge_ctx. Read the current render target (fb_base). */
    struct virge_ctx *hw = (struct virge_ctx *)ctx.backend_data;
    uint8_t *base = (uint8_t *)hw->fb + hw->fb_base;
    uint32_t stride = hw->stride;

    printf("\ntexprobe: face-on UV-gradient quad (%dx%d stride %u)\n", W, H, stride);
    printf("quad TL(%d,%d) BR(%d,%d)  z=%.2f w=%.2f  tex %dx%d ARGB1555 NEAREST CLAMP\n",
           x0, y0, x1, y1, zv, w, TEX, TEX);
    printf("texture encodes texel coord: R=x>>1 (0..31), G=y>>1 (0..31), B=1.\n");
    printf("CORRECT => R rises 0..31 left->right, G rises 0..31 top->bottom.\n");
    printf("COLLAPSED UV => R,G stuck near 0 (tex_coord_fixed scale bug).\n");
    printf("Each cell below is actualR/expectedR (expected = model A, normalized UV).\n\n");

    int covered = 0, sampled = 0;

    printf("ACTUAL R / EXPECTED R  (rows = y, cols = x; expect left->right rise):\n   ");
    for (int x = x0; x <= x1; x += 50) printf("  x%-3d", x);
    printf("\n");
    for (int y = y0; y <= y1; y += 50) {
        printf("y%-3d", y);
        for (int x = x0; x <= x1; x += 50) {
            uint16_t px = *(uint16_t *)(base + (size_t)y * stride + (size_t)x * 2);
            int r = (px >> 10) & 0x1F;
            printf("  %2d/%-2d", r, exp_chan(x, x0, x1));
        }
        printf("\n");
    }

    printf("\nACTUAL G / EXPECTED G  (rows = y, cols = x; expect top->bottom rise):\n   ");
    for (int x = x0; x <= x1; x += 50) printf("  x%-3d", x);
    printf("\n");
    for (int y = y0; y <= y1; y += 50) {
        printf("y%-3d", y);
        for (int x = x0; x <= x1; x += 50) {
            uint16_t px = *(uint16_t *)(base + (size_t)y * stride + (size_t)x * 2);
            int g = (px >> 5) & 0x1F;
            printf("  %2d/%-2d", g, exp_chan(y, y0, y1));
        }
        printf("\n");
    }

    /* Coverage + auto-verdict over the whole quad interior. */
    for (int y = y0 + 2; y <= y1 - 2; y++) {
        for (int x = x0 + 2; x <= x1 - 2; x++) {
            uint16_t px = *(uint16_t *)(base + (size_t)y * stride + (size_t)x * 2);
            sampled++;
            if (px != 0) covered++;
        }
    }
    int r_left = ((*(uint16_t *)(base + (size_t)((y0+y1)/2)*stride + (size_t)(x0+5)*2)) >> 10) & 0x1F;
    int r_right = ((*(uint16_t *)(base + (size_t)((y0+y1)/2)*stride + (size_t)(x1-5)*2)) >> 10) & 0x1F;
    int g_top = ((*(uint16_t *)(base + (size_t)(y0+5)*stride + (size_t)((x0+x1)/2)*2)) >> 5) & 0x1F;
    int g_bot = ((*(uint16_t *)(base + (size_t)(y1-5)*stride + (size_t)((x0+x1)/2)*2)) >> 5) & 0x1F;

    printf("\n=== VERDICT ===\n");
    printf("coverage: %d/%d sampled pixels non-zero (%d%%)\n",
           covered, sampled, sampled ? covered * 100 / sampled : 0);
    printf("R at left edge=%d  right edge=%d   (correct ~= 0 .. 31)\n", r_left, r_right);
    printf("G at top edge=%d   bottom edge=%d  (correct ~= 0 .. 31)\n", g_top, g_bot);
    if (covered == 0) {
        printf("=> NOTHING RENDERED: textured path draws no pixels (bind/CMD_SET/Z issue).\n");
    } else if (r_left <= 2 && r_right <= 2 && g_top <= 2 && g_bot <= 2) {
        printf("=> UV COLLAPSED: R,G stuck ~0 across the quad -> tex_coord_fixed scale bug\n");
        printf("   (engine sampling texel 0 only; stored U is a texel coord, not normalized).\n");
    } else if (r_right - r_left >= 20 && g_bot - g_top >= 20) {
        printf("=> UV mapping LOOKS CORRECT (R and G span the full range). Bug is elsewhere\n");
        printf("   -- re-examine perspective (W) or the rotated/diagonal path, not UV scale.\n");
    } else {
        printf("=> UV PARTIAL/ANOMALOUS: R/G vary but do not span 0..31. Check scale factor\n");
        printf("   or axis orientation. Paste the grids above for analysis.\n");
    }

    printf("\nDone. Ctrl-C to exit.\n");
    while (running) { if (getchar() == EOF) break; }
    l10gl_destroy(&ctx);
    return 0;
}

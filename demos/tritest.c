/*
 * tritest.c - 3D triangle + Z-buffer readback for the S3 ViRGE.
 *
 * Prior run (Z OFF): a full-height triangle rendered correctly to
 * y=579 -- the 3D rasterizer is sound. But the cube/triangle demos run
 * with Z ON and cut off ~2/5 down, so Z-buffering is the culprit. This
 * test pins down which part:
 *   1. clears the Z-buffer to far (1.0 -> 0xFFFF),
 *   2. CPU-reads the Z region back to verify the clear covered ALL rows,
 *   3. draws the full-height triangle with Z ON (LEQUAL, the init default),
 *   4. CPU-reads the framebuffer to see whether it still reaches the bottom.
 *
 * Outcomes:
 *   Z clear complete  + triangle full  -> Z works (cube issue is elsewhere).
 *   Z clear complete  + triangle cut   -> 3D Z-read addressing is wrong
 *                                         (reads garbage despite cleared Z).
 *   Z clear incomplete (bottom rows)   -> virge_clear_z doesn't reach all
 *                                         rows (DEST_BASE/stride/extent).
 *
 * Build: make tritest     Run: sudo ./tritest
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

#include "backends/virge/virge.h"

static volatile sig_atomic_t running = 1;
static void sighandler(int sig) { (void)sig; running = 0; }

static uint16_t read_px(const uint8_t *base, uint32_t stride, int x, int y)
{
    return *(const uint16_t *)(base + (size_t)y * stride + (size_t)x * 2);
}

int main(void)
{
    struct virge_ctx vctx;
    memset(&vctx, 0, sizeof(vctx));
    if (virge_init(&vctx, 800, 600, 2) < 0) {
        fprintf(stderr, "virge_init failed\n");
        return 1;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sighandler;   /* no SA_RESTART: interrupt getchar() */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    uint8_t *vram = (uint8_t *)vctx.fb;
    int W = vctx.width, H = vctx.height;
    uint32_t stride = vctx.stride;
    uint8_t *zram = vram + vctx.z_base;   /* Z region sits right after FB */

    printf("\n3D triangle + Z readback: %dx%d stride %u, Z base 0x%x\n",
           W, H, stride, vctx.z_base);

    /* CPU-clear framebuffer black. */
    for (int y = 0; y < H; y++) {
        uint16_t *row = (uint16_t *)(vram + (size_t)y * stride);
        for (int x = 0; x < W; x++) row[x] = 0;
    }

    /* Clear Z to far (1.0 = 0xFFFF), then immediately read it back. */
    virge_clear_z(&vctx, 1.0f);
    virge_wait_engine(&vctx);

    int z_bad = 0, z_minx = -1, z_miny = -1, z_maxx = -1, z_maxy = -1;
    uint16_t z_min = 0xFFFF, z_max = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint16_t z = read_px(zram, stride, x, y);
            if (z < z_min) z_min = z;
            if (z > z_max) z_max = z;
            if (z != 0xFFFF) {
                if (z_minx < 0 || x < z_minx) z_minx = x;
                if (z_maxx < 0 || x > z_maxx) z_maxx = x;
                if (z_miny < 0 || y < z_miny) z_miny = y;
                if (z_maxy < 0 || y > z_maxy) z_maxy = y;
                z_bad++;
            }
        }
    }
    printf("Z-clear readback: %d of %d pixels != 0xFFFF; "
           "z range=[%04x,%04x]\n", z_bad, W * H, z_min, z_max);
    if (z_bad == 0)
        printf("  -> Z-clear COMPLETE (all rows cleared to far).\n");
    else
        printf("  -> Z-clear INCOMPLETE: uncleared bbox x=[%d,%d] y=[%d,%d]\n",
               z_minx, z_maxx, z_miny, z_maxy);

    /* Draw the full-height triangle with Z ON (init default: LEQUAL).
     * z=0.5 at every vertex -> must pass LEQUAL against the cleared 0xFFFF. */
    struct virge_vertex v_blue  = { .x=120, .y=20,  .z=0.5f, .w=1,
                                    .r=0, .g=0, .b=1, .a=1 };
    struct virge_vertex v_green = { .x=700, .y=300, .z=0.5f, .w=1,
                                    .r=0, .g=1, .b=0, .a=1 };
    struct virge_vertex v_red   = { .x=400, .y=580, .z=0.5f, .w=1,
                                    .r=1, .g=0, .b=0, .a=1 };
    /* z_cmd_bits left at the virge_init default (Z ON, LEQUAL, Z-update). */

    printf("Drawing full-height Gouraud triangle with Z ON...\n");
    virge_draw_triangle_gouraud(&vctx, v_red, v_green, v_blue);
    virge_wait_engine(&vctx);

    int minx = -1, miny = -1, maxx = -1, maxy = -1, count = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (read_px(vram, stride, x, y)) {
                if (minx < 0 || x < minx) minx = x;
                if (maxx < 0 || x > maxx) maxx = x;
                if (miny < 0 || y < miny) miny = y;
                if (maxy < 0 || y > maxy) maxy = y;
                count++;
            }
        }
    }
    printf("Triangle (Z ON): %d px, bbox y=[%d,%d] (intended 20..580)\n",
           count, miny, maxy);
    if (maxy > 500)
        printf("  -> FULL HEIGHT with Z ON: Z-buffering works; the cube "
               "cutoff is NOT Z (re-examine cube geometry/usage).\n");
    else if (maxy > 0)
        printf("  -> CUT OFF at maxy=%d with Z ON: Z-test is rejecting the "
               "bottom. %s\n", maxy,
               z_bad == 0 ? "Z-clear was complete, so the 3D Z-READ "
                             "addressing is wrong." :
                            "Z-clear was incomplete -- fix virge_clear_z.");
    else
        printf("  -> NOTHING RENDERED with Z ON.\n");

    printf("\nTriangle on screen (blue TL, green right, red bottom). "
           "Photo, then Ctrl-C to exit.\n");
    while (running) {
        if (getchar() == EOF)
            break;
    }

    virge_cleanup(&vctx);
    return 0;
}

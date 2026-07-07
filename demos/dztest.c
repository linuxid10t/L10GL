/*
 * dztest.c - measure the 3D engine's per-pixel Z-gradients on silicon.
 *
 * Two probes, one run:
 *   (X) TdZdX  - Z = f(X) ramp, dzdy = 0.  VERIFIED CORRECT on DX silicon
 *       2026-07-07: measured/intended = 1.000 (run log in docs/HANDOFF.md).
 *       This FALSIFIED the 86Box-cited "TdZdX is half-strength (added to the
 *       already-<<1 Z accumulator, needs 2^16)" hypothesis -- on real DX the
 *       driver's VIRGE_Z_FIXED (2^15) is exactly right for TdZdX.
 *   (Y) TdZdY  - Z = f(Y) ramp, dzdx = 0.  The axis the X probe cannot reach:
 *       its constant-z left edge sets slope02 = 0 and dzdy = 0, so the
 *       programmed TdZdY = -dzdy + slope02*dzdx is 0 -- the per-scanline Z
 *       edge-walk is never exercised. The cube's tilted faces have real dzdy;
 *       if TdZdY is half-strength or sign-flipped, depth ordering breaks at
 *       the top/bottom of faces -- back-face bleedthrough at grazing angles,
 *       which the depth-range widening reduced but did not fix. This probe
 *       closes the last untested Z-gradient variable. (86Box's Z-domain model
 *       was already proven wrong about TdZdX above, so its "TdZdY is fine" is
 *       not trustworthy -- measure it.)
 *
 * Method: for each axis, draw one Gouraud triangle whose Z is a function of
 * that axis only (Z-update ON), CPU-read the Z buffer, average the written Z
 * per column (X) or per row (Y), and compute the rendered per-pixel Z-word
 * slope vs the intended dzd{Axis}*65535.
 *
 * Outcomes per axis:
 *   ratio ~= 0.5   -> gradient half-strength -> apply x2 to that delta term.
 *   ratio ~= 1.0   -> gradient correct on silicon.
 *   ratio < 0      -> sign flip in the programmed delta.
 *
 * Build: make -B BACKEND=virge dztest     Run: sudo ./dztest
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

#include "backends/virge/virge.h"

static volatile sig_atomic_t running = 1;
static void sighandler(int sig) { (void)sig; running = 0; }

static uint16_t read_z(const uint8_t *zram, uint32_t stride, int x, int y)
{
    return *(const uint16_t *)(zram + (size_t)y * stride + (size_t)x * 2);
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
    uint8_t *zram = vram + vctx.z_base;

    /* Triangle: Z = z_left + (x-x_left)/(x_right-x_left) * (z_right-z_left),
     * a function of X only (dzdy = 0). Vertices in bottom/mid/top Y-order,
     * which the engine requires (virge.c edge assignment):
     *   v0 bottom (x_left, 500) z=z_left   v1 mid (x_right,300) z=z_right
     *   v2 top   (x_left, 100) z=z_left
     * The constant-z left edge makes the ramp identical on every scanline. */
    const float z_left = 0.2f, z_right = 0.8f;
    const int   x_left = 100,  x_right = 700;
    const float intended_slope =
        (z_right - z_left) / (float)(x_right - x_left) * 65535.0f;  /* dzdx*65535 */

    printf("\n----- X Z-gradient (TdZdX) probe -----\n");
    printf("Geometry: %dx%d stride %u, Z base 0x%x\n",
           W, H, stride, vctx.z_base);
    printf("Triangle z=f(X): z=%.3f at x=%d, z=%.3f at x=%d (dzdy=0).\n",
           z_left, x_left, z_right, x_right);
    printf("Intended per-pixel Z-word slope = %.2f (dzdx*65535).\n\n",
           intended_slope);

    /* CPU-clear framebuffer black, then Z to far (1.0 = 0xFFFF). */
    for (int y = 0; y < H; y++) {
        uint16_t *row = (uint16_t *)(vram + (size_t)y * stride);
        for (int x = 0; x < W; x++) row[x] = 0;
    }
    virge_clear_z(&vctx, 1.0f);
    virge_wait_engine(&vctx);

    /* Draw the ramp triangle (white, so coverage is visible in the FB too).
     * z_cmd_bits left at the virge_init default: Z ON, LEQUAL, Z-update. */
    struct virge_vertex v0 = { .x = x_left,  .y = 500, .z = z_left,  .w = 1,
                               .r = 1, .g = 1, .b = 1, .a = 1 };
    struct virge_vertex v1 = { .x = x_right, .y = 300, .z = z_right, .w = 1,
                               .r = 1, .g = 1, .b = 1, .a = 1 };
    struct virge_vertex v2 = { .x = x_left,  .y = 100, .z = z_left,  .w = 1,
                               .r = 1, .g = 1, .b = 1, .a = 1 };
    virge_draw_triangle_gouraud(&vctx, v0, v1, v2);
    virge_wait_engine(&vctx);

    /* Average written Z per X column over the screen. A pixel is "rendered"
     * iff its Z word != 0xFFFF (the cleared far value; no rendered z reaches
     * 1.0 here, so this is an unambiguous coverage test). */
    long col_sum[1024];
    int  col_cnt[1024];
    for (int x = 0; x < W && x < 1024; x++) { col_sum[x] = 0; col_cnt[x] = 0; }

    int minx = -1, maxx = -1, rendered = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint16_t z = read_z(zram, stride, x, y);
            if (z != 0xFFFF) {
                col_sum[x] += z;
                col_cnt[x] += 1;
                rendered++;
                if (minx < 0 || x < minx) minx = x;
                if (maxx < 0 || x > maxx) maxx = x;
            }
        }
    }
    printf("Rendered Z-written pixels: %d, X range [%d,%d]\n", rendered, minx, maxx);

    if (rendered > 0) {
        /* Column-averaged Z at ~9 evenly spaced X positions, vs intended. */
        printf("\n  x      avg-Z-word   intended\n");
        for (int s = 0; s <= 8; s++) {
            int x = minx + (maxx - minx) * s / 8;
            if (x < 0 || x >= W || col_cnt[x] == 0) continue;
            float avg = (float)col_sum[x] / col_cnt[x];
            float inten = (z_left + (float)(x - x_left) / (x_right - x_left)
                           * (z_right - z_left)) * 65535.0f;
            printf("  %-4d   %10.1f   %10.1f\n", x, avg, inten);
        }

        /* Measured slope from column averages near the two ends, with a small
         * margin to avoid silhouette sub-pixel noise. */
        int margin = 8;
        int xa = minx + margin, xb = maxx - margin;
        while (xa < W && col_cnt[xa] == 0) xa++;
        while (xb > 0  && col_cnt[xb] == 0) xb--;
        if (xb > xa) {
            float za = (float)col_sum[xa] / col_cnt[xa];
            float zb = (float)col_sum[xb] / col_cnt[xb];
            float measured_slope = (zb - za) / (xb - xa);
            float ratio = measured_slope / intended_slope;

            printf("\nMeasured slope: (z[%d]=%.1f - z[%d]=%.1f) / (%d-%d)"
                   " = %.2f Z-words/pixel\n",
                   xb, zb, xa, za, xb, xa, measured_slope);
            printf("Intended slope: %.2f Z-words/pixel\n", intended_slope);
            printf("Ratio measured/intended = %.3f\n\n", ratio);

            if (ratio > 0.40f && ratio < 0.60f)
                printf("  -> TdZdX ~HALF-STRENGTH CONFIRMED: the X Z-gradient is\n"
                       "     scaled 2^15 but the hardware adds TdZdX to the\n"
                       "     already-<<1 Z accumulator (needs 2^16). Apply x2.\n");
            else if (ratio > 0.90f && ratio < 1.10f)
                printf("  -> X-gradient CORRECT on silicon. Bleedthrough cause is\n"
                       "     NOT the TdZdX scale; re-examine order/winding/edges.\n");
            else
                printf("  -> Ratio %.3f is neither ~0.5 nor ~1.0 -- raw samples\n"
                       "     above; re-examine the Z-gradient encoding first.\n",
                       ratio);
        } else {
            printf("\nCould not find two clean columns for a slope measurement.\n");
        }
    } else {
        printf("  -> NOTHING RENDERED (Z-write off or winding wrong?).\n");
    }

    /* ===========================================================
     * Y Z-gradient (TdZdY) probe -- the axis the X-only ramp above
     * could not exercise (its constant-z left edge drove the
     * programmed TdZdY to 0). If TdZdY is half-strength or
     * sign-flipped on real DX, the cube's tilted faces mis-order by
     * depth at their top/bottom -> back-face bleedthrough.
     * =========================================================== */
    printf("\n----- Y Z-gradient (TdZdY) probe -----\n");

    /* Re-clear FB black + Z far. */
    for (int y = 0; y < H; y++) {
        uint16_t *row = (uint16_t *)(vram + (size_t)y * stride);
        for (int x = 0; x < W; x++) row[x] = 0;
    }
    virge_clear_z(&vctx, 1.0f);
    virge_wait_engine(&vctx);

    /* Triangle z = f(Y) only (dzdx = 0): all three vertices lie on
     * z = 0.2 + (y-100)*0.0015, so dzdx is exactly 0 and dzdy = 0.0015
     * -> intended per-pixel Z-word slope = 0.0015*65535 = 98.30.
     * Vertices pre-sorted bottom/mid/top (the engine sorts anyway). */
    const float z_top = 0.2f, z_mid = 0.5f, z_bot = 0.8f;
    const float y_intended_slope =
        (z_bot - z_top) / (500.0f - 100.0f) * 65535.0f;  /* dzdy*65535 */
    printf("Triangle z=f(Y): z=%.2f@y=500, z=%.2f@y=300, z=%.2f@y=100 (dzdx=0).\n",
           z_bot, z_mid, z_top);
    printf("Intended per-pixel Z-word slope (Y) = %.2f (dzdy*65535).\n\n",
           y_intended_slope);

    struct virge_vertex y0 = { .x = 100, .y = 500, .z = z_bot, .w = 1,
                               .r = 1, .g = 1, .b = 1, .a = 1 };
    struct virge_vertex y1 = { .x = 500, .y = 300, .z = z_mid, .w = 1,
                               .r = 1, .g = 1, .b = 1, .a = 1 };
    struct virge_vertex y2 = { .x = 700, .y = 100, .z = z_top, .w = 1,
                               .r = 1, .g = 1, .b = 1, .a = 1 };
    virge_draw_triangle_gouraud(&vctx, y0, y1, y2);
    virge_wait_engine(&vctx);

    /* Average written Z per ROW (scanline). Coverage test: z != 0xFFFF. */
    static long row_sum[1024];
    static int row_cnt[1024];
    for (int y = 0; y < H && y < 1024; y++) { row_sum[y] = 0; row_cnt[y] = 0; }

    int miny = -1, maxy = -1, y_rendered = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint16_t z = read_z(zram, stride, x, y);
            if (z != 0xFFFF) {
                row_sum[y] += z;
                row_cnt[y] += 1;
                y_rendered++;
                if (miny < 0 || y < miny) miny = y;
                if (maxy < 0 || y > maxy) maxy = y;
            }
        }
    }
    printf("Rendered Z-written pixels: %d, Y range [%d,%d]\n", y_rendered, miny, maxy);

    if (y_rendered > 0) {
        printf("\n  y      avg-Z-word   intended\n");
        for (int s = 0; s <= 8; s++) {
            int y = miny + (maxy - miny) * s / 8;
            if (y < 0 || y >= H || row_cnt[y] == 0) continue;
            float avg = (float)row_sum[y] / row_cnt[y];
            float inten = (z_top + (float)(y - 100) / (500 - 100)
                           * (z_bot - z_top)) * 65535.0f;
            printf("  %-4d   %10.1f   %10.1f\n", y, avg, inten);
        }

        int ymargin = 8;
        int ya = miny + ymargin, yb = maxy - ymargin;
        while (ya < H && row_cnt[ya] == 0) ya++;
        while (yb > 0  && row_cnt[yb] == 0) yb--;
        if (yb > ya) {
            float za = (float)row_sum[ya] / row_cnt[ya];
            float zb = (float)row_sum[yb] / row_cnt[yb];
            float ymeasured = (zb - za) / (yb - ya);
            float yratio = ymeasured / y_intended_slope;

            printf("\nMeasured slope: (z[y=%d]=%.1f - z[y=%d]=%.1f) / (%d-%d)"
                   " = %.2f Z-words/pixel\n", yb, zb, ya, za, yb, ya, ymeasured);
            printf("Intended slope: %.2f Z-words/pixel\n", y_intended_slope);
            printf("Ratio measured/intended = %.3f\n\n", yratio);

            if (yratio > 0.40f && yratio < 0.60f)
                printf("  -> TdZdY ~HALF-STRENGTH CONFIRMED: the Y Z-gradient is\n"
                       "     scaled 2^15 but needs 2^16. Apply x2 to the TdZdY term\n"
                       "     (virge.c, both triangle paths).\n");
            else if (yratio > 0.90f && yratio < 1.10f)
                printf("  -> Y-gradient CORRECT. Both Z-gradient axes now verified\n"
                       "     on silicon -> bleedthrough is NOT a gradient scale\n"
                       "     error; pivot to edge/precision (sub-pixel seam, LEQUAL\n"
                       "     ties at shared cube edges).\n");
            else
                printf("  -> Ratio %.3f is neither ~0.5 nor ~1.0 (negative = sign\n"
                       "     flip in the programmed TdZdY). Raw rows above;\n"
                       "     re-examine the TdZdY encoding/sign first.\n", yratio);
        } else {
            printf("\nCould not find two clean rows for a Y-slope measurement.\n");
        }
    } else {
        printf("  -> NOTHING RENDERED for the Y probe (Z-write off or winding?).\n");
    }

    printf("\nProbe complete. Ctrl-C to exit.\n");
    while (running) {
        if (getchar() == EOF)
            break;
    }

    virge_cleanup(&vctx);
    return 0;
}

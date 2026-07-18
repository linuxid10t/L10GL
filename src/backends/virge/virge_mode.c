/* Pure timing/PLL support for P6 native ViRGE modesetting. */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include "virge_mode.h"

/* The first P6 hardware pass uses only the conservative 60 Hz entries. The
 * 75 Hz entries are defined now so the eventual register writer consumes one
 * fixed, reviewable table rather than synthesizing timings at runtime.
 * Horizontal values are canonical VESA timings and are all divisible by 8;
 * the verified ViRGE 15/16bpp path represents them at two CRTC character
 * clocks per eight pixels (equivalently, one count per four pixels). */
static const struct virge_mode virge_modes[] = {
    {  640, 480, 60, 25175,  640,  656,  752,  800,
       480, 490, 492, 525, 0 },
    {  640, 480, 75, 31500,  640,  656,  720,  840,
       480, 481, 484, 500, 0 },
    {  800, 600, 60, 40000,  800,  840,  968, 1056,
       600, 601, 605, 628,
       VIRGE_MODE_HSYNC_POSITIVE | VIRGE_MODE_VSYNC_POSITIVE },
    {  800, 600, 75, 49500,  800,  816,  896, 1056,
       600, 601, 604, 625,
       VIRGE_MODE_HSYNC_POSITIVE | VIRGE_MODE_VSYNC_POSITIVE },
    { 1024, 768, 60, 65000, 1024, 1048, 1184, 1344,
       768, 771, 777, 806, 0 },
    { 1024, 768, 75, 78750, 1024, 1040, 1136, 1312,
       768, 769, 772, 800,
       VIRGE_MODE_HSYNC_POSITIVE | VIRGE_MODE_VSYNC_POSITIVE },
};

int virge_mode_validate(const struct virge_mode *mode)
{
    uint64_t frame_rate_den;
    uint64_t clock_hz;
    uint64_t requested_hz;
    uint64_t error;

    if (!mode || !mode->width || !mode->height || !mode->refresh_hz ||
        !mode->pixel_clock_khz)
        return -EINVAL;
    if (mode->width != mode->hdisplay || mode->height != mode->vdisplay)
        return -EINVAL;
    if (!(mode->hdisplay < mode->hsync_start &&
          mode->hsync_start < mode->hsync_end &&
          mode->hsync_end < mode->htotal))
        return -EINVAL;
    if (!(mode->vdisplay < mode->vsync_start &&
          mode->vsync_start < mode->vsync_end &&
          mode->vsync_end < mode->vtotal))
        return -EINVAL;
    if (mode->hdisplay < 4u || mode->htotal < 20u || mode->vtotal < 2u)
        return -ERANGE;

    /* At 16bpp the hardware-verified path doubles the VGA horizontal
     * counters, so each timing boundary must be an integral 8-pixel VGA
     * character before the x2 conversion. DB019-B section 16, PDF pp.149-151
     * documents the 9-bit horizontal fields and CR5D extensions. */
    if ((mode->hdisplay | mode->hsync_start | mode->hsync_end |
         mode->htotal) & 7u)
        return -ERANGE;
    if (mode->htotal / 4u - 5u > 0x1ffu ||
        mode->hdisplay / 4u - 1u > 0x1ffu ||
        mode->vtotal - 2u > 0x7ffu ||
        mode->vdisplay - 1u > 0x7ffu ||
        mode->vsync_start > 0x7ffu)
        return -ERANGE;

    /* Reject a mislabeled fixed entry. Allow two percent for nominal refresh
     * labels such as 640x480's 59.94 Hz "60 Hz" mode. */
    frame_rate_den = (uint64_t)mode->htotal * mode->vtotal;
    clock_hz = (uint64_t)mode->pixel_clock_khz * 1000u;
    requested_hz = (uint64_t)mode->refresh_hz * frame_rate_den;
    error = clock_hz > requested_hz ? clock_hz - requested_hz
                                    : requested_hz - clock_hz;
    if (error * 100u > requested_hz * 2u)
        return -ERANGE;

    return 0;
}

const struct virge_mode *virge_mode_find(unsigned width, unsigned height,
                                          unsigned refresh_hz)
{
    size_t i;

    for (i = 0; i < sizeof(virge_modes) / sizeof(virge_modes[0]); i++) {
        const struct virge_mode *mode = &virge_modes[i];

        if (mode->width == width && mode->height == height &&
            mode->refresh_hz == refresh_hz && !virge_mode_validate(mode))
            return mode;
    }
    return NULL;
}

int virge_pll_compute(uint32_t target_khz, struct virge_pll *pll)
{
    /* DB019-B identifies the normal external crystal as 14.318 MHz
     * (pin description) and section 9.1 defines:
     *   fout = (M + 2) * fref / ((N + 2) * 2^R)
     * with M=1..127, N=1..31, R=0..3 and a 135..270 MHz VCO. */
    const uint32_t ref_khz = 14318;
    uint64_t best_diff = UINT64_MAX;
    uint32_t best_den = 0;
    unsigned best_m = 0, best_n = 0, best_r = 0;
    unsigned r, n, m;

    if (!target_khz || !pll)
        return -EINVAL;

    for (r = 0; r <= 3; r++) {
        for (n = 1; n <= 31; n++) {
            uint32_t n_div = n + 2u;
            uint32_t output_den = n_div << r;

            for (m = 1; m <= 127; m++) {
                uint64_t numerator = (uint64_t)(m + 2u) * ref_khz;
                uint64_t target_numerator;
                uint64_t diff;

                if (numerator < (uint64_t)135000u * n_div ||
                    numerator > (uint64_t)270000u * n_div)
                    continue;
                target_numerator = (uint64_t)target_khz * output_den;
                diff = numerator > target_numerator
                     ? numerator - target_numerator
                     : target_numerator - numerator;
                if (best_den && diff * best_den >= best_diff * output_den)
                    continue;
                best_diff = diff;
                best_den = output_den;
                best_m = m;
                best_n = n;
                best_r = r;
            }
        }
    }

    if (!best_den)
        return -ERANGE;

    pll->error_ppm = (uint32_t)((best_diff * 1000000u +
                       (uint64_t)target_khz * best_den / 2u) /
                      ((uint64_t)target_khz * best_den));
    if (pll->error_ppm > 5000u)
        return -ERANGE;

    pll->m = (uint8_t)best_m;
    pll->n = (uint8_t)best_n;
    pll->r = (uint8_t)best_r;
    pll->sr12 = (uint8_t)((best_r << 5) | best_n);
    pll->sr13 = (uint8_t)best_m;
    pll->actual_khz = (uint32_t)(((uint64_t)(best_m + 2u) * ref_khz +
                                  best_den / 2u) / best_den);
    return 0;
}

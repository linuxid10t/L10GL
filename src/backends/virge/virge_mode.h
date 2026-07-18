/* Hardware-independent native-mode definitions for the S3 ViRGE. */

#ifndef VIRGE_MODE_H
#define VIRGE_MODE_H

#include <stdint.h>

#define VIRGE_MODE_HSYNC_POSITIVE (1u << 0)
#define VIRGE_MODE_VSYNC_POSITIVE (1u << 1)

struct virge_mode {
    uint16_t width;
    uint16_t height;
    uint16_t refresh_hz;
    uint32_t pixel_clock_khz;
    uint16_t hdisplay;
    uint16_t hsync_start;
    uint16_t hsync_end;
    uint16_t htotal;
    uint16_t vdisplay;
    uint16_t vsync_start;
    uint16_t vsync_end;
    uint16_t vtotal;
    uint8_t sync_flags;
};

struct virge_pll {
    uint8_t m;
    uint8_t n;
    uint8_t r;
    uint8_t sr12;
    uint8_t sr13;
    uint32_t actual_khz;
    uint32_t error_ppm;
};

/* Return one of P6's fixed VESA timings. There is deliberately no general
 * modeline calculator in the native modeset path. */
const struct virge_mode *virge_mode_find(unsigned width, unsigned height,
                                          unsigned refresh_hz);

/* Validate that a fixed timing is representable by the base ViRGE CRTC in
 * the verified 16bpp/horizontal-multiply-by-two path. */
int virge_mode_validate(const struct virge_mode *mode);

/* Compute SR12/SR13 DCLK parameters from DB019-B section 9.1, PDF pp.65-66.
 * The result is constrained to the documented M/N ranges, 135-270 MHz VCO,
 * and 0.5 percent output tolerance. */
int virge_pll_compute(uint32_t target_khz, struct virge_pll *pll);

#endif

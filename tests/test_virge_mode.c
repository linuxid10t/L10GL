/* Hardware-independent tests for P6 fixed modes and DCLK PLL selection. */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>

#include "backends/virge/virge_mode.h"

static int failed;

#define EXPECT(condition, label) do {                                     \
    if (!(condition)) {                                                   \
        fprintf(stderr, "test-virge-mode: FAIL: %s\n", (label));       \
        failed = 1;                                                       \
    }                                                                     \
} while (0)

static void test_fixed_modes(void)
{
    static const struct {
        unsigned width, height, refresh, clock;
        unsigned hs, he, ht, vs, ve, vt, flags;
    } expected[] = {
        {  640, 480, 60, 25175,  656,  752,  800, 490, 492, 525, 0 },
        {  640, 480, 75, 31500,  656,  720,  840, 481, 484, 500, 0 },
        {  800, 600, 60, 40000,  840,  968, 1056, 601, 605, 628, 3 },
        {  800, 600, 75, 49500,  816,  896, 1056, 601, 604, 625, 3 },
        { 1024, 768, 60, 65000, 1048, 1184, 1344, 771, 777, 806, 0 },
        { 1024, 768, 75, 78750, 1040, 1136, 1312, 769, 772, 800, 3 },
    };
    unsigned i;

    for (i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
        const struct virge_mode *mode =
            virge_mode_find(expected[i].width, expected[i].height,
                            expected[i].refresh);
        struct virge_pll pll;
        struct virge_crtc_image image;
        unsigned hde, vde, lsw;

        EXPECT(mode != NULL, "find fixed mode");
        if (!mode)
            continue;
        EXPECT(virge_mode_validate(mode) == 0, "validate fixed mode");
        EXPECT(mode->pixel_clock_khz == expected[i].clock,
               "fixed pixel clock");
        EXPECT(mode->hsync_start == expected[i].hs &&
               mode->hsync_end == expected[i].he &&
               mode->htotal == expected[i].ht,
               "fixed horizontal timing");
        EXPECT(mode->vsync_start == expected[i].vs &&
               mode->vsync_end == expected[i].ve &&
               mode->vtotal == expected[i].vt,
               "fixed vertical timing");
        EXPECT(mode->sync_flags == expected[i].flags,
               "fixed sync polarity");
        EXPECT(virge_pll_compute(mode->pixel_clock_khz, &pll) == 0 &&
               pll.error_ppm <= 5000,
               "fixed mode has representable DCLK");
        EXPECT(virge_mode_encode_16bpp(mode, mode->width * 2u,
                                       4u * 1024u * 1024u, &image) == 0,
               "fixed mode has representable CRTC image");
        hde = image.value[0x01] |
              (unsigned)((image.value[0x5d] >> 1) & 1u) << 8;
        vde = image.value[0x12] |
              (unsigned)((image.value[0x07] >> 1) & 1u) << 8 |
              (unsigned)((image.value[0x07] >> 6) & 1u) << 9 |
              (unsigned)((image.value[0x5e] >> 1) & 1u) << 10;
        lsw = image.value[0x13] |
              (unsigned)((image.value[0x51] >> 4) & 3u) << 8;
        EXPECT((hde + 1u) * 4u == mode->width &&
               vde + 1u == mode->height && lsw * 8u == mode->width * 2u,
               "CRTC image decodes to requested geometry and pitch");
    }

    EXPECT(virge_mode_find(1280, 720, 60) == NULL,
           "reject unsupported geometry");
    EXPECT(virge_mode_find(640, 480, 72) == NULL,
           "reject unsupported refresh");
}

static void test_mode_validation(void)
{
    struct virge_mode mode = *virge_mode_find(640, 480, 60);

    mode.hsync_start++;
    EXPECT(virge_mode_validate(&mode) == -ERANGE,
           "reject non-character horizontal timing");
    mode = *virge_mode_find(640, 480, 60);
    mode.hsync_end = mode.hsync_start;
    EXPECT(virge_mode_validate(&mode) == -EINVAL,
           "reject unordered horizontal timing");
    mode = *virge_mode_find(640, 480, 60);
    mode.pixel_clock_khz = 40000;
    EXPECT(virge_mode_validate(&mode) == -ERANGE,
           "reject mislabeled refresh");
    EXPECT(virge_mode_validate(NULL) == -EINVAL, "reject null mode");
}

static void expect_pll(uint32_t target, uint8_t sr12, uint8_t sr13,
                       uint32_t actual, const char *label)
{
    struct virge_pll pll = {0};
    int ret = virge_pll_compute(target, &pll);

    if (ret || pll.sr12 != sr12 || pll.sr13 != sr13 ||
        pll.actual_khz != actual || pll.error_ppm > 5000) {
        fprintf(stderr,
                "test-virge-mode: FAIL: %s: ret=%d SR12=%02x SR13=%02x "
                "actual=%u error=%uppm\n",
                label, ret, pll.sr12, pll.sr13, pll.actual_khz,
                pll.error_ppm);
        failed = 1;
    }
}

static void test_pll(void)
{
    struct virge_pll pll;

    /* Exact expected encodings using the documented 14.318 MHz reference. */
    expect_pll(25175, 0x67, 0x7d, 25255, "25.175 MHz");
    expect_pll(40000, 0x49, 0x79, 40025, "40.000 MHz");
    expect_pll(65000, 0x44, 0x6b, 65028, "65.000 MHz");

    EXPECT(virge_pll_compute(0, &pll) == -EINVAL, "reject zero clock");
    EXPECT(virge_pll_compute(10000, &pll) == -ERANGE,
           "reject clock below PLL range");
    EXPECT(virge_pll_compute(40000, NULL) == -EINVAL,
           "reject null PLL output");
}

static unsigned snapshot_count(const struct virge_crtc_image *image)
{
    unsigned count = 0;
    unsigned i;

    for (i = 0; i < VIRGE_CRTC_IMAGE_SIZE; i++)
        count += image->mask[i] != 0;
    return count;
}

static void test_crtc_image(void)
{
    const struct virge_mode *mode = virge_mode_find(800, 600, 60);
    struct virge_crtc_image image;
    static const uint8_t extended[] = {
        0x31, 0x33, 0x34, 0x3a, 0x3b, 0x42, 0x43, 0x50,
        0x51, 0x55, 0x56, 0x58, 0x5d, 0x5e, 0x67, 0x69,
    };
    unsigned i;

    EXPECT(virge_mode_encode_16bpp(mode, 1600, 4u * 1024u * 1024u,
                                   &image) == 0,
           "encode 800x600 CRTC image");
    EXPECT(image.value[0x00] == 0x03 && image.value[0x01] == 0xc7 &&
           image.value[0x02] == 0xc8 && image.value[0x04] == 0xd2,
           "800x600 horizontal low bytes");
    EXPECT(image.value[0x06] == 0x72 && image.value[0x07] == 0xf0 &&
           image.value[0x10] == 0x59 && image.value[0x12] == 0x57,
           "800x600 vertical bytes");
    EXPECT(image.value[0x13] == 0xc8 && image.value[0x17] == 0xe3,
           "800x600 pitch and addressing");
    EXPECT(image.value[0x3b] == 0xea && image.fifo_fetch == 234 &&
           image.value[0x5d] == 0x21,
           "hardware-verified 800x600 x2/SFF bytes");
    EXPECT(image.value[0x50] == 0x10 && image.value[0x51] == 0x00 &&
           image.value[0x67] == 0x30,
           "RGB555 pixel length and scanout mode");
    EXPECT(image.value[0x58] == 0x13 && image.misc_value == 0x0f &&
           image.misc_mask == 0xcf,
           "4MB linear window and positive sync");
    EXPECT(image.seq_value[0x08] == 0x06 &&
           image.seq_value[0x12] == image.pll.sr12 &&
           image.seq_value[0x13] == image.pll.sr13 &&
           image.seq_mask[0x15] == 0x20,
           "sequencer unlock, PLL, and load-bit save image");
    EXPECT(image.dac_mask_value == 0xff && image.dac_mask_mask == 0xff,
           "DAC unblank/save image");
    EXPECT(snapshot_count(&image) == 25u +
           sizeof(extended) / sizeof(extended[0]),
           "complete standard plus extended CRTC save set");
    for (i = 0; i <= 0x18; i++)
        EXPECT(image.mask[i] == 0xff, "standard CRTC fully snapshotted");
    for (i = 0; i < sizeof(extended) / sizeof(extended[0]); i++)
        EXPECT(image.mask[extended[i]] != 0,
               "extended CRTC register snapshotted");

    mode = virge_mode_find(640, 480, 60);
    EXPECT(virge_mode_encode_16bpp(mode, 1280, 2u * 1024u * 1024u,
                                   &image) == 0 &&
           image.value[0x58] == 0x12 && image.misc_value == 0xcf,
           "2MB window and negative sync encoding");
    EXPECT(virge_mode_encode_16bpp(mode, 1279, 2u * 1024u * 1024u,
                                   &image) == -EINVAL,
           "reject unaligned pitch");
    EXPECT(virge_mode_encode_16bpp(mode, 1024, 2u * 1024u * 1024u,
                                   &image) == -EINVAL,
           "reject pitch narrower than scanout");
    EXPECT(virge_mode_encode_16bpp(mode, 1280, 512u * 1024u,
                                   &image) == -ERANGE,
           "reject unsupported linear-window size");
    EXPECT(virge_mode_encode_16bpp(mode, 1280, 2u * 1024u * 1024u,
                                   NULL) == -EINVAL,
           "reject null CRTC image");
}

int main(void)
{
    test_fixed_modes();
    test_mode_validation();
    test_pll();
    test_crtc_image();
    if (failed)
        return 1;
    printf("test-virge-mode: PASS (fixed modes, DCLK PLL, CRTC/save image)\n");
    return 0;
}

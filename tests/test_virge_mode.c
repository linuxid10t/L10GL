/* Hardware-independent tests for P6 fixed modes and DCLK PLL selection. */

#include <errno.h>
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

int main(void)
{
    test_fixed_modes();
    test_mode_validation();
    test_pll();
    if (failed)
        return 1;
    printf("test-virge-mode: PASS (six fixed modes, validation, DCLK PLL)\n");
    return 0;
}

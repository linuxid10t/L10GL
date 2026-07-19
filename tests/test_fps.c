/* Deterministic tests for the shared demo FPS counter. */

#include <math.h>
#include <stdio.h>

#include "l10gl_fps.h"

#define SECOND UINT64_C(1000000000)
#define EPSILON 1.0e-9

static int failures;

static void expect_int(const char *label, long actual, long expected)
{
    if (actual == expected)
        return;
    fprintf(stderr, "test-fps: %s is %ld, expected %ld\n",
            label, actual, expected);
    failures++;
}

static void expect_double(const char *label, double actual, double expected)
{
    if (fabs(actual - expected) <= EPSILON)
        return;
    fprintf(stderr, "test-fps: %s is %.12g, expected %.12g\n",
            label, actual, expected);
    failures++;
}

int main(void)
{
    struct l10gl_fps_counter counter;
    struct l10gl_fps_sample sample;

    l10gl_fps_start_at(&counter, 10 * SECOND);
    expect_int("first half-second frame has no report",
               l10gl_fps_frame_at(&counter, 10 * SECOND + SECOND / 2,
                                  &sample), 0);
    expect_int("one-second frame has no report",
               l10gl_fps_frame_at(&counter, 11 * SECOND, &sample), 0);
    expect_int("one-and-half-second frame has no report",
               l10gl_fps_frame_at(&counter, 11 * SECOND + SECOND / 2,
                                  &sample), 0);
    expect_int("two-second frame reports",
               l10gl_fps_frame_at(&counter, 12 * SECOND, &sample), 1);
    expect_int("first interval frames", sample.interval_frames, 4);
    expect_int("first total frames", sample.total_frames, 4);
    expect_double("first interval FPS", sample.interval_fps, 2.0);
    expect_double("first average FPS", sample.average_fps, 2.0);

    l10gl_fps_frame_at(&counter, 12 * SECOND + SECOND / 2, &sample);
    l10gl_fps_frame_at(&counter, 13 * SECOND, &sample);
    l10gl_fps_frame_at(&counter, 13 * SECOND + SECOND / 2, &sample);
    expect_int("second interval reports",
               l10gl_fps_frame_at(&counter, 14 * SECOND, &sample), 1);
    expect_int("second interval frames", sample.interval_frames, 4);
    expect_int("second total frames", sample.total_frames, 8);
    expect_double("second interval FPS", sample.interval_fps, 2.0);
    expect_double("second average FPS", sample.average_fps, 2.0);

    expect_int("finish sample exists",
               l10gl_fps_finish_at(&counter, 15 * SECOND, &sample), 1);
    expect_int("finish total frames", sample.total_frames, 8);
    expect_double("finish seconds", sample.total_seconds, 5.0);
    expect_double("finish average", sample.average_fps, 1.6);

    expect_int("backward interval is ignored",
               l10gl_fps_frame_at(&counter, 13 * SECOND, &sample), 0);
    expect_int("backward interval leaves total unchanged",
               counter.total_frames, 8);
    expect_int("backward interval leaves interval unchanged",
               counter.interval_frames, 0);
    expect_int("null counter rejected",
               l10gl_fps_frame_at(NULL, 20 * SECOND, &sample), 0);
    l10gl_fps_start_at(NULL, 20 * SECOND);

    if (failures) {
        fprintf(stderr, "test-fps: %d failure(s)\n", failures);
        return 1;
    }
    printf("test-fps: all tests passed\n");
    return 0;
}

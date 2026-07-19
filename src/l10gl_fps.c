/* Shared monotonic FPS reporting for the animated demos. */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "l10gl_fps.h"

#define L10GL_FPS_INTERVAL_NS UINT64_C(2000000000)
#define NS_PER_SECOND 1000000000.0

static uint64_t monotonic_now_ns(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) +
           (uint64_t)now.tv_nsec;
}

void l10gl_fps_start_at(struct l10gl_fps_counter *counter, uint64_t now_ns)
{
    if (!counter)
        return;
    memset(counter, 0, sizeof(*counter));
    counter->start_ns = now_ns;
    counter->interval_start_ns = now_ns;
    counter->started = 1;
}

int l10gl_fps_frame_at(struct l10gl_fps_counter *counter, uint64_t now_ns,
                       struct l10gl_fps_sample *sample)
{
    uint64_t interval_ns, total_ns;

    if (!counter || !counter->started || !sample ||
        now_ns < counter->interval_start_ns || now_ns < counter->start_ns)
        return 0;
    counter->total_frames++;
    counter->interval_frames++;
    interval_ns = now_ns - counter->interval_start_ns;
    if (interval_ns < L10GL_FPS_INTERVAL_NS)
        return 0;
    total_ns = now_ns - counter->start_ns;

    sample->interval_seconds = (double)interval_ns / NS_PER_SECOND;
    sample->total_seconds = (double)total_ns / NS_PER_SECOND;
    sample->interval_frames = counter->interval_frames;
    sample->total_frames = counter->total_frames;
    sample->interval_fps = counter->interval_frames /
                           sample->interval_seconds;
    sample->average_fps = counter->total_frames / sample->total_seconds;

    counter->interval_start_ns = now_ns;
    counter->interval_frames = 0;
    return 1;
}

int l10gl_fps_finish_at(const struct l10gl_fps_counter *counter,
                        uint64_t now_ns, struct l10gl_fps_sample *sample)
{
    uint64_t total_ns;

    if (!counter || !counter->started || !sample ||
        counter->total_frames == 0 || now_ns <= counter->start_ns)
        return 0;
    total_ns = now_ns - counter->start_ns;
    sample->interval_fps = 0.0;
    sample->interval_seconds = 0.0;
    sample->interval_frames = counter->interval_frames;
    sample->total_seconds = (double)total_ns / NS_PER_SECOND;
    sample->total_frames = counter->total_frames;
    sample->average_fps = counter->total_frames / sample->total_seconds;
    return 1;
}

void l10gl_fps_start(struct l10gl_fps_counter *counter)
{
    uint64_t now_ns = monotonic_now_ns();

    if (!now_ns) {
        memset(counter, 0, sizeof(*counter));
        return;
    }
    l10gl_fps_start_at(counter, now_ns);
}

void l10gl_fps_frame(struct l10gl_fps_counter *counter)
{
    struct l10gl_fps_sample sample;
    uint64_t now_ns = monotonic_now_ns();

    if (!now_ns || !l10gl_fps_frame_at(counter, now_ns, &sample))
        return;
    printf("L10GL FPS: interval=%.2f average=%.2f frames=%lu seconds=%.2f\n",
           sample.interval_fps, sample.average_fps,
           sample.total_frames, sample.total_seconds);
    fflush(stdout);
}

void l10gl_fps_finish(const struct l10gl_fps_counter *counter)
{
    struct l10gl_fps_sample sample;
    uint64_t now_ns = monotonic_now_ns();

    if (!now_ns || !l10gl_fps_finish_at(counter, now_ns, &sample))
        return;
    printf("L10GL FPS final: average=%.2f frames=%lu seconds=%.2f\n",
           sample.average_fps, sample.total_frames, sample.total_seconds);
}

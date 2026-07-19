/* Monotonic interval/whole-run FPS measurement for demos and benchmarks. */

#ifndef L10GL_FPS_H
#define L10GL_FPS_H

#include <stdint.h>

struct l10gl_fps_counter {
    uint64_t start_ns;
    uint64_t interval_start_ns;
    unsigned long total_frames;
    unsigned long interval_frames;
    int started;
};

struct l10gl_fps_sample {
    double interval_fps;
    double average_fps;
    double interval_seconds;
    double total_seconds;
    unsigned long interval_frames;
    unsigned long total_frames;
};

/* The _at variants accept an injected monotonic timestamp for deterministic
 * tests. frame_at returns 1 only when the two-second report interval elapsed.
 * finish_at returns 1 when at least one frame and positive elapsed time exist. */
void l10gl_fps_start_at(struct l10gl_fps_counter *counter, uint64_t now_ns);
int l10gl_fps_frame_at(struct l10gl_fps_counter *counter, uint64_t now_ns,
                       struct l10gl_fps_sample *sample);
int l10gl_fps_finish_at(const struct l10gl_fps_counter *counter,
                        uint64_t now_ns, struct l10gl_fps_sample *sample);

/* Runtime wrappers use CLOCK_MONOTONIC and print parser-friendly output. */
void l10gl_fps_start(struct l10gl_fps_counter *counter);
void l10gl_fps_frame(struct l10gl_fps_counter *counter);
void l10gl_fps_finish(const struct l10gl_fps_counter *counter);

#endif /* L10GL_FPS_H */

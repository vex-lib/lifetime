#ifndef LIFETIME_CLOCK_H
#define LIFETIME_CLOCK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// lifetime/clock.h — the R0 ground-truth clock.
//
// Single Class Per File Law: Clock.
//
// R0 owns time. Nanos come from CLOCK_MONOTONIC_RAW (raw hardware counter,
// unslewed by NTP, never backwards) for frame ticks and loop(t) deltas.
// Millis come from CLOCK_REALTIME (unix wall) for timestamps and logging.
// Seconds (double) is the single conversion site for App_frame(scene, t).
// No pause/scale here — ground truth only. A paused/scaled view is a
// vexspoke concern borrowing this tick, never a second clock.
// Across a hot swap the host holds the clock, so time continues.

// Monotonic nanos since an arbitrary host epoch. Never decreases.
uint64_t Clock_nowNanos(void);

// Unix wall millis since 1970-01-01T00:00:00Z. For timestamps, never deltas.
uint64_t Clock_nowMillis(void);

// Monotonic seconds (double) for module frame functions. One conversion.
double Clock_nowSeconds(void);

#endif

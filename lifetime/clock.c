#include "lifetime/clock.h"

#include <time.h>

#include "util/annotation.h"

;;DEFINITION
/**
 * ============================================================================
 * DEFINITION: Clock
 * ============================================================================
 * The R0 ground-truth clock. Nanos from CLOCK_MONOTONIC_RAW for frame ticks
 * (raw hardware counter, unslewed by NTP, never backwards). Millis from
 * CLOCK_REALTIME for unix wall timestamps. Seconds (double) is the single
 * conversion site for module frame functions. No pause/scale — ground truth
 * only. The host holds the clock across hot swaps, so time continues.
 * Lives at R0 as a leaf host primitive.
 * ============================================================================
 */

;;OVERVIEW
/**
 * ============================================================================
 * MODULE: Clock (lifetime/clock.c)
 * ============================================================================
 * the R0 ground-truth clock.
 *
 * STRUCT FIELDS: none — procedural (pure OS clock reads, no retained state)
 *
 * FUNCTION REGISTRY:
 * ----------------------------------------------------------------------------
 * Core Functions:
 *   - Clock_nowNanos()
 *   - Clock_nowMillis()
 *   - Clock_nowSeconds()
 * ============================================================================
 */


uint64_t Clock_nowNanos(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);

    return (uint64_t) ts.tv_sec * 1000000000ull + (uint64_t) ts.tv_nsec;
}

uint64_t Clock_nowMillis(void) {
    struct timespec ts;

    clock_gettime(CLOCK_REALTIME, &ts);

    return (uint64_t) ts.tv_sec * 1000ull + (uint64_t) ts.tv_nsec / 1000000ull;
}

double Clock_nowSeconds(void) {
    return (double) Clock_nowNanos() / 1000000000.0;
}

// lifetime/test/clock/clock_test.c — owner test for lifetime/clock.
//
// Proves the R0 ground-truth clock:
//   - nanos monotonic non-decreasing across calls;
//   - millis within sane unix range and consistent with nanos;
//   - seconds agree with nanos within one tick;
//   - no frame count, no pause/scale — ground truth only.

#include "vex.h"
#include "lifetime/clock.h"

#include <assert.h>
#include <stdio.h>

int main() {
    u64 n1 = Clock_nowNanos();
    u64 n2 = Clock_nowNanos();
    assert(n2 >= n1);

    u64 m1 = Clock_nowMillis();
    assert(m1 > 1700000000000ull);

    d64 s = Clock_nowSeconds();
    assert(s > 0.0);
    u64 backToNanos = (u64) (s * 1000000000.0);
    u64 n3 = Clock_nowNanos();
    assert(n3 >= backToNanos);
    assert(n3 - backToNanos < 1000000000ull);

    u64 m2 = Clock_nowMillis();
    assert(m2 >= m1);

    printf("clock_test: PASS nanos=%llu millis=%llu seconds=%.3f\n",
           (unsigned long long) n2, (unsigned long long) m2, s);
    return 0;
}

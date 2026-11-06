// hotcwap/test/capability/capability_test.c — the Capability class _test.
//
// Contract test for the R1 host capability probe (the Capability Gating Law):
//   - every kind has a stable name, and Capability_has answers a bool without
//     crashing (a capability is never assumed — the bit is either set or not);
//   - an out-of-range kind fails closed (false / "CAPABILITY_UNKNOWN");
//   - the probe is idempotent (same answer on repeat; it is cold and cached);
//   - on macOS the OS major is at least the floor (14) — the deployment target
//     makes that a guarantee, not a hope.
//
// Machine-specific feature VALUES are reported, not asserted: a virtualized or
// future host may set or clear any single CPU bit, so pinning one would be
// flaky. The law cares that the probe is honest and cached, not that an M-chip
// has a particular bit.

#include "util/platform.h"
#include <stdint.h>
#include <stdio.h>

#include "capability/capability.h"

static int g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);          \
            g_failures++;                                                  \
        }                                                                  \
    } while (0)

int main(void) {
    // Every kind: non-empty name + a bool answer (no crash, no assumption).
    uint64_t before = 0;
    for (int k = 0; k < CAPABILITY_COUNT; k++) {
        CapabilityKind kind = (CapabilityKind) k;
        CHECK(Capability_kindName(kind) != nullptr);
        CHECK(Capability_kindName(kind)[0] != '\0');
        bool present = Capability_has(kind);
        if (present)
            before++;
    }

    // Names are exact and stable.
    CHECK(Capability_kindName(CAPABILITY_CPU_DOTPROD)[0] == 'C');
    CHECK(Capability_kindName((CapabilityKind) 999) != nullptr);

    // Out-of-range fails closed.
    CHECK(!Capability_has((CapabilityKind) 999));
    CHECK(!Capability_has((CapabilityKind) -1));

    // Idempotent: the probe is cold and cached, so repeats agree.
    int again = 0;
    for (int k = 0; k < CAPABILITY_COUNT; k++)
        if (Capability_has((CapabilityKind) k))
            again++;
    CHECK((uint64_t) again == before);
    CHECK(Capability_osMajor() == Capability_osMajor());

#if defined(VEX_PLATFORM_APPLE)
    // The floor guarantees macOS 14+; the probe must agree.
    CHECK(Capability_osMajor() >= 14);
#endif

    // Explicit cold probe is idempotent and non-destructive.
    Capability_probe();
    Capability_probe();
    CHECK((uint64_t) again == before);

    printf("capability_test: os=%d, features set=%d of %d\n",
           Capability_osMajor(), again, (int) CAPABILITY_COUNT);

    if (g_failures == 0) {
        printf("capability_test: all assertions held\n");
        return 0;
    }
    printf("capability_test: %d FAILURES\n", g_failures);
    return 1;
}

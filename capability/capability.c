#include "../capability/capability.h"

// following the directory going forward:
// worktree > organization > repository > <directory tree> > file
#include "types.h"
#include "util/annotation.h"
#include "util/platform.h"

#if defined(VEX_PLATFORM_APPLE)
#include <sys/sysctl.h>
#endif

;;DEFINITION
/**
 * ============================================================================
 * DEFINITION: Capability
 * ============================================================================
 * The R1 host capability probe. It answers, at runtime, whether THIS machine
 * has a host/CPU feature that is newer than the platform floor, so a feature
 * never raises the floor (the Capability Gating Law). The host is read ONCE:
 * the first query fills a 64-bit bitset and an OS-major word, and every later
 * query branches on that cache. The image constructor below performs that read
 * before `main`, so the first query is already a cache hit. A host with no
 * backend answers false for every kind — a capability is never assumed. Device/GPU capabilities are NOT here;
 * they belong to the R3 driver (graphvex).
 *
 * On Apple Silicon the CPU bits come from the documented hw.optional.arm.FEAT_*
 * sysctls and the OS major from kern.osproductversion. The non-Apple backend
 * (Linux getauxval, Windows IsProcessorFeaturePresent) is a stated follow-up;
 * until it lands those hosts answer false, which is the honest "not proven here".
 * ============================================================================
 */

;;OVERVIEW
/**
 * ============================================================================
 * CLASS: Capability (capability/capability.c)
 * ============================================================================
 * Runtime host/CPU capability probe, probed once and cached (the Capability
 * Gating Law). CPU features are the bitset; the OS release is a word.
 *
 * STRUCT FIELDS: none — procedural. State is file-static and set exactly once:
 *   u64 s_bits;   // one bit per CapabilityKind
 *   bool     s_probed; // the one-time probe has run
 *   int      s_osMajor;// running macOS major (0 = unknown), -1 = not yet probed
 *
 * PRIVATE HELPERS:
 * ----------------------------------------------------------------------------
 *   sysctlFlag(name)  : read a hw.optional.* boolean (Apple)
 *   probeOsMajor()    : read kern.osproductversion major (Apple; 0 elsewhere)
 *   probeOnce()       : fill s_bits / s_osMajor exactly once
 *
 * FUNCTION REGISTRY:
 * ----------------------------------------------------------------------------
 * Core Functions:
 *   - Capability_has(kind)
 *   - Capability_probe()
 *   - Capability_osMajor()
 *
 * Getters:
 *   - Capability_kindName(kind)
 * ============================================================================
 */
static u64 s_bits = 0;      // one bit per CapabilityKind
static bool s_probed = false;    // the one-time probe has run
static int s_osMajor = -1;       // running macOS major (0 = unknown)

#if defined(VEX_PLATFORM_APPLE)
// Read one hw.optional.* boolean sysctl; missing/nonzero semantics -> bool.
static bool sysctlFlag(const char *name) {
    u64 value = 0;
    bytes size = sizeof(value);
    if (sysctlbyname(name, &value, &size, nullptr, 0) != 0)
        return false;
    return value != 0;
}
#endif

// The running macOS major version, or 0 when unknown.
static int probeOsMajor() {
    #if defined(VEX_PLATFORM_APPLE)
        char buf[32];
        bytes size = sizeof buf;
        if (sysctlbyname("kern.osproductversion", buf, &size, nullptr, 0) != 0)
            return 0;
        buf[sizeof buf - 1] = '\0';
        int major = 0;
        for (const char *p = buf; *p >= '0' && *p <= '9'; p++)
            major = major * 10 + (*p - '0');
        return major;
    #else
        return 0;
    #endif
}

// The one-time cold probe. Idempotent; guarded by s_probed.
static fn probeOnce() {
    if (s_probed)
        return;
    s_probed = true;
    s_bits = 0;
    s_osMajor = probeOsMajor();
    #if defined(VEX_PLATFORM_APPLE)
        if (sysctlFlag("hw.optional.arm.FEAT_DotProd")) s_bits |= ((u64) 1 << CAPABILITY_CPU_DOTPROD);
        if (sysctlFlag("hw.optional.arm.FEAT_AES"))     s_bits |= ((u64) 1 << CAPABILITY_CPU_AES);
        if (sysctlFlag("hw.optional.arm.FEAT_SHA256"))  s_bits |= ((u64) 1 << CAPABILITY_CPU_SHA256);
        if (sysctlFlag("hw.optional.arm.FEAT_SHA512"))  s_bits |= ((u64) 1 << CAPABILITY_CPU_SHA512);
        if (sysctlFlag("hw.optional.arm.FEAT_SHA3"))    s_bits |= ((u64) 1 << CAPABILITY_CPU_SHA3);
        if (sysctlFlag("hw.optional.arm.FEAT_LSE"))     s_bits |= ((u64) 1 << CAPABILITY_CPU_LSE);
        if (sysctlFlag("hw.optional.arm.FEAT_FP16"))    s_bits |= ((u64) 1 << CAPABILITY_CPU_FP16);
        if (sysctlFlag("hw.optional.arm.FEAT_BF16"))    s_bits |= ((u64) 1 << CAPABILITY_CPU_BF16);
        if (sysctlFlag("hw.optional.arm.FEAT_I8MM"))    s_bits |= ((u64) 1 << CAPABILITY_CPU_I8MM);
        if (sysctlFlag("hw.optional.arm.FEAT_SVE"))     s_bits |= ((u64) 1 << CAPABILITY_CPU_SVE);
        if (sysctlFlag("hw.optional.arm.FEAT_SME"))     s_bits |= ((u64) 1 << CAPABILITY_CPU_SME);
    #endif
}

// The one pre-main hook. The linker records this pointer in the image's
// initializer list; the runtime walks that list before the entry point, so the
// host facts are already cached by the time `main` runs. No caller, no order
// dependency on other units, and no I/O — sysctl only.
// INTENTIONAL(vex): the only constructor in the tree. (the static {} way)
__attribute__((constructor)) static fn capabilityBoot() {
    probeOnce();
}

// CORE FUNCTIONS (PUBLIC & PRIVATE)
/** Returns whether a valid capability kind is present in the cached probe. */
bool Capability_has(CapabilityKind kind) {
    if ((u32) kind < 0 || kind >= CAPABILITY_COUNT)
        return false;
    probeOnce();
    return (s_bits & ((u64) 1 << (u32) kind)) != 0;
}

/** Performs the idempotent cold host capability probe. */
fn Capability_probe() {
    probeOnce();
}

/** Returns the cached operating-system major version, or zero when unknown. */
int Capability_osMajor() {
    probeOnce();
    return s_osMajor;
}

// GETTERS (PUBLIC & PRIVATE)
;;GETTER
/** Returns the symbolic name for a capability kind, or the unknown label. */
const char *Capability_kindName(CapabilityKind kind) {
    switch (kind) {
        case CAPABILITY_CPU_DOTPROD: return "CAPABILITY_CPU_DOTPROD";
        case CAPABILITY_CPU_AES:     return "CAPABILITY_CPU_AES";
        case CAPABILITY_CPU_SHA256:  return "CAPABILITY_CPU_SHA256";
        case CAPABILITY_CPU_SHA512:  return "CAPABILITY_CPU_SHA512";
        case CAPABILITY_CPU_SHA3:    return "CAPABILITY_CPU_SHA3";
        case CAPABILITY_CPU_LSE:     return "CAPABILITY_CPU_LSE";
        case CAPABILITY_CPU_FP16:    return "CAPABILITY_CPU_FP16";
        case CAPABILITY_CPU_BF16:    return "CAPABILITY_CPU_BF16";
        case CAPABILITY_CPU_I8MM:    return "CAPABILITY_CPU_I8MM";
        case CAPABILITY_CPU_SVE:     return "CAPABILITY_CPU_SVE";
        case CAPABILITY_CPU_SME:     return "CAPABILITY_CPU_SME";
        default:                     return "CAPABILITY_UNKNOWN";
    }
}

#ifndef CAPABILITY_CAPABILITY_H
#define CAPABILITY_CAPABILITY_H

// capability/capability.h — the R1 host capability probe (the Capability
// Gating Law). Answers "can THIS machine do X?" at runtime for host/CPU
// features that are NEWER than the platform floor (the Platform Support Floor
// Law), so a feature never raises the floor: it is probed once at boot,
// cached, and either used or fallen back from.
//
// This class owns HOST and CPU capabilities. DEVICE / GPU capabilities (ray
// tracing, GPU families, mesh shaders) live in the R3 driver (graphvex), per
// the Capability Gating Law rule 6.
//
// Probe-once contract: the first query (or an explicit Capability_probe) reads
// the host once and caches the answer; hot paths branch on the cache and never
// re-probe (the Cold-Strict, Hot-Minimal Validation Law).

typedef enum CapabilityKind {
    CAPABILITY_CPU_DOTPROD = 0, // ASIMD dot product (FEAT_DotProd)
    CAPABILITY_CPU_AES,         // ARMv8 AES
    CAPABILITY_CPU_SHA256,      // ARMv8 SHA2-256
    CAPABILITY_CPU_SHA512,      // ARMv8 SHA2-512
    CAPABILITY_CPU_SHA3,        // ARMv8 SHA3
    CAPABILITY_CPU_LSE,         // large-system extensions (atomics)
    CAPABILITY_CPU_FP16,        // IEEE half-precision arithmetic
    CAPABILITY_CPU_BF16,        // bf3216
    CAPABILITY_CPU_I8MM,        // int8 matrix multiply
    CAPABILITY_CPU_SVE,         // scalable vector extension
    CAPABILITY_CPU_SME,         // scalable matrix extension
    CAPABILITY_COUNT
} CapabilityKind;

// True when THIS host has the capability. Probes once and caches. False for an
// out-of-range kind, and false for every kind on a host with no backend (a
// capability is never assumed).
bool Capability_has(CapabilityKind kind);

// Stable symbolic name, e.g. "CAPABILITY_CPU_DOTPROD". Never nullptr.
const char *Capability_kindName(CapabilityKind kind);

// Running macOS major version (>= 14, the floor), or 0 when unknown.
int Capability_osMajor(void);

// Force the one-time probe now (cold; idempotent). Optional: the first
// Capability_has / Capability_osMajor call probes lazily.
void Capability_probe(void);

#endif

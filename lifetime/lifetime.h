#ifndef SPOKE_LIFETIME_H
#define SPOKE_LIFETIME_H

#include "vex.h"

// spoke/lifetime.h — the Lifetime memory substrate contract for the R0 host.
//
// Single Class Per File Law: Lifetime.
//
// R0 Host (lifetime) operates strictly on opaque any handles and never includes
// higher-tier headers. lifetime verifies pointer legitimacy (alignment,
// valid address space, non-null, and provider attestation) while keeping the
// memory structures completely opaque.
//
// Every arena pointer held by Lifetime is an opaque any. All checks are performed
// by Lifetime_isLegit() to guarantee memory consistency across module hot-reloads.

#define LIFETIME_TYPE_ARENA ((u64) 1)
#define VEXSPOKE_TYPE_ARENA LIFETIME_TYPE_ARENA

typedef struct Lifetime {
    any persistentArena;    // master session arena (opaque any)
    any transientArena;     // per-tick scratch arena (opaque any)
    u64 persistentType;  // provider-reported type id, nonzero = attested
    u64 transientType;   // provider-reported type id, nonzero = attested
    any relational;         // relational engine symbol root (opaque any)
} Lifetime;

// Pointer Legitimacy Verification:
// Checks that ptr is non-null, 16-byte aligned ((pointer)ptr & 15 == 0),
// and within valid user address space (>= 0x10000).
bool Lifetime_isLegit(const_any ptr);

// Validates that the Lifetime struct holds legitimate, attested arenas.
bool Lifetime_isValid(const Lifetime *lifetime);

// Lifecycle: create arenas using opaque handles, verifying legitimacy.
Lifetime Lifetime_create(bytes persistentBytes, bytes transientBytes);
fn Lifetime_destroy(Lifetime *lifetime);
fn Lifetime_resetTransient(Lifetime *lifetime);

// Aligned arena allocators (returns verified, aligned any payloads)
any Lifetime_allocPersistent(Lifetime *lifetime, u64 typeId, bytes Bytes);
any Lifetime_allocTransient(Lifetime *lifetime, u64 typeId, bytes Bytes);
fn Lifetime_freePersistent(Lifetime *lifetime, any ptr);

// Bind external opaque arenas (e.g. passed from host launcher or relational-engine)
Lifetime Lifetime_bind(any persistentArena, any transientArena,
                       u64 persistentType, u64 transientType,
                       any relational);

// Relational symbol root and arena accessors (Symmetric Getter/Setter Completeness Law)
any Lifetime_getRelational(const Lifetime *lifetime);
fn Lifetime_setRelational(Lifetime *lifetime, any relational);
any Lifetime_getPersistentArena(const Lifetime *lifetime);
any Lifetime_getTransientArena(const Lifetime *lifetime);

#endif


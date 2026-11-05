#ifndef SPOKE_LIFETIME_H
#define SPOKE_LIFETIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// spoke/lifetime.h — the Lifetime memory substrate contract for the R0 host.
//
// Single Class Per File Law: Lifetime.
//
// R0 Host (lifetime) operates strictly on opaque void* handles and never includes
// higher-tier headers. lifetime verifies pointer legitimacy (alignment,
// valid address space, non-null, and provider attestation) while keeping the
// memory structures completely opaque.
//
// Every arena pointer held by Lifetime is an opaque void*. All checks are performed
// by Lifetime_isLegit() to guarantee memory consistency across module hot-reloads.

#define LIFETIME_TYPE_ARENA ((uint64_t) 1)
#define VEXSPOKE_TYPE_ARENA LIFETIME_TYPE_ARENA

typedef struct Lifetime {
    void *persistentArena;    // master session arena (opaque void*)
    void *transientArena;     // per-tick scratch arena (opaque void*)
    uint64_t persistentType;  // provider-reported type id, nonzero = attested
    uint64_t transientType;   // provider-reported type id, nonzero = attested
    void *relational;         // relational engine symbol root (opaque void*)
} Lifetime;

// Pointer Legitimacy Verification:
// Checks that ptr is non-null, 16-byte aligned ((uintptr_t)ptr & 15 == 0),
// and within valid user address space (>= 0x10000).
bool Lifetime_isLegit(const void *ptr);

// Validates that the Lifetime struct holds legitimate, attested arenas.
bool Lifetime_isValid(const Lifetime *lifetime);

// Lifecycle: create arenas using opaque handles, verifying legitimacy.
Lifetime Lifetime_create(size_t persistentBytes, size_t transientBytes);
void Lifetime_destroy(Lifetime *lifetime);
void Lifetime_resetTransient(Lifetime *lifetime);

// Aligned arena allocators (returns verified, aligned void* payloads)
void *Lifetime_allocPersistent(Lifetime *lifetime, uint64_t typeId, size_t Bytes);
void *Lifetime_allocTransient(Lifetime *lifetime, uint64_t typeId, size_t Bytes);
void Lifetime_freePersistent(Lifetime *lifetime, void *ptr);

// Bind external opaque arenas (e.g. passed from host launcher or relational-engine)
Lifetime Lifetime_bind(void *persistentArena, void *transientArena,
                       uint64_t persistentType, uint64_t transientType,
                       void *relational);

// Relational symbol root and arena accessors (Symmetric Getter/Setter Completeness Law)
void *Lifetime_getRelational(const Lifetime *lifetime);
void Lifetime_setRelational(Lifetime *lifetime, void *relational);
void *Lifetime_getPersistentArena(const Lifetime *lifetime);
void *Lifetime_getTransientArena(const Lifetime *lifetime);

#endif


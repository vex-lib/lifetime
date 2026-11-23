#ifndef SPOKE_LIFETIME_H
#define SPOKE_LIFETIME_H

#include "vex.h"

/**
 * spoke/lifetime.h, the Lifetime memory substrate contract for the R0 host.
 *
 * Single Class Per File Law: Lifetime.
 *
 * R0 Host (lifetime) operates strictly on opaque any handles and never includes
 * higher-tier headers. lifetime verifies pointer legitimacy (alignment,
 * valid address space, non-null, and provider attestation) while keeping the
 * memory structures completely opaque.
 *
 * Every arena pointer held by Lifetime is an opaque any. All checks are performed
 * by Lifetime_isLegit() to guarantee memory consistency across module hot-reloads.
 */

#define LIFETIME_TYPE_ARENA ((u64) 1)
#define VEXSPOKE_TYPE_ARENA LIFETIME_TYPE_ARENA

/**
 * Memory substrate holding persistent and transient arena handles for the R0 host.
 *
 * any persistentArena - master session arena (opaque any)
 * any transientArena  - per-tick scratch arena (opaque any)
 * u64 persistentType  - provider-reported type id, nonzero = attested
 * u64 transientType   - provider-reported type id, nonzero = attested
 * any relational      - relational engine symbol root (opaque any)
 */
typedef struct Lifetime {
    any persistentArena;
    any transientArena;
    u64 persistentType;
    u64 transientType;
    any relational;
} Lifetime;

/**
 * Verifies pointer legitimacy: non-null, 16-byte aligned, and within valid user address space (>= 0x10000).
 * @param ptr pointer to check
 * @return true if ptr passes all legitimacy checks
 */
bool Lifetime_isLegit(const_any ptr);

/**
 * Validates that the Lifetime struct holds legitimate, attested arenas.
 * @param lifetime lifetime record to validate
 * @return true if both arenas and optional relational root are valid
 */
bool Lifetime_isValid(const Lifetime *lifetime);

/**
 * Allocates persistent and transient arenas using opaque handles and verifies legitimacy.
 * @param persistentBytes byte capacity for the persistent arena (0 uses default 64 MiB)
 * @param transientBytes  byte capacity for the transient arena (0 uses default 64 MiB)
 * @return initialized Lifetime record, or zeroed on failure
 */
Lifetime Lifetime_2(bytes persistentBytes, bytes transientBytes);
#define Lifetime(persistentBytes, transientBytes) Lifetime_2((persistentBytes), (transientBytes))

/**
 * Destroys both arenas and clears the Lifetime record.
 * @param lifetime lifetime record to destroy
 */
fn Lifetime_destroy(Lifetime *lifetime);

/**
 * Releases all allocations from the transient scratch arena.
 * @param lifetime lifetime record whose transient arena is reset
 */
fn Lifetime_resetTransient(Lifetime *lifetime);

/**
 * Allocates typed bytes from the persistent arena.
 * @param lifetime lifetime record
 * @param typeId   type identifier for the allocation header
 * @param Bytes    payload byte count
 * @return verified aligned pointer, or nullptr on failure
 */
any Lifetime_allocPersistent(Lifetime *lifetime, u64 typeId, bytes Bytes);

/**
 * Allocates typed bytes from the transient arena.
 * @param lifetime lifetime record
 * @param typeId   type identifier for the allocation header
 * @param Bytes    payload byte count
 * @return verified aligned pointer, or nullptr on failure
 */
any Lifetime_allocTransient(Lifetime *lifetime, u64 typeId, bytes Bytes);

/**
 * Frees a pointer previously allocated from the persistent arena.
 * @param lifetime lifetime record
 * @param ptr      pointer to release
 */
fn Lifetime_freePersistent(Lifetime *lifetime, any ptr);

/**
 * Binds externally supplied opaque arenas after verifying legitimacy.
 * @param persistentArena external persistent arena handle
 * @param transientArena  external transient arena handle
 * @param persistentType  provider attestation type id for persistent arena
 * @param transientType   provider attestation type id for transient arena
 * @param relational      optional relational engine symbol root
 * @return validated Lifetime record, or zeroed on failure
 */
Lifetime Lifetime_bind(any persistentArena, any transientArena,
                       u64 persistentType, u64 transientType,
                       any relational);

/**
 * Returns the bound relational symbol root.
 * @param lifetime lifetime record
 * @return relational symbol root handle, or nullptr
 */
any Lifetime_getRelational(const Lifetime *lifetime);

/**
 * Updates the relational symbol root after validating pointer legitimacy.
 * @param lifetime   lifetime record
 * @param relational new relational symbol root handle
 */
fn Lifetime_setRelational(Lifetime *lifetime, any relational);

/**
 * Returns the persistent arena handle.
 * @param lifetime lifetime record
 * @return persistent arena handle, or nullptr
 */
any Lifetime_getPersistentArena(const Lifetime *lifetime);

/**
 * Returns the transient arena handle.
 * @param lifetime lifetime record
 * @return transient arena handle, or nullptr
 */
any Lifetime_getTransientArena(const Lifetime *lifetime);

#endif

#include "lifetime.h"

#include <stdint.h>
#include <stddef.h>

#include "annotation/annotation.h"

;;DEFINITION
/**
 * ============================================================================
 * DEFINITION: Lifetime
 * ============================================================================
 * memory substrate abstraction bridging R1 host execution with R2 persistent
 * and transient memory arenas. hotcwap operates purely on opaque void* pointers
 * without including vexspoke headers, preserving decoupling while strictly
 * validating pointer legitimacy (16-byte alignment, non-null, user address range >= 64KB).
 *
 * Lifetime encapsulates the persistent session arena, per-tick transient scratch
 * arena, provider type attestations, and relational root. Allocations and binds
 * verify memory consistency across module reloads, ensuring zero heap corruption.
 * ============================================================================
 */

;;OVERVIEW
/**
 * ============================================================================
 * CLASS: Lifetime (spoke/lifetime.c — defined in spoke/lifetime.h)
 * ============================================================================
 * SUMMARY:
 *   Provides the memory substrate for the R0 host kernel. Owns the master
 *   persistent arena and the per-tick transient scratch arena as opaque void* handles.
 *
 *   R0 Host (lifetime) NEVER includes higher-tier headers. Instead,
 *   lifetime treats all memory pointers as opaque void* and actively enforces
 *   pointer legitimacy via Lifetime_isLegit:
 *     1. Non-null pointer check
 *     2. 16-byte alignment doctrine ((uintptr_t)ptr & 15 == 0)
 *     3. Valid user address range (ptr >= 0x10000 to avoid null/guard pages)
 *
 *   All allocations and bindings are validated through Lifetime_isLegit and
 *   Lifetime_isValid to ensure memory integrity across dynamic hot-reloads.
 *
 * STRUCT FIELDS (Mirroring spoke/lifetime.h — exactly this file's class):
 * ----------------------------------------------------------------------------
 *   void *persistentArena;    // master session arena (opaque void*)
 *   void *transientArena;     // per-tick scratch arena (opaque void*)
 *   uint64_t persistentType;  // LIFETIME_TYPE_ARENA (nonzero = attested)
 *   uint64_t transientType;   // LIFETIME_TYPE_ARENA (nonzero = attested)
 *   void *relational;         // relational symbol root (opaque void*)
 *
 * PRIVATE HELPERS:
 * ----------------------------------------------------------------------------
 *   (none)
 *
 * FUNCTION REGISTRY:
 * ----------------------------------------------------------------------------
 * Public Constructors: (.h)
 *   - Lifetime_create(persistentBytes, transientBytes) : Allocate verified pair of arenas
 *   - Lifetime_bind(pArena, tArena, pType, tType, rel) : Wrap external verified arenas
 *
 * Private Constructors: (.c static)
 *   - (none)
 *
 * Public Core Functions: (.h)
 *   - Lifetime_isLegit(ptr)                            : Verify 16B alignment & user address
 *   - Lifetime_isValid(lifetime)                       : Validate arena handles and types
 *   - Lifetime_destroy(lifetime)                       : Release arena resources
 *   - Lifetime_resetTransient(lifetime)                : Free all scratch allocations
 *   - Lifetime_allocPersistent(lifetime, type, Bytes)  : Allocate from master arena
 *   - Lifetime_allocTransient(lifetime, type, Bytes)   : Allocate from scratch arena
 *   - Lifetime_freePersistent(lifetime, ptr)           : Free master arena allocation
 *
 * Private Core Functions: (.c static)
 *   - (none)
 *
 * Public Setters: (.h)
 *   - (none)
 *
 * Private Setters: (.c static)
 *   - (none)
 *
 * Public Getters: (.h)
 *   - (none)
 *
 * Private Getters: (.c static)
 *   - (none)
 * ============================================================================
 */

// Dynamic link symbols provided by the underlying memory runtime (pure void* ABI).
// hotcwap does NOT include nio/mem.h or any vexspoke header files.
extern void *MemoryArena_create(size_t totalBytes);
extern void MemoryArena_destroy(void *arena);
extern void MemoryArena_freeAll(void *arena);
extern void *MemoryArena_alloc(void *arena, uint64_t typeId, size_t numBytes);
extern void MemoryArena_free(void *arena, void *ptr);

// CONSTRUCTORS (PUBLIC & PRIVATE)
/** Creates persistent and transient arenas, substituting defaults for zero sizes. */
Lifetime Lifetime_create(size_t persistentBytes, size_t transientBytes) {
    Lifetime lt = {0};
    if (persistentBytes == 0)
        persistentBytes = 64 * 1024 * 1024;
    if (transientBytes == 0)
        transientBytes = 64 * 1024 * 1024;

    void *p = MemoryArena_create(persistentBytes);
    if (!Lifetime_isLegit(p)) {
        if (p != nullptr) {
            MemoryArena_destroy(p);
        }
        return lt;
    }

    void *t = MemoryArena_create(transientBytes);
    if (!Lifetime_isLegit(t)) {
        MemoryArena_destroy(p);
        if (t != nullptr) {
            MemoryArena_destroy(t);
        }
        return lt;
    }

    lt.persistentArena = p;
    lt.transientArena = t;
    lt.persistentType = VEXSPOKE_TYPE_ARENA;
    lt.transientType = VEXSPOKE_TYPE_ARENA;
    lt.relational = nullptr;
    return lt;
}

/** Wraps externally supplied arenas only when their pointers and type IDs validate. */
Lifetime Lifetime_bind(void *persistentArena, void *transientArena,
                       uint64_t persistentType, uint64_t transientType,
                       void *relational) {
    Lifetime lt = {
        .persistentArena = persistentArena,
        .transientArena = transientArena,
        .persistentType = persistentType,
        .transientType = transientType,
        .relational = relational,
    };
    if (!Lifetime_isValid(&lt)) {
        Lifetime zero = {0};
        return zero;
    }
    return lt;
}

// CORE FUNCTIONS (PUBLIC & PRIVATE)
/** Checks non-null, 16-byte alignment, and the minimum user-address threshold. */
bool Lifetime_isLegit(const void *ptr) {
    if (ptr == nullptr) {
        return false;
    }
    uintptr_t addr = (uintptr_t) ptr;
    // 16-byte arena alignment doctrine
    if ((addr & 15) != 0) {
        return false;
    }
    // Guard page avoidance: valid user-space pointer must be >= 64KB (0x10000)
    if (addr < 0x10000ULL) {
        return false;
    }
    return true;
}

/** Validates both arena handles, their attestations, and an optional relational root. */
bool Lifetime_isValid(const Lifetime *lifetime) {
    if (lifetime == nullptr) {
        return false;
    }
    if (!Lifetime_isLegit((*lifetime).persistentArena)) {
        return false;
    }
    if (!Lifetime_isLegit((*lifetime).transientArena)) {
        return false;
    }
    if ((*lifetime).persistentType == 0 || (*lifetime).transientType == 0) {
        return false;
    }
    if ((*lifetime).relational != nullptr && !Lifetime_isLegit((*lifetime).relational)) {
        return false;
    }
    return true;
}

/** Destroys valid transient and persistent arenas and clears the record fields. */
void Lifetime_destroy(Lifetime *lifetime) {
    if (lifetime == nullptr)
        return;
    if ((*lifetime).transientArena != nullptr) {
        if (Lifetime_isLegit((*lifetime).transientArena)) {
            MemoryArena_destroy((*lifetime).transientArena);
        }
        (*lifetime).transientArena = nullptr;
        (*lifetime).transientType = 0;
    }
    if ((*lifetime).persistentArena != nullptr) {
        if (Lifetime_isLegit((*lifetime).persistentArena)) {
            MemoryArena_destroy((*lifetime).persistentArena);
        }
        (*lifetime).persistentArena = nullptr;
        (*lifetime).persistentType = 0;
    }
    (*lifetime).relational = nullptr;
}

/** Releases all allocations from the valid transient arena. */
void Lifetime_resetTransient(Lifetime *lifetime) {
    if (lifetime == nullptr || !Lifetime_isLegit((*lifetime).transientArena))
        return;
    MemoryArena_freeAll((*lifetime).transientArena);
}

/** Allocates typed bytes from the persistent arena and validates the result pointer. */
void *Lifetime_allocPersistent(Lifetime *lifetime, uint64_t typeId, size_t Bytes) {
    if (lifetime == nullptr || !Lifetime_isLegit((*lifetime).persistentArena))
        return nullptr;
    void *ptr = MemoryArena_alloc((*lifetime).persistentArena, typeId, Bytes);
    if (!Lifetime_isLegit(ptr)) {
        return nullptr;
    }
    return ptr;
}

/** Allocates typed bytes from the transient arena and validates the result pointer. */
void *Lifetime_allocTransient(Lifetime *lifetime, uint64_t typeId, size_t Bytes) {
    if (lifetime == nullptr || !Lifetime_isLegit((*lifetime).transientArena))
        return nullptr;
    void *ptr = MemoryArena_alloc((*lifetime).transientArena, typeId, Bytes);
    if (!Lifetime_isLegit(ptr)) {
        return nullptr;
    }
    return ptr;
}

/** Frees a validated pointer through the persistent arena. */
void Lifetime_freePersistent(Lifetime *lifetime, void *ptr) {
    if (lifetime == nullptr || !Lifetime_isLegit((*lifetime).persistentArena) || !Lifetime_isLegit(ptr))
        return;
    MemoryArena_free((*lifetime).persistentArena, ptr);
}

// ACCESSORS (PUBLIC)
/** Gets the bound relational symbol root (null-safe). */
void *Lifetime_getRelational(const Lifetime *lifetime) {
    if (lifetime == nullptr)
        return nullptr;
    return (*lifetime).relational;
}

/** Sets the relational symbol root after validating pointer legitimacy. */
void Lifetime_setRelational(Lifetime *lifetime, void *relational) {
    if (lifetime == nullptr)
        return;
    if (relational != nullptr && !Lifetime_isLegit(relational))
        return;
    (*lifetime).relational = relational;
}

/** Gets the persistent arena handle (null-safe). */
void *Lifetime_getPersistentArena(const Lifetime *lifetime) {
    if (lifetime == nullptr)
        return nullptr;
    return (*lifetime).persistentArena;
}

/** Gets the transient arena handle (null-safe). */
void *Lifetime_getTransientArena(const Lifetime *lifetime) {
    if (lifetime == nullptr)
        return nullptr;
    return (*lifetime).transientArena;
}

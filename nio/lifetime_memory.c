#include "vex.h"
#include "nio/lifetime_memory.h"

;;DEFINITION
/**
 * ============================================================================
 * DEFINITION: LifetimeMemory (lifetime/nio/lifetime_memory.c)
 * ============================================================================
 * Per-library MemoryArena owner for Lifetime, backed by Relational Engine's
 * MemoryPool. Starts at zero OS pages and grows on demand until OS OOM.
 * ============================================================================
 */

;;OVERVIEW
/**
 * ============================================================================
 * MODULE: LifetimeMemory (nio/lifetime_memory)
 * ============================================================================
 * Lazily creates and shuts down Lifetime's dedicated MemoryArena.
 * ============================================================================
 */

static MemoryArena *s_lifetimeArena = nullptr;

/** Lazily create and return Lifetime's page-pooled MemoryArena. */
MemoryArena *Lifetime_arena() {
    if (s_lifetimeArena == nullptr)
        s_lifetimeArena = MemoryArena();
    return s_lifetimeArena;
}

/** Destroy Lifetime's MemoryArena and return all pages to MemoryPool. */
fn Lifetime_arenaShutdown() {
    if (s_lifetimeArena != nullptr) {
        MemoryArena_destroy(s_lifetimeArena);
        s_lifetimeArena = nullptr;
    }
}

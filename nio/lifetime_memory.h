#ifndef NIO_LIFETIME_MEMORY_H
#define NIO_LIFETIME_MEMORY_H

#include "vex.h"
#include "nio/mem.h"

MemoryArena *Lifetime_arena();
fn Lifetime_arenaShutdown();

#endif

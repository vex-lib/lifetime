#ifndef CORE_PROCESS_SPAWN_H
#define CORE_PROCESS_SPAWN_H

#include "vex.h"
#include "re/type/re_type.h"

// INTENTIONAL(vex): Lifetime has no project byte in the type registry yet, so
// the block keeps its original re-owned id until the host gets its own byte.
#define TYPE_PROCESS_SPAWN_SINGLETON Type_make(PROJ_RELATIONAL_ENGINE, FORM_SINGLETON, 5u)

#define PROCESS_SPAWN_JOBS_MAX 8u
#define PROCESS_SPAWN_POLL_MAX_NS 100000000ULL

// SLOT RECORD — one child job row (zero behavior of its own; all behavior
// hangs off the ProcessSpawn table class).
typedef struct ProcessSpawnJob {
    i32 pid;
    i32 exitCode;
    bool done;
} ProcessSpawnJob;

;;CLASSNAME("ProcessSpawn")
typedef struct ProcessSpawn {
    i32 pid;
    u64 timeoutMs;
    i32 exitCode;
    bool cancelFlag;
    ProcessSpawnJob jobs[PROCESS_SPAWN_JOBS_MAX];
    u32 count;
} ProcessSpawn;

ProcessSpawn *ProcessSpawn_0();
ProcessSpawn *ProcessSpawn_1(u64 timeoutMs);
#define ProcessSpawn(...) CONSTRUCTOR_DISPATCH(ProcessSpawn, __VA_ARGS__)

fn ProcessSpawn_free(ProcessSpawn *self);
bool ProcessSpawn_spawn(const char *const *argv, u64 timeoutMs, ProcessSpawn *dest);
bool ProcessSpawn_poll(ProcessSpawn *self, u64 budgetNs);
fn ProcessSpawn_cancel(ProcessSpawn *self);

fn ProcessSpawn_setPid(ProcessSpawn *self, i32 pid);
i32 ProcessSpawn_getPid(const ProcessSpawn *self);
fn ProcessSpawn_setTimeoutMs(ProcessSpawn *self, u64 timeoutMs);
u64 ProcessSpawn_getTimeoutMs(const ProcessSpawn *self);
fn ProcessSpawn_setExitCode(ProcessSpawn *self, i32 exitCode);
i32 ProcessSpawn_getExitCode(const ProcessSpawn *self);
fn ProcessSpawn_setCancelFlag(ProcessSpawn *self, bool cancelFlag);
bool ProcessSpawn_isCancelFlag(const ProcessSpawn *self);
u32 ProcessSpawn_getCount(const ProcessSpawn *self);
i32 ProcessSpawn_getJobPid(const ProcessSpawn *self, u32 i);
i32 ProcessSpawn_getJobExitCode(const ProcessSpawn *self, u32 i);
bool ProcessSpawn_isJobDone(const ProcessSpawn *self, u32 i);

#endif

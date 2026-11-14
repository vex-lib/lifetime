#include "vex.h"
#include "process_spawn.h"
// Lifetime host; bounded child-job ABI preserved.

#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "re/nio/mem.h"

extern char **environ;

;;DEFINITION
/**
 * ============================================================================
 * DEFINITION: ProcessSpawn
 * ============================================================================
 * A bounded child-process job table in R0 Lifetime: fixed slots
 * (PROCESS_SPAWN_JOBS_MAX), per-job pid/exit/done rows, table-level mirrors,
 * a cancel flag, and a timeout — zero steady-state allocation, no threads.
 * Spawns via posix_spawnp (never system(), never a blocking waitpid, never
 * UINT64_MAX); poll reaps with WNOHANG in ~1ms slices up to a 100ms budget,
 * and cancel raises the flag and SIGTERMs unfinished jobs. This is the
 * decoder-binary seam: callers such as graphvex FrameImporter spawn external
 * decoders through this shape instead of popen or libav links.
 * ============================================================================
 */

;;OVERVIEW
/**
 * ============================================================================
 * CLASS: ProcessSpawn (core/process_spawn.c)
 * ============================================================================
 * A bounded child-process job table in R0 Lifetime. Fixed slots
 * (PROCESS_SPAWN_JOBS_MAX), per-job pid/exit/done rows, table-level
 * mirrors, cancel flag, and timeout — zero steady-state allocation, no
 * threads. Spawns via posix_spawnp (never system(), never a blocking
 * waitpid, never UINT64_MAX): ProcessSpawn_poll reaps with WNOHANG in
 * ~1ms slices up to budgetNs clamped to PROCESS_SPAWN_POLL_MAX_NS
 * (100ms, the Bounded Wait Law); ProcessSpawn_cancel raises the flag and SIGTERMs
 * unfinished jobs. The decoder-binary seam: callers (e.g. graphvex
 * FrameImporter) spawn external decoders through this shape instead of
 * popen/libav links. Reaping decrements the live count exactly once; completed
 * slots are reusable. Destruction requires the caller to finish reaping jobs.
 *
 * STRUCT FIELDS (Mirroring core/process_spawn.h — exactly this file's class):
 * ----------------------------------------------------------------------------
 *   ProcessSpawn {
 *     i32 pid;                                // most recent child pid
 *     u64 timeoutMs;                         // default spawn budget (ms)
 *     i32 exitCode;                           // last reaped exit code
 *     bool cancelFlag;                            // cancel raised; poll degrades
 *     ProcessSpawnJob jobs[PROCESS_SPAWN_JOBS_MAX]; // fixed table, no alloc
 *     u32 count;                             // live jobs (0..JOBS_MAX)
 *   }
 *
 * SLOT RECORD (ProcessSpawnJob — Single Class Per File Law, zero behavior):
 * ----------------------------------------------------------------------------
 *   i32 pid;       // child pid (> 0 tracked, 0 = slot free)
 *   i32 exitCode;  // WEXITSTATUS / -signal once done
 *   bool done;         // true once waitpid has reaped the child
 *
 * FUNCTION REGISTRY:
 * ----------------------------------------------------------------------------
 * Constructors:
 *   - ProcessSpawn()              : ProcessSpawn_0()
 *   - ProcessSpawn(timeoutMs)     : ProcessSpawn_1(timeoutMs)
 *
 * Core Functions:
 *   - ProcessSpawn_free(self)
 *   - ProcessSpawn_spawn(argv, timeoutMs, dest)
 *   - ProcessSpawn_poll(self, budgetNs)
 *   - ProcessSpawn_cancel(self)
 *
 * Setters:
 *   - ProcessSpawn_setPid(self, pid)
 *   - ProcessSpawn_setTimeoutMs(self, timeoutMs)
 *   - ProcessSpawn_setExitCode(self, exitCode)
 *   - ProcessSpawn_setCancelFlag(self, cancelFlag)
 *
 * Getters:
 *   - ProcessSpawn_getPid(self)
 *   - ProcessSpawn_getTimeoutMs(self)
 *   - ProcessSpawn_getExitCode(self)
 *   - ProcessSpawn_isCancelFlag(self)
 *   - ProcessSpawn_getCount(self)
 *   - ProcessSpawn_getJobPid(self, i)
 *   - ProcessSpawn_getJobExitCode(self, i)
 *   - ProcessSpawn_isJobDone(self, i)
 * ============================================================================
 */

// core/process_spawn.c — ProcessSpawn port. posix_spawnp launch, WNOHANG
// reap slices, SIGTERM cancel. No system(), no blocking wait, no threads.

/** Read monotonic time in nanoseconds for bounded child polling. */
static u64 processSpawnNowNs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64) ts.tv_sec * ONE_BILLION + (u64) ts.tv_nsec;
}

/** Sleep for one short interval between non-blocking child reaps. */
static fn processSpawnSleepSlice() {
    struct timespec slice;
    slice.tv_sec = 0;
    slice.tv_nsec = ONE_MILLION;
    nanosleep(&slice, nullptr);
}

/** Allocate and initialize an empty fixed-slot child-job table. */
static ProcessSpawn *processSpawnCreate(u64 timeoutMs) {
    ProcessSpawn *self = Memory_alloc(TYPE_PROCESS_SPAWN_SINGLETON, sizeof(ProcessSpawn));
    if (!self)
        return nullptr;
    (*self).pid = ZERO;
    (*self).timeoutMs = timeoutMs;
    (*self).exitCode = 0;
    (*self).cancelFlag = false;
    for (u32 i = 0; i < PROCESS_SPAWN_JOBS_MAX; i++) {
        ProcessSpawnJob *job = &(*self).jobs[i];
        (*job).pid = 0;
        (*job).exitCode = 0;
        (*job).done = true;
    }
    (*self).count = 0;
    return self;
}

/** Reap each completed child once without blocking and update its slot and live count. */
static bool processSpawnReapOnce(ProcessSpawn *self) {
    bool allDone = true;
    for (u32 i = 0; i < PROCESS_SPAWN_JOBS_MAX; i++) {
        ProcessSpawnJob *job = &(*self).jobs[i];
        if ((*job).pid == 0 || (*job).done)
            continue;
        int status = 0;
        pid_t got = waitpid((pid_t)(*job).pid, &status, WNOHANG);
        if (got == 0) {
            allDone = false;
            continue;
        }
        if (got < 0) {
            (*job).exitCode = -1;
            (*job).done = true;
            (*self).count -= 1;
            continue;
        }
        if (WIFEXITED(status))
            (*job).exitCode = WEXITSTATUS(status);
        else if (WIFSIGNALED(status))
            (*job).exitCode = -WTERMSIG(status);
        else
            (*job).exitCode = -1;
        (*job).done = true;
        (*self).count -= 1;
        (*self).exitCode = (*job).exitCode;
    }
    return allDone;
}

// CONSTRUCTORS
/** Construct a process-spawn table with a zero default timeout. */
ProcessSpawn *ProcessSpawn_0() {
    return processSpawnCreate(0);
}

/** Construct a process-spawn table with the supplied default timeout. */
ProcessSpawn *ProcessSpawn_1(u64 timeoutMs) {
    return processSpawnCreate(timeoutMs);
}

// CORE FUNCTIONS
/** Release the table allocation; callers must first stop and reap its child jobs. */
fn ProcessSpawn_free(ProcessSpawn *self) {
    if (!self)
        return;
    Memory_free(self);
}

/** Start argv[0] through posix_spawnp in a free job slot and record its pid and timeout. */
bool ProcessSpawn_spawn(const char *const *argv, u64 timeoutMs,
                        ProcessSpawn *dest) {
    if (!argv || !(*argv) || !dest)
        return false;
    if ((*dest).cancelFlag)
        return false;
    ProcessSpawnJob *slot = nullptr;
    for (u32 i = 0; i < PROCESS_SPAWN_JOBS_MAX; i++) {
        ProcessSpawnJob *job = &(*dest).jobs[i];
        if ((*job).pid == 0 || (*job).done) {
            slot = job;
            break;
        }
    }
    if (!slot)
        return false;
    pid_t child = 0;
    char *const *args = (char *const*) argv;
    int rc = posix_spawnp(&child, (*argv), nullptr, nullptr, args, environ);
    if (rc != 0)
        return false;
    (*slot).pid = (i32) child;
    (*slot).exitCode = 0;
    (*slot).done = false;
    (*dest).pid = (i32) child;
    (*dest).timeoutMs = timeoutMs;
    (*dest).count += 1;
    return true;
}

/** Reap child jobs in bounded non-blocking slices; return true only when all jobs are done. */
bool ProcessSpawn_poll(ProcessSpawn *self, u64 budgetNs) {
    if (!self)
        return false;
    if (budgetNs > PROCESS_SPAWN_POLL_MAX_NS)
        budgetNs = PROCESS_SPAWN_POLL_MAX_NS;
    if ((*self).cancelFlag)
        return processSpawnReapOnce(self);
    u64 start = processSpawnNowNs();
    for (;;) {
        bool allDone = processSpawnReapOnce(self);
        if (allDone)
            return true;
        if ((*self).cancelFlag)
            return false;
        if (processSpawnNowNs() - start >= budgetNs)
            return false;
        processSpawnSleepSlice();
    }
}

/** Mark the table cancelled and send SIGTERM to every unfinished tracked child. */
fn ProcessSpawn_cancel(ProcessSpawn *self) {
    if (!self)
        return;
    (*self).cancelFlag = true;
    for (u32 i = 0; i < PROCESS_SPAWN_JOBS_MAX; i++) {
        ProcessSpawnJob *job = &(*self).jobs[i];
        if ((*job).pid != ZERO && !(*job).done)
            kill((*job).pid, SIGTERM);
    }
}

// SETTERS
/** Replace the table's mirrored most-recent pid field. */
fn ProcessSpawn_setPid(ProcessSpawn *self, i32 pid) {
    if (!self)
        return;
    (*self).pid = pid;
}

/** Replace the table's default timeout value in milliseconds. */
fn ProcessSpawn_setTimeoutMs(ProcessSpawn *self, u64 timeoutMs) {
    if (!self)
        return;
    (*self).timeoutMs = timeoutMs;
}

/** Replace the table's mirrored last exit-code field. */
fn ProcessSpawn_setExitCode(ProcessSpawn *self, i32 exitCode) {
    if (!self)
        return;
    (*self).exitCode = exitCode;
}

/** Set or clear the cancellation flag without signaling jobs. */
fn ProcessSpawn_setCancelFlag(ProcessSpawn *self, bool cancelFlag) {
    if (!self)
        return;
    (*self).cancelFlag = cancelFlag;
}

// GETTERS
/** Return the most-recent pid field, or zero for a null table. */
i32 ProcessSpawn_getPid(const ProcessSpawn *self) {
    if (!self)
        return ZERO;
    return (*self).pid;
}

/** Return the default timeout in milliseconds, or zero for a null table. */
u64 ProcessSpawn_getTimeoutMs(const ProcessSpawn *self) {
    if (!self)
        return ZERO;
    return (*self).timeoutMs;
}

/** Return the last reaped exit code, or zero for a null table. */
i32 ProcessSpawn_getExitCode(const ProcessSpawn *self) {
    if (!self)
        return 0;
    return (*self).exitCode;
}

/** Return the cancellation state, treating a null table as cancelled. */
bool ProcessSpawn_isCancelFlag(const ProcessSpawn *self) {
    if (!self)
        return true;
    return (*self).cancelFlag;
}

/** Return the number of child jobs not yet reaped, or zero for a null table. */
u32 ProcessSpawn_getCount(const ProcessSpawn *self) {
    if (!self)
        return 0;
    return (*self).count;
}

/** Return a job slot's pid, or zero for a null table or out-of-range slot. */
i32 ProcessSpawn_getJobPid(const ProcessSpawn *self, u32 i) {
    if (!self)
        return 0;
    if (i >= PROCESS_SPAWN_JOBS_MAX)
        return 0;
    const ProcessSpawnJob *job = &(*self).jobs[i];
    return (*job).pid;
}

/** Return a job slot's exit code, or zero for a null table or out-of-range slot. */
i32 ProcessSpawn_getJobExitCode(const ProcessSpawn *self, u32 i) {
    if (!self)
        return 0;
    if (i >= PROCESS_SPAWN_JOBS_MAX)
        return 0;
    const ProcessSpawnJob *job = &(*self).jobs[i];
    return (*job).exitCode;
}

/** Return whether a job slot is marked done; invalid table or index defaults to true. */
bool ProcessSpawn_isJobDone(const ProcessSpawn *self, u32 i) {
    if (!self)
        return true;
    if (i >= PROCESS_SPAWN_JOBS_MAX)
        return true;
    const ProcessSpawnJob *job = &(*self).jobs[i];
    return (*job).done;
}

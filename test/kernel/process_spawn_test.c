// Engine child table: actual spawning, exhaustion without mutation, reap/reuse,
// cancellation-before-free and null-safe accessors. Outer runner has a watchdog.
// Existing silent errors, destructor precondition and absent toString are gaps.
#include "core/process_spawn.h"
#include "re/nio/mem.h"
#include <assert.h>
#include <time.h>

/* Reads a monotonic timestamp for the bounded child-process reap watchdog. */
static u64 nowNs(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (u64) t.tv_sec * 1000000000ULL + (u64) t.tv_nsec;
}

/* Polls a child until completion while enforcing a two-second external bound. */
static void reap(ProcessSpawn *owner) {
    u64 start = nowNs();
    while (!ProcessSpawn_poll(owner, PROCESS_SPAWN_POLL_MAX_NS))
        assert(nowNs() - start < 2000000000ULL);
}

/* Proves spawn, capacity, reap/reuse, cancellation, and null-safe accessors. */
int main(void) {
    ProcessSpawn *a = ProcessSpawn();
    ProcessSpawn *b = ProcessSpawn(20);
    assert(a && b && ProcessSpawn_getTimeoutMs(b) == 20);
    assert(ProcessSpawn_getPid(nullptr) == 0 && ProcessSpawn_getExitCode(nullptr) == 0);
    assert(ProcessSpawn_getTimeoutMs(nullptr) == 0 && ProcessSpawn_getCount(nullptr) == 0);
    assert(ProcessSpawn_isCancelFlag(nullptr));
    assert(ProcessSpawn_getJobPid(nullptr, 0) == 0);
    assert(ProcessSpawn_getJobExitCode(nullptr, 0) == 0);
    assert(ProcessSpawn_isJobDone(nullptr, 0));
    assert(ProcessSpawn_poll(a, 0));
    ProcessSpawn_setPid(a, 7); ProcessSpawn_setTimeoutMs(a, 9);
    ProcessSpawn_setExitCode(a, 11); ProcessSpawn_setCancelFlag(a, true);
    assert(ProcessSpawn_getPid(a) == 7 && ProcessSpawn_getTimeoutMs(a) == 9);
    assert(ProcessSpawn_getExitCode(a) == 11 && ProcessSpawn_isCancelFlag(a));
    const char *args[] = {"/usr/bin/true", nullptr};
    const char *invalid[] = {"/nonexistent-re-owner-program", nullptr};
    const char *empty[] = {nullptr};
    assert(!ProcessSpawn_spawn(args, 0, a));
    ProcessSpawn_setCancelFlag(a, false);
    assert(!ProcessSpawn_spawn(nullptr, 0, a));
    assert(!ProcessSpawn_spawn(empty, 0, a));
    assert(!ProcessSpawn_spawn(args, 0, nullptr));
    assert(!ProcessSpawn_spawn(invalid, 0, a));
    assert(ProcessSpawn_getPid(a) == 7 && ProcessSpawn_getCount(a) == 0);
    for (unsigned i = 0; i < PROCESS_SPAWN_JOBS_MAX; ++i)
        assert(ProcessSpawn_spawn(args, 0, a));
    assert(!ProcessSpawn_spawn(args, 0, a));
    assert(ProcessSpawn_getCount(a) == PROCESS_SPAWN_JOBS_MAX);
    reap(a);
    assert(ProcessSpawn_getCount(a) == 0);
    for (unsigned i = 0; i < PROCESS_SPAWN_JOBS_MAX; ++i) {
        assert(ProcessSpawn_isJobDone(a, i));
        assert(ProcessSpawn_getJobPid(a, i) > 0);
        assert(ProcessSpawn_getJobExitCode(a, i) == 0);
    }
    assert(ProcessSpawn_getJobPid(a, UINT32_MAX) == 0);
    assert(ProcessSpawn_getJobExitCode(a, UINT32_MAX) == 0);
    assert(ProcessSpawn_isJobDone(a, UINT32_MAX));
    assert(ProcessSpawn_spawn(args, 0, a));
    assert(ProcessSpawn_getCount(a) == 1);
    reap(a);
    assert(ProcessSpawn_getCount(a) == 0);
    const char *pending[] = {"/bin/sleep", "30", nullptr};
    assert(ProcessSpawn_spawn(pending, 0, b));
    ProcessSpawn_cancel(b);
    reap(b);
    assert(ProcessSpawn_getCount(b) == 0);
    assert(ProcessSpawn_isCancelFlag(b));
    ProcessSpawn_setPid(nullptr, 0); ProcessSpawn_setTimeoutMs(nullptr, 0);
    ProcessSpawn_setExitCode(nullptr, 0); ProcessSpawn_setCancelFlag(nullptr, false);
    ProcessSpawn_cancel(nullptr); assert(!ProcessSpawn_poll(nullptr, 0));
    ProcessSpawn_free(a); ProcessSpawn_free(b); ProcessSpawn_free(nullptr);
    Memory_freeAll();
    return 0;
}

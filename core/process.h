#ifndef HOT_PROCESS_PROCESS_H
#define HOT_PROCESS_PROCESS_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

// One caller-thread invocation at a time. Name, context and hot association
// are borrowed; code and data must remain live while referenced. No module
// pin, thread or cancellation mechanism is created here. Before free,
// deregister and externally exclude ALL concurrent API callers.
typedef int (*ProcessEntry)(void *context);

typedef enum ProcessResult {
    PROCESS_OK = 0,
    PROCESS_INVALID,
    PROCESS_BUSY
} ProcessResult;

typedef struct Process {
    _Atomic(ProcessEntry) entry;
    _Atomic(void*) context;
    _Atomic(void*) hot;             // association only, NOT a loader pin
    _Atomic(const char*) name;
    atomic_bool occupied;          // admission for run, replace, mutation, free
    atomic_bool inFlight;          // callback execution telemetry
    atomic_uint invocationCount;   // completed callbacks
} Process;

// Null entries fail construction; null context/name are allowed.
Process *Process_1(ProcessEntry entry);
Process *Process_2(ProcessEntry entry, void *context);
Process *Process_3(const char *name, ProcessEntry entry, void *context);

#define PROCESS_CHOOSER(_1, _2, _3, NAME, ...) NAME
#define Process(...) PROCESS_CHOOSER(__VA_ARGS__, Process_3, Process_2, Process_1)(__VA_ARGS__)

// Admission status is separate from callback result (Dest-Last Law).
// exitStatus is required and untouched on failure. No waiting or allocation.
ProcessResult Process_run(Process *self, int *exitStatus);

// Replace the entire binding between calls; hot may be null for unmanaged code.
// Busy/invalid leaves the old binding intact. Does not free data or unload code.
ProcessResult Process_replace(Process *self, ProcessEntry entry, void *context, void *hot);
ProcessResult Process_setName(Process *self, const char *name);

// False on null/busy. Exclude new users before attempting free.
bool Process_free(Process *self);
bool Process_isRunning(const Process *self);
uint32_t Process_getInvocationCount(const Process *self);

// Individual atomic observations, NOT a coherent multi-field snapshot or
// permission to call/unload entry. Invoke exclusively via Process_run.
ProcessEntry Process_getEntry(const Process *self);
void *Process_getContext(const Process *self);
void *Process_getHot(const Process *self);
const char *Process_getName(const Process *self);

#endif

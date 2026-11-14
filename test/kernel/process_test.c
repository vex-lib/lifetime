#include "core/kernel.h"
#include "util/annotation.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

;;OVERVIEW
/**
 * MODULE: ProcessTest
 * LEVEL: L3 — Module Code (headless contract harness)
 * Tests borrowed bindings, replacement, admission errors and Kernel dispatch.
 * PRIVATE HELPERS: TestCall stores process, rendezvous flags and worker results.
 * FUNCTIONS: original, replacement, reentrant, waitFor, heldEntry, worker, main.
 */
static int failures;
#define CHECK(label, condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s (line %d)\n", label, __LINE__); \
        failures++; \
    } \
} while (0)

typedef struct TestCall {
    Process *process;
    atomic_bool entered;
    atomic_bool release;
    pthread_t thread;
    ProcessResult result;
    int exitStatus;
} TestCall;

/** Returns the original entry's context value or its null-context sentinel. */
static int original(any context) {
    return context ? *(int*) context : 11;
}

/** Returns the replacement entry's offset value or its null-context sentinel. */
static int replacement(any context) {
    return context ? *(int*) context + 100 : 22;
}

/** Verifies nested invocation and mutation are refused while the process runs. */
static int reentrant(any context) {
    Process *process = (Process*) context;
    int result = 123;
    CHECK("running", Process_isRunning(process));
    CHECK("reentrant run busy", Process_run(process, &result) == PROCESS_BUSY);
    CHECK("busy output untouched", result == 123);
    CHECK("reentrant replace busy", Process_replace(process, original, nullptr, nullptr) == PROCESS_BUSY);
    CHECK("rename busy", Process_setName(process, "no") == PROCESS_BUSY);
    return -1;
}

// Bounded rendezvous; terminate the test rather than join a stuck worker.
static bool waitFor(atomic_bool *flag) {
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    const struct timespec slice = {0, 1000000L};
    while (!atomic_load_explicit(flag, memory_order_acquire)) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec - start.tv_sec >= 5)
            return false;
        nanosleep(&slice, nullptr);
    }
    return true;
}

/** Holds a process invocation until the test releases its bounded rendezvous. */
static int heldEntry(any context) {
    TestCall *call = (TestCall*) context;
    (*call).thread = pthread_self();
    atomic_store_explicit(&(*call).entered, true, memory_order_release);
    if (!waitFor(&(*call).release))
        _Exit(2);
    return 77;
}

/** Runs the held process callback on the test's worker thread. */
static any worker(any context) {
    TestCall *call = (TestCall*) context;
    (*call).result = Process_run((*call).process, &(*call).exitStatus);
    return nullptr;
}

int main() {
    int result = 123;
    int oldContext = -1;
    int newContext = 7;
    int association = 0;
    CHECK("null constructor", Process(nullptr) == nullptr);
    CHECK("null run", Process_run(nullptr, &result) == PROCESS_INVALID && result == 123);
    CHECK("null replace", Process_replace(nullptr, original, nullptr, nullptr) == PROCESS_INVALID);
    CHECK("null rename", Process_setName(nullptr, "x") == PROCESS_INVALID);
    CHECK("null getters", !Process_getEntry(nullptr) && !Process_getContext(nullptr)
          && !Process_getHot(nullptr) && !Process_getName(nullptr));
    CHECK("null telemetry", !Process_isRunning(nullptr) && Process_getInvocationCount(nullptr) == 0);
    CHECK("null free", !Process_free(nullptr));

    Process *process = Process("task", original, &oldContext);
    if (!process)
        return 2;
    CHECK("name", strcmp(Process_getName(process), "task") == 0);
    CHECK("missing destination", Process_run(process, nullptr) == PROCESS_INVALID);
    CHECK("original run", Process_run(process, &result) == PROCESS_OK && result == -1);
    CHECK("invalid replacement", Process_replace(process, nullptr, &newContext, &association) == PROCESS_INVALID);
    CHECK("invalid preserves binding", Process_getEntry(process) == original
          && Process_getContext(process) == &oldContext && !Process_getHot(process));
    CHECK("replace", Process_replace(process, replacement, &newContext, &association) == PROCESS_OK);
    CHECK("replacement binding", Process_getEntry(process) == replacement
          && Process_getContext(process) == &newContext && Process_getHot(process) == &association);
    CHECK("new run", Process_run(process, &result) == PROCESS_OK && result == 107);
    CHECK("rerun", Process_run(process, &result) == PROCESS_OK && result == 107);
    CHECK("counter", Process_getInvocationCount(process) == 3);
    CHECK("rename", Process_setName(process, nullptr) == PROCESS_OK && !Process_getName(process));
    CHECK("bind reentrant", Process_replace(process, reentrant, process, nullptr) == PROCESS_OK);
    CHECK("negative callback succeeds", Process_run(process, &result) == PROCESS_OK && result == -1);
    CHECK("free", Process_free(process));

    TestCall call = {0};
    atomic_init(&call.entered, false);
    atomic_init(&call.release, false);
    call.process = Process(heldEntry, &call);
    if (!call.process)
        return 2;
    pthread_t thread;
    if (pthread_create(&thread, nullptr, worker, &call) != 0)
        return 2;
    if (!waitFor(&call.entered))
        _Exit(2);
    CHECK("caller thread", pthread_equal(thread, call.thread));
    CHECK("running worker", Process_isRunning(call.process));
    result = 123;
    CHECK("parallel run busy", Process_run(call.process, &result) == PROCESS_BUSY && result == 123);
    CHECK("parallel replace busy", Process_replace(call.process, replacement, &newContext, nullptr) == PROCESS_BUSY);
    CHECK("binding unchanged", Process_getEntry(call.process) == heldEntry
          && Process_getContext(call.process) == &call);
    atomic_store_explicit(&call.release, true, memory_order_release);
    CHECK("join", pthread_join(thread, nullptr) == 0);
    CHECK("worker result", call.result == PROCESS_OK && call.exitStatus == 77);
    CHECK("completed", !Process_isRunning(call.process) && Process_getInvocationCount(call.process) == 1);
    CHECK("replace after completion", Process_replace(call.process, replacement, &newContext, nullptr) == PROCESS_OK);
    CHECK("run after completion", Process_run(call.process, &result) == PROCESS_OK && result == 107);
    CHECK("free worker process", Process_free(call.process));

    Process *simple = Process(original);
    Kernel *kernel = Kernel();
    if (!simple || !kernel)
        return 2;
    CHECK("kernel single dispatch", Kernel_run(kernel, simple) == 11);
    CHECK("register", Kernel_addProcess(kernel, simple));
    CHECK("kernel all dispatch", Kernel_run(kernel) == 11);
    CHECK("deregister", Kernel_removeProcess(kernel, simple));
    CHECK("free simple", Process_free(simple));
    CHECK("free kernel", Kernel_free(kernel));
    printf("Process contract tests: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}

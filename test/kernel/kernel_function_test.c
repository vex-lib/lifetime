#include "vex.h"
#include <pthread.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include "core/kernel.h"

;;OVERVIEW
/**
 * ============================================================================
 * MODULE: KernelFunctionTest (hotcwap/test/kernel_function_test.c)
 * LEVEL: L3 — Module Code (headless verification harness)
 * ============================================================================
 * Unit test suite verifying Kernel_addRunFunction and Kernel_addEndFunction.
 * Validates:
 *   1. Run functions execute concurrently in their own background worker threads
 *      without stalling Thread 0.
 *   2. Multiple run functions can be added dynamically and supervised to completion.
 *   3. End functions execute upon Kernel completion or when Kernel_stop is invoked.
 *   4. End functions fire exactly once.
 *   5. Getters accurately reflect the registered function counts.
 *
 * STRUCT FIELDS: none — procedural test harness.
 *
 * FUNCTION REGISTRY:
 * ----------------------------------------------------------------------------
 * Core Functions:
 *   - main()
 * ============================================================================
 */

static int g_failures = 0;

#define CHECK(name, cond) do { \
    if (cond) { printf("[kernel_function_test] PASS %s\n", name); } \
    else { printf("[kernel_function_test] FAIL %s\n", name); g_failures++; } \
} while (0)

typedef struct TestRunContext {
    _Atomic int counter;
    _Atomic bool threadRan;
    pthread_t threadId;
} TestRunContext;

/** Updates the run fixture from a worker thread to prove asynchronous execution. */
static fn sampleRunFunction(any userdata) {
    TestRunContext *ctx = (TestRunContext*) userdata;
    (*ctx).threadId = pthread_self();
    atomic_store_explicit(&(*ctx).threadRan, true, memory_order_relaxed);
    for (int i = 0; i < 5; i++) {
        atomic_fetch_add_explicit(&(*ctx).counter, 1, memory_order_relaxed);
        struct timespec ts = {0, 10000000L}; // 10ms
        nanosleep(&ts, nullptr);
    }
}

typedef struct TestEndContext {
    _Atomic int endCalls;
    Kernel *receivedKernel;
} TestEndContext;

/** Records the kernel and call count observed by the end-hook fixture. */
static fn sampleEndFunction(Kernel *kernel, any userdata) {
    TestEndContext *ctx = (TestEndContext*) userdata;
    (*ctx).receivedKernel = kernel;
    atomic_fetch_add_explicit(&(*ctx).endCalls, 1, memory_order_relaxed);
}

int main() {
    printf("=== Running Kernel Run/End Function Test Suite ===\n");

    // Test 1: Count getters and initial state
    {
        Kernel *k = Kernel();
        CHECK("initial run count 0", Kernel_getRunFunctionCount(k) == 0);
        CHECK("initial end count 0", Kernel_getEndFunctionCount(k) == 0);

        TestRunContext runCtx1 = {0};
        TestRunContext runCtx2 = {0};
        TestEndContext endCtx = {0};

        CHECK("add run fn 1", Kernel_addRunFunction(k, sampleRunFunction, &runCtx1));
        CHECK("add run fn 2", Kernel_addRunFunction(k, sampleRunFunction, &runCtx2));
        CHECK("run count is 2", Kernel_getRunFunctionCount(k) == 2);

        CHECK("add end fn", Kernel_addEndFunction(k, sampleEndFunction, &endCtx));
        CHECK("end count is 1", Kernel_getEndFunctionCount(k) == 1);

        // Test 2: Execution in Kernel_run
        pthread_t mainThread = pthread_self();
        int rc = Kernel_run(k);
        CHECK("run returns OK", rc == KERNEL_EXIT_OK);

        CHECK("run fn 1 executed", atomic_load_explicit(&runCtx1.threadRan, memory_order_relaxed));
        CHECK("run fn 2 executed", atomic_load_explicit(&runCtx2.threadRan, memory_order_relaxed));
        CHECK("run fn 1 on worker thread", !pthread_equal(mainThread, runCtx1.threadId));
        CHECK("run fn 2 on worker thread", !pthread_equal(mainThread, runCtx2.threadId));
        CHECK("run fn 1 counter complete", atomic_load_explicit(&runCtx1.counter, memory_order_relaxed) == 5);
        CHECK("run fn 2 counter complete", atomic_load_explicit(&runCtx2.counter, memory_order_relaxed) == 5);

        // Test 3: End function executed
        CHECK("end fn called once", atomic_load_explicit(&endCtx.endCalls, memory_order_relaxed) == 1);
        CHECK("end fn received kernel", endCtx.receivedKernel == k);

        // Disarmed state check
        CHECK("kernel not running after exit", !Kernel_isRunning(k));

        Kernel_free(k);
    }

    // Test 4: Kernel_stop fires end functions once
    {
        Kernel *k = Kernel();
        TestEndContext stopCtx = {0};
        Kernel_addEndFunction(k, sampleEndFunction, &stopCtx);

        Kernel_stop(k);
        CHECK("stop triggers end fn", atomic_load_explicit(&stopCtx.endCalls, memory_order_relaxed) == 1);

        // Calling stop again should not fire end hooks twice (fire-once guard)
        Kernel_stop(k);
        CHECK("stop does not re-fire end fn", atomic_load_explicit(&stopCtx.endCalls, memory_order_relaxed) == 1);

        Kernel_free(k);
    }

    // Test 5: Dynamic capacity doubling
    {
        Kernel *k = Kernel();
        // Add 10 run functions and 10 end functions to force dynamic reallocation beyond initial cap 4
        TestRunContext manyRun[10] = {0};
        TestEndContext manyEnd[10] = {0};
        bool allRunAdded = true;
        bool allEndAdded = true;

        for (int i = 0; i < 10; i++) {
            if (!Kernel_addRunFunction(k, sampleRunFunction, &manyRun[i]))
                allRunAdded = false;
            if (!Kernel_addEndFunction(k, sampleEndFunction, &manyEnd[i]))
                allEndAdded = false;
        }

        CHECK("add 10 run functions succeeds", allRunAdded);
        CHECK("run function count is 10", Kernel_getRunFunctionCount(k) == 10);
        CHECK("add 10 end functions succeeds", allEndAdded);
        CHECK("end function count is 10", Kernel_getEndFunctionCount(k) == 10);

        Kernel_free(k);
    }

    printf("=== Suite complete: %d failure(s) ===\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}

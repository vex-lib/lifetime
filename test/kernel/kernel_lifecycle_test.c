#include "core/kernel.h"
#include "util/annotation.h"
#include <pthread.h>
#include <stdio.h>
#include <time.h>

;;OVERVIEW
/**
 * MODULE: KernelLifecycleTest
 * LEVEL: L3 — Module Code (headless contract harness)
 * Pins the stop/end/free ordering: end hooks fire only AFTER run workers
 * quiesce, teardown is owned by the arming thread, registration is refused
 * while RUNNING/DRAINING, and the phase machine returns to READY.
 * PRIVATE HELPERS: DrainProbe (worker-done flag + hook observations), slow
 * worker fn, observer end fn, in-run registrar, run-thread body.
 * FUNCTIONS: slowWorker, observerHook, registrar, runner, main.
 */
static int failures;
#define CHECK(label, condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s (line %d)\n", label, __LINE__); \
        failures++; \
    } \
} while (0)

typedef struct DrainProbe {
    atomic_bool workerDone;   // set by the worker as its LAST act
    atomic_bool hookSawDone;  // what the end hook observed
    atomic_bool hookSawDraining;
    atomic_int hookCalls;
} DrainProbe;

/** Marks worker completion after its fixture work interval. */
static void slowWorker(void *context) {
    DrainProbe *probe = (DrainProbe*) context;
    struct timespec ts = {0, 80000000L}; // 80ms of "work"
    nanosleep(&ts, nullptr);
    atomic_store_explicit(&(*probe).workerDone, true, memory_order_release);
}

/** Captures worker quiescence and phase when the end hook runs. */
static void observerHook(Kernel *kernel, void *userdata) {
    DrainProbe *probe = (DrainProbe*) userdata;
    atomic_store_explicit(&(*probe).hookSawDone,
                          atomic_load_explicit(&(*probe).workerDone, memory_order_acquire),
                          memory_order_release);
    atomic_store_explicit(&(*probe).hookSawDraining,
                          Kernel_getPhase(kernel) == KERNEL_PHASE_DRAINING, memory_order_relaxed);
    atomic_fetch_add_explicit(&(*probe).hookCalls, 1, memory_order_relaxed);
}

/** Checks live-phase registration rejection while keeping the run active. */
static void registrar(void *context) {
    Kernel *kernel = (Kernel*) context;
    CHECK("phase RUNNING inside run", Kernel_getPhase(kernel) == KERNEL_PHASE_RUNNING);
    CHECK("addRunFunction refused while live",
          !Kernel_addRunFunction(kernel, slowWorker, nullptr));
    // Park so the run is still live when main probes free-refusal at 20ms.
    struct timespec hold = {0, 80000000L};
    nanosleep(&hold, nullptr);
}

/** Calls Kernel_run on a separate thread for lifecycle-order assertions. */
static void *runner(void *context) {
    return (void*) (intptr_t) Kernel_run((Kernel*) context);
}

int main(void) {
    // 1. External stop: hooks fire once, after worker quiescence, in DRAINING.
    Kernel *kernel = Kernel();
    DrainProbe probe = {0};
    if (!kernel)
        return 2;
    atomic_init(&probe.workerDone, false);
    atomic_init(&probe.hookSawDone, false);
    atomic_init(&probe.hookSawDraining, false);
    atomic_init(&probe.hookCalls, 0);
    CHECK("register worker", Kernel_addRunFunction(kernel, slowWorker, &probe));
    CHECK("register hook", Kernel_addEndFunction(kernel, observerHook, &probe));
    pthread_t thread;
    if (pthread_create(&thread, nullptr, runner, kernel) != 0)
        return 2;
    struct timespec ts = {0, 20000000L}; // 20ms: worker is mid-flight
    nanosleep(&ts, nullptr);
    CHECK("stop accepted", Kernel_isRunning(kernel));
    Kernel_stop(kernel);
    CHECK("join", pthread_join(thread, nullptr) == 0);
    CHECK("worker finished", atomic_load_explicit(&probe.workerDone, memory_order_acquire));
    CHECK("hook fired once", atomic_load_explicit(&probe.hookCalls, memory_order_relaxed) == 1);
    CHECK("hook saw quiescence", atomic_load_explicit(&probe.hookSawDone, memory_order_relaxed));
    CHECK("hook ran in DRAINING", atomic_load_explicit(&probe.hookSawDraining, memory_order_relaxed));
    CHECK("back to READY", Kernel_getPhase(kernel) == KERNEL_PHASE_READY && !Kernel_isRunning(kernel));
    CHECK("registration reopened", Kernel_addRunFunction(kernel, slowWorker, &probe));
    CHECK("free", Kernel_free(kernel));

    // 2. Second stop (READY) must not re-fire the burnt hooks.
    Kernel *burnt = Kernel();
    DrainProbe burntProbe = {0};
    if (!burnt)
        return 2;
    atomic_init(&burntProbe.hookCalls, 0);
    Kernel_addEndFunction(burnt, observerHook, &burntProbe);
    Kernel_stop(burnt);
    Kernel_stop(burnt);
    CHECK("ready stop fired once", atomic_load_explicit(&burntProbe.hookCalls, memory_order_relaxed) == 1);
    CHECK("free burnt", Kernel_free(burnt));

    // 3. Refuse free while a run is live; run fn refusals inside the reactor.
    Kernel *live = Kernel();
    if (!live)
        return 2;
    Kernel_addRunFunction(live, registrar, live);
    pthread_t liveThread;
    if (pthread_create(&liveThread, nullptr, runner, live) != 0)
        return 2;
    nanosleep(&ts, nullptr);
    CHECK("free refused while live", !Kernel_free(live));
    Kernel_stop(live);
    CHECK("join live", pthread_join(liveThread, nullptr) == 0);
    CHECK("free live", Kernel_free(live));

    printf("Kernel lifecycle tests: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}

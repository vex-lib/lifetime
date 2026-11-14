#include "application.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <time.h>

#include "lifetime/lifetime.h"

#include "util/annotation.h"
#include "../../vexspoke/input/key.h"
#include "../../vexspoke/input/mouse.h"
#include "../../vexspoke/input/touch.h"
#include "../../vexspoke/system/system.h"

;;DEFINITION
/**
 * ============================================================================
 * DEFINITION: Application
 * ============================================================================
 * RAISON D'ÊTRE:
 *   The executable manifest for a running graphical application: identity
 *   (name/author/version/icon), top-level window registry, and hot-reload slot.
 *   Application classifies GUI executables that present pixels through a Window;
 *   it supervises startup work and window lifetime, never a scene/render worker.
 *
 * MEMORY LAYOUT & LIFECYCLE:
 *   Flat fixed-size buffers for strings and an array of up to 16 Window pointers.
 *   Atomic fields ensure thread-safe telemetry (fps, frametimeUs) and running state.
 *   Allocation is cold-path heap via calloc; windows are referenced, never owned.
 *
 * OPERATIONAL INVARIANTS:
 *   - The owner-thread lifetime loop or Kernel reactor services native events.
 *   - Application_start blocks until all windows close or an explicit close request.
 *   - Frame scheduling and GPU presentation belong strictly to R3 GfxLoop.
 * ============================================================================
 */

;;OVERVIEW
/**
 * ============================================================================
 * CLASS: Application (kernel/application.c)
 * ============================================================================
 * SUMMARY:
 *   The manifest for a running executable: name, author, version, icon, and the
 *   window registry. Application is GUI by definition (presents pixels through
 *   a Window); CLI functions are Process, TUI sessions are Console. It is a
 *   lifetime supervisor — it NEVER owns a scene tick or a present worker. The
 *   Kernel (R1) drives the event pump and observes the running flag; graphvex
 *   (R3) drives frame scheduling, presentation, and telemetry writes.
 *
 * STRUCT FIELDS (Mirroring kernel/application.h — exactly this file's class):
 * ----------------------------------------------------------------------------
 *   char name[APP_MAX_NAME];               // app name (default "vex")
 *   char author[APP_MAX_NAME];             // author / studio (default "")
 *   char version[APP_MAX_VERSION];         // version string, e.g. "1.2.3"
 *   char iconPath[APP_MAX_ICON_PATH];      // icon path reference (default "")
 *   Window *windows[APP_MAX_WINDOWS];      // registered top-level windows
 *   u32 window_count;                 // used slots in windows[]
 *   atomic_uint phase;                     // AppPhase: the single lifecycle word
 *   any hot;                           // reload watcher, opaque (setHot)
 *   AppHotPollFn hotPoll;                // reload poll seam (setHotPoll)
 *   _Atomic u32 fps;                  // live telemetry: FPS (graphvex writes)
 *   _Atomic u32 frametimeUs;          // live telemetry: frametime in microseconds
 *   AppHotReloadFn hotReloadFn;            // hot-reload notification callback (nullable)
 *   any hotReloadUserdata;               // userdata for hotReloadFn
 *
 * PRIVATE HELPERS:
 * ----------------------------------------------------------------------------
 *   appAllWindowsClosed(const self) : true when window_count>0 and EVERY
 *       registered window reports shouldClose (plain if/read — the Window
 *       owns its own events; this only asks). False for a windowless
 *       manifest (nothing to end).
 *   appParkSlice(self)             : one owner service turn + 5ms park;
 *       each slice lets the Window pump its own queue (Window_pollEvents)
 *       and re-checks the running flag. This is what keeps an empty window
 *       alive and responsive — never a present, never a tick.
 *
 * FUNCTION REGISTRY:
 * ----------------------------------------------------------------------------
 * Public Constructors: (.h)
 *   - Application()                            : Application_0()
 *   - Application(name)                        : Application_1(name)
 *   - Application(name, author, version)       : Application_3(name, author, version)
 *
 * Private Constructors: (.c static)
 *   - (none)
 *
 * Public Core Functions: (.h)
 *   - Application_init()                       : One-shot bootstrap
 *   - Application_shutdown()                   : Teardown input/key state
 *   - Application_free(self)                   : Release manifest memory
 *   - Application_start(self)                  : Begin, service, close/join
 *   - Application_stop(self)                   : Thread-safe close request
 *   - Application_isRunning(self)              : Queries atomic running flag
 *   - Application_isFinished(self)             : Completion predicate for Kernel
 *   - Application_run(self)                    : Keep-alive parked loop + worker join
 *   - Application_pollHot(self)                : Generation-driven swap
 *   - Application_onHotReload(self, fn, user)  : Assign hot-reload callback
 *   - Application_addWindow(self, win)         : Register top-level window
 *   - Application_removeWindow(self, win)      : Unregister top-level window
 *
 * Private Core Functions: (.c static)
 *   - appAllWindowsClosed(self)                : Test if all registered windows closed
 *   - appParkSlice(self)                       : owner/input service and 5ms park
 *
 * Public Setters: (.h)
 *   - Application_setHot(self, hot)
 *   - Application_setName(self, name)
 *   - Application_setAuthor(self, author)
 *   - Application_setVersion(self, version)
 *   - Application_setIconPath(self, iconPath)
 *
 * Private Setters: (.c static)
 *   - (none)
 *
 * Public Getters: (.h)
 *   - Application_getPhase(self)               : The one lifecycle word
 *   - Application_isActive(self)               : STARTING/RUNNING/CLOSING
 *   - Application_getPhaseName(phase)          : Stable symbolic name
 *   - Application_getFps(self)
 *   - Application_getFrametimeUs(self)
 *   - Application_getHot(self)
 *   - Application_getName(self)
 *   - Application_getAuthor(self)
 *   - Application_getVersion(self)
 *   - Application_getIconPath(self)
 *   - Application_getWindow(self, index)
 *   - Application_getWindowCount(self)
 *   - Application_getWindows(self, out, cap)
 *
 * Private Getters: (.c static)
 *   - (none)
 * ============================================================================
 */

// BOOTSTRAP — runs exactly once (first Application() constructor triggers it).
// Callers just construct and free; init is the constructor's job, teardown is
// the caller's (they opened the Application, they close it).
// Plain bool, not _Atomic: construction is single-threaded cold-path (the Cold-Strict, Hot-Minimal Validation Law).
static bool s_bootstrapped = false;
static _Thread_local Application *s_current;

// --- PHASE MACHINE -----------------------------------------------------------
// The ONLY place lifecycle transitions are written. Every guard and getter
// reads the same word, so no second flag can disagree with it.
//
//   appPhaseBegin : IDLE|STOPPED -> STARTING   (refused from admitted phases)
//   appPhaseAdvance: STARTING    -> RUNNING    (startup worker finished)
//   appPhaseClose : admitted     -> CLOSING    (idempotent, no-op when idle)
//   appPhaseStop  : CLOSING      -> STOPPED    (owner, after the join)
//
// workerDone and hadWindows are one-way latches, not phases: they are set at
// most once per launch and never combined into a lifecycle decision.

static AppPhase appPhase(const Application *self) {
    return (AppPhase) atomic_load_explicit(&(*self).phase, memory_order_acquire);
}

static bool appPhaseRunning(AppPhase phase) {
    return phase == APP_PHASE_STARTING || phase == APP_PHASE_RUNNING;
}

static bool appPhaseActive(AppPhase phase) {
    return appPhaseRunning(phase) || phase == APP_PHASE_CLOSING;
}

/** IDLE or STOPPED -> STARTING. Refused from every admitted phase. */
static bool appPhaseBegin(Application *self) {
    u32 expected = atomic_load_explicit(&(*self).phase, memory_order_relaxed);
    for (;;) {
        if (expected != APP_PHASE_IDLE && expected != APP_PHASE_STOPPED)
            return false;
        if (atomic_compare_exchange_weak_explicit(&(*self).phase, &expected,
                APP_PHASE_STARTING, memory_order_acq_rel, memory_order_relaxed))
            return true;
    }
}

/** STARTING -> RUNNING, by the startup worker (or by begin when it is empty). */
static fn appPhaseAdvance(Application *self) {
    u32 expected = APP_PHASE_STARTING;
    atomic_compare_exchange_strong_explicit(&(*self).phase, &expected,
        APP_PHASE_RUNNING, memory_order_acq_rel, memory_order_relaxed);
}

/** Any admitted phase -> CLOSING. Idempotent; a no-op in IDLE/STOPPED. */
static fn appPhaseClose(Application *self) {
    u32 expected = atomic_load_explicit(&(*self).phase, memory_order_relaxed);
    for (;;) {
        if (!appPhaseActive((AppPhase) expected))
            return;
        if (atomic_compare_exchange_weak_explicit(&(*self).phase, &expected,
                APP_PHASE_CLOSING, memory_order_acq_rel, memory_order_relaxed))
            return;
    }
}

/** CLOSING -> STOPPED, by the owner after the worker join. */
static fn appPhaseStop(Application *self) {
    atomic_store_explicit(&(*self).phase, APP_PHASE_STOPPED, memory_order_release);
}

;;INTENTION("Application_start owns lifetime, not scene timing: startup callbacks "
            "run on a supervised worker; native operations are serviced on the "
            "owner thread. Hiding never completes an app; all windows must close.")

/** Bootstraps shared system services once; returns true after initialization. */
bool Application_init() {
    if (s_bootstrapped)
        return true;
    s_bootstrapped = true;
    System_initializeAll();
    // NOTE: no Vfs_init() here by design — the VFS lives in darling R4
    // (io/vfs.c) since the split and boots itself once UI paths resolve.
    // R1 never reaches up the stack (the Vertical Integration Law).
    return true;
}

/** Shuts down the key-input subsystem used by applications. */
fn Application_shutdown() {
    Key_shutdown();
}

// KEEP-ALIVE PARKED LOOP — the app lives on its own: Application_run parks
// the _main thread here until EVERY registered window is closed, ending the
// application. It does two things only: lets the Window chew its own event
// queue (Window_pollEvents — events are the Window's job, never the app's),
// and asks each window whether it shouldClose (a plain if/read). The park
// cadence is a 5ms parked slice, so shutdown/input and focus pacing stay
// responsive without busy-spinning. Nothing
// here touches graphvex, present, or GfxLoop — hotcwap owns the window end.
#define APP_PARK_SLICE_NS   (5 * 1000 * 1000)
#define APP_PARK_SLICES     1

/** Returns true only when the application had windows and all are closing. */
static bool appAllWindowsClosed(const Application *self) {
    if ((*self).window_count == 0)
        return (*self).hadWindows;
    for (u32 i = 0; i < (*self).window_count; i++)
        if (!Window_shouldClose((*self).windows[i]))
            return false;
    return true;
}

/** Services window/input work on the owner thread, then parks for one short slice.
 * Closes each pass by resetting the host transient arena: scratch borrowed
 * during this pass dies here, never across frames or swaps. */
static fn appParkSlice(Application *self) {
    for (u32 i = 0; i < APP_PARK_SLICES; i++) {
        if (!appPhaseRunning(appPhase(self)))
            return;
        Window_pollEvents(); // the Window pumps its own queue (its job)
        Application *previous = s_current; s_current = self;
        Key_dispatchEvents(); Mouse_dispatchEvents(); Touch_dispatchEvents();
        s_current = previous;
        Application_poll(self);
        if ((*self).hostLifetime)
            Lifetime_resetTransient((Lifetime*) (*self).hostLifetime);
        struct timespec slice = { APP_PARK_SLICE_NS / 1000000000ULL,
                                  APP_PARK_SLICE_NS % 1000000000ULL };
        nanosleep(&slice, nullptr);
    }
}

// CONSTRUCTORS (PUBLIC & PRIVATE)
/** Allocates an application manifest with the default name. */
Application *Application_0() {
    Application_init();
    Application *self = (Application*) calloc(1, sizeof(Application));
    if (!self) return nullptr;
    if (pthread_mutex_init(&(*self).invokeMutex, nullptr) != 0) { free(self); return nullptr; }
    if (pthread_cond_init(&(*self).invokeCondition, nullptr) != 0) {
        pthread_mutex_destroy(&(*self).invokeMutex); free(self); return nullptr;
    }
    strncpy((*self).name, "vex", APP_MAX_NAME - 1);
    return self;
}

/** Creates an application and copies its name into the bounded field. */
Application *Application_1(const char *name) {
    Application *self = Application_0();
    if (!self) return nullptr;
    Application_setName(self, name);
    return self;
}

/** Creates an application and copies its name, author, and version. */
Application *Application_3(const char *name, const char *author, const char *version) {
    Application *self = Application_0();
    if (!self) return nullptr;
    Application_setName(self, name);
    Application_setAuthor(self, author);
    Application_setVersion(self, version);
    return self;
}

// CORE FUNCTIONS (PUBLIC & PRIVATE)
/** Frees an inactive application; leaves active instances allocated. */
fn Application_free(Application *self) {
    if (!self) return;
    // Caller must return from start/run (worker quiescence) before freeing.
    if (appPhaseActive(appPhase(self)))
        return;
    pthread_cond_destroy(&(*self).invokeCondition);
    pthread_mutex_destroy(&(*self).invokeMutex);
    free(self);
}

/** Runs registered start callbacks on the worker until stopped, then marks it done. */
static any appStartWorker(any userdata) {
    Application *self = userdata;
    for (u32 i = 0; i < (*self).startCount && Application_isRunning(self); i++)
        (*self).startFns[i](self, (*self).startUsers[i]);
    atomic_store_explicit(&(*self).workerDone, true, memory_order_release);
    appPhaseAdvance(self); // STARTING -> RUNNING; a pending close keeps CLOSING
    return nullptr;
}

/** Returns the application currently being serviced on this thread, if any. */
Application *Application_current() {
    return s_current;
}

/** Marks the application active, shows its windows, and starts its worker callbacks. */
bool Application_begin(Application *self) {
    if (!self) return false;
    if (!appPhaseBegin(self)) return false;
    (*self).ownerThread = pthread_self();
    bool hasWorker = (*self).startCount != 0;
    atomic_store(&(*self).workerDone, !hasWorker);
    for (u32 i = 0; i < (*self).window_count; i++)
        if (!Window_shouldClose((*self).windows[i])) Window_show((*self).windows[i]);
    if (hasWorker) {
        if (pthread_create(&(*self).startThread, nullptr, appStartWorker, self) != 0) {
            atomic_store(&(*self).workerDone, true);
            appPhaseClose(self);
            Application_finish(self);
            return false;
        }
        (*self).workerLaunched = true;
    } else {
        appPhaseAdvance(self);
    }
    return true;
}

/** Begins the application and runs its owner-thread keep-alive loop. */
fn Application_start(Application *self) {
    if (Application_begin(self)) Application_run(self);
}

/** Requests closure and wakes callers waiting for owner-thread invocation. */
fn Application_close(Application *self) {
    if (!self) return;
    appPhaseClose(self);
    pthread_mutex_lock(&(*self).invokeMutex);
    pthread_cond_broadcast(&(*self).invokeCondition);
    pthread_mutex_unlock(&(*self).invokeMutex);
}

/** Registers a startup callback before the application becomes active. */
bool Application_addStartEvent(Application *self, ApplicationEventFn fn, any userdata) {
    if (!self || !fn || appPhaseActive(appPhase(self)) || (*self).startCount == APP_MAX_EVENTS) return false;
    u32 i = (*self).startCount++;
    (*self).startFns[i] = fn; (*self).startUsers[i] = userdata;
    return true;
}

/** Registers an owner-thread poll callback, accepting an identical registration once. */
bool Application_addPollEvent(Application *self, ApplicationEventFn fn, any userdata) {
    if (!self || !fn) return false;
    if (appPhaseActive(appPhase(self)) && !pthread_equal(pthread_self(), (*self).ownerThread)) return false;
    u32 empty = APP_MAX_EVENTS;
    for (u32 i = 0; i < APP_MAX_EVENTS; i++) {
        if ((*self).pollFns[i] == fn && (*self).pollUsers[i] == userdata) return true;
        if (!(*self).pollFns[i] && empty == APP_MAX_EVENTS) empty = i;
    }
    if (empty == APP_MAX_EVENTS) return false;
    (*self).pollFns[empty] = fn; (*self).pollUsers[empty] = userdata; return true;
}

/** Removes the matching poll callback and userdata pair. */
bool Application_removePollEvent(Application *self, ApplicationEventFn fn, const_any userdata) {
    if (!self) return false;
    if (appPhaseActive(appPhase(self)) && !pthread_equal(pthread_self(), (*self).ownerThread)) return false;
    for (u32 i = 0; i < APP_MAX_EVENTS; i++)
        if ((*self).pollFns[i] == fn && (*self).pollUsers[i] == userdata) {
            (*self).pollFns[i] = nullptr; (*self).pollUsers[i] = nullptr; return true;
        }
    return false;
}

/** Runs immediately on the owner thread or queues work and waits for its completion. */
bool Application_invoke(Application *self, ApplicationEventFn fn, any userdata) {
    if (!self || !fn || !Application_isRunning(self)) return false;
    if (pthread_equal(pthread_self(), (*self).ownerThread)) {
        Application *previous = s_current; s_current = self;
        fn(self, userdata); s_current = previous; return true;
    }
    pthread_mutex_lock(&(*self).invokeMutex);
    while ((*self).invokePhase != APP_INVOKE_IDLE && Application_isRunning(self))
        pthread_cond_wait(&(*self).invokeCondition, &(*self).invokeMutex);
    if (!Application_isRunning(self)) { pthread_mutex_unlock(&(*self).invokeMutex); return false; }
    (*self).invokeFn = fn; (*self).invokeUser = userdata;
    (*self).invokePhase = APP_INVOKE_PENDING;
    // Once admitted, wait for completion even if the callback requests close.
    while ((*self).invokePhase != APP_INVOKE_IDLE)
        pthread_cond_wait(&(*self).invokeCondition, &(*self).invokeMutex);
    pthread_mutex_unlock(&(*self).invokeMutex);
    return true;
}

/** Executes queued owner work and registered poll callbacks on the owner thread. */
fn Application_poll(Application *self) {
    if (!self || !appPhaseActive(appPhase(self)) || !pthread_equal(pthread_self(), (*self).ownerThread)) return;
    Application *previous = s_current; s_current = self;
    pthread_mutex_lock(&(*self).invokeMutex);
    ApplicationEventFn fn = (*self).invokePhase == APP_INVOKE_PENDING ? (*self).invokeFn : nullptr;
    any userdata = (*self).invokeUser;
    if (fn) (*self).invokePhase = APP_INVOKE_EXECUTING;
    pthread_mutex_unlock(&(*self).invokeMutex);
    if (fn) {
        fn(self, userdata);
        pthread_mutex_lock(&(*self).invokeMutex);
        (*self).invokeFn = nullptr;
        (*self).invokePhase = APP_INVOKE_IDLE;
        pthread_cond_broadcast(&(*self).invokeCondition);
        pthread_mutex_unlock(&(*self).invokeMutex);
    }
    if (Application_isRunning(self))
        for (u32 i = 0; i < APP_MAX_EVENTS; i++)
            if ((*self).pollFns[i]) (*self).pollFns[i](self, (*self).pollUsers[i]);
    s_current = previous;
}

/** Closes registered windows, drains admitted owner work, joins startup work, and deactivates. */
fn Application_finish(Application *self) {
    if (!self || !appPhaseActive(appPhase(self)) || !pthread_equal(pthread_self(), (*self).ownerThread)) return;
    Application_close(self);
    for (u32 i = 0; i < (*self).window_count; i++)
        Window_close((*self).windows[i]);
    // Drain admitted owner work before joining a worker waiting on that work.
    while (!atomic_load_explicit(&(*self).workerDone, memory_order_acquire)) {
        Application_poll(self);
        Window_pollEvents();
        struct timespec slice = {0, 1000000}; nanosleep(&slice, nullptr);
    }
    if ((*self).workerLaunched) { pthread_join((*self).startThread, nullptr); (*self).workerLaunched = false; }
    appPhaseStop(self);
}

/** Requests application closure through Application_close. */
fn Application_stop(Application *self) {
    if (!self) return;
    Application_close(self);
}

/** Reads the lifecycle word; an application is running in STARTING or RUNNING. */
bool Application_isRunning(const Application *self) {
    return self ? appPhaseRunning(appPhase(self)) : false;
}

;;GETTER
/** True in the admitted phases (STARTING, RUNNING, CLOSING); false when freeable. */
bool Application_isActive(const Application *self) {
    return self ? appPhaseActive(appPhase(self)) : false;
}

;;GETTER
/** The one lifecycle word; a null application reads IDLE. */
AppPhase Application_getPhase(const Application *self) {
    return self ? appPhase(self) : APP_PHASE_IDLE;
}

;;GETTER
/** Stable symbolic name for a phase (cold path; never null). */
const char *Application_getPhaseName(AppPhase phase) {
    switch (phase) {
        case APP_PHASE_IDLE:     return "APP_PHASE_IDLE";
        case APP_PHASE_STARTING: return "APP_PHASE_STARTING";
        case APP_PHASE_RUNNING:  return "APP_PHASE_RUNNING";
        case APP_PHASE_CLOSING:  return "APP_PHASE_CLOSING";
        case APP_PHASE_STOPPED:  return "APP_PHASE_STOPPED";
        default:                 return "APP_PHASE_UNKNOWN";
    }
}

/** Reports completion when stopped or when all applicable registered windows close. */
bool Application_isFinished(const Application *self) {
    if(self == nullptr)
        return true;
    if(!appPhaseRunning(appPhase(self)))
        return true;
    if(appAllWindowsClosed(self))
        return true;
    return false;
}

// The parked keep-alive loop. BLOCKS until every registered window is closed
// (or an external stop flips running). The empty window lives on its own here
// — no graphvex, no present, no GfxLoop: hotcwap manages the Windows and ends
// the Application when they are all gone. The Window pumps its own events; the
// app only ASKS "any window still open?" at the 250ms cadence. So
// Kernel_run(kernel, app) blocks: the app stays alive exactly as long as a
// window is open.
/** Parks and services the application until stopped or all registered windows close. */
fn Application_run(Application *self) {
    if (!self) return;
    if (!appPhaseActive(appPhase(self)) && !Application_begin(self)) return;
    if (!pthread_equal(pthread_self(), (*self).ownerThread)) return;
    while (appPhaseRunning(appPhase(self))) {
        if (appAllWindowsClosed(self)) {
            appPhaseClose(self);
            break;
        }
        appParkSlice(self);
        Application_pollHot(self); // generation-driven hot swap at ~250ms cadence
    }
    Application_finish(self);
}

/** Adds a borrowed window unless duplicated, full, or disallowed by active ownership. */
bool Application_addWindow(Application *self, Window *win) {
    if (!self || !win) return false;
    if (appPhaseActive(appPhase(self)) && !pthread_equal(pthread_self(), (*self).ownerThread)) return false;
    if (appPhase(self) == APP_PHASE_CLOSING) return false;
    for (u32 i = 0; i < (*self).window_count; i++)
        if ((*self).windows[i] == win) return false;
    if ((*self).window_count >= APP_MAX_WINDOWS) return false;
    (*self).windows[(*self).window_count++] = win;
    (*self).hadWindows = true;
    return true;
}

/** Removes a registered borrowed window without destroying it. */
bool Application_removeWindow(Application *self, Window *win) {
    if (!self || !win) return false;
    if (appPhaseActive(appPhase(self)) && !pthread_equal(pthread_self(), (*self).ownerThread)) return false;
    for (u32 i = 0; i < (*self).window_count; i++) {
        if ((*self).windows[i] == win) {
            (*self).windows[i] = (*self).windows[--(*self).window_count];
            (*self).windows[(*self).window_count] = nullptr;
            return true;
        }
    }
    return false;
}

/** Installs the callback notified after a successful generation reload. */
fn Application_onHotReload(Application *self, AppHotReloadFn fn, any userdata) {
    if (!self) return;
    (*self).hotReloadFn = fn;
    (*self).hotReloadUserdata = userdata;
}

/** Polls the associated reload watcher via the hotcwap-provided seam. */
fn Application_pollHot(Application *self) {
    if (!self || !(*self).hot || !(*self).hotPoll)
        return;
    u32 loaded = 0;
    int r = (*self).hotPoll((*self).hot, &loaded);
    if (r == 0 && loaded > 0 && (*self).hotReloadFn)
        (*self).hotReloadFn(self, loaded, (*self).hotReloadUserdata);
}

// SETTERS (PUBLIC & PRIVATE)
;;SETTER
/** Associates a borrowed reload watcher (opaque; hotcwap owns the type). */
fn Application_setHot(Application *self, any hot) {
    if (!self) return;
    (*self).hot = hot;
}

;;SETTER
/** Installs the reload poll seam (hotcwap provides; R0 never names HotModule). */
fn Application_setHotPoll(Application *self, AppHotPollFn poll) {
    if (!self) return;
    (*self).hotPoll = poll;
}

;;SETTER
/** Binds the owning Kernel Lifetime so the host pump can reset scratch per pass. */
fn Application_setHostLifetime(Application *self, any lifetime) {
    if (!self) return;
    (*self).hostLifetime = lifetime;
}

;;SETTER
/** Copies a name into the application's fixed-capacity name field. */
fn Application_setName(Application *self, const char *name) {
    if (!self || !name) return;
    strncpy((*self).name, name, APP_MAX_NAME - 1);
    (*self).name[APP_MAX_NAME - 1] = '\0';
}

;;SETTER
/** Copies an author label into the application's fixed-capacity field. */
fn Application_setAuthor(Application *self, const char *author) {
    if (!self || !author) return;
    strncpy((*self).author, author, APP_MAX_NAME - 1);
    (*self).author[APP_MAX_NAME - 1] = '\0';
}

;;SETTER
/** Copies a version string into the application's fixed-capacity field. */
fn Application_setVersion(Application *self, const char *version) {
    if (!self || !version) return;
    strncpy((*self).version, version, APP_MAX_VERSION - 1);
    (*self).version[APP_MAX_VERSION - 1] = '\0';
}

;;SETTER
/** Copies an icon path into the application's fixed-capacity field. */
fn Application_setIconPath(Application *self, const char *iconPath) {
    if (!self || !iconPath) return;
    strncpy((*self).iconPath, iconPath, APP_MAX_ICON_PATH - 1);
    (*self).iconPath[APP_MAX_ICON_PATH - 1] = '\0';
}

// GETTERS (PUBLIC & PRIVATE)
;;GETTER
/** Returns the latest atomically published frames-per-second value. */
u32 Application_getFps(const Application *self) {
    return self ? atomic_load_explicit(&(*self).fps, memory_order_relaxed) : 0;
}

;;GETTER
/** Returns the latest atomically published frame duration in microseconds. */
u32 Application_getFrametimeUs(const Application *self) {
    return self ? atomic_load_explicit(&(*self).frametimeUs, memory_order_relaxed) : 0;
}

;;GETTER
/** Returns the associated borrowed reload watcher (opaque), or nullptr for null self. */
any Application_getHot(const Application *self) {
    return self ? (*self).hot : nullptr;
}

;;GETTER
/** Returns the internal name buffer, or nullptr for null self. */
const char *Application_getName(const Application *self) {
    if (!self) return nullptr;
    return (*self).name;
}

;;GETTER
/** Returns the internal author buffer, or nullptr for null self. */
const char *Application_getAuthor(const Application *self) {
    if (!self) return nullptr;
    return (*self).author;
}

;;GETTER
/** Returns the internal version buffer, or nullptr for null self. */
const char *Application_getVersion(const Application *self) {
    if (!self) return nullptr;
    return (*self).version;
}

;;GETTER
/** Returns the internal icon-path buffer, or nullptr for null self. */
const char *Application_getIconPath(const Application *self) {
    if (!self) return nullptr;
    return (*self).iconPath;
}

;;GETTER
/** Returns the registered window at index, or nullptr when out of range. */
Window *Application_getWindow(const Application *self, u32 index) {
    if (!self) return nullptr;
    if (index >= (*self).window_count) return nullptr;
    return (*self).windows[index];
}

;;GETTER
/** Returns the number of registered windows. */
u32 Application_getWindowCount(const Application *self) {
    if (!self) return 0;
    return (*self).window_count;
}

;;GETTER
/** Copies up to cap borrowed window pointers into out and returns the copied count. */
u32 Application_getWindows(const Application *self, Window **out, u32 cap) {
    if (!self || !out) return 0;
    u32 n = (*self).window_count;
    if (n > cap) n = cap;
    for (u32 i = 0; i < n; i++)
        out[i] = (*self).windows[i];
    return n;
}

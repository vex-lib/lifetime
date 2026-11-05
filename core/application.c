#include "application.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <time.h>

#include "lifetime/lifetime.h"

#include "annotation/annotation.h"
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
 *   uint32_t window_count;                 // used slots in windows[]
 *   _Atomic bool running;                  // runtime active flag (Kernel writes, graphvex reads)
 *   void *hot;                           // reload watcher, opaque (setHot)
 *   AppHotPollFn hotPoll;                // reload poll seam (setHotPoll)
 *   _Atomic uint32_t fps;                  // live telemetry: FPS (graphvex writes)
 *   _Atomic uint32_t frametimeUs;          // live telemetry: frametime in microseconds
 *   AppHotReloadFn hotReloadFn;            // hot-reload notification callback (nullable)
 *   void *hotReloadUserdata;               // userdata for hotReloadFn
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
void Application_shutdown() {
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
    for (uint32_t i = 0; i < (*self).window_count; i++)
        if (!Window_shouldClose((*self).windows[i]))
            return false;
    return true;
}

/** Services window/input work on the owner thread, then parks for one short slice.
 * Closes each pass by resetting the host transient arena: scratch borrowed
 * during this pass dies here, never across frames or swaps. */
static void appParkSlice(Application *self) {
    for (uint32_t i = 0; i < APP_PARK_SLICES; i++) {
        if (!atomic_load_explicit(&(*self).running, memory_order_relaxed))
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
void Application_free(Application *self) {
    if (!self) return;
    // Caller must return from start/run (worker quiescence) before freeing.
    if (atomic_load(&(*self).active)) return;
    atomic_store_explicit(&(*self).running, false, memory_order_relaxed);
    pthread_cond_destroy(&(*self).invokeCondition);
    pthread_mutex_destroy(&(*self).invokeMutex);
    free(self);
}

/** Runs registered start callbacks on the worker until stopped, then marks it done. */
static void *appStartWorker(void *userdata) {
    Application *self = userdata;
    for (uint32_t i = 0; i < (*self).startCount && Application_isRunning(self); i++)
        (*self).startFns[i](self, (*self).startUsers[i]);
    atomic_store_explicit(&(*self).workerDone, true, memory_order_release);
    return nullptr;
}

/** Returns the application currently being serviced on this thread, if any. */
Application *Application_current() {
    return s_current;
}

/** Marks the application active, shows its windows, and starts its worker callbacks. */
bool Application_begin(Application *self) {
    if (!self) return false;
    bool expected = false;
    if (!atomic_compare_exchange_strong(&(*self).active, &expected, true)) return false;
    (*self).ownerThread = pthread_self();
    atomic_store(&(*self).closeRequested, false);
    atomic_store(&(*self).workerDone, (*self).startCount == 0);
    atomic_store(&(*self).running, true);
    for (uint32_t i = 0; i < (*self).window_count; i++)
        if (!Window_shouldClose((*self).windows[i])) Window_show((*self).windows[i]);
    if ((*self).startCount) {
        if (pthread_create(&(*self).startThread, nullptr, appStartWorker, self) != 0) {
            atomic_store(&(*self).workerDone, true);
            Application_close(self);
            Application_finish(self);
            return false;
        }
        (*self).workerLaunched = true;
    }
    return true;
}

/** Begins the application and runs its owner-thread keep-alive loop. */
void Application_start(Application *self) {
    if (Application_begin(self)) Application_run(self);
}

/** Requests closure and wakes callers waiting for owner-thread invocation. */
void Application_close(Application *self) {
    if (!self) return;
    atomic_store(&(*self).closeRequested, true);
    atomic_store(&(*self).running, false);
    pthread_mutex_lock(&(*self).invokeMutex);
    pthread_cond_broadcast(&(*self).invokeCondition);
    pthread_mutex_unlock(&(*self).invokeMutex);
}

/** Registers a startup callback before the application becomes active. */
bool Application_addStartEvent(Application *self, ApplicationEventFn fn, void *userdata) {
    if (!self || !fn || atomic_load(&(*self).active) || (*self).startCount == APP_MAX_EVENTS) return false;
    uint32_t i = (*self).startCount++;
    (*self).startFns[i] = fn; (*self).startUsers[i] = userdata;
    return true;
}

/** Registers an owner-thread poll callback, accepting an identical registration once. */
bool Application_addPollEvent(Application *self, ApplicationEventFn fn, void *userdata) {
    if (!self || !fn) return false;
    if (atomic_load(&(*self).active) && !pthread_equal(pthread_self(), (*self).ownerThread)) return false;
    uint32_t empty = APP_MAX_EVENTS;
    for (uint32_t i = 0; i < APP_MAX_EVENTS; i++) {
        if ((*self).pollFns[i] == fn && (*self).pollUsers[i] == userdata) return true;
        if (!(*self).pollFns[i] && empty == APP_MAX_EVENTS) empty = i;
    }
    if (empty == APP_MAX_EVENTS) return false;
    (*self).pollFns[empty] = fn; (*self).pollUsers[empty] = userdata; return true;
}

/** Removes the matching poll callback and userdata pair. */
bool Application_removePollEvent(Application *self, ApplicationEventFn fn, const void *userdata) {
    if (!self) return false;
    if (atomic_load(&(*self).active) && !pthread_equal(pthread_self(), (*self).ownerThread)) return false;
    for (uint32_t i = 0; i < APP_MAX_EVENTS; i++)
        if ((*self).pollFns[i] == fn && (*self).pollUsers[i] == userdata) {
            (*self).pollFns[i] = nullptr; (*self).pollUsers[i] = nullptr; return true;
        }
    return false;
}

/** Runs immediately on the owner thread or queues work and waits for its completion. */
bool Application_invoke(Application *self, ApplicationEventFn fn, void *userdata) {
    if (!self || !fn || !Application_isRunning(self)) return false;
    if (pthread_equal(pthread_self(), (*self).ownerThread)) {
        Application *previous = s_current; s_current = self;
        fn(self, userdata); s_current = previous; return true;
    }
    pthread_mutex_lock(&(*self).invokeMutex);
    while ((*self).invokeFn && Application_isRunning(self))
        pthread_cond_wait(&(*self).invokeCondition, &(*self).invokeMutex);
    if (!Application_isRunning(self)) { pthread_mutex_unlock(&(*self).invokeMutex); return false; }
    (*self).invokeFn = fn; (*self).invokeUser = userdata;
    // Once admitted, wait for completion even if the callback requests close.
    while ((*self).invokeFn)
        pthread_cond_wait(&(*self).invokeCondition, &(*self).invokeMutex);
    pthread_mutex_unlock(&(*self).invokeMutex);
    return true;
}

/** Executes queued owner work and registered poll callbacks on the owner thread. */
void Application_poll(Application *self) {
    if (!self || !atomic_load(&(*self).active) || !pthread_equal(pthread_self(), (*self).ownerThread)) return;
    Application *previous = s_current; s_current = self;
    pthread_mutex_lock(&(*self).invokeMutex);
    ApplicationEventFn fn = (*self).invokeExecuting ? nullptr : (*self).invokeFn;
    void *userdata = (*self).invokeUser;
    if (fn) (*self).invokeExecuting = true;
    pthread_mutex_unlock(&(*self).invokeMutex);
    if (fn) {
        fn(self, userdata);
        pthread_mutex_lock(&(*self).invokeMutex);
        (*self).invokeFn = nullptr;
        (*self).invokeExecuting = false;
        pthread_cond_broadcast(&(*self).invokeCondition);
        pthread_mutex_unlock(&(*self).invokeMutex);
    }
    if (Application_isRunning(self))
        for (uint32_t i = 0; i < APP_MAX_EVENTS; i++)
            if ((*self).pollFns[i]) (*self).pollFns[i](self, (*self).pollUsers[i]);
    s_current = previous;
}

/** Closes registered windows, drains admitted owner work, joins startup work, and deactivates. */
void Application_finish(Application *self) {
    if (!self || !atomic_load(&(*self).active) || !pthread_equal(pthread_self(), (*self).ownerThread)) return;
    Application_close(self);
    for (uint32_t i = 0; i < (*self).window_count; i++)
        Window_close((*self).windows[i]);
    // Drain admitted owner work before joining a worker waiting on that work.
    while (!atomic_load_explicit(&(*self).workerDone, memory_order_acquire)) {
        Application_poll(self);
        Window_pollEvents();
        struct timespec slice = {0, 1000000}; nanosleep(&slice, nullptr);
    }
    if ((*self).workerLaunched) { pthread_join((*self).startThread, nullptr); (*self).workerLaunched = false; }
    atomic_store(&(*self).active, false);
}

/** Requests application closure through Application_close. */
void Application_stop(Application *self) {
    if (!self) return;
    Application_close(self);
}

/** Reads the atomic running flag; null applications are not running. */
bool Application_isRunning(const Application *self) {
    return self ? atomic_load_explicit(&(*self).running, memory_order_relaxed) : false;
}

/** Reports completion when stopped or when all applicable registered windows close. */
bool Application_isFinished(const Application *self) {
    if(self == nullptr)
        return true;
    if(!atomic_load_explicit(&(*self).running, memory_order_relaxed))
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
void Application_run(Application *self) {
    if (!self) return;
    if (!atomic_load(&(*self).active) && !Application_begin(self)) return;
    if (!pthread_equal(pthread_self(), (*self).ownerThread)) return;
    while (atomic_load_explicit(&(*self).running, memory_order_relaxed)) {
        if (appAllWindowsClosed(self)) {
            atomic_store_explicit(&(*self).running, false, memory_order_relaxed);
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
    if (atomic_load(&(*self).active) && !pthread_equal(pthread_self(), (*self).ownerThread)) return false;
    if (atomic_load(&(*self).active) && !Application_isRunning(self)) return false;
    for (uint32_t i = 0; i < (*self).window_count; i++)
        if ((*self).windows[i] == win) return false;
    if ((*self).window_count >= APP_MAX_WINDOWS) return false;
    (*self).windows[(*self).window_count++] = win;
    (*self).hadWindows = true;
    return true;
}

/** Removes a registered borrowed window without destroying it. */
bool Application_removeWindow(Application *self, Window *win) {
    if (!self || !win) return false;
    if (atomic_load(&(*self).active) && !pthread_equal(pthread_self(), (*self).ownerThread)) return false;
    for (uint32_t i = 0; i < (*self).window_count; i++) {
        if ((*self).windows[i] == win) {
            (*self).windows[i] = (*self).windows[--(*self).window_count];
            (*self).windows[(*self).window_count] = nullptr;
            return true;
        }
    }
    return false;
}

/** Installs the callback notified after a successful generation reload. */
void Application_onHotReload(Application *self, AppHotReloadFn fn, void *userdata) {
    if (!self) return;
    (*self).hotReloadFn = fn;
    (*self).hotReloadUserdata = userdata;
}

/** Polls the associated reload watcher via the hotcwap-provided seam. */
void Application_pollHot(Application *self) {
    if (!self || !(*self).hot || !(*self).hotPoll)
        return;
    uint32_t loaded = 0;
    int r = (*self).hotPoll((*self).hot, &loaded);
    if (r == 0 && loaded > 0 && (*self).hotReloadFn)
        (*self).hotReloadFn(self, loaded, (*self).hotReloadUserdata);
}

// SETTERS (PUBLIC & PRIVATE)
;;SETTER
/** Associates a borrowed reload watcher (opaque; hotcwap owns the type). */
void Application_setHot(Application *self, void *hot) {
    if (!self) return;
    (*self).hot = hot;
}

;;SETTER
/** Installs the reload poll seam (hotcwap provides; R0 never names HotModule). */
void Application_setHotPoll(Application *self, AppHotPollFn poll) {
    if (!self) return;
    (*self).hotPoll = poll;
}

;;SETTER
/** Binds the owning Kernel Lifetime so the host pump can reset scratch per pass. */
void Application_setHostLifetime(Application *self, void *lifetime) {
    if (!self) return;
    (*self).hostLifetime = lifetime;
}

;;SETTER
/** Copies a name into the application's fixed-capacity name field. */
void Application_setName(Application *self, const char *name) {
    if (!self || !name) return;
    strncpy((*self).name, name, APP_MAX_NAME - 1);
    (*self).name[APP_MAX_NAME - 1] = '\0';
}

;;SETTER
/** Copies an author label into the application's fixed-capacity field. */
void Application_setAuthor(Application *self, const char *author) {
    if (!self || !author) return;
    strncpy((*self).author, author, APP_MAX_NAME - 1);
    (*self).author[APP_MAX_NAME - 1] = '\0';
}

;;SETTER
/** Copies a version string into the application's fixed-capacity field. */
void Application_setVersion(Application *self, const char *version) {
    if (!self || !version) return;
    strncpy((*self).version, version, APP_MAX_VERSION - 1);
    (*self).version[APP_MAX_VERSION - 1] = '\0';
}

;;SETTER
/** Copies an icon path into the application's fixed-capacity field. */
void Application_setIconPath(Application *self, const char *iconPath) {
    if (!self || !iconPath) return;
    strncpy((*self).iconPath, iconPath, APP_MAX_ICON_PATH - 1);
    (*self).iconPath[APP_MAX_ICON_PATH - 1] = '\0';
}

// GETTERS (PUBLIC & PRIVATE)
;;GETTER
/** Returns the latest atomically published frames-per-second value. */
uint32_t Application_getFps(const Application *self) {
    return self ? atomic_load_explicit(&(*self).fps, memory_order_relaxed) : 0;
}

;;GETTER
/** Returns the latest atomically published frame duration in microseconds. */
uint32_t Application_getFrametimeUs(const Application *self) {
    return self ? atomic_load_explicit(&(*self).frametimeUs, memory_order_relaxed) : 0;
}

;;GETTER
/** Returns the associated borrowed reload watcher (opaque), or nullptr for null self. */
void *Application_getHot(const Application *self) {
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
Window *Application_getWindow(const Application *self, uint32_t index) {
    if (!self) return nullptr;
    if (index >= (*self).window_count) return nullptr;
    return (*self).windows[index];
}

;;GETTER
/** Returns the number of registered windows. */
uint32_t Application_getWindowCount(const Application *self) {
    if (!self) return 0;
    return (*self).window_count;
}

;;GETTER
/** Copies up to cap borrowed window pointers into out and returns the copied count. */
uint32_t Application_getWindows(const Application *self, Window **out, uint32_t cap) {
    if (!self || !out) return 0;
    uint32_t n = (*self).window_count;
    if (n > cap) n = cap;
    for (uint32_t i = 0; i < n; i++)
        out[i] = (*self).windows[i];
    return n;
}

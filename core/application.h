#ifndef HOT_PROCESS_APPLICATION_H
#define HOT_PROCESS_APPLICATION_H

#include "vex.h"
#include <pthread.h>

#include "../window/window.h"

// kernel/application.h — Executable-level manifest, window registry & hot-module slot.
//
// Application is a lifetime supervisor: identity (name/author/version/icon), the
// window registry, and a hot-module slot. Application is GUI by definition —
// the process taxonomy (the Vertical Integration Law) classifies anything that
// presents pixels through a Window as an Application; CLI functions become
// Process, TUI sessions become Console. There is no "mode": a manifest with
// zero windows is a degenerate (headless-test) Application, nothing more.
//
// Application_start is the blocking lifetime entry. It services native events
// until ALL registered windows close or Application_close is requested.
// Start events execute on one supervised worker. Graphics scheduling stays R3.
//
// Application_run is the KEEP-ALIVE PARKED LOOP (hotcwap's own, no graphvex):
// it BLOCKS until every registered window is closed, then ends the app by
// moving the phase to CLOSING. It parks in 5ms slices, letting the Window
// pump its own events in between — so an empty window lives on its own and
// Kernel_run(kernel, app) returns only once the user closed all windows.
//
// Ownership law: Application REGISTERS windows, never destroys them during
// steady state. Application_free frees the Application manifest struct.

#define APP_MAX_WINDOWS 16
#define APP_MAX_NAME 64
#define APP_MAX_VERSION 16
#define APP_MAX_ICON_PATH 512

typedef struct Application Application;

typedef fn (*AppHotReloadFn)(Application *self, u32 loaded, any userdata);
typedef fn (*ApplicationEventFn)(Application *self, any userdata);
typedef int (*AppHotPollFn)(any hot, u32 *loaded);
#define APP_MAX_EVENTS 16

// One lifecycle, one word. The legal transitions are listed here and enforced
// in application.c — never inferred from a boolean combination at the call site.
//
//      IDLE ──begin──▶ STARTING ──worker done──▶ RUNNING
//        ▲                │                        │
//        │                └────close/finish────┐   │
//        │                                     ▼   ▼
//        │                                   CLOSING
//        │                                     │ finish (join + release)
//        └──────────── (re-begin) ────────── STOPPED
//
// IDLE and STOPPED are the two freeable phases. STARTING, RUNNING and CLOSING
// are "admitted" (what the old `active` flag meant). STARTING and RUNNING are
// "running" (what the old `running` flag meant). CLOSING means close requested
// while the owner is still draining and has not joined the startup worker.
typedef enum AppPhase {
    APP_PHASE_IDLE = 0,   // constructed; nothing admitted yet
    APP_PHASE_STARTING,   // admitted; startup worker still executing callbacks
    APP_PHASE_RUNNING,    // admitted; startup worker finished (or none present)
    APP_PHASE_CLOSING,    // close requested; draining, worker not yet joined
    APP_PHASE_STOPPED,    // finished; worker joined, owner released
    APP_PHASE_COUNT
} AppPhase;

// The owner-work mailbox is a three-state slot, not a bool beside a pointer:
// IDLE (nothing queued) -> PENDING (queued, waiting for the owner) -> EXECUTING
// (owner is running the callback) -> IDLE. Guarded by invokeMutex.
typedef enum AppInvokePhase {
    APP_INVOKE_IDLE = 0,
    APP_INVOKE_PENDING,
    APP_INVOKE_EXECUTING,
    APP_INVOKE_COUNT
} AppInvokePhase;

struct Application {
    char name[APP_MAX_NAME];               // app name (default "vex")
    char author[APP_MAX_NAME];             // author / studio (default "")
    char version[APP_MAX_VERSION];         // version string, e.g. "1.0.0"
    char iconPath[APP_MAX_ICON_PATH];      // icon path reference (default "")
    Window *windows[APP_MAX_WINDOWS];      // registered top-level windows
    u32 window_count;                 // used slots in windows[]
    bool hadWindows;                      // one-way latch: a window was ever registered
    atomic_uint phase;                     // AppPhase: the single lifecycle word
    any hot;                             // reload watcher (opaque; hotcwap fills via setHotPoll)
    AppHotPollFn hotPoll;                  // reload poll seam (nullable; hotcwap provides)
    any hostLifetime;                    // owning Kernel Lifetime (opaque; set at start)
    _Atomic u32 fps;                  // live telemetry: FPS (graphvex writes)
    _Atomic u32 frametimeUs;          // live telemetry: frametime in microseconds (graphvex writes)
    AppHotReloadFn hotReloadFn;            // hot-reload notification callback (nullable)
    any hotReloadUserdata;               // userdata for hotReloadFn
    ApplicationEventFn startFns[APP_MAX_EVENTS];
    any startUsers[APP_MAX_EVENTS];
    u32 startCount;
    ApplicationEventFn pollFns[APP_MAX_EVENTS]; // owner-thread service bridges
    any pollUsers[APP_MAX_EVENTS];
    pthread_t ownerThread, startThread;
    _Atomic bool workerDone;               // one-way latch: startup callbacks finished
    bool workerLaunched;                   // startThread exists and needs a join
    pthread_mutex_t invokeMutex;
    pthread_cond_t invokeCondition;
    ApplicationEventFn invokeFn;           // one synchronous worker -> owner mailbox
    any invokeUser;
    AppInvokePhase invokePhase;            // under invokeMutex
};

// --- Subsystem bootstrap & shutdown ---
// One-shot bootstrap (System/input/HotFile via System_initializeAll).
// Auto-invoked by the FIRST Application() constructor; idempotent beyond
// that. Explicit calls are optional and redundant. Teardown
// (Application_shutdown) stays caller-owned.
bool Application_init();
fn Application_shutdown();

// --- Overloaded constructors (the Window chooser idiom) ---
//
//   Application()                          -> defaults ("vex", no windows)
//   Application("name")                    -> named
//   Application("name", "author", "1.0.0") -> full identity
//
// Strings are copied in (fixed storage, zero steady-state malloc).
// Getters return pointers into internal storage, stable until the next set.
Application *Application_0();
Application *Application_1(const char *name);
Application *Application_3(const char *name, const char *author, const char *version);

#define APPLICATION_CHOOSER(_0, _1, _2, _3, NAME, ...) NAME

#define Application(...) APPLICATION_CHOOSER( \
    dummy __VA_OPT__(,) __VA_ARGS__, \
    Application_3, Application_2, Application_1, Application_0 \
)(__VA_ARGS__)

// Free the Application. Registered windows are untouched (OS-owned).
fn Application_free(Application *self);

// --- Lifecycle (start/run/free on the native owner thread) ---
fn Application_start(Application *self);
// Nonblocking arm for Kernel's multi-application reactor; false if already active.
bool Application_begin(Application *self);
// Service owner callbacks/mailbox; Kernel and Application_run call this.
fn Application_poll(Application *self);
// Stop, close registered windows on their owner thread, drain/join worker.
fn Application_finish(Application *self);
// Thread-safe, idempotent close request. Does not destroy borrowed Window handles.
fn Application_close(Application *self);
fn Application_stop(Application *self);
bool Application_isRunning(const Application *self);
// One lifecycle word, readable from any thread (snapshot semantics).
AppPhase Application_getPhase(const Application *self);
// True in STARTING, RUNNING and CLOSING — the admitted phases where owner-thread
// work is legal. False in IDLE and STOPPED (the freeable phases).
bool Application_isActive(const Application *self);
// Stable symbolic name for a phase (cold path; never null).
const char *Application_getPhaseName(AppPhase phase);
// Register before starting. Start callbacks run in registration order on a worker.
bool Application_addStartEvent(Application *self, ApplicationEventFn fn, any userdata);
bool Application_addPollEvent(Application *self, ApplicationEventFn fn, any userdata);
bool Application_removePollEvent(Application *self, ApplicationEventFn fn, const_any userdata);
// Synchronous owner-thread work; false if closing. Never call while holding UI locks.
bool Application_invoke(Application *self, ApplicationEventFn fn, any userdata);
// Current application on its native owner thread (NULL outside lifecycle).
Application *Application_current();

// --- Completion predicate (drives the Kernel completion reactor) ---
// True when the app can never end itself: the phase has left STARTING/RUNNING
// (external stop), or EVERY registered window reports shouldClose (including
// removal of the last previously registered window). An app that has never had
// windows lives until an explicit close/stop request.
bool Application_isFinished(const Application *self);

// --- Keep-alive parked loop (hotcwap's own) ---
// BLOCKS until every registered window is closed (or stop flips running).
// The Window pumps native events; spoke input and owner service callbacks run
// in 5ms slices. Empty windows live on their own — no graphvex dependency.
fn Application_run(Application *self);

// --- Hot-reload drive (generation-driven, _main thread only) ---
// Poll this app's reload watcher (bind via Application_setHot + setHotPoll):
// the seam polls the watcher, then the hotReloadFn notification fires for
// every executed swap. No-op when hot or hotPoll is NULL/unset. R0 never
// names the watcher type — hotcwap adapts. Called by the Kernel reactor
// and the parked loop at a ~250ms cadence.
fn Application_pollHot(Application *self);

// --- Lifecycle callbacks ---
fn Application_onHotReload(Application *self, AppHotReloadFn fn, any userdata);

// --- Telemetry ---
u32   Application_getFps(const Application *self);
u32   Application_getFrametimeUs(const Application *self);
any Application_getHot(const Application *self);
// Opt-in: assign a pre-initialized reload watcher (opaque; hotcwap owns the
// type) plus its poll seam. NULL disables.
fn        Application_setHot(Application *self, any hot);
fn        Application_setHotPoll(Application *self, AppHotPollFn poll);
fn        Application_setHostLifetime(Application *self, any lifetime);

// --- Identity: symmetric setters / getters (the Symmetric Getter/Setter Completeness Law) ---
fn        Application_setName(Application *self, const char *name);
const char *Application_getName(const Application *self);
fn        Application_setAuthor(Application *self, const char *author);
const char *Application_getAuthor(const Application *self);
fn        Application_setVersion(Application *self, const char *version);
const char *Application_getVersion(const Application *self);
fn        Application_setIconPath(Application *self, const char *iconPath);
const char *Application_getIconPath(const Application *self);

// --- Window registry (multiwindow) ---
// Register a live window. False on NULL, duplicate, or full registry.
bool      Application_addWindow(Application *self, Window *win);
// Unregister a window (swap-remove, order not preserved). False if absent.
bool      Application_removeWindow(Application *self, Window *win);
// Window at index, or NULL when out of range.
Window   *Application_getWindow(const Application *self, u32 index);
// Number of registered windows.
u32  Application_getWindowCount(const Application *self);
// Copy registry into out[] (up to cap), returns entries written.
u32  Application_getWindows(const Application *self, Window **out, u32 cap);

#endif

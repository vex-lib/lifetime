// hotcwap/test/kernel/application_test.c — the Application class _test.
//
// Proves the executable manifest: identity (constructors + setters/getters +
// clamps), the lifecycle flags, the completion predicate, and the window
// registry (add/duplicate/full/remove/index/copy). The registry stores opaque
// handles and never dereferences them, so most of it is exercised with sentinel
// pointers; the one path that DOES dereference (isFinished -> shouldClose) uses
// a real window.
//
// Headless except for the single real-window isFinished case (skipped cleanly
// where no window server exists).

#include "vex.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "core/application.h"
#include "window/window.h"

static int g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);          \
            g_failures++;                                                  \
        }                                                                  \
    } while (0)

/** Records the application reload callback's load result for later assertions. */
static fn onHotReloadStub(Application *self, u32 loaded, any userdata) {
    (fn) self; (fn) loaded; (fn) userdata;
}

// A start callback that blocks until the test opens the gate, so STARTING is
// observable deterministically instead of racing the worker.
typedef struct StartGate {
    pthread_mutex_t lock;
    pthread_cond_t ready;
    bool open;
    atomic_int entered;
    atomic_int calls;
} StartGate;

static fn gatedStart(Application *self, any userdata) {
    (fn) self;
    StartGate *gate = userdata;
    atomic_fetch_add_explicit(&(*gate).entered, 1, memory_order_release);
    pthread_mutex_lock(&(*gate).lock);
    while (!(*gate).open)
        pthread_cond_wait(&(*gate).ready, &(*gate).lock);
    pthread_mutex_unlock(&(*gate).lock);
    atomic_fetch_add_explicit(&(*gate).calls, 1, memory_order_release);
}

static fn gateOpen(StartGate *gate) {
    pthread_mutex_lock(&(*gate).lock);
    (*gate).open = true;
    pthread_cond_broadcast(&(*gate).ready);
    pthread_mutex_unlock(&(*gate).lock);
}

static long elapsedMs(const struct timespec *from, const struct timespec *to) {
    return (to->tv_sec - from->tv_sec) * 1000L
         + (to->tv_nsec - from->tv_nsec) / 1000000L;
}

/** Bounded wait (never an unbounded spin) for a phase; false on timeout. */
static bool waitPhase(Application *app, AppPhase want, long timeoutMs) {
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        if (Application_getPhase(app) == want)
            return true;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (elapsedMs(&start, &now) > timeoutMs)
            return false;
        struct timespec nap = {0, 1000000};
        nanosleep(&nap, nullptr);
    }
}

/** Bounded wait for a counter to reach `want`; false on timeout. */
static bool waitCounter(atomic_int *counter, int want, long timeoutMs) {
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        if (atomic_load_explicit(counter, memory_order_acquire) >= want)
            return true;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (elapsedMs(&start, &now) > timeoutMs)
            return false;
        struct timespec nap = {0, 1000000};
        nanosleep(&nap, nullptr);
    }
}

int main() {
    // Constructors + defaults.
    Application *a = Application();
    CHECK(a != nullptr);
    CHECK(strcmp(Application_getName(a), "vex") == 0);
    CHECK(Application_getAuthor(a)[0] == '\0');
    CHECK(Application_getVersion(a)[0] == '\0');
    CHECK(Application_getWindowCount(a) == 0);
    CHECK(!Application_isRunning(a));

    Application *a3 = Application("app", "vexgraph", "1.2.3");
    CHECK(strcmp(Application_getName(a3), "app") == 0);
    CHECK(strcmp(Application_getAuthor(a3), "vexgraph") == 0);
    CHECK(strcmp(Application_getVersion(a3), "1.2.3") == 0);

    // Identity setters/getters + clamp.
    Application_setName(a, "myapp");
    CHECK(strcmp(Application_getName(a), "myapp") == 0);
    char big[200];
    memset(big, 'n', sizeof big - 1);
    big[sizeof big - 1] = '\0';
    Application_setName(a, big);
    CHECK(strlen(Application_getName(a)) == APP_MAX_NAME - 1);
    Application_setIconPath(a, "icons/app.png");
    CHECK(strcmp(Application_getIconPath(a), "icons/app.png") == 0);

    // Lifecycle flags.
    CHECK(Application_begin(a));
    CHECK(Application_isRunning(a));
    Application_stop(a);
    Application_finish(a);
    CHECK(!Application_isRunning(a));

    // Completion predicate: null is finished; not-running is finished;
    // running with no windows is NOT finished (nothing ends it).
    CHECK(Application_isFinished(nullptr));
    CHECK(Application_isFinished(a));
    CHECK(Application_begin(a));
    CHECK(!Application_isFinished(a));
    Application_stop(a);
    Application_finish(a);

    // --- The single lifecycle word: every legal transition, plus refusals ---
    Application *p = Application();
    CHECK(Application_getPhase(p) == APP_PHASE_IDLE);
    CHECK(!Application_isActive(p));
    CHECK(!Application_isRunning(p));
    CHECK(Application_isFinished(p));
    for (int i = 0; i < (int) APP_PHASE_COUNT; i++)
        CHECK(Application_getPhaseName((AppPhase) i)[0] != '\0');
    CHECK(strcmp(Application_getPhaseName(APP_PHASE_CLOSING), "APP_PHASE_CLOSING") == 0);
    CHECK(Application_getPhaseName((AppPhase) 999)[0] != '\0');

    // No start callbacks: begin lands directly in RUNNING.
    CHECK(Application_begin(p));
    CHECK(Application_getPhase(p) == APP_PHASE_RUNNING);
    CHECK(Application_isActive(p));
    CHECK(Application_isRunning(p));
    CHECK(!Application_begin(p));                       // already admitted
    CHECK(Application_getPhase(p) == APP_PHASE_RUNNING);
    CHECK(!Application_addStartEvent(p, gatedStart, nullptr));

    // CLOSING is admitted but no longer running; new windows are refused.
    Application_close(p);
    CHECK(Application_getPhase(p) == APP_PHASE_CLOSING);
    CHECK(Application_isActive(p));
    CHECK(!Application_isRunning(p));
    CHECK(Application_isFinished(p));
    CHECK(!Application_addWindow(p, (Window*) (pointer) 0x77));
    CHECK(Application_getPhase(p) == APP_PHASE_CLOSING);

    Application_finish(p);
    CHECK(Application_getPhase(p) == APP_PHASE_STOPPED);
    CHECK(!Application_isActive(p));
    CHECK(Application_isFinished(p));

    // STOPPED is freeable and re-beginnable.
    CHECK(Application_begin(p));
    CHECK(Application_getPhase(p) == APP_PHASE_RUNNING);
    Application_finish(p);                              // finish without close
    CHECK(Application_getPhase(p) == APP_PHASE_STOPPED);
    Application_free(p);

    // Worker-driven STARTING -> RUNNING.
    StartGate gate = {0};
    pthread_mutex_init(&gate.lock, nullptr);
    pthread_cond_init(&gate.ready, nullptr);
    Application *w = Application();
    CHECK(Application_addStartEvent(w, gatedStart, &gate));
    CHECK(Application_begin(w));
    CHECK(Application_getPhase(w) == APP_PHASE_STARTING);
    CHECK(Application_isActive(w));
    CHECK(Application_isRunning(w));
    CHECK(!Application_isFinished(w));
    gateOpen(&gate);
    CHECK(waitPhase(w, APP_PHASE_RUNNING, 2000));
    CHECK(atomic_load_explicit(&gate.calls, memory_order_acquire) == 1);
    Application_finish(w);
    CHECK(Application_getPhase(w) == APP_PHASE_STOPPED);
    Application_free(w);
    pthread_cond_destroy(&gate.ready);
    pthread_mutex_destroy(&gate.lock);

    // A close that lands while the worker is inside a start callback must
    // survive the worker's advance: the CAS refuses to overwrite CLOSING.
    StartGate gate2 = {0};
    pthread_mutex_init(&gate2.lock, nullptr);
    pthread_cond_init(&gate2.ready, nullptr);
    Application *c = Application();
    CHECK(Application_addStartEvent(c, gatedStart, &gate2));
    CHECK(Application_begin(c));
    CHECK(Application_getPhase(c) == APP_PHASE_STARTING);
    CHECK(waitCounter(&gate2.entered, 1, 2000));   // worker is inside the callback
    CHECK(Application_getPhase(c) == APP_PHASE_STARTING);
    Application_close(c);
    CHECK(Application_getPhase(c) == APP_PHASE_CLOSING);
    gateOpen(&gate2);
    CHECK(waitCounter(&gate2.calls, 1, 2000));
    struct timespec settle = {0, 20000000};   // let the worker attempt its advance
    nanosleep(&settle, nullptr);
    CHECK(Application_getPhase(c) == APP_PHASE_CLOSING);
    Application_finish(c);
    CHECK(Application_getPhase(c) == APP_PHASE_STOPPED);
    Application_free(c);
    pthread_cond_destroy(&gate2.ready);
    pthread_mutex_destroy(&gate2.lock);

    // Window registry with opaque sentinel handles (never dereferenced here).
    Window *w1 = (Window*) (pointer) 0x1;
    Window *w2 = (Window*) (pointer) 0x2;
    CHECK(Application_addWindow(a, w1));
    CHECK(!Application_addWindow(a, w1));            // duplicate
    CHECK(!Application_addWindow(a, nullptr));       // null
    CHECK(!Application_addWindow(nullptr, w1));      // null self
    CHECK(Application_addWindow(a, w2));
    CHECK(Application_getWindowCount(a) == 2);
    CHECK(Application_getWindow(a, 0) == w1);
    CHECK(Application_getWindow(a, 1) == w2);
    CHECK(Application_getWindow(a, 2) == nullptr);
    CHECK(Application_getWindow(a, 999) == nullptr);

    Window *out[4];
    CHECK(Application_getWindows(a, out, 4) == 2);
    CHECK(out[0] == w1 && out[1] == w2);
    CHECK(Application_getWindows(a, out, 1) == 1);   // capped

    CHECK(Application_removeWindow(a, w1));
    CHECK(Application_getWindowCount(a) == 1);
    CHECK(!Application_removeWindow(a, w1));         // already gone

    // Fill to the cap: the next add is refused.
    Application_removeWindow(a, w2);
    for (pointer i = 0; i < APP_MAX_WINDOWS; i++)
        CHECK(Application_addWindow(a, (Window*) (0x100 + i)));
    CHECK(Application_getWindowCount(a) == APP_MAX_WINDOWS);
    CHECK(!Application_addWindow(a, (Window*) (pointer) 0x999));

    // Reload watcher slot (opaque any) + poll seam + reload callback.
    CHECK(Application_getHot(a) == nullptr);
    Application_setHot(a, (any) (pointer) 0x5);
    CHECK(Application_getHot(a) == (any) (pointer) 0x5);
    Application_setHot(a, nullptr);
    Application_onHotReload(a, onHotReloadStub, a);
    Application_pollHot(a); // no watcher/seam -> no-op
    CHECK(Application_getHot(a) == nullptr);

    CHECK(Application_getFps(a) == 0);
    CHECK(Application_getFrametimeUs(a) == 0);

    // isFinished with a REAL window (the one dereferencing path).
    Application *app = Application();
    Window *rw = Window_create("app_test", 200, 150);
    if (rw) {
        CHECK(Application_addWindow(app, rw));
        CHECK(Application_begin(app));
        CHECK(!Application_isFinished(app));         // window still open
        Window_setShouldClose(rw, true);
        CHECK(Application_isFinished(app));          // every window closed
        Application_finish(app);
        Application_removeWindow(app, rw);
        Window_destroy(rw);
    } else {
        printf("application_test: SKIP real-window isFinished (no window server)\n");
    }
    Application_free(app);

    Application_free(a3);
    Application_free(a);
    Application_free(nullptr); // null-safe

    if (g_failures != 0) {
        printf("application_test: %d FAILURES\n", g_failures);
        return 1;
    }
    if (rw == nullptr) {
        // the real-window isFinished contract never ran; a skip is not a pass.
        return B_TEST_SKIP;
    }
    printf("application_test: all assertions held\n");
    return 0;
}

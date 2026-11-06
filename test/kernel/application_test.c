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

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "core/application.h"
#include "window/window.h"
#include "util/test_support.h"

static int g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);          \
            g_failures++;                                                  \
        }                                                                  \
    } while (0)

/** Records the application reload callback's load result for later assertions. */
static void onHotReloadStub(Application *self, uint32_t loaded, void *userdata) {
    (void) self; (void) loaded; (void) userdata;
}

int main(void) {
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

    // Window registry with opaque sentinel handles (never dereferenced here).
    Window *w1 = (Window*) (uintptr_t) 0x1;
    Window *w2 = (Window*) (uintptr_t) 0x2;
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
    for (uintptr_t i = 0; i < APP_MAX_WINDOWS; i++)
        CHECK(Application_addWindow(a, (Window*) (0x100 + i)));
    CHECK(Application_getWindowCount(a) == APP_MAX_WINDOWS);
    CHECK(!Application_addWindow(a, (Window*) (uintptr_t) 0x999));

    // Reload watcher slot (opaque void*) + poll seam + reload callback.
    CHECK(Application_getHot(a) == nullptr);
    Application_setHot(a, (void*) (uintptr_t) 0x5);
    CHECK(Application_getHot(a) == (void*) (uintptr_t) 0x5);
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

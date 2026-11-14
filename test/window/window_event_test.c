// _tests/hotcwap/window_event_test.c — the WindowEvent lifecycle registry test
// (headless, no Window backend: exercises only the class API surface).
//
// Cold-seam coverage per the Cold-Strict, Hot-Minimal Validation Law: symmetric
// getter/setter completeness, fire-dispatch forwarding of self + window,
// null-slot no-ops, null-self safety, and the single vetoable slot
// (onQuitRequested — true = may quit, false = app declines).

#include <stdint.h>
#include <stdio.h>

#include "util/annotation.h"
#include "window/window_event.h"

;;OVERVIEW
/**
 * ============================================================================
 * CLASS: Recorder (window_event_test/test.c — WindowEvent class _test)
 * LEVEL: L2 — Behavior (headless seam test; no OS surfaces touched)
 * ============================================================================
 * A microscopic recording sink that verifies the WindowEvent fire dispatchers
 * forward self + window and deliver the right payload (w/h for resized, x/y for
 * moved, visible flag for occlusionChanged), that unset slots are silent no-ops,
 * that onQuitRequested keeps the vetoable bool contract, and that every
 * getter mirrors its setter.
 *
 * STRUCT FIELDS (local to this file — exactly this file's class):
 * ----------------------------------------------------------------------------
 *   Window *lastWindow;    // the window pointer the last fire forwarded
 *   int fired;             // count of fn-slot callbacks received
 *   int resizedW;          // received width from onResized
 *   int resizedH;          // received height from onResized
 *   int movedX;            // received x from onMoved
 *   int movedY;            // received y from onMoved
 *   int movedCalls;        // count of onMoved invocations
 *   bool quitAnswer;       // what the quit slot replies (true = may quit)
 *   int quitCalls;         // count of quit slot invocations
 *   bool focused;          // focus-state echo: true = gained, false = lost
 *   int occludedCalls;     // count of occlusion slot invocations
 *   bool lastVisible;      // received visible flag from onOcclusionChanged
 *
 * FUNCTION REGISTRY:
 * ----------------------------------------------------------------------------
 * Core Functions:
 *   - onResizedRec / onMovedRec / onFullscreenRec / onMinimizedRec /
 *     onRestoredRec / onPressedRec / onFocusGainedRec / onFocusLostRec /
 *     onZoomFilledRec / onOcclusionRec :
 *     record self -> window + payload into the shared Recorder
 *   - onQuitAllowRec / onQuitVetoRec : answer true / false respectively
 *   - main() : the assertion tour
 * ============================================================================
  */

typedef struct {
    Window *lastWindow;
    int fired;
    int resizedW;
    int resizedH;
    int movedX;
    int movedY;
    int movedCalls;
    bool quitAnswer;
    int quitCalls;
    bool focused;
    int occludedCalls;
    bool lastVisible;
} Recorder;

static Recorder g_rec;

// Counts invocations of a slot whose stored context is null (context is data).
static int g_nullOwnerHits;

// Null-guarding slot used to prove that a fire with a null stored context
// still reaches the callback (the callback is responsible for its own guard).
static fn onResizedNullOwnerRec(any self, Window *window, int width, int height) {
    (fn) self;
    (fn) window;
    (fn) width;
    (fn) height;
    g_nullOwnerHits++;
}

/** Records the resize callback's window and dimensions in the fixture. */
static fn onResizedRec(any self, Window *window, int width, int height) {
    Recorder *r = (Recorder*) self;
    (*r).lastWindow = window;
    (*r).resizedW = width;
    (*r).resizedH = height;
    (*r).fired++;
}

/** Records the move callback's window and top-left coordinates in the fixture. */
static fn onMovedRec(any self, Window *window, int x, int y) {
    Recorder *r = (Recorder*) self;
    (*r).lastWindow = window;
    (*r).movedX = x;
    (*r).movedY = y;
    (*r).movedCalls++;
}

/** Records delivery of the fullscreen callback and its window argument. */
static fn onFullscreenRec(any self, Window *window) {
    Recorder *r = (Recorder*) self;
    (*r).lastWindow = window;
    (*r).fired++;
}

/** Records delivery of the minimized callback and its window argument. */
static fn onMinimizedRec(any self, Window *window) {
    Recorder *r = (Recorder*) self;
    (*r).lastWindow = window;
    (*r).fired++;
}

/** Records delivery of the restored callback and its window argument. */
static fn onRestoredRec(any self, Window *window) {
    Recorder *r = (Recorder*) self;
    (*r).lastWindow = window;
    (*r).fired++;
}

/** Records delivery of the pressed callback and its window argument. */
static fn onPressedRec(any self, Window *window) {
    Recorder *r = (Recorder*) self;
    (*r).lastWindow = window;
    (*r).fired++;
}

/** Records focus acquisition and the associated window. */
static fn onFocusGainedRec(any self, Window *window) {
    Recorder *r = (Recorder*) self;
    (*r).lastWindow = window;
    (*r).focused = true;
    (*r).fired++;
}

/** Records focus loss and the associated window. */
static fn onFocusLostRec(any self, Window *window) {
    Recorder *r = (Recorder*) self;
    (*r).lastWindow = window;
    (*r).focused = false;
    (*r).fired++;
}

/** Records delivery of the zoom-filled callback and its window argument. */
static fn onZoomFilledRec(any self, Window *window) {
    Recorder *r = self;
    (*r).lastWindow = window;
    (*r).fired++;
}

/** Records the occlusion callback's window and visibility state. */
static fn onOcclusionRec(any self, Window *window, bool visible) {
    Recorder *r = (Recorder*) self;
    (*r).lastWindow = window;
    (*r).lastVisible = visible;
    (*r).occludedCalls++;
}

/** Records a quit request and answers that the window may close. */
static bool onQuitAllowRec(any self, Window *window) {
    Recorder *r = (Recorder*) self;
    (*r).lastWindow = window;
    (*r).quitCalls++;
    return true;
}

/** Records a quit request and answers that the window must remain open. */
static bool onQuitVetoRec(any self, Window *window) {
    Recorder *r = (Recorder*) self;
    (*r).lastWindow = window;
    (*r).quitCalls++;
    return false;
}

#define CHECK(cond)                                                          \
    do {                                                                     \
        if(!(cond)) {                                                        \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
            failures++;                                                      \
        }                                                                    \
    } while(0)

int main() {
    int failures = 0;
    WindowEvent ev;
    (fn) WindowEvent_init(&ev);
    Window *dummyWindow = (Window*) (pointer) 0x1234;
    Window *nullWindow = nullptr;

    // init zeroes every slot; symmetric getters report nullptr/0.
    CHECK(WindowEvent_getSelf(&ev) == nullptr);
    CHECK(WindowEvent_getOnQuitRequested(&ev) == nullptr);
    CHECK(WindowEvent_getOnResized(&ev) == nullptr);
    CHECK(WindowEvent_getOnMoved(&ev) == nullptr);
    CHECK(WindowEvent_getOnFullscreen(&ev) == nullptr);
    CHECK(WindowEvent_getOnMinimized(&ev) == nullptr);
    CHECK(WindowEvent_getOnRestored(&ev) == nullptr);
    CHECK(WindowEvent_getOnPressed(&ev) == nullptr);
    CHECK(WindowEvent_getOnFocusGained(&ev) == nullptr);
    CHECK(WindowEvent_getOnFocusLost(&ev) == nullptr);
    CHECK(WindowEvent_getOnZoomFilled(&ev) == nullptr);
    CHECK(WindowEvent_getOnOcclusionChanged(&ev) == nullptr);

    // Unset-slot fires are silent no-ops.
    WindowEvent_fireResized(&ev, dummyWindow, 800, 600);
    WindowEvent_fireMoved(&ev, dummyWindow, 10, 20);
    WindowEvent_firePressed(&ev, dummyWindow);
    WindowEvent_fireOcclusionChanged(&ev, dummyWindow, false);
    CHECK(g_rec.fired == 0);
    CHECK(g_rec.resizedW == 0);
    CHECK(g_rec.movedCalls == 0);
    CHECK(g_rec.occludedCalls == 0);

    // Quit with no slot set: default-proceed (may quit).
    CHECK(WindowEvent_fireQuitRequested(&ev, dummyWindow) == true);

    // Null-self fires are no-ops; null-self quit allows the quit.
    WindowEvent_fireResized(nullptr, dummyWindow, 1, 2);
    WindowEvent_fireMoved(nullptr, dummyWindow, 3, 4);
    WindowEvent_firePressed(nullptr, dummyWindow);
    WindowEvent_fireFocusGained(nullptr, dummyWindow);
    WindowEvent_fireOcclusionChanged(nullptr, dummyWindow, true);
    CHECK(WindowEvent_fireQuitRequested(nullptr, dummyWindow) == true);

    // Attach the recorder as self + every slot via the symmetric setter API.
    WindowEvent_setSelf(&ev, &g_rec);
    WindowEvent_setOnResized(&ev, onResizedRec);
    WindowEvent_setOnMoved(&ev, onMovedRec);
    WindowEvent_setOnFullscreen(&ev, onFullscreenRec);
    WindowEvent_setOnMinimized(&ev, onMinimizedRec);
    WindowEvent_setOnRestored(&ev, onRestoredRec);
    WindowEvent_setOnPressed(&ev, onPressedRec);
    WindowEvent_setOnFocusGained(&ev, onFocusGainedRec);
    WindowEvent_setOnFocusLost(&ev, onFocusLostRec);
    WindowEvent_setOnZoomFilled(&ev, onZoomFilledRec);
    WindowEvent_setOnOcclusionChanged(&ev, onOcclusionRec);

    // Getters mirror the setters (Exact Slot: the symmetric completeness law).
    CHECK(WindowEvent_getSelf(&ev) == &g_rec);
    CHECK(WindowEvent_getOnResized(&ev) == onResizedRec);
    CHECK(WindowEvent_getOnMoved(&ev) == onMovedRec);
    CHECK(WindowEvent_getOnFullscreen(&ev) == onFullscreenRec);
    CHECK(WindowEvent_getOnMinimized(&ev) == onMinimizedRec);
    CHECK(WindowEvent_getOnRestored(&ev) == onRestoredRec);
    CHECK(WindowEvent_getOnPressed(&ev) == onPressedRec);
    CHECK(WindowEvent_getOnFocusGained(&ev) == onFocusGainedRec);
    CHECK(WindowEvent_getOnFocusLost(&ev) == onFocusLostRec);
    CHECK(WindowEvent_getOnZoomFilled(&ev) == onZoomFilledRec);
    CHECK(WindowEvent_getOnOcclusionChanged(&ev) == onOcclusionRec);

    // Fire resized: self + window + payload arrive at the slot.
    WindowEvent_fireResized(&ev, dummyWindow, 1280, 720);
    CHECK(g_rec.fired == 1);
    CHECK(g_rec.lastWindow == dummyWindow);
    CHECK(g_rec.resizedW == 1280);
    CHECK(g_rec.resizedH == 720);

    // Fire moved: self + window + top-left payload arrive at the slot.
    WindowEvent_fireMoved(&ev, dummyWindow, 42, 99);
    CHECK(g_rec.movedCalls == 1);
    CHECK(g_rec.lastWindow == dummyWindow);
    CHECK(g_rec.movedX == 42);
    CHECK(g_rec.movedY == 99);

    // Void slots forward self + window; null window is a legal fire target.
    WindowEvent_fireFullscreen(&ev, nullWindow);
    WindowEvent_fireMinimized(&ev, dummyWindow);
    WindowEvent_fireRestored(&ev, dummyWindow);
    WindowEvent_firePressed(&ev, dummyWindow);
    WindowEvent_fireZoomFilled(&ev, dummyWindow);
    CHECK(g_rec.fired == 6);

    // Focus events distinguish gained vs lost through the shared context.
    WindowEvent_fireFocusGained(&ev, dummyWindow);
    CHECK(g_rec.focused == true);
    WindowEvent_fireFocusLost(&ev, dummyWindow);
    CHECK(g_rec.focused == false);
    CHECK(g_rec.fired == 8);

    // Occlusion carries the visible flag both ways through the shared context.
    WindowEvent_fireOcclusionChanged(&ev, dummyWindow, false);
    CHECK(g_rec.occludedCalls == 1);
    CHECK(g_rec.lastVisible == false);
    CHECK(g_rec.lastWindow == dummyWindow);
    WindowEvent_fireOcclusionChanged(&ev, dummyWindow, true);
    CHECK(g_rec.occludedCalls == 2);
    CHECK(g_rec.lastVisible == true);

    // Vetoable quit: the slot's bool answer is the fire's verdict.
    WindowEvent_setOnQuitRequested(&ev, onQuitAllowRec);
    CHECK(WindowEvent_fireQuitRequested(&ev, dummyWindow) == true);
    WindowEvent_setOnQuitRequested(&ev, onQuitVetoRec);
    CHECK(WindowEvent_fireQuitRequested(&ev, dummyWindow) == false);
    CHECK(g_rec.quitCalls == 2);

    // The stored self (slot context) is pure data: a fire still reaches the
    // slot when the context is null — the callback owns its own null-guard
    // (the Cold-Strict, Hot-Minimal Validation Law).
    WindowEvent_setSelf(&ev, nullptr);
    WindowEvent_setOnResized(&ev, onResizedNullOwnerRec);
    WindowEvent_fireResized(&ev, dummyWindow, 2, 4);
    CHECK(g_nullOwnerHits == 1);

    // Clear a slot back to null; fire becomes a no-op again.
    WindowEvent_setSelf(&ev, &g_rec);
    WindowEvent_setOnResized(&ev, nullptr);
    CHECK(WindowEvent_getOnResized(&ev) == nullptr);
    WindowEvent_fireResized(&ev, dummyWindow, 9, 9);
    CHECK(g_rec.resizedW == 1280);
    WindowEvent_setOnOcclusionChanged(&ev, nullptr);
    CHECK(WindowEvent_getOnOcclusionChanged(&ev) == nullptr);
    WindowEvent_fireOcclusionChanged(&ev, dummyWindow, false);
    CHECK(g_rec.occludedCalls == 2);

    // Setters with null self / null fn are safe, symmetric no-ops.
    WindowEvent_setSelf(nullptr, &g_rec);
    WindowEvent_setOnResized(nullptr, onResizedRec);
    WindowEvent_setOnMoved(nullptr, onMovedRec);
    WindowEvent_setOnResized(&ev, nullptr);
    WindowEvent_setOnQuitRequested(nullptr, onQuitAllowRec);
    WindowEvent_setOnOcclusionChanged(nullptr, onOcclusionRec);
    WindowEvent_setOnOcclusionChanged(&ev, nullptr);

    // Getters on null self return safe defaults per the symmetric law.
    CHECK(WindowEvent_getSelf(nullptr) == nullptr);
    CHECK(WindowEvent_getOnResized(nullptr) == nullptr);
    CHECK(WindowEvent_getOnMoved(nullptr) == nullptr);
    CHECK(WindowEvent_getOnFocusGained(nullptr) == nullptr);
    CHECK(WindowEvent_getOnZoomFilled(nullptr) == nullptr);
    CHECK(WindowEvent_getOnOcclusionChanged(nullptr) == nullptr);

    // init on null / on an initialized instance: null fails safe, re-init zeros.
    CHECK(WindowEvent_init(nullptr) == false);
    (fn) WindowEvent_init(&ev);
    CHECK(WindowEvent_getOnQuitRequested(&ev) == nullptr);
    CHECK(WindowEvent_getSelf(&ev) == nullptr);

    if(failures == 0)
        printf("PASS window_event_test: all assertions held\n");
    else
        fprintf(stderr, "FAIL window_event_test: %d assertion(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}

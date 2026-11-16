// hotcwap/test/window/window_test.c — the Window class _test.
//
// DEFAULT (automated, headless-safe, self-terminating): creates a real
// window, asserts the synchronous chrome/policy CONTRACT (style-mask bits,
// the atomic present/transparency/enabled mirrors and render-generation
// bumps, adapter add/remove identity, lifecycle + id), pumps a bounded
// frame budget, destroys, and exits nonzero on any failed check.
//
// `--tour` (IRL probe, NOT part of the automated suite): the visible
// traffic-light tour — each chrome combination is held ~2s so it can be
// eyeballed, then the window returns to the default decorated state to be
// closed by hand or with Esc.
//
// The Window Oracle Law's lab/IRL split: the automated half is deterministic
// and bounded; the tour is the IRL probe and is never presented as proof.

#include "vex.h"
#include <stdio.h>
#include <string.h>

#include "engine/loop.h"
#include "input/key.h"
#include "input/mouse.h"
#include "input/touch.h"
#include "window/window.h"

static i32 g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);          \
            g_failures++;                                                  \
        }                                                                  \
    } while (0)

typedef struct {
    Window *window;
    Loop loop;
    i32 frames;
    i32 frameBudget;
} win_ctx_t;

;;OVERVIEW
/**
 * ============================================================================
 * CLASS: win_ctx_t (hotcwap/test/window/window_test.c — Window class _test)
 * LEVEL: L4 — Self-Management (a test driving the L4 window)
 * ============================================================================
 * Automated chrome-contract asserts + a bounded event pump; in --tour mode,
 * the visible traffic-light tour. Input listeners echo key/mouse/scroll/move.
 *
 * STRUCT FIELDS (local to this file — exactly this file's class):
 * ----------------------------------------------------------------------------
 *   Window *window;    // test window (OS-owned handle)
 *   Loop loop;         // fixed-timestep loop state
 *   i32 frames;        // pumped frame counter
 *   i32 frameBudget;   // bounded stop (0 = run until closed, the tour case)
 *
 * FUNCTION REGISTRY:
 * ----------------------------------------------------------------------------
 * Core Functions:
 *   - on_key_down/up(self, keyEvent, nanos) : Key echo; Esc stops the loop
 *   - on_mouse_down(self, mouseEvent, nanos) : Button/tap/position echo
 *   - on_scroll(self, dx, dy)                : Scroll-delta echo
 *   - on_move(self, x, y)                    : Throttled (1/60) move echo
 *   - win_tick(userdata)                     : Per-frame pump + bounded stop
 *   - runChromeContract(window)              : The synchronous contract asserts
 *   - runTour(ctx)                           : The visible IRL traffic-light tour
 *   - main(argc, argv)                       : automated by default; --tour IRL
 * ============================================================================
 */

static win_ctx_t *g_winCtx = nullptr;

;;GETTER
static Window *WinCtx_window(const win_ctx_t *ctx) { return (*ctx).window; }
;;GETTER
static Loop *WinCtx_loopMut(win_ctx_t *ctx) { return &(*ctx).loop; }
;;GETTER
static i32 WinCtx_frames(const win_ctx_t *ctx) { return (*ctx).frames; }
;;SETTER
static fn WinCtx_setFrames(win_ctx_t *ctx, i32 v) { (*ctx).frames = v; }
;;GETTER
static i32 WinCtx_frameBudget(const win_ctx_t *ctx) { return (*ctx).frameBudget; }

// --- Input listeners: echo everything the pipeline produces. ---

static fn on_key_down(any self, i32 keyEvent, u64 nanos) {
    (discard) self; (discard) nanos;
    printf("key DOWN  %-12s code=%d\n", Key_name(Key_code(keyEvent)), Key_code(keyEvent));
}

/** Prints key-up events and stops the tour when Escape is released. */
static fn on_key_up(any self, i32 keyEvent, u64 nanos) {
    (discard) self; (discard) nanos;
    printf("key UP    %-12s hold=%.1fms taps=%d\n", Key_name(Key_code(keyEvent)),
           Key_lastHoldDurationNanos(Key_code(keyEvent)) / 1e6,
           Key_taps(Key_code(keyEvent)));
    if (Key_code(keyEvent) == KEY_ESCAPE && g_winCtx)
        Loop_stop(WinCtx_loopMut(g_winCtx)); // Esc exits the tour
}

/** Reports mouse-button presses with the current pointer position. */
static fn on_mouse_down(any self, i32 mouseEvent, u64 nanos) {
    (discard) self; (discard) nanos;
    printf("mouse DOWN %s taps=%d at (%.0f, %.0f)\n",
           Mouse_name(Mouse_button(mouseEvent)), Mouse_taps(Mouse_button(mouseEvent)),
           Mouse_x(), Mouse_y());
}

/** Reports scroll deltas received by the window input listener. */
static fn on_scroll(any self, d64 dx, d64 dy) {
    (discard) self;
    printf("scroll (%.2f, %.2f)\n", dx, dy);
}

static i32 g_moves = 0;

/** Reports pointer movement and counts events for the bounded demo. */
static fn on_move(any self, d64 x, d64 y) {
    (discard) self;
    if (++g_moves % 60 == 0) // throttle: one line per ~60 moves
        printf("move (%.0f, %.0f)\n", x, y);
}

static const KeyHandler g_keyListener = {
    .self = nullptr,
    .onKeyDown = on_key_down, .onKeyUp = on_key_up, .onKeyRepeat = nullptr, .onCharTyped = nullptr,
};
static const MouseHandler g_mouseListener = {
    .self = nullptr,
    .onMouseDown = on_mouse_down, .onMouseUp = nullptr, .onMouseRepeat = nullptr,
    .onMouseMove = on_move, .onMouseMoveDelta = nullptr, .onMouseScroll = on_scroll,
    .onMouseDrag = nullptr, .onMouseZoom = nullptr,
};
static const TouchHandler g_touchListener = {
    .self = nullptr,
    .onTouchDown = nullptr, .onTouchUp = nullptr, .onTouchMove = nullptr, .onTouchCancel = nullptr,
};

// --- Per-frame pump: poll, dispatch, and stop at the bounded budget. ---
static fn win_tick(any userdata) {
    win_ctx_t *ctx = userdata;
    Window_pollEvents();
    Window *w = WinCtx_window(ctx);
    Window_dispatchEvents(w);

    if (Window_shouldClose(w)) {
        Loop_stop(WinCtx_loopMut(ctx));
        return;
    }
    i32 frames = WinCtx_frames(ctx) + 1;
    WinCtx_setFrames(ctx, frames);
    i32 budget = WinCtx_frameBudget(ctx);
    if (budget > 0 && frames >= budget)
        Loop_stop(WinCtx_loopMut(ctx));
}

// --- The synchronous chrome/policy CONTRACT. ---
static fn runChromeContract(Window *w) {
    CHECK(w != nullptr);
    if (w == nullptr)
        return;

    // Present policy: mirrors are atomic; a change bumps renderGeneration.
    u64 gen0 = Window_renderGeneration(w);
    CHECK(Window_getPresentMode(w) == WINDOW_PRESENT_FIFO);
    Window_setPresentMode(w, WINDOW_PRESENT_IMMEDIATE);
    CHECK(Window_getPresentMode(w) == WINDOW_PRESENT_IMMEDIATE);
    CHECK(Window_renderGeneration(w) > gen0);

    // Transparency mirror + generation bump.
    u64 gen1 = Window_renderGeneration(w);
    CHECK(!Window_isTransparent(w));
    Window_setTransparent(w, true);
    CHECK(Window_isTransparent(w));
    CHECK(Window_renderGeneration(w) > gen1);

    // Input kill switch.
    CHECK(Window_isEnabled(w));
    Window_setEnabled(w, false);
    CHECK(!Window_isEnabled(w));
    Window_setEnabled(w, true);
    CHECK(Window_isEnabled(w));

    // Chrome capability toggles (style-mask reads: synchronous).
    Window_setResizable(w, false);
    CHECK(!Window_isResizable(w));
    Window_setResizable(w, true);
    CHECK(Window_isResizable(w));

    Window_setClosable(w, false);
    CHECK(!Window_isClosable(w));
    Window_setClosable(w, true);
    CHECK(Window_isClosable(w));

    Window_setMiniaturizable(w, false);
    CHECK(!Window_isMiniaturizable(w));
    Window_setMiniaturizable(w, true);
    CHECK(Window_isMiniaturizable(w));

    // Undecorated chrome modes round-trip.
    Window_setUndecorated(w, WINDOW_UNDECORATED_NAKED);
    CHECK(Window_isNaked(w));
    Window_setUndecorated(w, WINDOW_UNDECORATED_BORDERLESS);
    CHECK(Window_isBorderless(w));
    Window_setUndecorated(w, WINDOW_DECORATED);
    CHECK(Window_isDecorated(w));

    // Viewport flush-to-top chrome control:
    // When decorated, flush-to-top moves the viewport under the top bar while keeping decorated mode.
    CHECK(!Window_isViewportFlushToTop(w));
    Window_setViewportFlushToTop(w, true);
    CHECK(Window_isViewportFlushToTop(w));
    CHECK(Window_isDecorated(w));
    CHECK(!Window_isNaked(w));
    Window_setViewportFlushToTop(w, false);
    CHECK(!Window_isViewportFlushToTop(w));
    CHECK(Window_isDecorated(w));

    // When naked, Window_setViewportFlushToTop is disabled (no-op).
    Window_setUndecorated(w, WINDOW_UNDECORATED_NAKED);
    CHECK(Window_isNaked(w));
    CHECK(!Window_isViewportFlushToTop(w));
    Window_setViewportFlushToTop(w, true);
    CHECK(!Window_isViewportFlushToTop(w));
    CHECK(Window_isNaked(w));

    // When borderless, Window_setViewportFlushToTop is disabled (no-op).
    Window_setUndecorated(w, WINDOW_UNDECORATED_BORDERLESS);
    CHECK(Window_isBorderless(w));
    CHECK(!Window_isViewportFlushToTop(w));
    Window_setViewportFlushToTop(w, true);
    CHECK(!Window_isViewportFlushToTop(w));
    CHECK(Window_isBorderless(w));

    // Restore standard decorated mode.
    Window_setUndecorated(w, WINDOW_DECORATED);
    CHECK(Window_isDecorated(w));
    CHECK(!Window_isViewportFlushToTop(w));

    // shouldClose is a plain atomic mirror.
    Window_setShouldClose(w, true);
    CHECK(Window_shouldClose(w));
    Window_setShouldClose(w, false);
    CHECK(!Window_shouldClose(w));

    // Adapter registration by pointer identity: add (discard) then remove (bool).
    Window_addKeyAdapter(w, &g_keyListener);
    CHECK(Window_removeKeyAdapter(w, &g_keyListener));
    Window_addMouseAdapter(w, &g_mouseListener);
    CHECK(Window_removeMouseAdapter(w, &g_mouseListener));
    Window_addTouchAdapter(w, &g_touchListener);
    CHECK(Window_removeTouchAdapter(w, &g_touchListener));

    // Identity + lifecycle are live, and size is positive.
    CHECK(Window_id(w) != 0u);
    CHECK(Window_getLifecycle(w) != nullptr);
    CHECK(Window_width(w) > 0 && Window_height(w) > 0);

    f32 scale = Window_getScale(w);
    CHECK(scale >= 1.0f);

    // Native Pixel Law: Window_setSize operates in native physical display pixels.
    Window_setSize(w, 800, 600);
    CHECK(Window_width(w) == 800);
    CHECK(Window_height(w) == 600);
    CHECK(Window_widthPoints(w) > 0.0f);
    CHECK(Window_heightPoints(w) > 0.0f);

    Window_revalidate(w);
    CHECK(Window_width(w) == 800);
    CHECK(Window_height(w) == 600);

    Window_setSize(w, 900, 700);
    CHECK(Window_width(w) == 900);
    CHECK(Window_height(w) == 700);

    Window_setMinSize(w, 320, 240);
    Window_setMaxSize(w, 1920, 1080);
    Window_setLocation(w, 120, 120);
    Window_setCursorType(w, WINDOW_CURSOR_POINTING_HAND);
    CHECK(Window_getCursorType(w) == WINDOW_CURSOR_POINTING_HAND);

    // Exercised, not asserted (AppKit-animated / async): they must not crash.
    Window_setFullscreenButton(w, true);
    Window_setDRM(w, true);
    Window_setOpacity(w, 1.0f);
    Window_setAlwaysOnTop(w, false);
    Window_setClickThrough(w, false);
    Window_setShadow(w, true);
    Window_minimize(w);
    Window_restore(w);
}

// --- The visible IRL traffic-light tour (`--tour`). ---
#define STEP_FRAMES 120 // ~2s at 16ms

typedef struct {
    const char *label;
    bool closable;
    bool miniaturizable;
    bool resizable;
    bool fullscreen_button;
    i32 undecorated;
} chrome_step_t;

static const chrome_step_t g_steps[] = {
    { .label = "default: red + yellow + green(fullscreen)", .closable = true, .miniaturizable = true, .resizable = true, .fullscreen_button = true, .undecorated = -1 },
    { .label = "red only",                                 .closable = true, .miniaturizable = false, .resizable = false, .fullscreen_button = true, .undecorated = -1 },
    { .label = "yellow only",                              .closable = false, .miniaturizable = true, .resizable = false, .fullscreen_button = true, .undecorated = -1 },
    { .label = "green zoom only",                          .closable = false, .miniaturizable = false, .resizable = true, .fullscreen_button = false, .undecorated = -1 },
    { .label = "green fullscreen only",                    .closable = false, .miniaturizable = false, .resizable = true, .fullscreen_button = true, .undecorated = -1 },
    { .label = "yellow + green, no red",                   .closable = false, .miniaturizable = true, .resizable = true, .fullscreen_button = true, .undecorated = -1 },
    { .label = "red + yellow, no green",                   .closable = true, .miniaturizable = true, .resizable = false, .fullscreen_button = true, .undecorated = -1 },
    { .label = "red + green, no yellow",                   .closable = true, .miniaturizable = false, .resizable = true, .fullscreen_button = true, .undecorated = -1 },
    { .label = "naked: hidden title, traffic lights kept", .closable = true, .miniaturizable = true, .resizable = true, .fullscreen_button = true, .undecorated = WINDOW_UNDECORATED_NAKED },
    { .label = "borderless: no chrome at all",             .closable = true, .miniaturizable = true, .resizable = true, .fullscreen_button = true, .undecorated = WINDOW_UNDECORATED_BORDERLESS },
    { .label = "back to decorated",                        .closable = true, .miniaturizable = true, .resizable = true, .fullscreen_button = true, .undecorated = WINDOW_DECORATED },
};

#define STEP_COUNT ((i32)(sizeof(g_steps) / sizeof(g_steps[0])))

;;GETTER
static bool ChromeStep_closable(const chrome_step_t *s) { return (*s).closable; }
;;GETTER
static bool ChromeStep_miniaturizable(const chrome_step_t *s) { return (*s).miniaturizable; }
;;GETTER
static bool ChromeStep_resizable(const chrome_step_t *s) { return (*s).resizable; }
;;GETTER
static bool ChromeStep_fullscreenButton(const chrome_step_t *s) { return (*s).fullscreen_button; }
;;GETTER
static i32 ChromeStep_undecorated(const chrome_step_t *s) { return (*s).undecorated; }
;;GETTER
static const char *ChromeStep_label(const chrome_step_t *s) { return (*s).label; }

static const char *greenLabel(const chrome_step_t *step) {
    if (!ChromeStep_resizable(step))
        return "hidden";
    return ChromeStep_fullscreenButton(step) ? "fullscreen" : "zoom";
}

/** Applies one scripted window-chrome state to the live test window. */
static fn applyStep(win_ctx_t *ctx, const chrome_step_t *step) {
    Window *w = WinCtx_window(ctx);
    Window_setClosable(w, ChromeStep_closable(step));
    Window_setMiniaturizable(w, ChromeStep_miniaturizable(step));
    Window_setResizable(w, ChromeStep_resizable(step));
    Window_setFullscreenButton(w, ChromeStep_fullscreenButton(step));
    i32 undec = ChromeStep_undecorated(step);
    if (undec >= 0)
        Window_setUndecorated(w, undec);
    printf("step %2d/%d %s | red=%d yellow=%d green=%s\n",
           (WinCtx_frames(ctx) / STEP_FRAMES) + 1, STEP_COUNT, ChromeStep_label(step),
           Window_isClosable(w), Window_isMiniaturizable(w),
           greenLabel(step));
}

/** Advances the scripted chrome tour once per window-loop tick. */
static fn tour_tick(any userdata) {
    win_ctx_t *ctx = userdata;
    Window_pollEvents();
    Window *w = WinCtx_window(ctx);
    Window_dispatchEvents(w);

    if (Window_shouldClose(w)) {
        Loop_stop(WinCtx_loopMut(ctx));
        return;
    }
    i32 frames = WinCtx_frames(ctx);
    i32 step = frames / STEP_FRAMES;
    if (step < STEP_COUNT && frames % STEP_FRAMES == 0)
        applyStep(ctx, &g_steps[step]);
    WinCtx_setFrames(ctx, frames + 1);
}

/** Runs the bounded sequence of window chrome states used by the demo. */
static fn runTour(Window *w) {
    win_ctx_t ctx = { .window = w, .frames = 0, .frameBudget = 0 };
    g_winCtx = &ctx;
    Window_setTitle(w, "vex engine");
    Window_show(w);
    printf("chrome ready; stepping traffic-light combinations (close the window or press Esc to exit)\n");
    ctx.loop = (Loop){ .tick = tour_tick, .userdata = &ctx, .frame_ms = 16, .running = false };
    Loop_run(&ctx.loop);
    printf("window closed after %d frames\n", ctx.frames);
}

i32 main(i32 argc, char **argv) {
    bool tour = (argc > 1 && strcmp(argv[1], "--tour") == 0);

    Window *w = Window_create("vex", 640, 480);
    Window_addKeyAdapter(w, &g_keyListener);
    Window_addMouseAdapter(w, &g_mouseListener);
    Window_addTouchAdapter(w, &g_touchListener);
    Window_show(w);

    if (tour) {
        runTour(w);
        Window_destroy(w);
        return 0;
    }

    runChromeContract(w);

    // Bounded event pump: prove poll + dispatch + the stop path terminate.
    win_ctx_t ctx = { .window = w, .frames = 0, .frameBudget = 30 };
    g_winCtx = &ctx;
    ctx.loop = (Loop){ .tick = win_tick, .userdata = &ctx, .frame_ms = 16, .running = false };
    Loop_run(&ctx.loop);
    CHECK(ctx.frames == 30);

    Window_destroy(w);

    if (g_failures == 0) {
        printf("window_test: all assertions held (%d frames pumped)\n", ctx.frames);
        return 0;
    }
    printf("window_test: %d FAILURES\n", g_failures);
    return 1;
}

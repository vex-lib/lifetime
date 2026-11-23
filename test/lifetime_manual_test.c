/**
 * Consolidated human-gated test lane for vex/lifetime.
 * Usage:
 * lifetime_manual_test window_tour         (visual traffic-light chrome tour; Esc or close to exit)
 * LIFETIME_MANUAL_PROMPTS=1 lifetime_manual_test permission_prompt
 */

#include "vex.h"
#include "re/primitive/vex-string.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "debug/test_support.h"
#include "vexspoke/engine/loop.h"
#include "vexspoke/input/key.h"
#include "permission/permission.h"
#include "window/window.h"
#include "lifetime_test.h"

enum { TOUR_STEP_FRAMES = 120 };

static const ChromeStep g_tourSteps[] = {
    { .label = "default: red + yellow + green(fullscreen)", .closable = true, .miniaturizable = true, .resizable = true, .fullscreen_button = true, .undecorated = -1 },
    { .label = "red only", .closable = true, .miniaturizable = false, .resizable = false, .fullscreen_button = true, .undecorated = -1 },
    { .label = "yellow only", .closable = false, .miniaturizable = true, .resizable = false, .fullscreen_button = true, .undecorated = -1 },
    { .label = "green zoom only", .closable = false, .miniaturizable = false, .resizable = true, .fullscreen_button = false, .undecorated = -1 },
    { .label = "green fullscreen only", .closable = false, .miniaturizable = false, .resizable = true, .fullscreen_button = true, .undecorated = -1 },
    { .label = "yellow + green, no red", .closable = false, .miniaturizable = true, .resizable = true, .fullscreen_button = true, .undecorated = -1 },
    { .label = "red + yellow, no green", .closable = true, .miniaturizable = true, .resizable = false, .fullscreen_button = true, .undecorated = -1 },
    { .label = "red + green, no yellow", .closable = true, .miniaturizable = false, .resizable = true, .fullscreen_button = true, .undecorated = -1 },
    { .label = "naked: hidden title, traffic lights kept", .closable = true, .miniaturizable = true, .resizable = true, .fullscreen_button = true, .undecorated = WINDOW_UNDECORATED_NAKED },
    { .label = "borderless: no chrome at all", .closable = true, .miniaturizable = true, .resizable = true, .fullscreen_button = true, .undecorated = WINDOW_UNDECORATED_BORDERLESS },
    { .label = "back to decorated", .closable = true, .miniaturizable = true, .resizable = true, .fullscreen_button = true, .undecorated = WINDOW_DECORATED },
};

static TourCtx *g_tourCtx = nullptr;

;;GETTER
static Window *TourCtx_window(const TourCtx *ctx)
{
    return (*ctx).window;
}
;;GETTER
static Loop *TourCtx_loopMut(TourCtx *ctx)
{
    return &(*ctx).loop;
}
;;GETTER
static i32 TourCtx_frames(const TourCtx *ctx)
{
    return (*ctx).frames;
}
;;SETTER
static fn TourCtx_setFrames(TourCtx *ctx, i32 v)
{
    (*ctx).frames = v;
}

;;GETTER
static bool ChromeStep_closable(const ChromeStep *s)
{
    return (*s).closable;
}
;;GETTER
static bool ChromeStep_miniaturizable(const ChromeStep *s)
{
    return (*s).miniaturizable;
}
;;GETTER
static bool ChromeStep_resizable(const ChromeStep *s)
{
    return (*s).resizable;
}
;;GETTER
static bool ChromeStep_fullscreenButton(const ChromeStep *s)
{
    return (*s).fullscreen_button;
}
;;GETTER
static i32 ChromeStep_undecorated(const ChromeStep *s)
{
    return (*s).undecorated;
}
;;GETTER
static string ChromeStep_label(const ChromeStep *s)
{
    return (*s).label;
}

/** Stops the manual window tour when Escape is released. */
static fn tour_key_up(any self, i32 keyEvent, u64 nanos) {
    (discard) self;
    (discard) nanos;
    if (Key_code(keyEvent) == KEY_ESCAPE && g_tourCtx)
        Loop_stop(TourCtx_loopMut(g_tourCtx));
}

static const KeyHandler g_tourKeys = {
    .self = nullptr,
    .onKeyDown = nullptr,
    .onKeyUp = tour_key_up,
    .onKeyRepeat = nullptr,
    .onCharTyped = nullptr,
};

/** Applies one chrome combination step to the live window. */
static fn apply_tour_step(TourCtx *ctx, const ChromeStep *step) {
    Window *w = TourCtx_window(ctx);
    Window_setClosable(w, ChromeStep_closable(step));
    Window_setMiniaturizable(w, ChromeStep_miniaturizable(step));
    Window_setResizable(w, ChromeStep_resizable(step));
    Window_setFullscreenButton(w, ChromeStep_fullscreenButton(step));
    i32 undec = ChromeStep_undecorated(step);
    if (undec >= 0)
        Window_setUndecorated(w, undec);
    i32 total = (i32) (sizeof(g_tourSteps) / sizeof(g_tourSteps[0]));
    print("step ", (TourCtx_frames(ctx) / TOUR_STEP_FRAMES) + 1, "/", total, " ", ChromeStep_label(step));
}

/** Pumps one frame of the manual chrome tour. */
static fn tour_tick(any userdata) {
    TourCtx *ctx = userdata;
    Window_pollEvents();
    Window *w = TourCtx_window(ctx);
    Window_dispatchEvents(w);
    if (Window_shouldClose(w)) {
        Loop_stop(TourCtx_loopMut(ctx));
        return;
    }
    i32 frames = TourCtx_frames(ctx);
    i32 step = frames / TOUR_STEP_FRAMES;
    i32 total = (i32) (sizeof(g_tourSteps) / sizeof(g_tourSteps[0]));
    if (step < total && frames % TOUR_STEP_FRAMES == 0)
        apply_tour_step(ctx, &g_tourSteps[step]);
    TourCtx_setFrames(ctx, frames + 1);
}

/** Runs the interactive visual traffic-light chrome tour. */
static i32 manual_window_tour() {
    Window *w = Window("vex chrome tour", 640, 480);
    if (!w)
        return B_TEST_SKIP;
    Window_addKeyAdapter(w, &g_tourKeys);
    Window_show(w);
    TourCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.window = w;
    g_tourCtx = &ctx;
    ctx.loop = (Loop) { .tick = tour_tick, .userdata = &ctx, .frame_ms = 16, .running = false };
    Loop_run(&ctx.loop);
    Window_destroy(w);
    return 0;
}

/** Exercises interactive macOS TCC permission request and System Settings jump. */
static i32 manual_permission_prompt() {
    string allow = getenv("LIFETIME_MANUAL_PROMPTS");
    if (!allow || !string_equals(allow, "1")) {
        fputs("SKIP: permission prompts require LIFETIME_MANUAL_PROMPTS=1\n", stderr);
        return B_TEST_SKIP;
    }
    (discard) Permission_request(PERMISSION_SCREEN_CAPTURE);
    (discard) Permission_request(PERMISSION_ACCESSIBILITY);
    return 0;
}

/** Dispatches human-gated vex/lifetime checks; skips unless a manual case is selected. */
i32 main(i32 argc, utf8 **argv) {
    if (argc < 2) {
        fputs("SKIP: pass 'window_tour' or 'permission_prompt'\n", stderr);
        return B_TEST_SKIP;
    }
    if (string_equals(argv[1], "window_tour"))
        return manual_window_tour();
    if (string_equals(argv[1], "permission_prompt"))
        return manual_permission_prompt();
    fprint(stderr, "lifetime_manual_test: unknown case '", argv[1], "'");
    return 2;
}

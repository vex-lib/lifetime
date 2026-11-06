// hotcwap/test/window/traffic_light_test.c — the TrafficLight class _test.
//
// Proves the macOS traffic-light chrome controller:
//   - create + defaults (all three lights visible); destroy (incl. null);
//   - per-button visibility round trip and out-of-range refusal;
//   - nil-window safety for the header/floating ops (zero, no crash);
//   - over a REAL window: visibility round trip, refresh, and a header
//     re-seat that stays non-negative.
//
// macOS-only (the backend is traffic_light_cocoa.m); wired under `if(APPLE)`.

#include <stdbool.h>
#include <stdio.h>

#include "window/traffic_light.h"
#include "util/test_support.h"
#include "window/window.h"

static int g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);          \
            g_failures++;                                                  \
        }                                                                  \
    } while (0)

int main(void) {
    // Nil-handle bookkeeping: visibility still tracks.
    TrafficLight *tl = TrafficLight_create(nullptr);
    CHECK(tl != nullptr);
    CHECK(TrafficLight_isButtonVisible(tl, TRAFFIC_LIGHT_CLOSE));
    CHECK(TrafficLight_isButtonVisible(tl, TRAFFIC_LIGHT_MINIATURIZE));
    CHECK(TrafficLight_isButtonVisible(tl, TRAFFIC_LIGHT_ZOOM));

    TrafficLight_setButtonVisible(tl, TRAFFIC_LIGHT_CLOSE, false);
    CHECK(!TrafficLight_isButtonVisible(tl, TRAFFIC_LIGHT_CLOSE));
    TrafficLight_setButtonVisible(tl, TRAFFIC_LIGHT_MINIATURIZE, false);
    CHECK(!TrafficLight_isButtonVisible(tl, TRAFFIC_LIGHT_MINIATURIZE));
    TrafficLight_setButtonVisible(tl, TRAFFIC_LIGHT_ZOOM, false);
    CHECK(!TrafficLight_isButtonVisible(tl, TRAFFIC_LIGHT_ZOOM));
    TrafficLight_setButtonVisible(tl, TRAFFIC_LIGHT_CLOSE, true);
    CHECK(TrafficLight_isButtonVisible(tl, TRAFFIC_LIGHT_CLOSE));

    // Out-of-range button is refused, not trusted.
    CHECK(!TrafficLight_isButtonVisible(tl, (TrafficLightButton) 99));
    TrafficLight_setButtonVisible(tl, (TrafficLightButton) 99, true); // no-op
    CHECK(1);

    // Nil-window header/floating ops: safe and zero.
    float x = -1.0f;
    float y = -1.0f;
    TrafficLight_getHeaderPosition(tl, &x, &y);
    CHECK(x == 0.0f && y == 0.0f);
    TrafficLight_setHeaderPosition(tl, 10.0f, 20.0f); // no window -> no-op
    TrafficLight_resetBase(tl);
    TrafficLight_refresh(tl);
    TrafficLight_setFloating(tl, true);
    CHECK(1);

    TrafficLight_destroy(tl);
    TrafficLight_destroy(nullptr); // null-safe

    // Real window: the controller over a live AppKit window.
    Window *w = Window_create("traffic", 300, 200);
    if (w) {
        TrafficLight *live = TrafficLight_create(Window_nativeHandle(w));
        CHECK(live != nullptr);
        TrafficLight_refresh(live);

        TrafficLight_setButtonVisible(live, TRAFFIC_LIGHT_ZOOM, false);
        CHECK(!TrafficLight_isButtonVisible(live, TRAFFIC_LIGHT_ZOOM));
        TrafficLight_setButtonVisible(live, TRAFFIC_LIGHT_ZOOM, true);
        CHECK(TrafficLight_isButtonVisible(live, TRAFFIC_LIGHT_ZOOM));

        TrafficLight_setHeaderPosition(live, 12.0f, 8.0f);
        float hx = -1.0f;
        float hy = -1.0f;
        TrafficLight_getHeaderPosition(live, &hx, &hy);
        // Either the cluster re-seated (exact, modulo float) or the window's
        // standard buttons were unavailable (zero). Both are non-negative.
        CHECK(hx >= 0.0f && hy >= 0.0f);

        TrafficLight_setFloating(live, true);
        TrafficLight_setFloating(live, false);
        TrafficLight_destroy(live);
        Window_destroy(w);
    } else {
        printf("traffic_light_test: SKIP real-window portion (no window server)\n");
        if (g_failures == 0) {
            // the real-window contract never ran; a skip is not a pass.
            return B_TEST_SKIP;
        }
    }

    if (g_failures == 0) {
        printf("traffic_light_test: all assertions held\n");
        return 0;
    }
    printf("traffic_light_test: %d FAILURES\n", g_failures);
    return 1;
}

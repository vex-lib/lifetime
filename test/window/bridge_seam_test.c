#include "vex.h"
#include <stdio.h>

#include "window/window.h"

;;OVERVIEW
/**
 * ============================================================================
 * MODULE: BridgeSeamTest (bridge_seam_test/test.c)
 * LEVEL: L3 — Module Code (headless verification harness, Apple-only)
 * ============================================================================
 * Pins the bridging doctrine (docs/bridging.md #3): the pixel seam is
 * inert BY DESIGN — a pure AppKit window owns no raster target, no layer,
 * no present worker. Every inert function is null-safe and degrades to its
 * documented default, so consumers compile against one stable header and
 * can never lean on a pixel hotcwap does not own. All calls below pass
 * NULL handles: the stubs must never dereference.
 *
 * STRUCT FIELDS: none — procedural test harness.
 *
 * FUNCTION REGISTRY:
 * ----------------------------------------------------------------------------
 * Core Functions:
 *   - main()
 * ============================================================================
 */

static int g_failures = 0;

#define CHECK(name, cond) do { \
    if (cond) { printf("[bridge_seam_test] PASS %s\n", name); } \
    else { printf("[bridge_seam_test] FAIL %s\n", name); g_failures++; } \
} while (0)

int main() {
    printf("=== Running Bridge Seam Test Suite ===\n");

    // #1 Pane/board compositing is the consumer's pass — inert here.
    CHECK("attach panes false", Window_attachPanes(nullptr, nullptr, 640, 400) == false);
    CHECK("resize panes false", Window_resizePanes(nullptr, nullptr, 800, 600) == false);
    Window_compositePanes(nullptr, nullptr);
    CHECK("composite panes no-op", true);
    Window_compositeBoards(nullptr);
    CHECK("composite boards no-op", true);

    // #2 No layer here: metal + gravity degrade cleanly.
    CHECK("metal layer null", Window_metalLayer(nullptr) == nullptr);
    Window_setGravityTopLeft(nullptr);
    CHECK("gravity no-op", true);

    // #3 No present worker: the transaction seam is a no-op pair.
    Window_workerPresentBegin();
    Window_workerPresentEnd();
    CHECK("worker present no-op", true);

    // #4 No raster target: software present stays inert.
    CHECK("present false", Window_present(nullptr, nullptr) == false);

    if (g_failures == 0)
        printf("PASS bridge_seam_test: all assertions held\n");
    else
        printf("FAIL bridge_seam_test: %d failures\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}

// hotcwap/test/window/present_surface_test.c — mirrors window_cocoa.m
//
// The zero-copy present seam: the window hands out IOSurface handles, a CALayer
// displays them, and present publishes a surface by swapping the layer's
// contents. Apple-only; elsewhere the surface is NULL and calls are no-ops.

#include "util/platform.h"
#include <stdio.h>

#include "window/window.h"

static int g_fail = 0;
#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);          \
            g_fail++;                                                      \
        }                                                                  \
    } while (0)

int main(void) {
    Window *w = Window_create("present surface", 320, 240);
    CHECK(w != nullptr);

#if defined(VEX_PLATFORM_APPLE)
    // two IOSurfaces = the double buffer; the layer shows whichever we publish
    void *front = Window_createPresentSurface(w, 320, 240);
    void *back = Window_createPresentSurface(w, 320, 240);
    CHECK(front != nullptr && back != nullptr);

    if (front && back) {
        Window_presentSurface(w, front);
        CHECK(Window_presentSurfaceContents(w) == front);
        Window_presentSurface(w, back);          // swap
        CHECK(Window_presentSurfaceContents(w) == back);
        Window_presentSurface(w, front);         // and back
        CHECK(Window_presentSurfaceContents(w) == front);
    }

    Window_destroyPresentSurface(w, front);
    Window_destroyPresentSurface(w, back);
#else
    CHECK(Window_createPresentSurface(w, 320, 240) == nullptr);   // capability-gated
#endif

    // null-safety across the whole seam
    Window_presentSurface(nullptr, nullptr);
    Window_presentSurface(w, nullptr);
    Window_destroyPresentSurface(nullptr, nullptr);
    CHECK(Window_presentSurfaceContents(nullptr) == nullptr);

    Window_destroy(w);
    printf("present_surface_test: ALL PASS\n");
    return g_fail == 0 ? 0 : 1;
}

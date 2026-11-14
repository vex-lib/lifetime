// window/traffic_light_cocoa.m — macOS traffic-light button management.
//
// Segregated component managing macOS traffic-light buttons (close, minimize,
// zoom) layout, visibility, and header positioning. Decoupled from the core
// Window state per the Single Class Per File Law and the Window Decoupling Law.

#import <AppKit/AppKit.h>

#include "vex.h"
#include <stdlib.h>

#include "window/traffic_light.h"

;;DEFINITION
/**
 * ============================================================================
 * DEFINITION: TrafficLight
 * ============================================================================
 * Manages native macOS window chrome traffic-light buttons (close, minimize,
 * and zoom/fullscreen). Handles button visibility toggling, native frame
 * snapshotting, style mask full-size content view f32ing, and header offset
 * positioning. Strictly decoupled from the core Window state per the Single
 * Class Per File Law.
 * ============================================================================
 */

;;OVERVIEW
/**
 * ============================================================================
 * CLASS: TrafficLight (window/traffic_light_cocoa.m)
 * ============================================================================
 * SUMMARY:
 *   Encapsulates macOS traffic-light button chrome layout and positioning.
 *   Provides visibility controls and custom header offset positioning over
 *   full-size content views (NAKED chrome).
 *
 * STRUCT FIELDS:
 * ----------------------------------------------------------------------------
 *   NSWindow *window;                                     // Weak AppKit window handle
 *   bool lightVisible[TRAFFIC_LIGHT_COUNT];               // Per-button visibility
 *   f32 lightOX;                                        // Cluster offset X in points
 *   f32 lightOY;                                        // Cluster offset Y in points
 *   bool lightBaseSet;                                    // Base layout snapshotted flag
 *   NSRect lightBase[TRAFFIC_LIGHT_COUNT];                // Native frame snapshot
 *
 * FUNCTION REGISTRY:
 * ----------------------------------------------------------------------------
 * Constructors:
 *   - TrafficLight_create(nsWindowHandle)
 *   - TrafficLight_destroy(self)
 *
 * Visibility:
 *   - TrafficLight_setButtonVisible(self, button, visible)
 *   - TrafficLight_isButtonVisible(self, button)
 *
 * Header Positioning:
 *   - TrafficLight_setHeaderPosition(self, x, y)
 *   - TrafficLight_getHeaderPosition(self, outX, outY)
 *
 * State & Layout:
 *   - TrafficLight_resetBase(self)
 *   - TrafficLight_refresh(self)
 *   - TrafficLight_setFloating(self, f32ing)
 * ============================================================================
 */

struct TrafficLight {
    __unsafe_unretained NSWindow *window;
    bool lightVisible[TRAFFIC_LIGHT_COUNT];
    f32 lightOX;
    f32 lightOY;
    bool lightBaseSet;
    NSRect lightBase[TRAFFIC_LIGHT_COUNT];
};

/** Creates a traffic-light adapter for the supplied native window handle. */
TrafficLight *TrafficLight_create(any nsWindowHandle) {
    TrafficLight *self = (TrafficLight*) calloc(1, sizeof(TrafficLight));
    if (self == nullptr)
        return nullptr;

    (*self).window = (__bridge NSWindow*) nsWindowHandle;
    (*self).lightVisible[TRAFFIC_LIGHT_CLOSE] = true;
    (*self).lightVisible[TRAFFIC_LIGHT_MINIATURIZE] = true;
    (*self).lightVisible[TRAFFIC_LIGHT_ZOOM] = true;
    (*self).lightOX = 0.0f;
    (*self).lightOY = 0.0f;
    (*self).lightBaseSet = false;
    return self;
}

/** Releases adapter-owned bookkeeping without destroying the native window. */
fn TrafficLight_destroy(TrafficLight *self) {
    if (self != nullptr)
        free(self);
}

/** Recomputes the traffic-light buttons' base positions from the native window. */
fn TrafficLight_resetBase(TrafficLight *self) {
    if (self != nullptr)
        (*self).lightBaseSet = false;
}

/** Applies the adapter's current visibility, position, and f32ing settings. */
fn TrafficLight_refresh(TrafficLight *self) {
    if (self == nullptr || (*self).window == nil)
        return;

    @autoreleasepool {
        NSWindow *nsw = (*self).window;
        NSButton *buttons[TRAFFIC_LIGHT_COUNT];
        buttons[TRAFFIC_LIGHT_CLOSE] = [nsw standardWindowButton:NSWindowCloseButton];
        buttons[TRAFFIC_LIGHT_MINIATURIZE] = [nsw standardWindowButton:NSWindowMiniaturizeButton];
        buttons[TRAFFIC_LIGHT_ZOOM] = [nsw standardWindowButton:NSWindowZoomButton];

        if (!(*self).lightBaseSet) {
            for (int i = 0; i < TRAFFIC_LIGHT_COUNT; i++)
                (*self).lightBase[i] = buttons[i] ? [buttons[i] frame] : NSZeroRect;
            (*self).lightBaseSet = true;
        }

        for (int i = 0; i < TRAFFIC_LIGHT_COUNT; i++) {
            if (buttons[i] == nil)
                continue;
            [buttons[i] setHidden:(*self).lightVisible[i] ? NO : YES];
            if ((*self).lightVisible[i] && !NSIsEmptyRect((*self).lightBase[i])) {
                NSPoint origin = (*self).lightBase[i].origin;
                origin.x += (CGFloat) (*self).lightOX;
                origin.y += (CGFloat) (*self).lightOY;
                [buttons[i] setFrameOrigin:origin];
            }
        }
    }
}

/** Shows or hides the selected native traffic-light button. */
fn TrafficLight_setButtonVisible(TrafficLight *self, TrafficLightButton button, bool visible) {
    if (self == nullptr || (int) button < 0 || button >= TRAFFIC_LIGHT_COUNT)
        return;
    (*self).lightVisible[button] = visible;
    TrafficLight_refresh(self);
}

/** Reports whether the selected button is configured as visible. */
bool TrafficLight_isButtonVisible(const TrafficLight *self, TrafficLightButton button) {
    if (self == nullptr || (int) button < 0 || button >= TRAFFIC_LIGHT_COUNT)
        return false;
    return (*self).lightVisible[button];
}

/** Sets the header-relative position used by the adapter. */
fn TrafficLight_setHeaderPosition(TrafficLight *self, f32 x, f32 y) {
    if (self == nullptr || (*self).window == nil)
        return;

    @autoreleasepool {
        NSWindow *nsw = (*self).window;
        NSButton *close = [nsw standardWindowButton:NSWindowCloseButton];
        if (close == nil)
            return;
        if (!(*self).lightBaseSet)
            TrafficLight_refresh(self);
        NSRect base = (*self).lightBase[TRAFFIC_LIGHT_CLOSE];
        if (NSIsEmptyRect(base))
            return;
        CGFloat h = [[nsw contentView] bounds].size.height;
        CGFloat wantX = (CGFloat) x;
        CGFloat wantY = h - (CGFloat) y - base.size.height;
        (*self).lightOX = (f32) (wantX - base.origin.x);
        (*self).lightOY = (f32) (wantY - base.origin.y);
        TrafficLight_refresh(self);
    }
}

/** Returns the configured header-relative position through optional outputs. */
fn TrafficLight_getHeaderPosition(const TrafficLight *self, f32 *outX, f32 *outY) {
    f32 x = 0.0f;
    f32 y = 0.0f;
    if (self != nullptr && (*self).window != nil) {
        @autoreleasepool {
            NSWindow *nsw = (*self).window;
            NSButton *close = [nsw standardWindowButton:NSWindowCloseButton];
            NSRect base = (*self).lightBase[TRAFFIC_LIGHT_CLOSE];
            if (close != nil && (*self).lightBaseSet && !NSIsEmptyRect(base)) {
                CGFloat h = [[nsw contentView] bounds].size.height;
                CGFloat curX = base.origin.x + (CGFloat) (*self).lightOX;
                CGFloat curY = base.origin.y + (CGFloat) (*self).lightOY;
                x = (f32) curX;
                y = (f32) (h - curY - base.size.height);
            }
        }
    }
    if (outX)
        *outX = x;
    if (outY)
        *outY = y;
}

/** Enables or disables f32ing behavior for the native traffic-light buttons. */
fn TrafficLight_setFloating(TrafficLight *self, bool f32ing) {
    if (self == nullptr || (*self).window == nil)
        return;

    @autoreleasepool {
        NSWindow *nsw = (*self).window;
        NSWindowStyleMask mask = [nsw styleMask];
        if (f32ing) {
            [nsw setStyleMask:(mask | NSWindowStyleMaskFullSizeContentView)];
            [nsw setTitlebarAppearsTransparent:YES];
            [nsw setTitleVisibility:NSWindowTitleHidden];
        } else {
            [nsw setStyleMask:(mask & ~NSWindowStyleMaskFullSizeContentView)];
            [nsw setTitlebarAppearsTransparent:NO];
            [nsw setTitleVisibility:NSWindowTitleVisible];
        }
        (*self).lightBaseSet = false;
        TrafficLight_refresh(self);
    }
}

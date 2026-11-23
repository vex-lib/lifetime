/**
 * window/traffic_light_cocoa.m, macOS traffic-light button management.
 *
 * Segregated component managing macOS traffic-light buttons (close, minimize,
 * zoom) layout, visibility, and header positioning. Decoupled from the core
 * Window state per the Single Class Per File Law and the Window Decoupling Law.
 */

#import <AppKit/AppKit.h>

#include "vex.h"
#include "nio/lifetime_memory.h"
#include "re/type/re_type.h"

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

;;GETTER
static NSWindow *TrafficLight_window(const TrafficLight *self)
{
    return (*self).window;
}
;;SETTER
static fn TrafficLight_setWindow(TrafficLight *self, NSWindow *w)
{
    (*self).window = w;
}
;;GETTER
static bool TrafficLight_lightVisibleAt(const TrafficLight *self, i32 i)
{
    return (*self).lightVisible[i];
}
;;SETTER
static fn TrafficLight_setLightVisibleAt(TrafficLight *self, i32 i, bool v)
{
    (*self).lightVisible[i] = v;
}
;;GETTER
static NSRect TrafficLight_lightBaseAt(const TrafficLight *self, i32 i)
{
    return (*self).lightBase[i];
}
;;SETTER
static fn TrafficLight_setLightBaseAt(TrafficLight *self, i32 i, NSRect r)
{
    (*self).lightBase[i] = r;
}
;;GETTER
static f32 TrafficLight_lightOX(const TrafficLight *self)
{
    return (*self).lightOX;
}
;;SETTER
static fn TrafficLight_setLightOX(TrafficLight *self, f32 v)
{
    (*self).lightOX = v;
}
;;GETTER
static f32 TrafficLight_lightOY(const TrafficLight *self)
{
    return (*self).lightOY;
}
;;SETTER
static fn TrafficLight_setLightOY(TrafficLight *self, f32 v)
{
    (*self).lightOY = v;
}
;;GETTER
static bool TrafficLight_lightBaseSet(const TrafficLight *self)
{
    return (*self).lightBaseSet;
}
;;SETTER
static fn TrafficLight_setLightBaseSet(TrafficLight *self, bool v)
{
    (*self).lightBaseSet = v;
}

/** Creates a traffic-light adapter for the supplied native window handle. */
;;CONSTRUCTOR
TrafficLight *TrafficLight_1(any nsWindowHandle) {
    TrafficLight *self = (TrafficLight*) MemoryArena_alloc(Lifetime_arena(), TYPE_BYTE_ARRAY, sizeof(TrafficLight));
    if (self == nullptr)
        return nullptr;
    memset(self, 0, sizeof(TrafficLight));

    TrafficLight_setWindow(self, (__bridge NSWindow*) nsWindowHandle);
    TrafficLight_setLightVisibleAt(self, TRAFFIC_LIGHT_CLOSE, true);
    TrafficLight_setLightVisibleAt(self, TRAFFIC_LIGHT_MINIATURIZE, true);
    TrafficLight_setLightVisibleAt(self, TRAFFIC_LIGHT_ZOOM, true);
    TrafficLight_setLightOX(self, 0.0f);
    TrafficLight_setLightOY(self, 0.0f);
    TrafficLight_setLightBaseSet(self, false);
    return self;
}

/** Releases adapter-owned bookkeeping without destroying the native window. */
fn TrafficLight_destroy(TrafficLight *self) {
    if (self != nullptr)
        MemoryArena_free(Lifetime_arena(), self);
}

/** Recomputes the traffic-light buttons' base positions from the native window. */
fn TrafficLight_resetBase(TrafficLight *self) {
    if (self != nullptr)
        TrafficLight_setLightBaseSet(self, false);
}

/** Applies the adapter's current visibility, position, and f32ing settings. */
fn TrafficLight_refresh(TrafficLight *self) {
    if (self == nullptr || TrafficLight_window(self) == nil)
        return;

    @autoreleasepool {
        NSWindow *nsw = TrafficLight_window(self);
        NSButton *buttons[TRAFFIC_LIGHT_COUNT];
        buttons[TRAFFIC_LIGHT_CLOSE] = [nsw standardWindowButton:NSWindowCloseButton];
        buttons[TRAFFIC_LIGHT_MINIATURIZE] = [nsw standardWindowButton:NSWindowMiniaturizeButton];
        buttons[TRAFFIC_LIGHT_ZOOM] = [nsw standardWindowButton:NSWindowZoomButton];

        if (!TrafficLight_lightBaseSet(self)) {
            for (i32 i = 0; i < TRAFFIC_LIGHT_COUNT; i++)
                TrafficLight_setLightBaseAt(self, i, buttons[i] ? [buttons[i] frame] : NSZeroRect);
            TrafficLight_setLightBaseSet(self, true);
        }

        for (i32 i = 0; i < TRAFFIC_LIGHT_COUNT; i++) {
            if (buttons[i] == nil)
                continue;
            bool vis = TrafficLight_lightVisibleAt(self, i);
            [buttons[i] setHidden:vis ? NO : YES];
            NSRect base = TrafficLight_lightBaseAt(self, i);
            if (vis && !NSIsEmptyRect(base)) {
                NSPoint origin = base.origin;
                origin.x += (CGFloat) TrafficLight_lightOX(self);
                origin.y += (CGFloat) TrafficLight_lightOY(self);
                [buttons[i] setFrameOrigin:origin];
            }
        }
    }
}

/** Shows or hides the selected native traffic-light button. */
fn TrafficLight_setButtonVisible(TrafficLight *self, TrafficLightButton button, bool visible) {
    if (self == nullptr || (i32) button < 0 || button >= TRAFFIC_LIGHT_COUNT)
        return;
    TrafficLight_setLightVisibleAt(self, button, visible);
    TrafficLight_refresh(self);
}

/** Reports whether the selected button is configured as visible. */
bool TrafficLight_isButtonVisible(const TrafficLight *self, TrafficLightButton button) {
    if (self == nullptr || (i32) button < 0 || button >= TRAFFIC_LIGHT_COUNT)
        return false;
    return TrafficLight_lightVisibleAt(self, button);
}

/** Sets the header-relative position used by the adapter. */
fn TrafficLight_setHeaderPosition(TrafficLight *self, f32 x, f32 y) {
    if (self == nullptr || TrafficLight_window(self) == nil)
        return;

    @autoreleasepool {
        NSWindow *nsw = TrafficLight_window(self);
        NSButton *close = [nsw standardWindowButton:NSWindowCloseButton];
        if (close == nil)
            return;
        if (!TrafficLight_lightBaseSet(self))
            TrafficLight_refresh(self);
        NSRect base = TrafficLight_lightBaseAt(self, TRAFFIC_LIGHT_CLOSE);
        if (NSIsEmptyRect(base))
            return;
        CGFloat h = [[nsw contentView] bounds].size.height;
        CGFloat wantX = (CGFloat) x;
        CGFloat wantY = h - (CGFloat) y - base.size.height;
        TrafficLight_setLightOX(self, (f32) (wantX - base.origin.x));
        TrafficLight_setLightOY(self, (f32) (wantY - base.origin.y));
        TrafficLight_refresh(self);
    }
}

/** Returns the configured header-relative position through optional outputs. */
fn TrafficLight_getHeaderPosition(const TrafficLight *self, f32 *outX, f32 *outY) {
    f32 x = 0.0f;
    f32 y = 0.0f;
    if (self != nullptr && TrafficLight_window(self) != nil) {
        @autoreleasepool {
            NSWindow *nsw = TrafficLight_window(self);
            NSButton *close = [nsw standardWindowButton:NSWindowCloseButton];
            NSRect base = TrafficLight_lightBaseAt(self, TRAFFIC_LIGHT_CLOSE);
            if (close != nil && TrafficLight_lightBaseSet(self) && !NSIsEmptyRect(base)) {
                CGFloat h = [[nsw contentView] bounds].size.height;
                CGFloat curX = base.origin.x + (CGFloat) TrafficLight_lightOX(self);
                CGFloat curY = base.origin.y + (CGFloat) TrafficLight_lightOY(self);
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
    if (self == nullptr || TrafficLight_window(self) == nil)
        return;

    @autoreleasepool {
        NSWindow *nsw = TrafficLight_window(self);
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
        TrafficLight_setLightBaseSet(self, false);
        TrafficLight_refresh(self);
    }
}

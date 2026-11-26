/**
 * window/window_cocoa.m, the AppKit window backend (the one ObjC file).
 *
 * vex's core is pure C23; this is the ONE file that talks to AppKit, because
 * NSWindow/NSApplication are ObjC objects and there is no pure-C way to
 * create them. Everything above this boundary stays C; everything here is
 * "dip into the OS, hand back a handle, pump the OS event queue".
 *
 * This is the FRESH window backend, rebuilt from scratch after the _trash-era
 * shim retired. It is deliberately LEAN: a window with bridges, and nothing
 * else. It creates and owns an NSWindow, mirrors the OS's size/move/focus/
 * monitor state into C-visible words, fires the per-window WindowEvent
 * lifecycle (quit veto, resized, fullscreen, minimized, restored, pressed,
 * focus, zoom), and routes OS input into the vexspoke Key/Mouse/Touch rings.
 * There is NO Vulkan, NO Metal, NO CAMetalLayer, NO board/pane compositing,
 * and NO present worker in this file, rendering is the render personal' job
 * (graphvex/darling) and reaches the screen through the content view and the
 * event bridges, exactly per the Window Decoupling Law (a Window is a dumb
 * surface + callback bridge, never a renderer).
 *
 * The GPU-era composite surface (boards, panes, worker present, software
 * frame present) is retained as thin INERT stubs at the bottom of this file
 * so the still-unmigrated darling compositor links; each carries an
 * ;;INTENTION marker. When darling migrates to the WindowEvent bridge, the
 * stubs and their window.h declarations retire together.
 */

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>
#import <ImageIO/ImageIO.h>
#import <CoreVideo/CoreVideo.h>
#import <IOSurface/IOSurface.h>
#import <stdatomic.h>

#include "vex.h"
#include "nio/lifetime_memory.h"
#include "re/type/re_type.h"
#include <math.h>

#define LIFETIME_WINDOW_IMPLEMENTATION
#include "window/window.h"
#include "window/traffic_light.h"
#include "input/focus.h"
#include "input/key.h"
#include "input/mouse.h"
#include "input/touch.h"

;;DEFINITION
/**
 * ============================================================================
 * DEFINITION: Window
 * ============================================================================
 * Platform abstraction encapsulating an OS-managed desktop display surface.
 * On macOS, implements the native AppKit backend via an NSWindow handle, owning
 * the window chrome, display scaling, frame geometry, and event dispatch seam.
 * Strictly decoupled from GPU rendering pipelines according to the Window
 * Decoupling Law, serving solely as a dumb presentation surface and callback bridge.
 *
 * Dispatches input events directly into vexspoke device rings (Key, Mouse, Touch)
 * and forwards window lifecycle state changes (geometry resize + move, minimize,
 * restore, fullscreen, key focus, and vetoable termination requests) through the
 * embedded WindowEvent registry on Thread 0. Geometry is reflected EVERY tracking
 * step, onResized(w,h) and onMoved(x,y) both fire the moment the window changes,
 * not on settle.
 * ============================================================================
 */

;;OVERVIEW
/**
 * ============================================================================
 * CLASS: Window (window/window_cocoa.m)
 * ============================================================================
 * SUMMARY:
 *   The fresh, lean AppKit window backend. One opaque C handle per NSWindow;
 *   the engine loop constructs it, configures the chrome, shows it, then pumps
 *   Window_pollEvents once per frame while a render path draws through the
 *   content view / event bridges. OS input is routed into the vexspoke device
 *   rings (tagged with this window's id); OS lifecycle (quit, resize, move,
 *   fullscreen, minimize, restore, press, focus, zoom) fires the embedded
 *   WindowEvent. Zero Vulkan, zero Metal, zero compositing, a Window is a
 *   dumb surface + callback bridge per the Window Decoupling Law.
 *
 * STRUCT FIELDS (Mirroring window/window.h incomplete tag, completed here):
 * ----------------------------------------------------------------------------
 *   NSWindow *nsWindow;           // AppKit window (we own it; releasedWhenClosed NO)
 *   WindowDelegate *delegate;     // per-window close/resize/focus delegate
 *   WindowEvent lifecycle;        // OS lifecycle registry (window/window_event.h)
 *   u32 id;                  // engine window id (1..N, 0 = FOCUS_BROADCAST)
 *   _Atomic bool shouldClose;     // true once close requested (Thread 0 writes, loop reads)
 *   _Atomic u64 sizeGeneration; // resize-reflection counter (thread 0 bumps)
 *   _Atomic i32 cachedWidth;      // content width in physical pixels at last thread-0 event (any thread reads)
 *   _Atomic i32 cachedHeight;     // content height in physical pixels at last thread-0 event
 *   d64 cachedScale;           // active backing scale factor
 *   d64 cachedX;               // top-left screen X at last thread-0 event
 *   d64 cachedY;               // top-left screen Y at last thread-0 event
 *   d64 cachedContentX;        // CONTENT top-left X (below title bar)
 *   d64 cachedContentY;        // CONTENT top-left Y (below title bar)
 *   _Atomic bool liveResizing;    // thread 0 during NSViewLiveResize; renderer consumes
 *   _Atomic bool miniaturizing;   // thread 0 during genie minimize; suppress merge
 *   _Atomic i32 presentMode;      // present pacing (FIFO/IMMEDIATE), pure state
 *   _Atomic bool transparent;     // composite transparency request, pure state
 *   _Atomic u64 renderGeneration; // policy-reflection counter (rebuild ticket)
 *   _Atomic bool enabled;         // false mutes ALL OS input for this window
 *   bool lastFocused;             // focus-flip detection during the pump
 *   _Atomic u32 monitorId;   // CGDirectDisplayID mirror (0 = unmapped)
 *   WindowCursorType cursorType;  // active OS cursor style
 *   TrafficLight *trafficLights;  // macOS traffic-light chrome controller (window/traffic_light.h)
 *   WindowResizeRenderFn resizeRenderFn;  // resize-cadence render hook
 *   any resizeRenderUserdata;   // hook userdata
 *
 * PRIVATE HELPERS (kept file-local, no external API):
 * ----------------------------------------------------------------------------
 *   WindowContentView : NSView     , flipped content view (top-left origin)
 *     BOOL active;                  // unused; class exists for flipped geometry
 *   WindowAppDelegate : NSObject
 *     (implements applicationShouldTerminateAfterLastWindowClosed)
 *   WindowDelegate : NSObject <NSWindowDelegate>
 *     atomic_bool *shouldClosePtr;  // weak assignment into the owning handle
 *     Window *handlePtr;            // weak assignment back to the owning handle
 *   WindowSlot                     , id-registry slot record
 *     NSWindow *window;             // OS window owning this id
 *     Window *handle;               // C handle owning that window
 *
 * FUNCTION REGISTRY:
 * ----------------------------------------------------------------------------
 * Public Constructors: (.h)
 *   - Window_0()
 *   - Window_1(title)
 *   - Window_3(title, width, height)
 *   - Window_new(desc)
 *   - Window_create(title, width, height)
 *
 * Private Constructors: (.c static)
 *   - windowAlloc(desc)               : shared constructor core
 *   - descResolve(desc)               : resolve configuration defaults
 *
 * Public Core Functions: (.h)
 *   - Window_destroy(window)          : detach delegate, close, free handle
 *   - Window_destroyAll()
 *   - Window_shouldClose(window)
 *   - Window_pollEvents()         : drain OS queue once per frame
 *   - Window_width(window)
 *   - Window_height(window)
 *   - Window_center(window)
 *   - Window_show(window)
 *   - Window_hide(window)
 *   - Window_attachPanes(window, panel, width, height) : inert (;;INTENTION)
 *   - Window_resizePanes(window, panel, width, height) : inert (;;INTENTION)
 *   - Window_compositePanes(window, contentPanel)      : inert (;;INTENTION)
 *   - Window_compositeBoards(window)  : inert (;;INTENTION)
 *   - Window_renderGeneration(window)
 *   - Window_bringToFront(window)
 *   - Window_minimize(window)
 *   - Window_restore(window)
 *   - Window_toggleFullscreen(window)
 *   - Window_contentView(window)
 *   - Window_metalLayer(window)       : nullptr, no Metal here (;;INTENTION)
 *   - Window_present(window, frame)   : inert (;;INTENTION)
 *   - Window_workerPresentBegin(window) : inert (;;INTENTION)
 *   - Window_workerPresentEnd(window)   : inert (;;INTENTION)
 *   - Window_orderLayers(window)      : inert (;;INTENTION)
 *   - Window_addKeyAdapter(window, adapter)
 *   - Window_removeKeyAdapter(window, adapter)
 *   - Window_addMouseAdapter(window, adapter)
 *   - Window_removeMouseAdapter(window, adapter)
 *   - Window_addTouchAdapter(window, adapter)
 *   - Window_removeTouchAdapter(window, adapter)
 *   - Window_addWindowAdapter(window, adapter)
 *   - Window_removeWindowAdapter(window, adapter)
 *   - Window_dispatchEvents(window)
 *   - Window_id(window)
 *   - Window_focus(window)
 *   - Window_sizeGeneration(window)
 *   - Window_getScale(window)
 *   - Window_revalidate(window)
 *   - Window_getSizePoints(window, outWidth, outHeight)
 *   - Window_widthPoints(window)
 *   - Window_heightPoints(window)
 *
 * Private Core Functions: (.c static)
 *   - routeEvent(event)               : OS event dispatches to device rings + WindowEvent
 *   - windowRefreshSize(window)       : compute geometry and trigger resize callbacks
 *   - windowRefreshFocus(window, key) : track key window focus transitions
 *   - windowRefreshMonitor(window)    : update active monitor identity
 *   - windowBackingScale(window)      : query active backing scale factor
 *   - windowIdAcquire(win, handle)    : register handle in slot registry
 *   - windowIdRelease(id)             : release slot registry handle
 *   - windowIdOf(win)                 : resolve window ID from NSWindow pointer
 *   - windowHandleOf(win)             : resolve Window handle from NSWindow pointer
 *   - recenterIfLocked()          : cursor lock recenter helper
 *   - claimKeyAfterActivation(win)    : take key window status on app activation
 *
 * Public Setters: (.h)
 *   - Window_setShouldClose(window, shouldClose)
 *   - Window_setTitle(window, title)
 *   - Window_setSize(window, width, height)
 *   - Window_setSizePoints(window, width, height)
 *   - Window_setLocation(window, x, y)
 *   - Window_setVisible(window, visible)
 *   - Window_setPresentMode(window, mode)
 *   - Window_setTransparent(window, transparent)
 *   - Window_setEnabled(window, enabled)
 *   - Window_setKeyEnabled(window, enabled)
 *   - Window_setResizable(window, resizable)
 *   - Window_setClosable(window, closable)
 *   - Window_setMiniaturizable(window, miniaturizable)
 *   - Window_setFullscreenButton(window, enabled)
 *   - Window_setUndecorated(window, type)
 *   - Window_setDecorated(window, decorated)
 *   - Window_setNaked(window, naked)
 *   - Window_setBorderless(window, borderless)
 *   - Window_setViewportFlushToTop(window, flush)
 *   - Window_setFloatingTrafficLights(window, f32ing)
 *   - Window_macOS_setTrafficLightButtonVisible(window, light, visible)
 *   - Window_macOS_setTrafficLightHeaderPosition(window, x, y)
 *   - Window_macOS_hasLiquidGlass() / Window_macOS_setLiquidGlass(window, desc)
 *   - Window_setOpacity(window, opacity)
 *   - Window_setTransparentBackground(window, transparent)
 *   - Window_setAlwaysOnTop(window, onTop)
 *   - Window_setClickThrough(window, clickThrough)
 *   - Window_setShadow(window, shadow)
 *   - Window_setMovableByBackground(window, movable)
 *   - Window_setFullscreen(window, fullscreen)
 *   - Window_setDRM(window, enabled)
 *   - Window_setMinSize(window, width, height)
 *   - Window_setMaxSize(window, width, height)
 *   - Window_setCursorLocked(window, locked)
 *   - Window_setCursorType(window, type)
 *   - Window_setGravityTopLeft(window)  : no-op (;;INTENTION)
 *   - Window_setResizeRenderHook(window, fn, userdata)
 *   - Window_setTopLayer(window, layer)
 *   - Window_setBottomLayer(window, layer)
 *
 * Private Setters: (.c static)
 *   - (none)
 *
 * Public Getters: (.h)
 *   - Window_getLocation(window, outX, outY)
 *   - Window_getContentOrigin(window, outX, outY)
 *   - Window_getPresentMode(window)
 *   - Window_isTransparent(window)
 *   - Window_isEnabled(window)
 *   - Window_isKeyEnabled(window)
 *   - Window_isLiveResizing(window)
 *   - Window_isResizable(window)
 *   - Window_isClosable(window)
 *   - Window_isMiniaturizable(window)
 *   - Window_isDecorated(window)
 *   - Window_isNaked(window)
 *   - Window_isBorderless(window)
 *   - Window_isViewportFlushToTop(window)
 *   - Window_macOS_isTrafficLightButtonVisible(window, light)
 *   - Window_macOS_getTrafficLightHeaderPosition(window, outX, outY)
 *   - Window_isMinimized(window)
 *   - Window_isFullscreen(window)
 *   - Window_isFocused(window)
 *   - Window_getMonitorId(window)
 *   - Window_getCursorType(window)
 *   - Window_getLifecycle(window)
 *   - Window_getTopLayer(window)
 *   - Window_getBottomLayer(window)
 *   - Window_getResizeRenderHook(window)
 *
 * Private Getters: (.c static)
 *   - (none)
 * ============================================================================
 */
;;INTENTION("GPU-era composite surface (attachPanes/resizePanes/compositePanes/compositeBoards/orderLayers/metalLayer/setGravityTopLeft/workerPresentBegin/workerPresentEnd/present) is retained as inert stubs so the still-unmigrated darling compositor keeps linking; zero Vulkan/Metal code lives in this file. They retire together with their window.h declarations once darling migrates onto the WindowEvent bridge (the Window Decoupling Law).")
;;INTENTION("presentMode/transparent/renderGeneration are pure atomic policy state (swapchain-rebuild tickets for a future render path), not GPU calls; no render logic exists here.")

/** Multi-tap window for d64-click style counting (legacy parity: 250ms). */
static const u64 kTapThresholdNanos = 250000000ULL;

/**
 * Carbon virtual keycode  to  KEY_* code. -1 = unmapped. Same table the legacy
 * macOSWindow built (physical F1-F12, not Fn-f64d media keys).
 */
static i32 macKeyMap[128] = {
    [0] = KEY_A,                   [1] = KEY_S,
    [2] = KEY_D,                   [3] = KEY_F,
    [4] = KEY_H,                   [5] = KEY_G,
    [6] = KEY_Z,                   [7] = KEY_X,
    [8] = KEY_C,                   [9] = KEY_V,
    [11] = KEY_B,                  [12] = KEY_Q,
    [13] = KEY_W,                  [14] = KEY_E,
    [15] = KEY_R,                  [16] = KEY_Y,
    [17] = KEY_T,                  [18] = KEY_NUM_1,
    [19] = KEY_NUM_2,              [20] = KEY_NUM_3,
    [21] = KEY_NUM_4,              [22] = KEY_NUM_6,
    [23] = KEY_NUM_5,              [24] = KEY_EQUAL,
    [25] = KEY_NUM_9,              [26] = KEY_NUM_7,
    [27] = KEY_MINUS,              [28] = KEY_NUM_8,
    [29] = KEY_NUM_0,              [30] = KEY_RIGHT_BRACKET,
    [31] = KEY_O,                  [32] = KEY_U,
    [33] = KEY_LEFT_BRACKET,       [34] = KEY_I,
    [35] = KEY_P,                  [36] = KEY_ENTER,
    [37] = KEY_L,                  [38] = KEY_J,
    [39] = KEY_APOSTROPHE,         [40] = KEY_K,
    [41] = KEY_SEMICOLON,          [42] = KEY_BACKSLASH,
    [43] = KEY_COMMA,              [44] = KEY_SLASH,
    [45] = KEY_N,                  [46] = KEY_M,
    [47] = KEY_PERIOD,             [48] = KEY_TAB,
    [49] = KEY_SPACE,              [50] = KEY_GRAVE_ACCENT,
    [51] = KEY_BACKSPACE,          [53] = KEY_ESCAPE,
    [54] = KEY_RIGHT_SUPER,        [55] = KEY_LEFT_SUPER,
    [56] = KEY_LEFT_SHIFT,         [57] = KEY_CAPS_LOCK,
    [58] = KEY_LEFT_ALT,           [59] = KEY_LEFT_CONTROL,
    [60] = KEY_RIGHT_SHIFT,        [61] = KEY_RIGHT_ALT,
    [62] = KEY_RIGHT_CONTROL,      [63] = KEY_FN,
    [96] = KEY_F5,                 [97] = KEY_F6,
    [98] = KEY_F7,                 [99] = KEY_F3,
    [100] = KEY_F8,                [101] = KEY_F9,
    [103] = KEY_F11,               [109] = KEY_F10,
    [111] = KEY_F12,               [118] = KEY_F4,
    [120] = KEY_F2,                [122] = KEY_F1,
    [123] = KEY_LEFT,              [124] = KEY_RIGHT,
    [125] = KEY_DOWN,              [126] = KEY_UP,
};

/**
 * Cursor-lock state. While locked the pointer is decoupled from motion and
 * re-warped to the anchor centre every pump pass; NSEvent deltas feed mouse.
 */
static bool s_cursorLocked = false;
static CGPoint s_lockCenter = {0, 0};

@class WindowDelegate;

/**
 * Opaque AppKit window handle handed back to C.
 * Holds the retained NS objects plus C-visible reflection words and the
 * embedded WindowEvent lifecycle bridge (Window Decoupling Law: publishes live
 * state and callbacks, never renders).
 * NSWindow *nsWindow - owned AppKit window (releasedWhenClosed NO)
 * WindowDelegate *delegate - per-window close/resize/focus delegate
 * WindowEvent lifecycle - embedded OS lifecycle callback registry
 * u32 id - engine window id (1..N, 0 is FOCUS_BROADCAST)
 * _Atomic bool shouldClose - true once close requested
 * _Atomic u64 sizeGeneration - resize-reflection counter
 * _Atomic i32 cachedWidth - content width in native physical pixels
 * _Atomic i32 cachedHeight - content height in native physical pixels
 * d64 cachedScale - active backing scale factor
 * d64 cachedX - top-left screen X
 * d64 cachedY - top-left screen Y
 * d64 cachedContentX - content top-left X below title bar
 * d64 cachedContentY - content top-left Y below title bar
 * _Atomic bool liveResizing - true during NSViewLiveResize
 * _Atomic bool miniaturizing - true during genie minimize
 * _Atomic i32 presentMode - pure-state present pacing policy
 * _Atomic bool transparent - pure-state transparency request
 * _Atomic u64 renderGeneration - policy-reflection rebuild ticket
 * _Atomic bool enabled - false mutes all OS input for this window
 * _Atomic bool keyEnabled - false makes canBecomeKeyWindow refuse (modal hold)
 * bool lastFocused - focus-flip detection during the pump
 * _Atomic u32 monitorId - CGDirectDisplayID mirror (0 unmapped)
 * WindowCursorType cursorType - active OS cursor style
 * TrafficLight *trafficLights - owned macOS traffic-light controller (nullptr if borderless)
 * NSView *glView - draw view above contentView where presents land (layer.contents)
 * any backdropView - NSVisualEffectView behind glView, or nullptr
 * any glassView - macOS 26 NSGlassEffectView behind glView, or nullptr
 * WindowLiquidGlassDesc liquidGlass - last-applied Liquid Glass descriptor
 * WindowResizeRenderFn resizeRenderFn - synchronous resize-cadence render hook
 * any resizeRenderUserdata - resize render hook userdata
 * i32 undecoratedMode - WINDOW_DECORATED, WINDOW_UNDECORATED_NAKED, or WINDOW_UNDECORATED_BORDERLESS
 * bool viewportFlushToTop - true when decorated viewport is flushed into top bar
 */
struct Window {
    NSWindow *nsWindow;
    WindowDelegate *delegate;
    WindowEvent lifecycle;
    u32 id;
    _Atomic bool shouldClose;
    _Atomic u64 sizeGeneration;
    _Atomic i32 cachedWidth;
    _Atomic i32 cachedHeight;
    d64 cachedScale;
    d64 cachedX;
    d64 cachedY;
    d64 cachedContentX;
    d64 cachedContentY;
    _Atomic bool liveResizing;
    _Atomic bool miniaturizing;
    _Atomic i32 presentMode;
    _Atomic bool transparent;
    _Atomic u64 renderGeneration;
    _Atomic bool enabled;
    _Atomic bool keyEnabled;
    bool lastFocused;
    _Atomic u32 monitorId;
    WindowCursorType cursorType;
    TrafficLight *trafficLights;
    NSView *glView;
    any backdropView;
    any glassView;
    WindowLiquidGlassDesc liquidGlass;
    WindowResizeRenderFn resizeRenderFn;
    any resizeRenderUserdata;
    i32 undecoratedMode;
    bool viewportFlushToTop;
};

;;GETTER
static NSWindow *Window_nsWindow(const Window *w)
{
    return (*w).nsWindow;
}
;;SETTER
static fn Window_setNsWindow(Window *w, NSWindow *v)
{
    (*w).nsWindow = v;
}
;;SETTER
static fn Window_setDelegate(Window *w, WindowDelegate *v)
{
    (*w).delegate = v;
}
;;GETTER
static WindowEvent *Window_lifecycleMut(Window *w)
{
    return &(*w).lifecycle;
}
;;GETTER
static u32 Window_idRaw(const Window *w)
{
    return (*w).id;
}
;;SETTER
static fn Window_setIdRaw(Window *w, u32 v)
{
    (*w).id = v;
}
;;GETTER
static _Atomic bool *Window_shouldCloseMut(Window *w) { return &(*w).shouldClose; }
;;GETTER
static const _Atomic bool *Window_shouldClosePtr(const Window *w) { return &(*w).shouldClose; }
;;GETTER
static _Atomic u64 *Window_sizeGenerationMut(Window *w) { return &(*w).sizeGeneration; }
;;GETTER
static const _Atomic u64 *Window_sizeGenerationPtr(const Window *w) { return &(*w).sizeGeneration; }
;;GETTER
static _Atomic i32 *Window_cachedWidthMut(Window *w) { return &(*w).cachedWidth; }
;;GETTER
static _Atomic i32 *Window_cachedHeightMut(Window *w) { return &(*w).cachedHeight; }
;;GETTER
static d64 Window_cachedScale(const Window *w)
{
    return (*w).cachedScale;
}
;;SETTER
static fn Window_setCachedScale(Window *w, d64 v)
{
    (*w).cachedScale = v;
}
;;GETTER
static d64 Window_cachedX(const Window *w)
{
    return (*w).cachedX;
}
;;SETTER
static fn Window_setCachedX(Window *w, d64 v)
{
    (*w).cachedX = v;
}
;;GETTER
static d64 Window_cachedY(const Window *w)
{
    return (*w).cachedY;
}
;;SETTER
static fn Window_setCachedY(Window *w, d64 v)
{
    (*w).cachedY = v;
}
;;GETTER
static d64 Window_cachedContentX(const Window *w)
{
    return (*w).cachedContentX;
}
;;SETTER
static fn Window_setCachedContentX(Window *w, d64 v)
{
    (*w).cachedContentX = v;
}
;;GETTER
static d64 Window_cachedContentY(const Window *w)
{
    return (*w).cachedContentY;
}
;;SETTER
static fn Window_setCachedContentY(Window *w, d64 v)
{
    (*w).cachedContentY = v;
}
;;GETTER
static _Atomic bool *Window_liveResizingMut(Window *w) { return &(*w).liveResizing; }
;;GETTER
static const _Atomic bool *Window_liveResizingPtr(const Window *w) { return &(*w).liveResizing; }
;;GETTER
static _Atomic bool *Window_miniaturizingMut(Window *w) { return &(*w).miniaturizing; }
;;GETTER
static _Atomic i32 *Window_presentModeMut(Window *w) { return &(*w).presentMode; }
;;GETTER
static const _Atomic i32 *Window_presentModePtr(const Window *w) { return &(*w).presentMode; }
;;GETTER
static _Atomic bool *Window_transparentMut(Window *w) { return &(*w).transparent; }
;;GETTER
static const _Atomic bool *Window_transparentPtr(const Window *w) { return &(*w).transparent; }
;;GETTER
static _Atomic u64 *Window_renderGenerationMut(Window *w) { return &(*w).renderGeneration; }
;;GETTER
static const _Atomic u64 *Window_renderGenerationPtr(const Window *w) { return &(*w).renderGeneration; }
;;GETTER
static _Atomic bool *Window_enabledMut(Window *w) { return &(*w).enabled; }
;;GETTER
static const _Atomic bool *Window_enabledPtr(const Window *w) { return &(*w).enabled; }
;;GETTER
static _Atomic bool *Window_keyEnabledMut(Window *w) { return &(*w).keyEnabled; }
;;GETTER
static const _Atomic bool *Window_keyEnabledPtr(const Window *w) { return &(*w).keyEnabled; }
;;GETTER
static bool Window_lastFocused(const Window *w)
{
    return (*w).lastFocused;
}
;;SETTER
static fn Window_setLastFocused(Window *w, bool v)
{
    (*w).lastFocused = v;
}
;;GETTER
static _Atomic u32 *Window_monitorIdMut(Window *w) { return &(*w).monitorId; }
;;GETTER
static const _Atomic u32 *Window_monitorIdPtr(const Window *w) { return &(*w).monitorId; }
;;GETTER
static WindowCursorType Window_cursorType(const Window *w)
{
    return (*w).cursorType;
}
;;SETTER
static fn Window_setCursorTypeRaw(Window *w, WindowCursorType v)
{
    (*w).cursorType = v;
}
;;GETTER
static TrafficLight *Window_trafficLights(const Window *w)
{
    return (*w).trafficLights;
}
;;SETTER
static fn Window_setTrafficLights(Window *w, TrafficLight *v)
{
    (*w).trafficLights = v;
}
;;GETTER
static NSView *Window_glView(const Window *w)
{
    return (*w).glView;
}
;;SETTER
static fn Window_setGlView(Window *w, NSView *v)
{
    (*w).glView = v;
}
;;GETTER
static any Window_backdropView(const Window *w)
{
    return (*w).backdropView;
}
;;SETTER
static fn Window_setBackdropView(Window *w, any v)
{
    (*w).backdropView = v;
}
;;GETTER
static any Window_glassView(const Window *w)
{
    return (*w).glassView;
}
;;SETTER
static fn Window_setGlassView(Window *w, any v)
{
    (*w).glassView = v;
}
;;GETTER
static WindowLiquidGlassDesc Window_liquidGlass(const Window *w)
{
    return (*w).liquidGlass;
}
;;SETTER
static fn Window_setLiquidGlass(Window *w, WindowLiquidGlassDesc v)
{
    (*w).liquidGlass = v;
}
;;GETTER
static WindowResizeRenderFn Window_resizeRenderFn(const Window *w)
{
    return (*w).resizeRenderFn;
}
;;SETTER
static fn Window_setResizeRenderFnRaw(Window *w, WindowResizeRenderFn v)
{
    (*w).resizeRenderFn = v;
}
;;GETTER
static any Window_resizeRenderUserdata(const Window *w)
{
    return (*w).resizeRenderUserdata;
}
;;SETTER
static fn Window_setResizeRenderUserdata(Window *w, any v)
{
    (*w).resizeRenderUserdata = v;
}
;;GETTER
static i32 Window_undecoratedMode(const Window *w)
{
    return (*w).undecoratedMode;
}
;;SETTER
static fn Window_setUndecoratedMode(Window *w, i32 v)
{
    (*w).undecoratedMode = v;
}
;;GETTER
static bool Window_viewportFlushToTop(const Window *w)
{
    return (*w).viewportFlushToTop;
}
;;SETTER
static fn Window_setViewportFlushToTopRaw(Window *w, bool v)
{
    (*w).viewportFlushToTop = v;
}

;;GETTER
static string WindowDesc_title(const WindowDesc *d)
{
    return (*d).title;
}
;;GETTER
static i32 WindowDesc_width(const WindowDesc *d)
{
    return (*d).width;
}
;;GETTER
static i32 WindowDesc_height(const WindowDesc *d)
{
    return (*d).height;
}
;;GETTER
static i32 WindowDesc_x(const WindowDesc *d)
{
    return (*d).x;
}
;;GETTER
static i32 WindowDesc_y(const WindowDesc *d)
{
    return (*d).y;
}
;;GETTER
static bool WindowDesc_centered(const WindowDesc *d)
{
    return (*d).centered;
}
;;GETTER
static bool WindowDesc_shown(const WindowDesc *d)
{
    return (*d).shown;
}

/**
 * Window id registry: slot i holds the entries for engine id i (index = id,
 * slot 0 reserved for FOCUS_BROADCAST). Grows by doubling (the Dynamic
 * Scalability & Anti-Hardcoding Law, no fixed window ceiling; the pump and
 * the id scan are the only walkers, and live windows are few).
 */
static WindowSlot *s_refs = nullptr;
static u32 s_refCap = 0;

/** Grow the NSWindow-to-handle registry geometrically and initialize new slots. */
static bool windowRefsGrow() {
    u32 nextCap = (s_refCap == 0) ? 8u : s_refCap * 2u;
    WindowSlot *grown = s_refs
        ? (WindowSlot*) MemoryArena_realloc(Lifetime_arena(), s_refs, (bytes) nextCap * sizeof(WindowSlot))
        : (WindowSlot*) MemoryArena_alloc(Lifetime_arena(), TYPE_BYTE_ARRAY, (bytes) nextCap * sizeof(WindowSlot));
    if (grown == nullptr)
        return false;
    memset((any) &grown[s_refCap], 0, (bytes) (nextCap - s_refCap) * sizeof(WindowSlot));
    s_refs = grown;
    s_refCap = nextCap;
    return true;
}

/** Resolve an id for a newly created window, or 0 when the table cannot grow. */
static u32 windowIdAcquire(NSWindow *window, Window *handle) {
    if (s_refCap == 0 && !windowRefsGrow())
        return FOCUS_BROADCAST;
    for (u32 i = 1; i < s_refCap; i++) {
        if (s_refs[i].window == nil) {
            s_refs[i].window = window;
            s_refs[i].handle = handle;
            return i;
        }
    }
    if (!windowRefsGrow())
        return FOCUS_BROADCAST;
    for (u32 i = 1; i < s_refCap; i++) {
        if (s_refs[i].window == nil) {
            s_refs[i].window = window;
            s_refs[i].handle = handle;
            return i;
        }
    }
    return FOCUS_BROADCAST;
}

/** Clear the registry slot after native delegates and input routes are detached. */
static fn windowIdRelease(u32 id) {
    if (id != FOCUS_BROADCAST && id < s_refCap) {
        s_refs[id].window = nil;
        s_refs[id].handle = nullptr;
    }
}

/** Reverse lookup: which engine id does this OS window carry? 0 if unknown. */
static u32 windowIdOf(NSWindow *window) {
    if (window == nil)
        return FOCUS_BROADCAST;
    for (u32 i = 1; i < s_refCap; i++)
        if (s_refs[i].window == window)
            return i;
    return FOCUS_BROADCAST;
}

static Window *windowHandleOf(NSWindow *window) {
    if (window == nil)
        return nullptr;
    for (u32 i = 1; i < s_refCap; i++)
        if (s_refs[i].window == window)
            return s_refs[i].handle;
    return nullptr;
}

/**
 * Flipped content view so sublayers and Cocoa geometry natively speak
 * TOP-LEFT coordinates (matching darling Container_resolve layout 1:1). Also
 * tracks live-resize start/end onto the handle so Window_isLiveResizing works
 * without any GPU state.
 * setFrameSize: override: the SYNCHRONOUS per-step resize seam. AppKit calls
 * setFrameSize: on the content view inside the live-resize tracking loop,
 * immediately after the window frame changes and BEFORE the compositor draws
 * the new bounds, the same beat the old VulkanView used. Firing the resize
 * hook here closes the one-step gap: windowDidResize: (post-display) and the
 * pump reflection remain as idempotent backups (windowRefreshSize no-ops when
 * the rounded size is unchanged), but the drag-step chase now runs at geometry
 * time, so layer frame + drawableSize + the forced present land in the same
 * composite pass as the moved edge.
 */
static fn windowRefreshSize(Window *window);
static fn windowRefreshOrigin(Window *window);
static CGFloat windowBackingScale(const Window *window);
@interface WindowContentView : NSView
@end

@implementation WindowContentView
/** Keep AppKit view coordinates in the engine's top-left origin convention. */
- (BOOL)isFlipped {
    return YES;
}

/** Publish each content-size step and invoke the resize render hook in the tracking turn. */
- (discard)setFrameSize:(NSSize)newSize {
    [super setFrameSize:newSize];
    Window *w = windowHandleOf([self window]);
    if (getenv("VEX_GEOMETRY_LOG") != nullptr)
        fprint(stderr, "sf: ", newSize.width, "x", newSize.height);
    if (w) {
        u64 previousSizeGeneration = Window_sizeGeneration(w);
        windowRefreshSize(w);
        /**
         * Repaint the WHOLE frame on EVERY geometry step, no exceptions. The
         * public resize event speaks rounded logical points, so a drag step
         * that crosses no rounding boundary still moves the seam's native
         * pixels and must repaint, and a step AppKit never marked as a live
         * resize must repaint too. The sizeGeneration compare fires the hook
         * exactly once per step: a rounded-point change already ran it inside
         * windowRefreshSize, every other step (fractional or non-live) runs it
         * here. The hook itself no-ops when the seam is already in sync, so a
         * redundant call is cheap; dropping a step is what leaves the trailing
         * edge stale (the Continuous Real-Time Live Resize Law).
         */
        if (Window_sizeGeneration(w) == previousSizeGeneration &&
            !atomic_load_explicit(Window_miniaturizingMut(w), memory_order_relaxed) &&
            ![[self window] isMiniaturized] && Window_resizeRenderFn(w))
            Window_resizeRenderFn(w)(Window_resizeRenderUserdata(w));
        /**
         * A drag off the top or left edge changes BOTH the content size and
         * the window origin in the same tracking tick; refresh the origin here
         * so onMoved fires per step alongside onResized (windowDidMove: is the
         * post-display backup, exactly as windowDidResize: is for size).
         */
        windowRefreshOrigin(w);
    }
}

/** Mark the window as resizing before AppKit begins its live-resize callbacks. */
- (discard)viewWillStartLiveResize {
    Window *w = windowHandleOf([self window]);
    if (w)
        atomic_store_explicit(Window_liveResizingMut(w), true, memory_order_relaxed);
    [super viewWillStartLiveResize];
}

/** Clear live-resize state and reconcile the final size and render hook. */
- (discard)viewDidEndLiveResize {
    Window *w = windowHandleOf([self window]);
    if (w) {
        atomic_store_explicit(Window_liveResizingMut(w), false, memory_order_relaxed);
        windowRefreshSize(w);
        /**
         * Final geometry reconciliation only: each live step already publishes
         * its current extent. The Frame hook no-ops when this size is unchanged;
         * no frozen drawable, delayed rebuild or mandatory settle frame exists.
         */
        if (Window_resizeRenderFn(w))
            Window_resizeRenderFn(w)(Window_resizeRenderUserdata(w));
    }
    [super viewDidEndLiveResize];
}
@end

/** Posts a synthetic wake-up event so Window_waitEvents returns immediately on close/quit. */
static fn windowPostWakeEvent() {
    if (NSApp == nil)
        return;
    NSEvent *wake = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                       location:NSMakePoint(0.0, 0.0)
                                  modifierFlags:0
                                      timestamp:0.0
                                   windowNumber:0
                                        context:nil
                                        subtype:0
                                          data1:0
                                          data2:0];
    if (wake != nil)
        [NSApp postEvent:wake atStart:YES];
}

/** Installs standard macOS main menu shortcuts (Cmd+Q, Cmd+W, Cmd+M, Cmd+H, Ctrl+Cmd+F). */
static fn windowInstallMainMenu() {
    if (NSApp == nil || [NSApp mainMenu] != nil)
        return;
    NSMenu *mainMenu = [[NSMenu alloc] initWithTitle:@""];
    NSMenuItem *appItem = [[NSMenuItem alloc] initWithTitle:@"" action:nil keyEquivalent:@""];
    NSMenu *appMenu = [[NSMenu alloc] initWithTitle:@""];
    NSString *appName = [[NSProcessInfo processInfo] processName];
    [appMenu addItemWithTitle:[NSString stringWithFormat:@"Hide %@", appName]
                       action:@selector(hide:)
                keyEquivalent:@"h"];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:[NSString stringWithFormat:@"Quit %@", appName]
                       action:@selector(terminate:)
                keyEquivalent:@"q"];
    [appItem setSubmenu:appMenu];
    [mainMenu addItem:appItem];

    NSMenuItem *winItem = [[NSMenuItem alloc] initWithTitle:@"Window" action:nil keyEquivalent:@""];
    NSMenu *winMenu = [[NSMenu alloc] initWithTitle:@"Window"];
    [winMenu addItemWithTitle:@"Close Window" action:@selector(performClose:) keyEquivalent:@"w"];
    [winMenu addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
    NSMenuItem *fsItem = [[NSMenuItem alloc] initWithTitle:@"Toggle Full Screen"
                                                    action:@selector(toggleFullScreen:)
                                             keyEquivalent:@"f"];
    [fsItem setKeyEquivalentModifierMask:(NSEventModifierFlagControl | NSEventModifierFlagCommand)];
    [winMenu addItem:fsItem];
    [winItem setSubmenu:winMenu];
    [mainMenu addItem:winItem];

    [NSApp setMainMenu:mainMenu];
    [NSApp setWindowsMenu:winMenu];
}

/** App-level delegate: routes Cmd+Q and last-window-close into the C run loop immediately. */
@interface WindowAppDelegate : NSObject <NSApplicationDelegate>
@end

@implementation WindowAppDelegate
/** Allow the process to terminate after its final native window closes. */
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*) sender {
    (discard) sender;
    return YES;
}

/** Convert Cmd+Q / Dock Quit into an immediate clean close of every open Window handle. */
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*) sender {
    (discard) sender;
    for (u32 i = 1; i < s_refCap; i++) {
        Window *handle = s_refs[i].handle;
        NSWindow *nsw = s_refs[i].window;
        if (handle != nullptr) {
            if (!WindowEvent_fireQuitRequested(Window_lifecycleMut(handle), handle))
                return NSTerminateCancel;
            atomic_store_explicit(Window_shouldCloseMut(handle), true, memory_order_relaxed);
        }
        if (nsw != nil)
            [nsw orderOut:nil];
    }
    windowPostWakeEvent();
    return NSTerminateCancel;
}
@end

/**
 * Window-level delegate: this is how we learn the user clicked the close
 * button, resized, zoomed, or used the green orb. The pointers are (assign)
 * because the delegate must not own our C struct.
 */
@interface WindowDelegate : NSObject <NSWindowDelegate>
@property(nonatomic, assign) atomic_bool *shouldClosePtr;
@property(nonatomic, assign) Window *handlePtr;
@end

@implementation WindowDelegate
/** Ask lifecycle listeners whether AppKit should accept the close request and close immediately. */
- (BOOL)windowShouldClose:(NSWindow*) sender {
    Window *w = self.handlePtr;
    if (w && !WindowEvent_fireQuitRequested(Window_lifecycleMut(w), w))
        return NO;
    if (self.shouldClosePtr)
        atomic_store_explicit(self.shouldClosePtr, true, memory_order_relaxed);
    [sender orderOut:nil];
    windowPostWakeEvent();
    return YES;
}

/** Set the C close latch when AppKit reports that the native window closed. */
- (discard)windowWillClose:(NSNotification*) notification {
    (discard) notification;
    if (self.shouldClosePtr)
        atomic_store_explicit(self.shouldClosePtr, true, memory_order_relaxed);
    windowPostWakeEvent();
}

/** Revalidate physical size and monitor state after the window changes screens. */
- (discard)windowDidChangeScreen:(NSNotification*) notification {
    (discard) notification;
    Window *w = self.handlePtr;
    if (w)
        Window_revalidate(w);
}

/** Revalidate pixel geometry after AppKit changes backing scale properties. */
- (discard)windowDidChangeBackingProperties:(NSNotification*) notification {
    (discard) notification;
    Window *w = self.handlePtr;
    if (w)
        Window_revalidate(w);
}

/** Refresh cached size and origin after AppKit completes a resize notification. */
- (discard)windowDidResize:(NSNotification*) notification {
    (discard) notification;
    Window *w = self.handlePtr;
    if (getenv("VEX_GEOMETRY_LOG") != nullptr)
        fprint(stderr, "dr:");
    if (w) {
        windowRefreshSize(w);
        windowRefreshOrigin(w);
    }
}

/** Refresh the cached screen origin after AppKit moves the window. */
- (discard)windowDidMove:(NSNotification*) notification {
    (discard) notification;
    Window *w = self.handlePtr;
    if (getenv("VEX_GEOMETRY_LOG") != nullptr)
        fprint(stderr, "dm:");
    if (w)
        windowRefreshOrigin(w);
}

/** Mark fullscreen transition work as live resizing before AppKit animates. */
- (discard)windowWillEnterFullScreen:(NSNotification*) notification {
    (discard) notification;
    Window *w = self.handlePtr;
    if (w)
        atomic_store_explicit(Window_liveResizingMut(w), true, memory_order_relaxed);
}

/** Publish final fullscreen geometry and notify lifecycle listeners. */
- (discard)windowDidEnterFullScreen:(NSNotification*) notification {
    (discard) notification;
    Window *w = self.handlePtr;
    if (w) {
        atomic_store_explicit(Window_liveResizingMut(w), false, memory_order_relaxed);
        windowRefreshSize(w);
        windowRefreshOrigin(w);
        WindowEvent_fireFullscreen(Window_lifecycleMut(w), w);
    }
}

/** Mark fullscreen exit as live resizing until AppKit completes the transition. */
- (discard)windowWillExitFullScreen:(NSNotification*) notification {
    (discard) notification;
    Window *w = self.handlePtr;
    if (w)
        atomic_store_explicit(Window_liveResizingMut(w), true, memory_order_relaxed);
}

/** Reconcile restored geometry, render once after transition, and notify listeners. */
- (discard)windowDidExitFullScreen:(NSNotification*) notification {
    (discard) notification;
    Window *w = self.handlePtr;
    if (w) {
        atomic_store_explicit(Window_liveResizingMut(w), false, memory_order_relaxed);
        windowRefreshSize(w);
        windowRefreshOrigin(w);
        /**
         * Same settle beat as viewDidEndLiveResize: geometry changed while
         * the live flag was set, so force one hook pass with it clear.
         */
        if (Window_resizeRenderFn(w))
            Window_resizeRenderFn(w)(Window_resizeRenderUserdata(w));
        WindowEvent_fireRestored(Window_lifecycleMut(w), w);
    }
}

/** Mark the native minimize animation so resize rendering can be suppressed. */
- (discard)windowWillMiniaturize:(NSNotification*) notification {
    (discard) notification;
    Window *w = self.handlePtr;
    if (w)
        atomic_store_explicit(Window_miniaturizingMut(w), true, memory_order_relaxed);
}

/** Clear transition state and publish the minimized lifecycle event. */
- (discard)windowDidMiniaturize:(NSNotification*) notification {
    (discard) notification;
    Window *w = self.handlePtr;
    if (w) {
        atomic_store_explicit(Window_miniaturizingMut(w), false, memory_order_relaxed);
        WindowEvent_fireMinimized(Window_lifecycleMut(w), w);
    }
}

/** Mark the restore animation so intermediate geometry is not rendered. */
- (discard)windowWillDeminiaturize:(NSNotification*) notification {
    (discard) notification;
    Window *w = self.handlePtr;
    if (w)
        atomic_store_explicit(Window_miniaturizingMut(w), true, memory_order_relaxed);
}

/** Clear transition state, refresh size, and publish restoration. */
- (discard)windowDidDeminiaturize:(NSNotification*) notification {
    (discard) notification;
    Window *w = self.handlePtr;
    if (w) {
        atomic_store_explicit(Window_miniaturizingMut(w), false, memory_order_relaxed);
        windowRefreshSize(w);
        WindowEvent_fireRestored(Window_lifecycleMut(w), w);
    }
}

/**
 * Zoom-to-fill (green button / d64-click titlebar): publish the lifecycle
 * event before AppKit performs the standard-frame change.
 */
/** Publish the zoom-fill lifecycle event before AppKit applies the target frame. */
- (BOOL)windowShouldZoom:(NSWindow*) sender toFrame:(NSRect)newFrame {
    (discard) sender;
    (discard) newFrame;
    Window *w = self.handlePtr;
    if (w)
        WindowEvent_fireZoomFilled(Window_lifecycleMut(w), w);
    return YES;
}

/**
 * Occlusion flip (buried under windows / minimized / hidden Space / ordered
 * out): publish the lifecycle event with current visibility. An ordered-out
 * window reads zero state bits, so it reports not-visible and the renderer
 * rests. The notification itself is the flip signal, no extra cache.
 */
/** Publish whether the native window is currently visible to the user. */
- (discard)windowDidChangeOcclusionState:(NSNotification*) notification {
    (discard) notification;
    Window *w = self.handlePtr;
    if (w) {
        bool visible = false;
        NSWindow *nsw = Window_nsWindow(w);
        if (nsw != nil)
            visible = ([nsw occlusionState] & NSWindowOcclusionStateVisible) != 0;
        WindowEvent_fireOcclusionChanged(Window_lifecycleMut(w), w, visible);
    }
}
@end

static CGFloat windowBackingScale(const Window *window) {
    NSWindow *nsw = window != nullptr ? Window_nsWindow(window) : nil;
    if (nsw == nil) {
        NSScreen *main = [NSScreen mainScreen];
        return main ? [main backingScaleFactor] : 1.0;
    }
    CGFloat s = [nsw backingScaleFactor];
    return (s > 0.0) ? s : 1.0;
}

/**
 * Resize reflection (Native Pixel Law): content dimensions are cached and
 * fired in native physical display pixels. On Retina/HiDPI screens, convertRectToBacking
 * provides the exact hardware pixel size. Thread 0 only.
 */
static fn windowRefreshSize(Window *window) {
    NSWindow *nsw = window != nullptr ? Window_nsWindow(window) : nil;
    if (nsw == nil)
        return;
    @autoreleasepool {
        NSView *cv = [nsw contentView];
        NSRect content = cv != nil ? [cv bounds] : [nsw contentRectForFrameRect:[nsw frame]];
        CGFloat scale = windowBackingScale(window);
        Window_setCachedScale(window, (d64) scale);

        i32 cw, ch;
        if (cv != nil) {
            NSRect backing = [cv convertRectToBacking:content];
            cw = (i32) lround(backing.size.width);
            ch = (i32) lround(backing.size.height);
        } else {
            cw = (i32) lround(content.size.width * scale);
            ch = (i32) lround(content.size.height * scale);
        }

        i32 lastW = atomic_load_explicit(Window_cachedWidthMut(window), memory_order_relaxed);
        i32 lastH = atomic_load_explicit(Window_cachedHeightMut(window), memory_order_relaxed);
        if (cw != lastW || ch != lastH) {
            atomic_store_explicit(Window_cachedWidthMut(window), cw, memory_order_relaxed);
            atomic_store_explicit(Window_cachedHeightMut(window), ch, memory_order_relaxed);
            atomic_fetch_add_explicit(Window_sizeGenerationMut(window), 1, memory_order_release);
            WindowEvent_fireResized(Window_lifecycleMut(window), window, cw, ch);
            if (!atomic_load_explicit(Window_miniaturizingMut(window), memory_order_relaxed)
                    && ![nsw isMiniaturized]
                    && Window_resizeRenderFn(window))
                Window_resizeRenderFn(window)(Window_resizeRenderUserdata(window));
        }
    }
}

/**
 * Move reflection: top-left screen coords (the same space Window_setLocation
 * speaks) plus the CONTENT top-left (below the title bar) cached for
 * Window_getContentOrigin. Fires onMoved only when the top-left actually
 * changed, so a renderer polling once per frame pays two compares. Called from
 * the per-step setFrameSize: seam (a top/left-edge resize moves the origin in
 * the same tracking tick), from windowDidMove: (post-display backup), and from
 * the pump reflection pass. Thread 0 only.
 */
static fn windowRefreshOrigin(Window *window) {
    NSWindow *nsw = window != nullptr ? Window_nsWindow(window) : nil;
    if (nsw == nil)
        return;
    @autoreleasepool {
        NSRect frame = [nsw frame];
        NSRect content = [nsw contentRectForFrameRect:frame];
        CGFloat screenHeight = [[NSScreen mainScreen] frame].size.height;
        d64 tx = (d64) frame.origin.x;
        d64 ty = (d64) (screenHeight - frame.origin.y - frame.size.height);
        NSView *cv = [nsw contentView];
        if (cv != nil) {
            NSRect cvRectInWindow = [cv frame];
            NSRect cvRectInScreen = [nsw convertRectToScreen:cvRectInWindow];
            Window_setCachedContentX(window, (d64) cvRectInScreen.origin.x);
            Window_setCachedContentY(window, (d64) (screenHeight - cvRectInScreen.origin.y - cvRectInScreen.size.height));
        } else {
            Window_setCachedContentX(window, (d64) content.origin.x);
            Window_setCachedContentY(window, (d64) (screenHeight - content.origin.y - content.size.height));
        }
        if (tx != Window_cachedX(window) || ty != Window_cachedY(window)) {
            Window_setCachedX(window, tx);
            Window_setCachedY(window, ty);
            WindowEvent_fireMoved(Window_lifecycleMut(window), window, (i32) tx, (i32) ty);
        }
    }
}

/**
 * Mirror the OS key-window into the focus word AND the WindowEvent focus
 * slots (flips only). Thread 0 only, called at the end of each pump pass.
 */
static fn windowRefreshFocus(Window *window, NSWindow *keyWindow) {
    if (window == nullptr || Window_nsWindow(window) == nil)
        return;
    bool focused = (windowIdOf(keyWindow) == Window_idRaw(window));
    if (focused != Window_lastFocused(window)) {
        Window_setLastFocused(window, focused);
        if (focused)
            WindowEvent_fireFocusGained(Window_lifecycleMut(window), window);
        else
            WindowEvent_fireFocusLost(Window_lifecycleMut(window), window);
    }
}

/** Mirror the OS display assignment into the atomic word; 0 means unmapped. */
static u32 resolveMonitorId(Window *window) {
    if (window == nullptr)
        return 0;
    @autoreleasepool {
        NSScreen *screen = [Window_nsWindow(window) screen];
        if (screen == nil)
            return 0;
        NSNumber *displayId = [[screen deviceDescription] objectForKey:@"NSScreenNumber"];
        return displayId ? (u32) [displayId unsignedIntValue] : 0;
    }
}

/** Resolve and publish the screen identity currently carrying this window. */
static fn windowRefreshMonitor(Window *window) {
    if (window == nullptr)
        return;
    u32 next = resolveMonitorId(window);
    atomic_store_explicit(Window_monitorIdMut(window), next, memory_order_relaxed);
}

/**
 * Re-centre the cursor during the pump while locked, so the warp registers
 * before the next event loop exit.
 */
static fn recenterIfLocked() {
    if (s_cursorLocked)
        CGWarpMouseCursorPosition(s_lockCenter);
}

/** Reports whether a mouse event's location lies inside its target window's content view. */
static bool mouseInContentBounds(NSEvent *event) {
    NSWindow *eventWindow = [event window];
    if (eventWindow == nil)
        return false;
    NSView *cv = [eventWindow contentView];
    if (cv == nil)
        return false;
    NSPoint pt = [cv convertPoint:[event locationInWindow] fromView:nil];
    return NSPointInRect(pt, [cv bounds]);
}

/**
 * Content-area coordinates (top-left origin) for a mouse event in physical pixels (Native Pixel Law).
 * Events without an owning window report (-1, -1) instead of leaking raw screen-space coordinates.
 */
static fn mouseLocation(NSEvent *event, d64 *outX, d64 *outY) {
    NSWindow *eventWindow = [event window];
    if (eventWindow == nil) {
        *outX = -1.0;
        *outY = -1.0;
        return;
    }
    NSPoint p = [event locationInWindow];
    CGFloat scale = [eventWindow backingScaleFactor];
    if (scale <= 0.0)
        scale = 1.0;
    NSView *cv = [eventWindow contentView];
    if (cv != nil) {
        NSPoint pt = [cv convertPoint:p fromView:nil];
        *outX = (d64) pt.x * (d64) scale;
        *outY = (d64) pt.y * (d64) scale;
        return;
    }
    NSRect content = [eventWindow contentRectForFrameRect:[eventWindow frame]];
    p.y = content.size.height - p.y;
    *outX = (d64) p.x * (d64) scale;
    *outY = (d64) p.y * (d64) scale;
}

/** Map an NSTouch phase onto the Touch_* action codes. */
static i32 touchAction(NSTouchPhase phase) {
    if (phase & NSTouchPhaseBegan)
        return TOUCH_DOWN;
    if (phase & (NSTouchPhaseMoved | NSTouchPhaseStationary))
        return TOUCH_MOVE;
    if (phase & NSTouchPhaseEnded)
        return TOUCH_UP;
    return TOUCH_CANCEL;
}

/**
 * Feed one gesture/touch carrier event into input.Touch: resolve each active
 * touch into its slot (identity hash % TOUCH_MAX), normalize position into
 * content coords, and estimate pressure from the resting flag.
 */
static fn dispatchTouches(NSEvent *event, u32 wid) {
    NSWindow *eventWindow = [event window];
    if (eventWindow == nil)
        return;
    NSView *contentView = [eventWindow contentView];
    if (contentView == nil)
        return;

    NSSet *touches = [event touchesMatchingPhase:NSTouchPhaseAny inView:contentView];
    if (touches == nil || touches.count == 0)
        return;

    NSRect frame = [contentView frame];
    d64 winW = frame.size.width;
    d64 winH = frame.size.height;

    for (NSTouch *touch in touches) {
        NSUInteger slot = [[touch identity] hash] % TOUCH_MAX;
        NSPoint norm = [touch normalizedPosition];
        d64 posX = norm.x * winW;
        d64 posY = (1.0 - norm.y) * winH;
        d64 pressure = [touch isResting] ? 0.2 : 0.8;
        Touch_pushTouchEvent(wid, (i32) slot, touchAction([touch phase]),
                             posX, posY, pressure, kTapThresholdNanos);
    }
}

/**
 * Intercept-and-forward: read everything the engine cares about off each OS
 * event, push it into the input modules, then hand the event back to AppKit
 * so the responder chain keeps working. This is the Thread-0 producer side of
 * the input pipeline.
 */
static fn routeEvent(NSEvent *event) {
    NSEventType type = [event type];
    u32 wid = windowIdOf([event window]);

    Window *target = (wid != FOCUS_BROADCAST) ? windowHandleOf([event window]) : nullptr;
    if (target && !atomic_load_explicit(Window_enabledMut(target), memory_order_relaxed))
        return;

    switch (type) {
        case NSEventTypeFlagsChanged: {
            i16 macCode = [event keyCode];
            NSEventModifierFlags flags = [event modifierFlags];
            i32 stdKey = (macCode >= 0 && macCode < 128) ? macKeyMap[macCode] : -1;
            if (stdKey != -1) {
                bool isDown = false;
                if (macCode == 54 || macCode == 55) {
                    isDown = (flags & NSEventModifierFlagCommand) != 0;
                } else if (macCode == 56 || macCode == 60) {
                    isDown = (flags & NSEventModifierFlagShift) != 0;
                } else if (macCode == 58 || macCode == 61) {
                    isDown = (flags & NSEventModifierFlagOption) != 0;
                } else if (macCode == 59 || macCode == 62) {
                    isDown = (flags & NSEventModifierFlagControl) != 0;
                } else if (macCode == 57) {
                    isDown = (flags & NSEventModifierFlagCapsLock) != 0;
                }
                Key_pushEvent(wid, stdKey,
                              isDown ? KEY_ACTION_DOWN : KEY_ACTION_UP,
                              kTapThresholdNanos);
            }
            break;
        }

        case NSEventTypeKeyDown:
        case NSEventTypeKeyUp: {
            NSEventModifierFlags flags = [event modifierFlags];
            bool cmd = (flags & NSEventModifierFlagCommand) != 0;
            if (cmd != (Key_isDown(KEY_LEFT_SUPER) || Key_isDown(KEY_RIGHT_SUPER)))
                Key_pushEvent(wid, KEY_LEFT_SUPER, cmd ? KEY_ACTION_DOWN : KEY_ACTION_UP, kTapThresholdNanos);
            bool ctrl = (flags & NSEventModifierFlagControl) != 0;
            if (ctrl != (Key_isDown(KEY_LEFT_CONTROL) || Key_isDown(KEY_RIGHT_CONTROL)))
                Key_pushEvent(wid, KEY_LEFT_CONTROL, ctrl ? KEY_ACTION_DOWN : KEY_ACTION_UP, kTapThresholdNanos);
            bool alt = (flags & NSEventModifierFlagOption) != 0;
            if (alt != (Key_isDown(KEY_LEFT_ALT) || Key_isDown(KEY_RIGHT_ALT)))
                Key_pushEvent(wid, KEY_LEFT_ALT, alt ? KEY_ACTION_DOWN : KEY_ACTION_UP, kTapThresholdNanos);
            bool shift = (flags & NSEventModifierFlagShift) != 0;
            if (shift != (Key_isDown(KEY_LEFT_SHIFT) || Key_isDown(KEY_RIGHT_SHIFT)))
                Key_pushEvent(wid, KEY_LEFT_SHIFT, shift ? KEY_ACTION_DOWN : KEY_ACTION_UP, kTapThresholdNanos);
            i16 macCode = [event keyCode];
            if (macCode >= 0 && macCode < 128) {
                i32 stdKey = macKeyMap[macCode];
                if (stdKey != -1)
                    Key_pushEvent(wid, stdKey,
                                  type == NSEventTypeKeyDown ? KEY_ACTION_DOWN : KEY_ACTION_UP,
                                  kTapThresholdNanos);
                if (type == NSEventTypeKeyDown && stdKey != -1) {
                    NSString *chars = [event characters];
                    if (chars.length > 0) {
                        u8 c0 = (u8) [chars characterAtIndex:0];
                        if (c0 > 0)
                            Key_pushCharEvent(wid, c0);
                    }
                }
            }
            break;
        }

        case NSEventTypeScrollWheel: {
            d64 x, y;
            mouseLocation(event, &x, &y);
            Mouse_pushMoveEvent(wid, x, y);
            CGFloat scale = [event window] ? [[event window] backingScaleFactor] : 1.0;
            if (scale <= 0.0)
                scale = 1.0;
            d64 mult = [event hasPreciseScrollingDeltas] ? (d64) scale : (12.0 * (d64) scale);
            Mouse_pushScrollEvent(wid, [event scrollingDeltaX] * mult, [event scrollingDeltaY] * mult);
            break;
        }

        case NSEventTypeMagnify:
            Mouse_pushZoomEvent(wid, [event magnification]);
            break;

        case NSEventTypeLeftMouseDown:
        case NSEventTypeRightMouseDown:
        case NSEventTypeOtherMouseDown: {
            if (!mouseInContentBounds(event))
                break;
            i32 button = (type == NSEventTypeLeftMouseDown) ? MOUSE_LEFT
                       : ((type == NSEventTypeRightMouseDown) ? MOUSE_RIGHT
                                                              : (i32) [event buttonNumber]);
            d64 x, y;
            mouseLocation(event, &x, &y);
            Mouse_pushMoveEvent(wid, x, y);
            Mouse_pushButtonEvent(wid, button, KEY_ACTION_DOWN, kTapThresholdNanos);
            if (target)
                WindowEvent_firePressed(Window_lifecycleMut(target), target);
            break;
        }

        case NSEventTypeLeftMouseUp:
        case NSEventTypeRightMouseUp:
        case NSEventTypeOtherMouseUp: {
            i32 button = (type == NSEventTypeLeftMouseUp) ? MOUSE_LEFT
                       : ((type == NSEventTypeRightMouseUp) ? MOUSE_RIGHT
                                                             : (i32) [event buttonNumber]);
            d64 x, y;
            mouseLocation(event, &x, &y);
            Mouse_pushMoveEvent(wid, x, y);
            Mouse_pushButtonEvent(wid, button, KEY_ACTION_UP, kTapThresholdNanos);
            break;
        }

        case NSEventTypeMouseMoved:
        case NSEventTypeLeftMouseDragged:
        case NSEventTypeRightMouseDragged:
        case NSEventTypeOtherMouseDragged: {
            if (s_cursorLocked) {
                CGFloat scale = [event window] ? [[event window] backingScaleFactor] : 1.0;
                if (scale <= 0.0)
                    scale = 1.0;
                Mouse_pushMoveDeltaEvent(wid, [event deltaX] * (d64) scale, [event deltaY] * (d64) scale);
                break;
            }
            if (type == NSEventTypeMouseMoved) {
                if (mouseInContentBounds(event)) {
                    d64 x, y;
                    mouseLocation(event, &x, &y);
                    Mouse_pushMoveEvent(wid, x, y);
                } else {
                    Mouse_pushMoveEvent(wid, -1.0, -1.0);
                }
            } else if ([event window] != nil) {
                d64 x, y;
                mouseLocation(event, &x, &y);
                i32 button = (type == NSEventTypeLeftMouseDragged) ? MOUSE_LEFT
                           : ((type == NSEventTypeRightMouseDragged) ? MOUSE_RIGHT
                                                                      : (i32) [event buttonNumber]);
                Mouse_pushDragEvent(wid, button, x, y);
            }
            break;
        }

        case NSEventTypeGesture:
        case NSEventTypeBeginGesture:
        case NSEventTypeEndGesture:
            dispatchTouches(event, wid);
            break;

        default:
            break;
    }
}

/**
 * Drain the OS event queue. Called every frame from the engine loop (the
 * "poll" half of poll-then-tick). Returns immediately; never blocks. After
 * the pump, mirror the OS's key window into the focus word so the rest of
 * the engine can ask "who is focused?" without touching AppKit.
 */
static NSWindow *sLastWindow = nil;
/**
 * Key-window claim pending: set by Window_show/Window_focus, drained by the
 * pump once the app is active and the claim sticks.
 */
static NSWindow *sPendingKeyWindow = nil;
static WindowAppDelegate *sAppDelegate = nil; /** one app delegate for the whole process */

/** Drain AppKit events non-blockingly, optionally stopping after one event. */
static bool windowPollEvents(bool singleStep) {
    bool more = false;
    @autoreleasepool {
        NSEvent *event;
        while ((event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                            untilDate:[NSDate distantPast]
                                               inMode:NSDefaultRunLoopMode
                                              dequeue:YES])) {
            routeEvent(event);
            [NSApp sendEvent:event];
            if (singleStep)
                break;
        }
        if (singleStep)
            more = [NSApp nextEventMatchingMask:NSEventMaskAny
                                      untilDate:[NSDate distantPast]
                                         inMode:NSDefaultRunLoopMode
                                        dequeue:NO] != nil;
        [NSApp updateWindows];
        recenterIfLocked();
        Focus_set(windowIdOf([NSApp keyWindow]));

        if (sPendingKeyWindow) {
            if ([NSApp keyWindow] == sPendingKeyWindow) {
                sPendingKeyWindow = nil;
            } else if ([NSApp isActive] && [sPendingKeyWindow canBecomeKeyWindow]) {
                [sPendingKeyWindow makeKeyWindow];
                if ([NSApp keyWindow] == sPendingKeyWindow)
                    sPendingKeyWindow = nil;
            }
        }

        NSWindow *keyWindow = [NSApp keyWindow];
        for (u32 i = 1; i < s_refCap; i++) {
            NSWindow *w = s_refs[i].window;
            if (w == nil)
                continue;
            Window *handle = s_refs[i].handle;
            if (handle == nullptr)
                continue;
            windowRefreshSize(handle);

            windowRefreshOrigin(handle);

            windowRefreshFocus(handle, keyWindow);
            windowRefreshMonitor(handle);
        }
    }
    return more;
}

/** Drain the process-wide AppKit queue and refresh registered window state. */
fn Window_pollEvents() {
    (discard) windowPollEvents(false);
}

/** Dispatch at most one AppKit event and report whether another remains queued. */
bool Window_pollEventStep() {
    return windowPollEvents(true);
}

/** Wait for one process-wide AppKit event for the requested bounded interval. */
fn Window_waitEvents(Window *window, i32 timeoutMs) {
    if (window != nullptr && Window_shouldClose(window))
        return;
    @autoreleasepool {
        NSDate *until = timeoutMs > 0
            ? [NSDate dateWithTimeIntervalSinceNow:(d64) timeoutMs / 1000.0]
            : [NSDate distantFuture];
        NSEvent *event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                            untilDate:until
                                               inMode:NSDefaultRunLoopMode
                                              dequeue:YES];
        if (event != nil) {
            routeEvent(event);
            [NSApp sendEvent:event];
        }
        [NSApp updateWindows];
        recenterIfLocked();
        Focus_set(windowIdOf([NSApp keyWindow]));
    }
}

/**
 * Key-gate window subclass: while a modal dialog holds its parent, the
 * parent's C flag flips and AppKit itself refuses it key, no focus flash,
 * no input gap, no yank-back. Render + ordering are untouched (only key).
 */
@interface VexWindow : NSWindow
@property (assign, nonatomic) Window *vexHandle;
@end

@implementation VexWindow
/** Respect the C key-focus gate before allowing AppKit key-window eligibility. */
- (BOOL)canBecomeKeyWindow {
    Window *h = self.vexHandle;
    if (h != nullptr && !atomic_load_explicit(Window_keyEnabledPtr(h), memory_order_relaxed))
        return NO;
    return [super canBecomeKeyWindow];
}

/**
 * The standard titlebar strip height (points). A FullSizeContentView window
 * reports a zero titlebar inset, so fall back to the platform's ~28pt strip.
 */
- (CGFloat)vexTitlebarHeight {
    NSRect frame = self.frame;
    NSRect content = [self contentRectForFrameRect:frame];
    NSSize frameSize = frame.size;
    NSSize contentSize = content.size;
    CGFloat h = frameSize.height - contentSize.height;
    return h > 0.0 ? h : 28.0;
}

/**
 * Titlebar d64-click is the accessibility affordance Windows ships as the
 * maximize (square) button, and macOS ships as the green zoom. On a NAKED /
 * full-size-content window the title bar is transparent, so AppKit's own
 * d64-click target is nearly unhittable, catch it explicitly. A plain
 * d64-click zooms (fills the visible frame, the macOS maximize); an
 * Option-d64-click enters fullscreen. Both are no-ops if already there.
 *
 * The resize border keeps its own behavior: when the pointer is on an edge or
 * corner (the up-down / diagonal resize cursor), the d64-click stays a
 * directional resize and we do not intercept, only the title-bar interior
 * (normal cursor) maximizes.
 */
static const CGFloat kWindowResizeBorderPoints = 6.0;

/** Route titlebar d64-click gestures before ordinary AppKit event delivery. */
- (discard)sendEvent:(NSEvent*) event {
    if (event.type == NSEventTypeLeftMouseDown && event.clickCount == 2) {
        NSPoint p = [event locationInWindow];
        NSSize size = self.frame.size;
        BOOL onResizeBorder = p.x <= kWindowResizeBorderPoints
                           || p.x >= size.width - kWindowResizeBorderPoints
                           || p.y <= kWindowResizeBorderPoints
                           || p.y >= size.height - kWindowResizeBorderPoints;
        CGFloat titlebar = [self vexTitlebarHeight];
        if (!onResizeBorder && p.y >= size.height - titlebar) {
            if ((event.modifierFlags & NSEventModifierFlagOption) != 0)
                [self toggleFullScreen:self];
            else
                [self performZoom:self];
            return;
        }
    }
    [super sendEvent:event];
}
@end

/**
 * Build the NSWindow + C handle. Shared by every constructor. The window is
 * created HIDDEN, visibility is an explicit Window_show() decision, so
 * construct  to  mutate  to  show never flashes a half-configured window.
 * Initial placement also happens here (not via a post-creation move):
 * creating already-placed keeps AppKit's move tracker quiet, a
 * setFrameOrigin: on a never-shown window logs "move completed
 * without beginning".
 */
static Window *windowAlloc(const WindowDesc *desc) {
    @autoreleasepool {
        if (NSApp == nil)
            [NSApplication sharedApplication];
        if (sAppDelegate == nil) {
            sAppDelegate = [[WindowAppDelegate alloc] init];
            [NSApp setDelegate:sAppDelegate];
            [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
            windowInstallMainMenu();
            [NSApp finishLaunching];
        }

        NSWindowStyleMask style = NSWindowStyleMaskTitled
                                | NSWindowStyleMaskClosable
                                | NSWindowStyleMaskMiniaturizable
                                | NSWindowStyleMaskResizable;

        CGFloat scale = [NSScreen mainScreen] ? [[NSScreen mainScreen] backingScaleFactor] : 1.0;
        if (scale <= 0.0)
            scale = 1.0;
        CGFloat cw = (CGFloat)WindowDesc_width(desc) / scale;
        CGFloat ch = (CGFloat)WindowDesc_height(desc) / scale;
        NSRect wantFrame = [NSWindow frameRectForContentRect:NSMakeRect(0, 0, cw, ch)
                                                  styleMask:style];
        i32 dx = WindowDesc_x(desc);
        i32 dy = WindowDesc_y(desc);
        if (WindowDesc_centered(desc) || (dx == 0 && dy == 0)) {
            NSRect avail = [[NSScreen mainScreen] visibleFrame];
            wantFrame.origin.x = avail.origin.x + (avail.size.width - wantFrame.size.width) * 0.5;
            wantFrame.origin.y = avail.origin.y + (avail.size.height - wantFrame.size.height) * 0.5;
        } else {
            NSRect screen = [[NSScreen mainScreen] frame];
            wantFrame.origin.x = (CGFloat)dx;
            wantFrame.origin.y = screen.size.height - (CGFloat)dy - wantFrame.size.height;
        }
        wantFrame.origin.x = (CGFloat) floor(wantFrame.origin.x + 0.5);
        wantFrame.origin.y = (CGFloat) floor(wantFrame.origin.y + 0.5);
        NSRect frame = [NSWindow contentRectForFrameRect:wantFrame
                                              styleMask:style];

        VexWindow *window = [[VexWindow alloc]
            initWithContentRect:frame
                       styleMask:style
                          backing:NSBackingStoreBuffered
                            defer:NO];
        [window setTitle:[NSString stringWithUTF8String:WindowDesc_title(desc)]];
        [window setReleasedWhenClosed:NO];

        WindowContentView *contentView = [[WindowContentView alloc] initWithFrame:frame];
        [contentView setWantsLayer:YES];
        [window setContentView:contentView];

        NSView *glView = [[NSView alloc] initWithFrame:contentView.bounds];
        [glView setWantsLayer:YES];
        [glView setAutoresizingMask:(NSViewWidthSizable | NSViewHeightSizable)];
        [contentView addSubview:glView];

        [window setCollectionBehavior:NSWindowCollectionBehaviorFullScreenPrimary];

        [window setPreservesContentDuringLiveResize:NO];

        [window setAcceptsMouseMovedEvents:YES];

        [window.contentView setAllowedTouchTypes:NSTouchTypeMaskDirect | NSTouchTypeMaskIndirect];

        WindowDelegate *delegate = [[WindowDelegate alloc] init];
        [window setDelegate:delegate];

        Window *w = (Window*) MemoryArena_alloc(Lifetime_arena(), TYPE_BYTE_ARRAY, sizeof(Window));
        if (w == nullptr)
            return nullptr;
        memset((any) w, 0, sizeof(Window));
        Window_setNsWindow(w, window);
        window.vexHandle = w;
        Window_setGlView(w, glView);
        Window_setBackdropView(w, nullptr);
        Window_setGlassView(w, nullptr);
        Window_setDelegate(w, delegate);
        WindowEvent_init(Window_lifecycleMut(w));
        atomic_store_explicit(Window_shouldCloseMut(w), false, memory_order_relaxed);
        atomic_store_explicit(Window_sizeGenerationMut(w), 0, memory_order_relaxed);
        CGFloat winScale = [window backingScaleFactor];
        if (winScale <= 0.0)
            winScale = scale;
        Window_setCachedScale(w, (d64) winScale);
        NSRect initialContent = [window contentRectForFrameRect:[window frame]];
        i32 initPxW = (i32) lround(initialContent.size.width * winScale);
        i32 initPxH = (i32) lround(initialContent.size.height * winScale);
        atomic_store_explicit(Window_cachedWidthMut(w), initPxW, memory_order_relaxed);
        atomic_store_explicit(Window_cachedHeightMut(w), initPxH, memory_order_relaxed);
        NSRect initialFrame = [window frame];
        CGFloat screenH = [[NSScreen mainScreen] frame].size.height;
        Window_setCachedX(w, (d64) initialFrame.origin.x);
        Window_setCachedY(w, (d64) (screenH - initialFrame.origin.y - initialFrame.size.height));
        Window_setCachedContentX(w, (d64) initialFrame.origin.x
                              + (d64) (initialContent.origin.x - initialFrame.origin.x));
        Window_setCachedContentY(w, (d64) (screenH - initialContent.origin.y - initialContent.size.height));
        atomic_store_explicit(Window_presentModeMut(w), WINDOW_PRESENT_FIFO, memory_order_relaxed);
        atomic_store_explicit(Window_transparentMut(w), false, memory_order_relaxed);
        atomic_store_explicit(Window_renderGenerationMut(w), 0, memory_order_relaxed);
        atomic_store_explicit(Window_enabledMut(w), true, memory_order_relaxed);
        atomic_store_explicit(Window_keyEnabledMut(w), true, memory_order_relaxed);
        Window_setLastFocused(w, false);
        atomic_store_explicit(Window_monitorIdMut(w), 0, memory_order_relaxed);
        Window_setCursorTypeRaw(w, WINDOW_CURSOR_DEFAULT);
        Window_setTrafficLights(w, TrafficLight((__bridge any) window));
        Window_setResizeRenderFnRaw(w, nullptr);
        Window_setResizeRenderUserdata(w, nullptr);
        Window_setIdRaw(w, windowIdAcquire(window, w));
        delegate.shouldClosePtr = Window_shouldCloseMut(w);
        delegate.handlePtr = w;

        sLastWindow = window;

        return w;
    }
}

/**
 * Merge a caller's Desc over the defaults. Unset (zero) fields fall back,
 * this is what makes partial designated initializers behave like overloads.
 */
static WindowDesc descResolve(const WindowDesc *desc) {
    WindowDesc d;
    memset(&d, 0, sizeof(d));
    d.title = "vex";
    d.width = 800;
    d.height = 600;
    d.centered = true;
    if (desc == nullptr)
        return d;
    if (WindowDesc_title(desc))
        d.title = WindowDesc_title(desc);
    if (WindowDesc_width(desc) > 0)
        d.width = WindowDesc_width(desc);
    if (WindowDesc_height(desc) > 0)
        d.height = WindowDesc_height(desc);
    if (WindowDesc_x(desc) != 0 || WindowDesc_y(desc) != 0) {
        d.x = WindowDesc_x(desc);
        d.y = WindowDesc_y(desc);
        d.centered = false;
    }
    d.shown = WindowDesc_shown(desc);
    return d;
}

/** --- Constructors ----------------------------------------------------------- */

/** Construct a default AppKit window through the common descriptor path. */
Window *Window_0() {
    return Window_new(nullptr);
}

/** Construct a window descriptor with the requested title and default dimensions. */
Window *Window_1(string title) {
    return Window_new(&(WindowDesc){ .title = title });
}

/** Construct a window descriptor with a title and explicit content dimensions. */
Window *Window_3(string title, i32 width, i32 height) {
    return Window_new(&(WindowDesc){ .title = title, .width = width, .height = height });
}

/** Resolve descriptor defaults, allocate the AppKit window, and show on request. */
Window *Window_new(const WindowDesc *desc) {
    WindowDesc d = descResolve(desc);
    Window *w = windowAlloc(&d);
    if (w == nullptr)
        return nullptr;
    if (d.shown)
        Window_show(w);
    return w;
}

/**
 * Tear down the window and free the handle. Safe to call whether the user
 * already closed the window or not: if it's still open we close it, and we
 * detach the delegate first so no callback can touch our freed memory.
 */
fn Window_destroy(Window *window) {
    if (window == nullptr)
        return;
    @autoreleasepool {
        NSWindow *nsw = Window_nsWindow(window);
        u32 wid = Window_idRaw(window);
        if (sPendingKeyWindow == nsw)
            sPendingKeyWindow = nil;
        [nsw setDelegate:nil];
        [nsw close];
        Key_detachWindowAll(wid);
        Mouse_detachWindowAll(wid);
        Touch_detachWindowAll(wid);
        TrafficLight_destroy(Window_trafficLights(window));
        windowIdRelease(wid);
        Window_setGlView(window, nil);
        Window_setDelegate(window, nil);
        Window_setNsWindow(window, nil);
    }
    MemoryArena_free(Lifetime_arena(), window);
}

/** Destroy each registered window while its delegate and handle are still valid. */
fn Window_destroyAll() {
    if (s_refs == nullptr || s_refCap == 0)
        return;
    for (u32 i = 1; i < s_refCap; i++) {
        Window *handle = s_refs[i].handle;
        if (handle != nullptr)
            Window_destroy(handle);
    }
}

/** Read the close latch, treating a missing window as already closed. */
bool Window_shouldClose(Window *window) {
    return window ? atomic_load_explicit(Window_shouldClosePtr(window), memory_order_relaxed) : true;
}

/** Set the close latch observed by the host lifecycle loop. */
fn Window_setShouldClose(Window *window, bool shouldClose) {
    if (window != nullptr)
        atomic_store_explicit(Window_shouldCloseMut(window), shouldClose, memory_order_relaxed);
}

/** Ask AppKit to close the native window and set the C-side close latch. */
fn Window_close(Window *window) {
    if (!window)
        return;
    @autoreleasepool { [Window_nsWindow(window) close]; }
    atomic_store_explicit(Window_shouldCloseMut(window), true, memory_order_relaxed);
}

/** --- Present policy (pure state; a future render path consumes it) ---------- */

fn Window_setPresentMode(Window *window, i32 mode) {
    if (window == nullptr)
        return;
    i32 prev = atomic_exchange_explicit(Window_presentModeMut(window), mode, memory_order_relaxed);
    if (prev != mode)
        atomic_fetch_add_explicit(Window_renderGenerationMut(window), 1, memory_order_release);
}

/** Read the stored present policy, defaulting to FIFO for a null handle. */
i32 Window_getPresentMode(const Window *window) {
    if (window == nullptr)
        return WINDOW_PRESENT_FIFO;
    return atomic_load_explicit(Window_presentModePtr(window), memory_order_relaxed);
}

/** Update the stored transparency policy and advance the render generation on change. */
fn Window_setTransparent(Window *window, bool transparent) {
    if (window == nullptr)
        return;
    bool prev = atomic_exchange_explicit(Window_transparentMut(window), transparent, memory_order_relaxed);
    if (prev != transparent)
        atomic_fetch_add_explicit(Window_renderGenerationMut(window), 1, memory_order_release);
}

/** Read the stored transparency policy, returning false for a null handle. */
bool Window_isTransparent(const Window *window) {
    return window ? atomic_load_explicit(Window_transparentPtr(window), memory_order_relaxed) : false;
}

/** Read the policy generation used by downstream render-resource rebuilds. */
u64 Window_renderGeneration(const Window *window) {
    return window ? atomic_load_explicit(Window_renderGenerationPtr(window), memory_order_acquire) : 0;
}

/** --- Graphics board slots (stored + ordered; rendering lives elsewhere) ----- */

/** ;;INTENTION("Graphics board slots and layer ordering are GPU-era stubs retired per the Window Decoupling Law. A Window is a dumb presentation surface; graphics layers and surfaces are managed by graphvex.") */
fn Window_orderLayers(Window *window) {
    (discard) window;
}

/** Retain the retired lower-layer setter as an inert compatibility seam. */
fn Window_setBottomLayer(Window *window, any layer) {
    (discard) window;
    (discard) layer;
}

/** Return no layer because the lean window no longer owns graphics layers. */
any Window_getBottomLayer(const Window *window) {
    (discard) window;
    return nullptr;
}

/** Retain the retired upper-layer setter as an inert compatibility seam. */
fn Window_setTopLayer(Window *window, any layer) {
    (discard) window;
    (discard) layer;
}

/** Return no layer because the lean window no longer owns graphics layers. */
any Window_getTopLayer(const Window *window) {
    (discard) window;
    return nullptr;
}

/** ;;INTENTION("Pane/board compositing was the GPU-era shim's job; on this window it is inert (still-unmigrated darling compositor retains the call). Pane work migrates to the render personal' own pass; retires together with the composite seam.") */
bool Window_attachPanes(Window *window, Panel *panel, i32 width, i32 height) {
    (discard) window;
    (discard) panel;
    (discard) width;
    (discard) height;
    return false;
}

/** Retain the retired pane API as an inert failure on the lean window backend. */
bool Window_resizePanes(Window *window, Panel *panel, i32 width, i32 height) {
    (discard) window;
    (discard) panel;
    (discard) width;
    (discard) height;
    return false;
}

/** Keep the retired pane-compositor entry point linkable without rendering. */
fn Window_compositePanes(Window *window, Panel *contentPanel) {
    (discard) window;
    (discard) contentPanel;
}

/** Keep the retired board-compositor entry point linkable without rendering. */
fn Window_compositeBoards(Window *window) {
    (discard) window;
}

/** --- Runtime state ---------------------------------------------------------- */

fn Window_setEnabled(Window *window, bool enabled) {
    if (window == nullptr)
        return;
    atomic_store_explicit(Window_enabledMut(window), enabled, memory_order_relaxed);
}

/** Read whether OS input forwarding is enabled for this window. */
bool Window_isEnabled(const Window *window) {
    return window ? atomic_load_explicit(Window_enabledPtr(window), memory_order_relaxed) : false;
}

/** Read live-resize state, including the file-local diagnostic override seam. */
bool Window_isLiveResizing(const Window *window) {
    FILE *probe = fopen("/tmp/vex_live", "r");
    if (probe) {
        fclose(probe);
        return window != nullptr;
    }
    return window ? atomic_load_explicit(Window_liveResizingPtr(window), memory_order_relaxed) : false;
}

/** --- Chrome / state API ----------------------------------------------------- */

static NSWindowStyleMask styleMaskOf(Window *window) {
    return [Window_nsWindow(window) styleMask];
}

/** Test whether the native window's current style mask contains the requested bit. */
static bool hasStyleBit(Window *window, NSWindowStyleMask bit) {
    return (styleMaskOf(window) & bit) != 0;
}

/**
 * Single mask-rewrite path for all capability toggles. While native fullscreen
 * AppKit owns the mask, so style mutations are skipped then.
 */
static fn updateStyleMask(Window *window, NSWindowStyleMask add, NSWindowStyleMask clear) {
    NSWindowStyleMask mask = styleMaskOf(window);
    if ((mask & NSWindowStyleMaskFullScreen) != 0)
        return;
    [Window_nsWindow(window) setStyleMask:(mask & ~clear) | add];
}

/**
 * Re-apply the traffic-light chrome through the segregated TrafficLight class
 * (window/traffic_light.h): per-button visibility, then re-seat the cluster
 * offset relative to the native layout (snapshotted on first touch; the
 * snapshot is invalidated whenever the style mask changes). Pure public AppKit
 * (standardWindowButton: + setHidden:/setFrameOrigin:), no private API.
 * BORDERLESS windows own no lights to re-seat. Null-safe.
 */
static fn refreshChrome(Window *w) {
    if (w == nullptr)
        return;
    TrafficLight_refresh(Window_trafficLights(w));
}

/** Set the AppKit title and reapply hidden-title chrome when required. */
fn Window_setTitle(Window *window, string title) {
    if (window == nullptr || title == nullptr)
        return;
    @autoreleasepool {
        NSWindow *nsw = Window_nsWindow(window);
        [nsw setTitle:[NSString stringWithUTF8String:title]];

        if ((styleMaskOf(window) & NSWindowStyleMaskFullSizeContentView) != 0) {
            [nsw setTitlebarAppearsTransparent:YES];
            [nsw setTitleVisibility:NSWindowTitleHidden];
        }
        refreshChrome(window);
    }
}

/** Read the cached content width in native physical pixels. */
i32 Window_width(Window *window) {
    if (window == nullptr)
        return 0;
    return atomic_load_explicit(Window_cachedWidthMut(window), memory_order_relaxed);
}

/** Read the cached content height in native physical pixels. */
i32 Window_height(Window *window) {
    if (window == nullptr)
        return 0;
    return atomic_load_explicit(Window_cachedHeightMut(window), memory_order_relaxed);
}

/** Convert requested physical dimensions to AppKit points and resize the content. */
fn Window_setSize(Window *window, i32 width, i32 height) {
    if (window == nullptr || width <= 0 || height <= 0)
        return;
    @autoreleasepool {
        CGFloat scale = windowBackingScale(window);
        CGFloat ptW = (CGFloat) width / scale;
        CGFloat ptH = (CGFloat) height / scale;
        [Window_nsWindow(window) setContentSize:NSMakeSize(ptW, ptH)];
        windowRefreshSize(window);
    }
}

/** Return the current AppKit backing scale factor for the window or main screen. */
f32 Window_getScale(const Window *window) {
    return (f32) windowBackingScale(window);
}

static CGDirectDisplayID windowDisplayId(const Window *window) {
    NSWindow *nsw = window != nullptr ? Window_nsWindow(window) : nil;
    NSScreen *screen = nsw != nil ? [nsw screen] : nil;
    if (screen == nil)
        screen = [NSScreen mainScreen];
    if (screen != nil) {
        NSNumber *num = [[screen deviceDescription] objectForKey:@"NSScreenNumber"];
        if (num != nil)
            return (CGDirectDisplayID) [num unsignedIntValue];
    }
    return CGMainDisplayID();
}

/** Query the carrying display's true physical dimensions in millimeters. */
fn Window_getPhysicalSizeMM(const Window *window, f32 *outWidthMM, f32 *outHeightMM) {
    CGSize mm = CGDisplayScreenSize(windowDisplayId(window));
    if (outWidthMM)
        *outWidthMM = (f32) mm.width;
    if (outHeightMM)
        *outHeightMM = (f32) mm.height;
}

/**
 * Resolves the carrying display's true hardware panel resolution in physical
 * pixels by scanning CGDisplayCopyAllDisplayModes for kDisplayModeNativeFlag
 * (0x02000000), falling back to the current display mode's pixel dimensions.
 * @param did       CoreGraphics display ID
 * @param outWidth  receives native panel pixel width
 * @param outHeight receives native panel pixel height
 */
static fn windowQueryNativePanelPixels(CGDirectDisplayID did, bytes *outWidth, bytes *outHeight) {
    bytes nativeW = 0u;
    bytes nativeH = 0u;
    CFArrayRef modes = CGDisplayCopyAllDisplayModes(did, nullptr);
    if (modes != nullptr) {
        CFIndex count = CFArrayGetCount(modes);
        for (CFIndex i = 0; i < count; i++) {
            CGDisplayModeRef m = (CGDisplayModeRef) CFArrayGetValueAtIndex(modes, i);
            if (m == nullptr)
                continue;
            u32 ioFlags = (u32) CGDisplayModeGetIOFlags(m);
            if ((ioFlags & 0x02000000u) != 0u) {
                bytes pw = CGDisplayModeGetPixelWidth(m);
                bytes ph = CGDisplayModeGetPixelHeight(m);
                if (pw > nativeW) {
                    nativeW = pw;
                    nativeH = ph;
                }
            }
        }
        CFRelease(modes);
    }
    if (nativeW == 0u || nativeH == 0u) {
        CGDisplayModeRef cur = CGDisplayCopyDisplayMode(did);
        if (cur != nullptr) {
            nativeW = CGDisplayModeGetPixelWidth(cur);
            nativeH = CGDisplayModeGetPixelHeight(cur);
            CGDisplayModeRelease(cur);
        }
    }
    if (outWidth)
        *outWidth = nativeW;
    if (outHeight)
        *outHeight = nativeH;
}

/**
 * Returns the carrying display's true hardware panel resolution in physical
 * pixels (independent of macOS More Space / Larger Text virtual scaling).
 * @param window    target window (nullptr queries the main display)
 * @param outWidth  receives native panel width in physical pixels (optional)
 * @param outHeight receives native panel height in physical pixels (optional)
 */
fn Window_getNativePixels(const Window *window, i32 *outWidth, i32 *outHeight) {
    @autoreleasepool {
        bytes w = 0u;
        bytes h = 0u;
        windowQueryNativePanelPixels(windowDisplayId(window), &w, &h);
        if (outWidth)
            *outWidth = (i32) w;
        if (outHeight)
            *outHeight = (i32) h;
    }
}

/**
 * Computes backing pixels per true physical monitor pixel:
 *   currentModeBackingWidth / nativePanelPixelWidth.
 * When multiplied by a physical pixel dimension (such as 1280x720) and passed
 * to Window_setSize, AppKit points become desiredPhysPx * (modeLogicalW / nativeW),
 * keeping the window and all widgets at the exact same physical monitor size
 * across macOS Larger Text, Default, and More Space presets.
 * @param window target window (nullptr queries the main display)
 * @return backing-to-physical pixel ratio (1.0 when native equals backing)
 */
f32 Window_getPhysicalScale(const Window *window) {
    @autoreleasepool {
        CGDirectDisplayID did = windowDisplayId(window);
        bytes nativeW = 0u;
        bytes nativeH = 0u;
        windowQueryNativePanelPixels(did, &nativeW, &nativeH);

        CGDisplayModeRef cur = CGDisplayCopyDisplayMode(did);
        bytes backingW = cur ? CGDisplayModeGetPixelWidth(cur) : 0u;
        if (cur)
            CGDisplayModeRelease(cur);

        if (nativeW > 0u && backingW > 0u) {
            f32 ratio = (f32) ((d64) backingW / (d64) nativeW);
            if (ratio < 0.25f)
                ratio = 0.25f;
            if (ratio > 4.0f)
                ratio = 4.0f;
            return ratio;
        }
        return 1.0f;
    }
}

/** Reconcile scale, physical size, screen origin, and monitor after display changes. */
fn Window_revalidate(Window *window) {
    NSWindow *nsw = window != nullptr ? Window_nsWindow(window) : nil;
    if (nsw == nil)
        return;
    @autoreleasepool {
        CGFloat curScale = windowBackingScale(window);
        d64 prevScale = Window_cachedScale(window);
        if (prevScale <= 0.0)
            prevScale = curScale;

        i32 targetPxW = atomic_load_explicit(Window_cachedWidthMut(window), memory_order_relaxed);
        i32 targetPxH = atomic_load_explicit(Window_cachedHeightMut(window), memory_order_relaxed);
        if (curScale != prevScale && targetPxW > 0 && targetPxH > 0) {
            CGFloat newPtW = (CGFloat) targetPxW / curScale;
            CGFloat newPtH = (CGFloat) targetPxH / curScale;
            [nsw setContentSize:NSMakeSize(newPtW, newPtH)];
        }

        Window_setCachedScale(window, (d64) curScale);
        windowRefreshSize(window);
        windowRefreshOrigin(window);
        windowRefreshMonitor(window);
    }
}

/** Set content dimensions directly in AppKit points and refresh pixel geometry. */
fn Window_setSizePoints(Window *window, f32 width, f32 height) {
    if (window == nullptr || width <= 0.0f || height <= 0.0f)
        return;
    @autoreleasepool {
        [Window_nsWindow(window) setContentSize:NSMakeSize((CGFloat) width, (CGFloat) height)];
        windowRefreshSize(window);
    }
}

/** Read content dimensions in points through optional output parameters. */
fn Window_getSizePoints(const Window *window, f32 *outWidth, f32 *outHeight) {
    NSWindow *nsw = window != nullptr ? Window_nsWindow(window) : nil;
    if (nsw == nil) {
        if (outWidth)
            *outWidth = 0.0f;
        if (outHeight)
            *outHeight = 0.0f;
        return;
    }
    @autoreleasepool {
        NSView *cv = [nsw contentView];
        NSRect content = cv != nil ? [cv bounds] : [nsw contentRectForFrameRect:[nsw frame]];
        if (outWidth)
            *outWidth = (f32) content.size.width;
        if (outHeight)
            *outHeight = (f32) content.size.height;
    }
}

/** Return the content width in points through the shared size getter. */
f32 Window_widthPoints(const Window *window) {
    f32 w = 0.0f;
    Window_getSizePoints(window, &w, nullptr);
    return w;
}

/** Return the content height in points through the shared size getter. */
f32 Window_heightPoints(const Window *window) {
    f32 h = 0.0f;
    Window_getSizePoints(window, nullptr, &h);
    return h;
}

/** Alias the physical content width as the viewport width. */
i32 Window_viewportWidth(const Window *window) {
    return Window_width((Window*) window);
}

/** Alias the physical content height as the viewport height. */
i32 Window_viewportHeight(const Window *window) {
    return Window_height((Window*) window);
}

/** Alias the logical content width as the viewport width in points. */
f32 Window_viewportWidthPoints(const Window *window) {
    return Window_widthPoints(window);
}

/** Alias the logical content height as the viewport height in points. */
f32 Window_viewportHeightPoints(const Window *window) {
    return Window_heightPoints(window);
}

/** Return the outer NSWindow frame width in native pixels. */
i32 Window_windowWidth(const Window *window) {
    NSWindow *nsw = window != nullptr ? Window_nsWindow(window) : nil;
    if (nsw == nil)
        return 0;
    @autoreleasepool {
        NSRect frame = [nsw frame];
        CGFloat scale = windowBackingScale(window);
        return (i32) lround(frame.size.width * scale);
    }
}

/** Return the outer NSWindow frame height in native pixels. */
i32 Window_windowHeight(const Window *window) {
    NSWindow *nsw = window != nullptr ? Window_nsWindow(window) : nil;
    if (nsw == nil)
        return 0;
    @autoreleasepool {
        NSRect frame = [nsw frame];
        CGFloat scale = windowBackingScale(window);
        return (i32) lround(frame.size.height * scale);
    }
}

/** Return the outer NSWindow frame width in AppKit points. */
f32 Window_windowWidthPoints(const Window *window) {
    NSWindow *nsw = window != nullptr ? Window_nsWindow(window) : nil;
    if (nsw == nil)
        return 0.0f;
    @autoreleasepool {
        return (f32) [nsw frame].size.width;
    }
}

/** Return the outer NSWindow frame height in AppKit points. */
f32 Window_windowHeightPoints(const Window *window) {
    NSWindow *nsw = window != nullptr ? Window_nsWindow(window) : nil;
    if (nsw == nil)
        return 0.0f;
    @autoreleasepool {
        return (f32) [nsw frame].size.height;
    }
}

/** Place the window by its top-left screen coordinate in the public pixel space. */
fn Window_setLocation(Window *window, i32 x, i32 y) {
    if (window == nullptr)
        return;
    @autoreleasepool {
        NSRect screen = [[NSScreen mainScreen] frame];
        [Window_nsWindow(window) setFrameTopLeftPoint:NSMakePoint((CGFloat) x, screen.size.height - (CGFloat) y)];
    }
}

/** Return the cached top-left screen origin through optional output pointers. */
fn Window_getLocation(const Window *window, i32 *outX, i32 *outY) {
    if (outX)
        *outX = window ? (i32) Window_cachedX(window) : 0;
    if (outY)
        *outY = window ? (i32) Window_cachedY(window) : 0;
}

/** Return the cached content-view screen origin through optional outputs. */
fn Window_getContentOrigin(const Window *window, i32 *outX, i32 *outY) {
    if (outX)
        *outX = window ? (i32) Window_cachedContentX(window) : 0;
    if (outY)
        *outY = window ? (i32) Window_cachedContentY(window) : 0;
}

/** Center the window in the main display's usable visible frame. */
fn Window_center(Window *window) {
    if (window == nullptr)
        return;
    @autoreleasepool {
        NSWindow *nsw = Window_nsWindow(window);
        NSRect avail = [[NSScreen mainScreen] visibleFrame];
        NSRect wf = [nsw frame];
        CGFloat x = avail.origin.x + (avail.size.width - wf.size.width) * 0.5;
        CGFloat y = avail.origin.y + (avail.size.height - wf.size.height) * 0.5;
        [nsw setFrameOrigin:NSMakePoint((CGFloat) floor(x + 0.5), (CGFloat) floor(y + 0.5))];
    }
}

/**
 * Claim key synchronously: activation is async, so spin the runloop briefly
 * until [NSApp isActive] commits, then open animated + take key while the
 * grant is fresh. Falls back to the pump's sPendingKeyWindow re-assertion if
 * activation is denied outright.
 */
static fn claimKeyAfterActivation(NSWindow *nsw) {
    if (nsw == nil)
        return;
    if (@available(macOS 14.0, *))
        [NSApp activate];
    else
        [[NSRunningApplication currentApplication]
            activateWithOptions:NSApplicationActivateAllWindows];
    [nsw makeKeyAndOrderFront:nil];
    NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:0.5];
    while (![NSApp isActive] &&
           [[NSDate date] compare:deadline] == NSOrderedAscending) {
        NSEvent *e = [NSApp nextEventMatchingMask:NSEventMaskAny
                                        untilDate:[NSDate dateWithTimeIntervalSinceNow:0.02]
                                           inMode:NSDefaultRunLoopMode
                                          dequeue:YES];
        if (e)
            [NSApp sendEvent:e];
        [NSApp updateWindows];
    }
    if (![NSApp isActive])
        NSLog(@"window: activation denied, opening behind the active app; click the Dock icon to take focus");
    if (![nsw isKeyWindow] && [nsw canBecomeKeyWindow])
        [nsw makeKeyWindow];
    sPendingKeyWindow = nsw;
}

/** Activate and show the native window, then refresh its monitor mirror. */
fn Window_show(Window *window) {
    if (window == nullptr)
        return;
    @autoreleasepool {
        claimKeyAfterActivation(Window_nsWindow(window));
        windowRefreshMonitor(window);
    }
}

/** Remove the native window from display and cancel its pending key-window claim. */
fn Window_hide(Window *window) {
    if (window == nullptr)
        return;
    @autoreleasepool {
        NSWindow *nsw = Window_nsWindow(window);
        if (sPendingKeyWindow == nsw)
            sPendingKeyWindow = nil;
        [nsw orderOut:nil];
    }
}

/** Select the native show or hide operation from the requested visibility. */
fn Window_setVisible(Window *window, bool visible) {
    if (visible)
        Window_show(window);
    else
        Window_hide(window);
}

/** Query the AppKit style mask for the resizable capability. */
bool Window_isResizable(Window *window) {
    if (window == nullptr)
        return false;
    return hasStyleBit(window, NSWindowStyleMaskResizable);
}

/** Add or remove the resizable style bit unless AppKit owns fullscreen chrome. */
fn Window_setResizable(Window *window, bool resizable) {
    if (window == nullptr)
        return;
    updateStyleMask(window, resizable ? NSWindowStyleMaskResizable : 0, resizable ? 0 : NSWindowStyleMaskResizable);
}

/** Query the AppKit style mask for the close-button capability. */
bool Window_isClosable(Window *window) {
    if (window == nullptr)
        return false;
    return hasStyleBit(window, NSWindowStyleMaskClosable);
}

/** Add or remove the native close-button style bit. */
fn Window_setClosable(Window *window, bool closable) {
    if (window == nullptr)
        return;
    updateStyleMask(window, closable ? NSWindowStyleMaskClosable : 0, closable ? 0 : NSWindowStyleMaskClosable);
}

/** Query the AppKit style mask for the minimize-button capability. */
bool Window_isMiniaturizable(Window *window) {
    if (window == nullptr)
        return false;
    return hasStyleBit(window, NSWindowStyleMaskMiniaturizable);
}

/** Add or remove the native minimize-button style bit. */
fn Window_setMiniaturizable(Window *window, bool miniaturizable) {
    if (window == nullptr)
        return;
    updateStyleMask(window, miniaturizable ? NSWindowStyleMaskMiniaturizable : 0, miniaturizable ? 0 : NSWindowStyleMaskMiniaturizable);
}

/** Enable or disable the native fullscreen collection behavior for the window. */
fn Window_setFullscreenButton(Window *window, bool enabled) {
    if (window == nullptr)
        return;
    @autoreleasepool {
        NSWindow *nsw = Window_nsWindow(window);
        NSWindowCollectionBehavior behavior = [nsw collectionBehavior];
        if (enabled)
            behavior |= NSWindowCollectionBehaviorFullScreenPrimary;
        else
            behavior &= ~NSWindowCollectionBehaviorFullScreenPrimary;
        [nsw setCollectionBehavior:behavior];
    }
}

/** Apply the requested native chrome mode and refresh traffic-light geometry. */
fn Window_setUndecorated(Window *window, i32 mode) {
    if (window == nullptr)
        return;
    @autoreleasepool {
        NSWindowStyleMask mask = styleMaskOf(window);
        if ((mask & NSWindowStyleMaskFullScreen) != 0)
            return;

        Window_setUndecoratedMode(window, mode);
        Window_setViewportFlushToTop(window, false);

        NSWindowStyleMask next;
        if (mode == WINDOW_UNDECORATED_BORDERLESS)
            next = 0;
        else if (mode == WINDOW_UNDECORATED_NAKED)
            next = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                 | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                 | NSWindowStyleMaskFullSizeContentView;
        else
            next = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                 | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;

        NSWindow *nsw = Window_nsWindow(window);
        [nsw setStyleMask:next];

        [nsw setCollectionBehavior:([nsw collectionBehavior] | NSWindowCollectionBehaviorFullScreenPrimary)];

        bool transparent = (mode == WINDOW_UNDECORATED_NAKED);
        [nsw setTitlebarAppearsTransparent:transparent];
        [nsw setTitleVisibility:(transparent ? NSWindowTitleHidden : NSWindowTitleVisible)];

        TrafficLight_resetBase(Window_trafficLights(window));
        refreshChrome(window);
        windowRefreshSize(window);
        windowRefreshOrigin(window);
    }
}

/** Translate the boolean decoration preference into the native chrome mode setter. */
fn Window_setDecorated(Window *window, bool decorated) {
    Window_setUndecorated(window, decorated ? WINDOW_DECORATED : WINDOW_UNDECORATED_BORDERLESS);
}

/** Report whether the stored mode selects standard decorated chrome. */
bool Window_isDecorated(const Window *window) {
    if (window == nullptr)
        return false;
    return Window_undecoratedMode(window) == WINDOW_DECORATED;
}

/** Translate the naked-titlebar preference into the native chrome mode setter. */
fn Window_setNaked(Window *window, bool naked) {
    Window_setUndecorated(window, naked ? WINDOW_UNDECORATED_NAKED : WINDOW_DECORATED);
}

/** Report whether the stored mode selects full-size-content naked chrome. */
bool Window_isNaked(const Window *window) {
    if (window == nullptr)
        return false;
    return Window_undecoratedMode(window) == WINDOW_UNDECORATED_NAKED;
}

/** Translate the borderless preference into the native chrome mode setter. */
fn Window_setBorderless(Window *window, bool borderless) {
    Window_setUndecorated(window, borderless ? WINDOW_UNDECORATED_BORDERLESS : WINDOW_DECORATED);
}

/** Report whether the stored mode selects borderless chrome. */
bool Window_isBorderless(const Window *window) {
    if (window == nullptr)
        return false;
    return Window_undecoratedMode(window) == WINDOW_UNDECORATED_BORDERLESS;
}

/** Extend or restore decorated content into the titlebar and refresh geometry. */
fn Window_setViewportFlushToTop(Window *window, bool flush) {
    if (window == nullptr)
        return;
    if (Window_undecoratedMode(window) != WINDOW_DECORATED)
        return;
    if (Window_viewportFlushToTop(window) == flush)
        return;
    @autoreleasepool {
        NSWindowStyleMask mask = styleMaskOf(window);
        if ((mask & NSWindowStyleMaskFullScreen) != 0)
            return;

        Window_setViewportFlushToTopRaw(window, flush);

        NSWindow *nsw = Window_nsWindow(window);
        if (flush) {
            updateStyleMask(window, NSWindowStyleMaskFullSizeContentView, 0);
            [nsw setTitlebarAppearsTransparent:YES];
            [nsw setTitleVisibility:NSWindowTitleHidden];
        } else {
            updateStyleMask(window, 0, NSWindowStyleMaskFullSizeContentView);
            [nsw setTitlebarAppearsTransparent:NO];
            [nsw setTitleVisibility:NSWindowTitleVisible];
        }
        TrafficLight_resetBase(Window_trafficLights(window));
        refreshChrome(window);
        windowRefreshSize(window);
        windowRefreshOrigin(window);
    }
}

/** Read whether decorated chrome currently extends content into the titlebar. */
bool Window_isViewportFlushToTop(const Window *window) {
    if (window == nullptr)
        return false;
    if (Window_undecoratedMode(window) != WINDOW_DECORATED)
        return false;
    return Window_viewportFlushToTop(window);
}

/** Route f32ing traffic-light behavior through the titlebar-flush setting. */
fn Window_setFloatingTrafficLights(Window *window, bool f32ing) {
    Window_setViewportFlushToTop(window, f32ing);
}

/**
 * Map the public WindowTrafficLight enum onto the TrafficLight class's button
 * index (the values coincide: close=0, miniaturize=1, zoom=2). -1 = unknown.
 */
static i32 lightIndex(WindowTrafficLight light) {
    if (light == WINDOW_TRAFFIC_LIGHT_CLOSE)
        return TRAFFIC_LIGHT_CLOSE;
    if (light == WINDOW_TRAFFIC_LIGHT_MINIMIZE)
        return TRAFFIC_LIGHT_MINIATURIZE;
    if (light == WINDOW_TRAFFIC_LIGHT_ZOOM)
        return TRAFFIC_LIGHT_ZOOM;
    return -1;
}

/** Set visibility for a validated native traffic-light button enum. */
fn Window_macOS_setTrafficLightButtonVisible(Window *window, WindowTrafficLight light, bool visible) {
    if (window == nullptr)
        return;
    i32 i = lightIndex(light);
    if (i < 0)
        return;
    @autoreleasepool {
        TrafficLight_setButtonVisible(Window_trafficLights(window), (TrafficLightButton) i, visible);
    }
}

/** Query configured visibility for a valid native traffic-light button. */
bool Window_macOS_isTrafficLightButtonVisible(const Window *window, WindowTrafficLight light) {
    i32 i = lightIndex(light);
    if (window == nullptr || i < 0)
        return false;
    return TrafficLight_isButtonVisible(Window_trafficLights(window), (TrafficLightButton) i);
}

/** Set the traffic-light cluster's header-relative position in AppKit points. */
fn Window_macOS_setTrafficLightHeaderPosition(Window *window, f32 x, f32 y) {
    if (window == nullptr)
        return;
    @autoreleasepool {
        TrafficLight_setHeaderPosition(Window_trafficLights(window), x, y);
    }
}

/** Return the configured header-relative button-cluster position through outputs. */
fn Window_macOS_getTrafficLightHeaderPosition(const Window *window, f32 *outX, f32 *outY) {
    f32 x = 0.0f;
    f32 y = 0.0f;
    if (window != nullptr)
        TrafficLight_getHeaderPosition(Window_trafficLights(window), &x, &y);
    if (outX)
        *outX = x;
    if (outY)
        *outY = y;
}

/** Set the native NSWindow alpha value. */
fn Window_setOpacity(Window *window, f32 opacity) {
    if (window == nullptr)
        return;
    @autoreleasepool {
        [Window_nsWindow(window) setAlphaValue:(CGFloat) opacity];
    }
}

/** Set the native opaque/background policy and publish the transparency state. */
fn Window_setTransparentBackground(Window *window, bool transparent) {
    if (window == nullptr)
        return;
    @autoreleasepool {
        NSWindow *nsw = Window_nsWindow(window);
        [nsw setOpaque:!transparent];
        [nsw setBackgroundColor:(transparent ? [NSColor clearColor] : [NSColor windowBackgroundColor])];
        if ([nsw contentView].layer)
            [nsw contentView].layer.opaque = !transparent;
        Window_setTransparent(window, transparent);
    }
}

/**
 * Frosted backdrop: a behindWindow NSVisualEffectView under the draw view, so
 * the transparent parts of a present show the blurred desktop through. radius
 * is a hint (AppKit materials are fixed); 0 removes it.
 */
fn Window_setBackdropBlur(Window *window, f32 radius) {
    if (window == nullptr)
        return;
    @autoreleasepool {
        NSView *content = [Window_nsWindow(window) contentView];
        if (content == nil)
            return;
        any backdrop = Window_backdropView(window);
        if (radius > 0.0f) {
            if (backdrop == nullptr) {
                NSVisualEffectView *ve = [[NSVisualEffectView alloc] initWithFrame:content.bounds];
                ve.blendingMode = NSVisualEffectBlendingModeBehindWindow;
                ve.material = NSVisualEffectMaterialHUDWindow;
                ve.state = NSVisualEffectStateActive;
                ve.autoresizingMask = (NSViewWidthSizable | NSViewHeightSizable);
                [content addSubview:ve
                        positioned:NSWindowBelow
                        relativeTo:Window_glView(window)];
                Window_setBackdropView(window, (__bridge_retained any ) ve);
            }
        } else if (backdrop != nullptr) {
            NSView *ve = (__bridge NSView *)backdrop;
            [ve removeFromSuperview];
            CFRelease(backdrop);
            Window_setBackdropView(window, nullptr);
        }
    }
}

/**
 * Liquid Glass (macOS 26+): a real NSGlassEffectView behind the draw view. The
 * class is resolved by name at runtime so this compiles against any SDK, and
 * @available gates the OS; where it is unavailable the NSVisualEffectView
 * backdrop (Window_setBackdropBlur) is the effective fallback.
 */
bool Window_macOS_hasLiquidGlass() {
    if (@available(macOS 26.0, *))
        return NSClassFromString(@"NSGlassEffectView") != Nil;
    return false;
}

/** Apply or remove the runtime-available Liquid Glass view from its descriptor. */
fn Window_macOS_setLiquidGlass(Window *window, const WindowLiquidGlassDesc *desc) {
    if (window == nullptr)
        return;
    Window_setLiquidGlass(window, desc != nullptr ? *desc : (WindowLiquidGlassDesc){ 0 });
    WindowLiquidGlassDesc glass = Window_liquidGlass(window);
    @autoreleasepool {
        any glassView = Window_glassView(window);
        if (!glass.enabled || !Window_macOS_hasLiquidGlass()) {
            if (glassView != nullptr) {
                NSView *g = (__bridge NSView *)glassView;
                [g removeFromSuperview];
                CFRelease(glassView);
                Window_setGlassView(window, nullptr);
            }
            return;
        }
        NSView *content = [Window_nsWindow(window) contentView];
        if (content == nil)
            return;
        if (glassView == nullptr) {
            Class glassClass = NSClassFromString(@"NSGlassEffectView");
            NSView *g = [[glassClass alloc] initWithFrame:content.bounds];
            g.autoresizingMask = (NSViewWidthSizable | NSViewHeightSizable);
            [content addSubview:g positioned:NSWindowBelow relativeTo:Window_glView(window)];
            glassView = (__bridge_retained any ) g;
            Window_setGlassView(window, glassView);
        }
        NSView *g = (__bridge NSView *)glassView;
        WindowLiquidGlassDesc d = Window_liquidGlass(window);
        if ([g respondsToSelector:NSSelectorFromString(@"setStyle:")])
            [g setValue:@(d.style) forKey:@"style"];
        if ([g respondsToSelector:NSSelectorFromString(@"setCornerRadius:")])
            [g setValue:@(d.cornerRadius) forKey:@"cornerRadius"];
        if ([g respondsToSelector:NSSelectorFromString(@"setTintColor:")]) {
            CGFloat r  = (CGFloat) ((d.tintColor >> 24) & 0xFFu) / 255.0;
            CGFloat gg = (CGFloat) ((d.tintColor >> 16) & 0xFFu) / 255.0;
            CGFloat b  = (CGFloat) ((d.tintColor >> 8) & 0xFFu) / 255.0;
            CGFloat a  = (CGFloat) (d.tintColor & 0xFFu) / 255.0;
            [g setValue:[NSColor colorWithSRGBRed:r green:gg blue:b alpha:a] forKey:@"tintColor"];
        }
    }
}

/** Copy the currently stored Liquid Glass descriptor to the caller's output. */
bool Window_macOS_getLiquidGlass(const Window *window, WindowLiquidGlassDesc *out) {
    if (window == nullptr || out == nullptr)
        return false;
    *out = Window_liquidGlass(window);
    return true;
}

/** Select the AppKit f32ing or normal window level. */
fn Window_setAlwaysOnTop(Window *window, bool onTop) {
    if (window == nullptr)
        return;
    @autoreleasepool {
        [Window_nsWindow(window) setLevel:(onTop ? NSFloatingWindowLevel : NSNormalWindowLevel)];
    }
}

/** Publish caller-owned RGBA pixels as layer contents without copying the buffer. */
fn Window_presentRGBA(Window *window, const_any pixels, bytes stride, i32 width, i32 height) {
    if (window == nullptr || pixels == nullptr || width <= 0 || height <= 0)
        return;
    @autoreleasepool {
        NSView *glView = Window_glView(window);
        NSView *view = glView ? glView : [Window_nsWindow(window) contentView];
        if (view == nil)
            return;
        CGDataProviderRef provider =
            CGDataProviderCreateWithData(nullptr, pixels, stride * (bytes) height, nullptr);
        if (provider == nullptr)
            return;
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        CGImageRef img = CGImageCreate((bytes) width, (bytes) height, 8, 32, stride, cs,
                                       kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big,
                                       provider, nullptr, false, kCGRenderingIntentDefault);
        CGColorSpaceRelease(cs);
        CGDataProviderRelease(provider);
        if (img == nullptr)
            return;
        view.wantsLayer = YES;
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        view.layer.contents = (__bridge id) img;
        view.layer.contentsGravity = kCAGravityResize;
        [CATransaction commit];
        if (Window_isLiveResizing(window))
            [CATransaction flush];
        CGImageRelease(img);
    }
}

/**
 * ── Zero-copy present surface (IOSurface + CALayer) ─────────────────────────
 * The window's draw view already wantsLayer; we hand the layer an IOSurface as
 * its contents. The render repo draws straight into the IOSurface on the GPU,
 * so publishing is a pointer swap, not a pixel copy.
 */
any Window_createPresentSurface(Window *window, i32 widthPx, i32 heightPx) {
    if (window == nullptr || widthPx <= 0 || heightPx <= 0)
        return nullptr;
    @autoreleasepool {
        i32 rowBytes = ((widthPx * 4) + 63) & ~63;
        NSDictionary *props = @{
            (__bridge id) kIOSurfaceWidth: @(widthPx),
            (__bridge id) kIOSurfaceHeight: @(heightPx),
            (__bridge id) kIOSurfaceBytesPerElement: @(4),
            (__bridge id) kIOSurfaceBytesPerRow: @(rowBytes),
            (__bridge id) kIOSurfacePixelFormat: @(kCVPixelFormatType_32RGBA),
        };
        IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef) props);
        return (any) surface;
    }
}

/** Release the retained IOSurface reference supplied by the caller. */
fn Window_destroyPresentSurface(Window *window, any surface) {
    (discard) window;
    if (surface != nullptr)
        CFRelease((CFTypeRef) surface);
}

/** Publish the IOSurface as the view layer's contents at the current backing scale. */
fn Window_presentSurface(Window *window, any surface) {
    if (window == nullptr || surface == nullptr)
        return;
    @autoreleasepool {
        NSView *glView = Window_glView(window);
        NSView *view = glView ? glView : [Window_nsWindow(window) contentView];
        if (view == nil)
            return;
        view.wantsLayer = YES;
        CALayer *layer = view.layer;
        if (layer == nil)
            return;
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        layer.contents = (__bridge id)(IOSurfaceRef) surface;
        layer.contentsGravity = kCAGravityResize;
        d64 cachedScale = Window_cachedScale(window);
        d64 scale = cachedScale > 0.0 ? cachedScale : 1.0;
        layer.contentsScale = scale;
        [CATransaction commit];
        if (Window_isLiveResizing(window))
            [CATransaction flush];
    }
}

/** Return the current layer contents handle for the active draw view. */
any Window_presentSurfaceContents(const Window *window) {
    if (window == nullptr)
        return nullptr;
    NSView *glView = Window_glView(window);
    NSView *view = glView ? glView : [Window_nsWindow(window) contentView];
    if (view == nil || view.layer == nil)
        return nullptr;
    return (__bridge any ) view.layer.contents;
}

/** Copy IOSurface pixels into the caller's RGBA destination when dimensions permit. */
bool Window_readPresentSurface(Window *window, any surface, any destRGBA, bytes destStride) {
    (discard) window;
    if (surface == nullptr || destRGBA == nullptr || destStride == 0)
        return false;
    IOSurfaceRef s = (IOSurfaceRef) surface;
    if (IOSurfaceLock(s, kIOSurfaceLockReadOnly, nullptr) != kIOReturnSuccess)
        return false;
    const u8 *base = (const u8*) IOSurfaceGetBaseAddress(s);
    bytes srcStride = IOSurfaceGetBytesPerRow(s);
    u32 w = (u32) IOSurfaceGetWidth(s);
    u32 h = (u32) IOSurfaceGetHeight(s);
    for (u32 y = 0; y < h; y++)
        memcpy((u8*) destRGBA + (bytes) y * destStride,
               base + (bytes) y * srcStride, (bytes) w * 4u);
    IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, nullptr);
    return true;
}

/**
 * Write a tightly packed RGBA8 buffer to a PNG file. The screenshot/CAPTURE
 * path for tests and agents; ImageIO does the encoding.
 */
bool Window_writePNG(const_any pixels, bytes stride, i32 width, i32 height, string path) {
    if (pixels == nullptr || width <= 0 || height <= 0 || path == nullptr)
        return false;
    bool ok = false;
    @autoreleasepool {
        CGDataProviderRef provider =
            CGDataProviderCreateWithData(nullptr, pixels, stride * (bytes) height, nullptr);
        if (provider == nullptr)
            return false;
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        CGImageRef img = CGImageCreate((bytes) width, (bytes) height, 8, 32, stride, cs,
                                       kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big,
                                       provider, nullptr, false, kCGRenderingIntentDefault);
        CGColorSpaceRelease(cs);
        CGDataProviderRelease(provider);
        if (img == nullptr)
            return false;
        CFURLRef url = CFURLCreateFromFileSystemRepresentation(
            nullptr, (const UInt8*) path, (CFIndex) strlen(path), false);
        if (url != nullptr) {
            CGImageDestinationRef dest =
                CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, nullptr);
            if (dest != nullptr) {
                CGImageDestinationAddImage(dest, img, nullptr);
                ok = CGImageDestinationFinalize(dest);
                CFRelease(dest);
            }
            CFRelease(url);
        }
        CGImageRelease(img);
    }
    return ok;
}

/** Enable or clear the native window's pointer-ignoring behavior. */
fn Window_setClickThrough(Window *window, bool clickThrough) {
    if (window == nullptr)
        return;
    @autoreleasepool {
        [Window_nsWindow(window) setIgnoresMouseEvents:clickThrough];
    }
}

/** Request the native non-client shadow policy for decorated chrome. */
fn Window_setShadow(Window *window, bool shadow) {
    if (window == nullptr)
        return;
    @autoreleasepool {
        [Window_nsWindow(window) setHasShadow:shadow];
    }
}

/** Configure whether background dragging is delegated to the native window. */
fn Window_setMovableByBackground(Window *window, bool movable) {
    if (window == nullptr)
        return;
    @autoreleasepool {
        [Window_nsWindow(window) setMovableByWindowBackground:movable];
    }
}

/** Order the native window to the front of the AppKit window stack. */
fn Window_bringToFront(Window *window) {
    if (window == nullptr)
        return;
    @autoreleasepool {
        [Window_nsWindow(window) orderFrontRegardless];
    }
}

/** Attach a child window using AppKit's parent-child ordering relationship. */
fn Window_attachChild(Window *parent, Window *child) {
    (discard) parent;
    (discard) child;
}

/** Remove an existing child-window relationship from its parent. */
fn Window_detachChild(Window *parent, Window *child) {
    (discard) parent;
    (discard) child;
}

/** --- Minimize --- */

fn Window_minimize(Window *window) {
    if (window == nullptr)
        return;
    @autoreleasepool {
        [Window_nsWindow(window) miniaturize:nil];
    }
}

/** Restore a miniaturized window and let AppKit publish its new geometry. */
fn Window_restore(Window *window) {
    if (window == nullptr)
        return;
    @autoreleasepool {
        [Window_nsWindow(window) deminiaturize:nil];
    }
}

/** Read the native miniaturization state for a valid window. */
bool Window_isMinimized(Window *window) {
    if (window == nullptr)
        return false;
    if (atomic_load_explicit(Window_miniaturizingMut(window), memory_order_relaxed))
        return true;
    @autoreleasepool {
        return [Window_nsWindow(window) isMiniaturized];
    }
}

/** --- Fullscreen --- */

bool Window_isFullscreen(Window *window) {
    if (window == nullptr)
        return false;
    @autoreleasepool {
        return (styleMaskOf(window) & NSWindowStyleMaskFullScreen) != 0;
    }
}

/** Ask AppKit to enter or exit its native fullscreen space. */
fn Window_setFullscreen(Window *window, bool fullscreen) {
    if (window == nullptr)
        return;
    @autoreleasepool {
        if (fullscreen != Window_isFullscreen(window))
            [Window_nsWindow(window) toggleFullScreen:nil];
    }
}

/** Toggle native fullscreen based on the window's current AppKit state. */
fn Window_toggleFullscreen(Window *window) {
    if (window == nullptr)
        return;
    @autoreleasepool {
        [Window_nsWindow(window) toggleFullScreen:nil];
    }
}

/** --- DRM / sharing --- */

fn Window_setDRM(Window *window, bool enabled) {
    if (window == nullptr)
        return;
    @autoreleasepool {
        [Window_nsWindow(window) setSharingType:(enabled ? NSWindowSharingNone : NSWindowSharingReadOnly)];
    }
}

/** --- Size constraints --- */

fn Window_setMinSize(Window *window, i32 width, i32 height) {
    if (window == nullptr)
        return;
    @autoreleasepool {
        CGFloat scale = windowBackingScale(window);
        CGFloat ptW = width > 0 ? ((CGFloat) width / scale) : 0.0;
        CGFloat ptH = height > 0 ? ((CGFloat) height / scale) : 0.0;
        [Window_nsWindow(window) setContentMinSize:NSMakeSize(ptW, ptH)];
    }
}

/** Convert maximum content dimensions from physical pixels and set AppKit limits. */
fn Window_setMaxSize(Window *window, i32 width, i32 height) {
    if (window == nullptr)
        return;
    @autoreleasepool {
        CGFloat scale = windowBackingScale(window);
        CGFloat ptW = width > 0 ? ((CGFloat) width / scale) : (CGFloat) CGFLOAT_MAX;
        CGFloat ptH = height > 0 ? ((CGFloat) height / scale) : (CGFloat) CGFLOAT_MAX;
        [Window_nsWindow(window) setContentMaxSize:NSMakeSize(ptW, ptH)];
    }
}

/** --- Cursor control --- */

/**
 * FPS-style relative cursor: hides the pointer, decouples it from movement,
 * and re-warps to the window centre each pump pass while deltas flow into the
 * mouse stream as move-delta events.
 */
fn Window_setCursorLocked(Window *window, bool locked) {
    if (locked == s_cursorLocked)
        return;
    if (locked) {
        NSWindow *anchor = window ? Window_nsWindow(window) : sLastWindow;
        NSRect frame = anchor ? [anchor frame] : [[NSScreen mainScreen] frame];
        s_lockCenter = NSMakePoint(frame.origin.x + frame.size.width / 2.0,
                                   frame.origin.y + frame.size.height / 2.0);
        CGAssociateMouseAndMouseCursorPosition(NO);
        CGWarpMouseCursorPosition(s_lockCenter);
        CGDisplayHideCursor(kCGDirectMainDisplay);
        s_cursorLocked = true;
    } else {
        s_cursorLocked = false;
        CGDisplayShowCursor(kCGDirectMainDisplay);
        CGAssociateMouseAndMouseCursorPosition(YES);
    }
}

/** Store the cursor shape that the event path applies to the active window. */
fn Window_setCursorType(Window *window, WindowCursorType type) {
    if (window == nullptr)
        return;
    WindowCursorType prev = Window_cursorType(window);
    Window_setCursorTypeRaw(window, type);
    @autoreleasepool {
        if (prev == WINDOW_CURSOR_HIDDEN && type != WINDOW_CURSOR_HIDDEN)
            [NSCursor unhide];
        switch (type) {
            case WINDOW_CURSOR_IBEAM:
                [[NSCursor IBeamCursor] set];
                break;
            case WINDOW_CURSOR_POINTING_HAND:
                [[NSCursor pointingHandCursor] set];
                break;
            case WINDOW_CURSOR_CROSSHAIR:
                [[NSCursor crosshairCursor] set];
                break;
            case WINDOW_CURSOR_RESIZE_EW:
                [[NSCursor resizeLeftRightCursor] set];
                break;
            case WINDOW_CURSOR_RESIZE_NS:
                [[NSCursor resizeUpDownCursor] set];
                break;
            case WINDOW_CURSOR_NOT_ALLOWED:
                [[NSCursor operationNotAllowedCursor] set];
                break;
            case WINDOW_CURSOR_HIDDEN:
                [NSCursor hide];
                break;
            case WINDOW_CURSOR_DEFAULT:
            default:
                [[NSCursor arrowCursor] set];
                break;
        }
    }
}

/** Return the requested cursor shape, defaulting for a missing window. */
WindowCursorType Window_getCursorType(const Window *window) {
    if (window == nullptr)
        return WINDOW_CURSOR_DEFAULT;
    return Window_cursorType(window);
}

/** --- Software frame presentation (inert without a render target) ------------ */

/** ;;INTENTION("Window_present on the fresh window is inert (`return false`): a pure AppKit window has no raster target of its own. Software present was GPU-era shim logic; the render personal own pixel paths. Retires with the composite seam.") */
bool Window_present(Window *window, const Buffer *frame) {
    (discard) window;
    (discard) frame;
    return false;
}

/** --- Surface anchors (thread 0) ---------------------------------------------- */

any Window_contentView(Window *window) {
    NSWindow *nsw = window != nullptr ? Window_nsWindow(window) : nil;
    if (nsw == nil)
        return nullptr;
    return (__bridge any) [nsw contentView];
}

any Window_nativeHandle(const Window *window) {
    NSWindow *nsw = window != nullptr ? Window_nsWindow(window) : nil;
    if (nsw == nil)
        return nullptr;
    return (__bridge any) nsw;
}

/** ;;INTENTION("Window_metalLayer is gone from this file by design (no Metal here). Returns nullptr so the VK_EXT_metal_surface path degrades cleanly until the render personal create their own CAMetalLayer on the content view. Retires with the composite seam.") */
any Window_metalLayer(Window *window) {
    (discard) window;
    return nullptr;
}

/** ;;INTENTION("Gravity ownership moved out of the window (no layer here). No-op; retires with the composite seam.") */
fn Window_setGravityTopLeft(Window *window) {
    (discard) window;
}

/** ;;INTENTION("Worker present transaction was GPU-era (presentsWithTransaction drawables). No layer means nothing to commit; no-op. Retires with the composite seam.") */
fn Window_workerPresentBegin() {
}

/** Close the retired worker-present transaction hook; this backend owns none. */
fn Window_workerPresentEnd() {
}

/** --- Event wiring (rings are engine-global; adapters attach by window id) ---- */

fn Window_addKeyAdapter(Window *window, const KeyHandler *adapter) {
    if (window == nullptr)
        return;
    Key_attachWindow(Window_idRaw(window), adapter);
}

/** Detach a key callback from the window's engine input slot. */
bool Window_removeKeyAdapter(Window *window, const KeyHandler *adapter) {
    if (window == nullptr)
        return false;
    return Key_detachWindow(Window_idRaw(window), adapter);
}

/** Attach a mouse callback to the window's engine input slot. */
fn Window_addMouseAdapter(Window *window, const MouseHandler *adapter) {
    if (window == nullptr)
        return;
    Mouse_attachWindow(Window_idRaw(window), adapter);
}

/** Detach a mouse callback from the window's engine input slot. */
bool Window_removeMouseAdapter(Window *window, const MouseHandler *adapter) {
    if (window == nullptr)
        return false;
    return Mouse_detachWindow(Window_idRaw(window), adapter);
}

/** Attach a touch callback to the window's engine input slot. */
fn Window_addTouchAdapter(Window *window, const TouchHandler *adapter) {
    if (window == nullptr)
        return;
    Touch_attachWindow(Window_idRaw(window), adapter);
}

/** Detach a touch callback from the window's engine input slot. */
bool Window_removeTouchAdapter(Window *window, const TouchHandler *adapter) {
    if (window == nullptr)
        return false;
    return Touch_detachWindow(Window_idRaw(window), adapter);
}

/** Drain key, mouse, and touch callback queues for the engine input modules. */
fn Window_dispatchEvents(Window *window) {
    (discard) window;
    Key_dispatchEvents();
    Mouse_dispatchEvents();
    Touch_dispatchEvents();
}

/** --- Focus (the spotlight: one focused window per machine) ------------------- */

u32 Window_id(Window *window) {
    return window ? Window_idRaw(window) : FOCUS_BROADCAST;
}

/** Request AppKit activation and schedule the window for key-window promotion. */
fn Window_focus(Window *window) {
    if (window == nullptr)
        return;
    @autoreleasepool {
        claimKeyAfterActivation(Window_nsWindow(window));
        Focus_set(Window_idRaw(window));
    }
}

/** Compare this window's id with the current process-wide focus identity. */
bool Window_isFocused(Window *window) {
    return window && Focus_isFocused(Window_idRaw(window));
}

/** Enable or suppress AppKit key-window eligibility for this window. */
fn Window_setKeyEnabled(Window *window, bool enabled) {
    if (window == nullptr)
        return;
    atomic_store_explicit(Window_keyEnabledMut(window), enabled, memory_order_relaxed);
}

/** Read whether this window currently accepts key focus. */
bool Window_isKeyEnabled(const Window *window) {
    if (window == nullptr)
        return false;
    return atomic_load_explicit(Window_keyEnabledPtr(window), memory_order_relaxed);
}

/**
 * --- Lifecycle registry ------------------------------------------------------
 * The ONLY outside path to the embedded WindowEvent: fill slots with
 * WindowEvent_setOn*(Window_getLifecycle(w), fn). Null-safe.
 */

WindowEvent *Window_getLifecycle(Window *window) {
    if (window == nullptr)
        return nullptr;
    return Window_lifecycleMut(window);
}

/** --- Monitor identity --------------------------------------------------------- */

u32 Window_getMonitorId(const Window *window) {
    return window ? atomic_load_explicit(Window_monitorIdPtr(window), memory_order_acquire) : 0;
}

/** --- Resize reflection --------------------------------------------------------- */

u64 Window_sizeGeneration(Window *window) {
    if (window == nullptr)
        return 0;
    return atomic_load_explicit(Window_sizeGenerationPtr(window), memory_order_acquire);
}

/** Register the callback invoked synchronously as native resize geometry changes. */
fn Window_setResizeRenderHook(Window *window, WindowResizeRenderFn fn, any userdata) {
    if (window == nullptr)
        return;
    Window_setResizeRenderFnRaw(window, fn);
    Window_setResizeRenderUserdata(window, userdata);
}

/** Return the callback registered for synchronous resize rendering. */
WindowResizeRenderFn Window_getResizeRenderHook(const Window *window) {
    return window ? Window_resizeRenderFn(window) : nullptr;
}

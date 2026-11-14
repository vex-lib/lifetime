#ifndef WINDOW_WINDOW_H
#define WINDOW_WINDOW_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lang/annotation.h"

#include "../../vexspoke/event/keyhandler.h"
#include "../../vexspoke/event/mousehandler.h"
#include "../../vexspoke/event/touchhandler.h"

// window/window.h — platform-agnostic window API.
//
// The implementation is window_cocoa.m (the one ObjC file). C callers
// never see AppKit: they get an opaque handle, create/destroy it, and poll
// the OS event queue once per frame. This is the seam where a Win32 or X11
// backend could later drop in with zero changes above this header.
//
// The method surface mirrors the legacy macOSWindow (the FFM backend that
// lived in legacy-java): title/size/position, chrome capability toggles
// (resizable/closable/miniaturizable/traffic lights), fullscreen, minimize,
// undecorated (naked) chrome, DRM (sharing) mode, and size constraints.
//
// Three state families live on the handle:
//   POLICY    — present pacing + clear color + transparency. Written by
//               thread 0 any time; the GPU consumer polls them through
//               atomic words and watches renderGeneration for policy changes.
//               Zero allocation, zero locks.
//   CONTENT   — exactly ONE container slot. nullptr root => the renderer has
//               nothing to draw and degrades to a clear-only pass. All
//               nesting happens INSIDE that root via Panel_addContainer;
//               the window influences whatever hangs under it.
//   ADAPTERS  — key/mouse/touch/window listener vtables attached by
//               pointer identity, dispatched on thread 0 during
//               Window_dispatchEvents. A disabled window mutes its input.

// Opaque handle; contents live in the backend file. The tag stays INCOMPLETE
// here on purpose (exception to preferences rule 3): the backend translation
// unit completes struct Window with its real fields. The struct Window forward
// typedef and the per-window lifecycle registry (WindowEvent) are owned by
// window/window_event.h (the Single Class Per File Law).
#include "../window/window_event.h"

typedef struct Panel Panel;

// Undecorated chrome modes for Window_setUndecorated.
#define WINDOW_DECORATED  0 // standard opaque title bar, title visible
#define WINDOW_UNDECORATED_BORDERLESS 1 // no title bar and no traffic lights
#define WINDOW_UNDECORATED_NAKED      2 // transparent title bar, hidden title, traffic lights kept

// Legacy pacing policy names; the on-demand Metal drawable presenter owns
// scheduling. These constants do not select Vulkan presentation modes.
#define WINDOW_PRESENT_FIFO      0 // display-synced, capped (default)
#define WINDOW_PRESENT_IMMEDIATE 1 // uncapped, no sync

// Constructor parameters. Every field has a default; a call site names only
// what it wants to change. Zero it for pure defaults.
typedef struct WindowDesc {
    const char *title;   // default "vex"
    int width;           // physical display pixels, default 800 (Native Pixel Law)
    int height;          // physical display pixels, default 600 (Native Pixel Law)
    int x;               // top-left, default 0 (only honored when non-zero)
    int y;               // default 0 (only honored when non-zero)
    bool centered;       // default true — the window lands centered on the _main
                         // screen's visible frame; only a non-zero x/y defeats it
    bool shown;          // default false — construct hidden, show() when ready
} WindowDesc;

// --- Overloaded constructors (the Vec4 chooser idiom) ---
//
//   Window()                     -> defaults, hidden
//   Window("title")              -> titled, hidden
//   Window("title", 800, 600)    -> legacy create, hidden
//   Window_new(&(WindowDesc){…}) -> every other field (x/y/centered/shown)
//
// All variants construct HIDDEN: construct -> mutate -> Window_show().
// Placement defaults to centered on the _main screen's visible frame (like an
// application should be); pass .x/.y in WindowDesc for a custom placement.
// The macro is function-like, so it never fires when `Window` is used as the
// type name — only at call sites with parentheses.

Window *Window_0();
Window *Window_1(const char *title);
Window *Window_3(const char *title, int width, int height);

#define WINDOW_CHOOSER(_0, _1, _2, _3, NAME, ...) NAME

#define Window(...) WINDOW_CHOOSER( \
    dummy __VA_OPT__(,) __VA_ARGS__, \
    Window_3, Window_2, Window_1, Window_0 \
)(__VA_ARGS__)

// Parameterized constructor: Desc fields applied on top of defaults.
// Pass &(WindowDesc){ .title = "...", .centered = true } — unset fields keep
// their defaults. Returns nullptr on failure.
Window *Window_new(const WindowDesc *desc);

// Legacy-style convenience constructor: titled, sized, created hidden.
Window *Window_create(const char *title, int width, int height);

// Close the window and free the handle. Safe if already closed.
fn Window_destroy(Window *window);

// Emergency teardown: close and free all active windows across the process.
fn Window_destroyAll();

// True once the user has asked to close (red button / Cmd+W).
bool Window_shouldClose(Window *window);
fn Window_setShouldClose(Window *window, bool shouldClose);
// Owner-thread force-close of the native window; retains the C handle until
// Window_destroy. Unlike hide, this makes Window_shouldClose true.
fn Window_close(Window *window);

// Drain the OS event queue. Call once per frame from the engine loop.
fn Window_pollEvents();
// Interactive graphics pump: service at most one queued OS event, then let the
// caller dispatch input and present before consuming the next event. Returns
// true if more OS events are already queued (do not park between steps).
bool Window_pollEventStep();

// Block until an OS event arrives (or timeoutMs elapses), then dispatch it once.
// The park half of poll-then-park: pair with Window_pollEventStep — drain while
// events are queued, wait when they are not, so an idle window costs no CPU.
// timeoutMs <= 0 waits indefinitely. Thread 0 (the app thread) only.
fn Window_waitEvents(Window *window, int timeoutMs);

// --- Title / size / position (Native Pixel Law: width/height speak in native physical pixels) ---
fn Window_setTitle(Window *window, const char *title);
int  Window_width(Window *window);  // content width in native physical display pixels
int  Window_height(Window *window); // content height in native physical display pixels
fn Window_setSize(Window *window, int width, int height); // physical display pixels
fn Window_setLocation(Window *window, int x, int y);
// Top-left corner in global desktop points (the space setLocation speaks).
fn Window_getLocation(const Window *window, int *outX, int *outY);
// Top-left of the CONTENT area (below the title bar) in desktop points —
// the space darling layouts and GPU caches speak. This is what renderers
// should join against, not getLocation (the frame includes chrome).
fn Window_getContentOrigin(const Window *window, int *outX, int *outY);
fn Window_center(Window *window);
fn Window_show(Window *window);
fn Window_hide(Window *window);
fn Window_setVisible(Window *window, bool visible);

// --- Native pixel & DPI scale helpers (Native Pixel Law) ---
f32 Window_getScale(const Window *window); // active backing scale factor (e.g. 2.0 on Retina, 1.0 on standard)
fn  Window_revalidate(Window *window);     // recompute points geometry across screen DPI transitions
fn  Window_setSizePoints(Window *window, f32 width, f32 height);
fn  Window_getSizePoints(const Window *window, f32 *outWidth, f32 *outHeight);
f32 Window_widthPoints(const Window *window);
f32 Window_heightPoints(const Window *window);

// --- Viewport of content vs Window frame (Anchoring Law) --------------------
// The content viewport is the visible canvas for content that UI panels anchor against.
// The window size is the supplementary OS container.
int   Window_viewportWidth(const Window *window);
int   Window_viewportHeight(const Window *window);
f32 Window_viewportWidthPoints(const Window *window);
f32 Window_viewportHeightPoints(const Window *window);
int   Window_windowWidth(const Window *window);
int   Window_windowHeight(const Window *window);
f32 Window_windowWidthPoints(const Window *window);
f32 Window_windowHeightPoints(const Window *window);

// --- Content: owned by Frame, never by the Window ---------------------------
//
// A Window owns AppKit state and events, not Panels or render targets. Darling
// Frame owns the single Metal seam and two retained offscreen Vulkan Boards.

// --- Graphics boards: opaque platform layers owned by graphvex -------------
//
// Compatibility-only layer slots; no on-screen per-board layers are created.
// The real scene/content targets are offscreen and composite into the Frame seam.
fn   Window_setBottomLayer(Window *window, any layer);
any Window_getBottomLayer(const Window *window);
fn   Window_setTopLayer(Window *window, any layer);
any Window_getTopLayer(const Window *window);
// Re-assert stack order (blur back, bottom, top front). Thread 0 only;
// off-thread callers are bounced to the _main queue asynchronously.
fn   Window_orderLayers(Window *window);

// --- Board compositing seam (inert no-op, retained for the migrating
//     darling compositor) ----------------------------------------------------
//
// Retired pane calls remain inert until their archived callers are removed.

bool Window_attachPanes(Window *window, Panel *panel, int width, int height);
bool Window_resizePanes(Window *window, Panel *panel, int width, int height);
fn Window_compositePanes(Window *window, Panel *contentPanel);
fn Window_compositeBoards(Window *window);

// --- Present policy -----------------------------------------------------------
//
// Written by thread 0 whenever; consumed by the GPU thread through atomic
// loads. Policy changes bump Window_renderGeneration() for consumers.
//
// NOTE: there is deliberately NO background color here. Color is content —
// it lives on the Frame-owned board panels, and the
// renderer clears its monitor cache to that panel's color. Unset panel
// color (PANEL_COLOR_CLEAR) means transparent across the board.

fn     Window_setPresentMode(Window *window, int mode);
int      Window_getPresentMode(const Window *window);

// Window transparency policy; the renderer selects the matching Metal format.
fn Window_setTransparent(Window *window, bool transparent);
bool Window_isTransparent(const Window *window);

// Raster presentation: copy a tightly packed opaque RGBA8 buffer (byte order
// R,G,B,A; native px; top-left origin, Y-down) into the window's content view.
// `stride` is the row pitch in Bytes (>= width*4). Thread 0 only. The buffer is
// referenced, not copied, so it must stay valid until the next present.
fn Window_presentRGBA(Window *window, const_any pixels, bytes stride, int width, int height);

// ── Zero-copy present surface (Apple; capability-gated) ─────────────────────
// An IOSurface the window's draw view (a CALayer) displays directly. The render
// repository imports this handle as a GPU target (VK_EXT_metal_objects) and
// draws straight into it; Window_presentSurface publishes it to the layer. The
// host owns the handle — graphvex never frees it. Off-Apple returns NULL and the
// rest are no-ops (the Capability Gating Law).
//
// Transaction policy (the presentsWithTransaction semantics): during a live
// resize the publish commits synchronously inside the resize transaction so the
// frame tracks the drag; at rest it commits asynchronously and lets Core
// Animation composite.
any Window_createPresentSurface(Window *window, int widthPx, int heightPx);
fn  Window_destroyPresentSurface(Window *window, any surface);
fn  Window_presentSurface(Window *window, any surface);
any Window_presentSurfaceContents(const Window *window);   // what the layer shows

// Read a present surface's pixels into a tightly packed RGBA8 buffer (the
// screenshot/CAPTURE path on the GPU seam). `destStride` is Bytes/row. Thread 0.
bool Window_readPresentSurface(Window *window, any surface, any destRGBA, bytes destStride);

// Screenshot: write a tightly packed RGBA8 buffer (same layout as
// Window_presentRGBA) to a PNG file. The CAPTURE() path for tests/agents.
// Apple-only (ImageIO). Returns true on success.
bool Window_writePNG(const_any pixels, bytes stride, int width, int height, const char *path);

// Monotonic counter bumped by thread 0 whenever presentation policy changes.
u64 Window_renderGeneration(const Window *window);

// --- Runtime state ---

// Input-only kill switch: a disabled window delivers NO OS input — events
// never enter the device rings, adapters stay silent, polling freezes.
// Presentation and rendering are untouched.
fn Window_setEnabled(Window *window, bool enabled);
bool Window_isEnabled(const Window *window);

// Live-resize flag: set by thread 0 while AppKit is inside an active window
// drag (NSViewLiveResize). The renderer reads it to keep presenting the
// seam at the current window-content backing size on every geometry step.
bool Window_isLiveResizing(const Window *window);

// --- Chrome capability toggles (style-mask API) ---
bool Window_isResizable(Window *window);
fn Window_setResizable(Window *window, bool resizable);
bool Window_isClosable(Window *window);
fn Window_setClosable(Window *window, bool closable);
bool Window_isMiniaturizable(Window *window);
fn Window_setMiniaturizable(Window *window, bool miniaturizable);

// Green traffic light (fullscreen entry). Gated by NSWindowCollectionBehavior
// FullScreenPrimary, set at creation. Call before the window shows to remove it.
fn Window_setFullscreenButton(Window *window, bool enabled);

// Switch window chrome at runtime: one of WINDOW_UNDECORATED_*.
fn Window_setUndecorated(Window *window, int type);
fn Window_setDecorated(Window *window, bool decorated);
bool Window_isDecorated(const Window *window);
fn Window_setNaked(Window *window, bool naked);
bool Window_isNaked(const Window *window);
fn Window_setBorderless(Window *window, bool borderless);
bool Window_isBorderless(const Window *window);

// Viewport flush-to-top chrome control:
// Flushes the content viewport to the top of the window frame under the top bar (and traffic lights).
// ONLY applies when the window is in decorated mode (WINDOW_DECORATED). If the window is
// currently naked (WINDOW_UNDECORATED_NAKED) or borderless/undecorated (WINDOW_UNDECORATED_BORDERLESS),
// this function is disabled (no-op).
fn Window_setViewportFlushToTop(Window *window, bool flush);
bool Window_isViewportFlushToTop(const Window *window);
fn Window_setFloatingTrafficLights(Window *window, bool f32ing); // Transparent titlebar, leaves traffic lights over content (alias for Window_setViewportFlushToTop)

// macOS-only traffic-light chrome (the Window_macOS_ infix IS the platform
// lock: these symbols exist only in the Cocoa backend; a non-Apple build that
// calls them fails to link, which is the flag). The lights live only in Titled
// chrome; under NAKED (FullSizeContentView) they f32 over content at the
// native top-left inset. Per-button visibility hides e.g. the yellows (keeping
// only red) or drops all three while still NAKED; the header position re-seats
// the cluster for a custom header — (x, y) is the desired top-left origin of
// the red button in content-view points from the content top-left. All public
// AppKit (standardWindowButton: + setHidden:/setFrameOrigin:), no private API.
typedef enum WindowTrafficLight {
    WINDOW_TRAFFIC_LIGHT_CLOSE = 0,    // red
    WINDOW_TRAFFIC_LIGHT_MINIMIZE = 1, // yellow
    WINDOW_TRAFFIC_LIGHT_ZOOM = 2      // green
} WindowTrafficLight;
#define WINDOW_TRAFFIC_LIGHT_RED WINDOW_TRAFFIC_LIGHT_CLOSE
#define WINDOW_TRAFFIC_LIGHT_YELLOW WINDOW_TRAFFIC_LIGHT_MINIMIZE
#define WINDOW_TRAFFIC_LIGHT_GREEN WINDOW_TRAFFIC_LIGHT_ZOOM
;;PLATFORM_EXCLUSIVE("macOS")
fn Window_macOS_setTrafficLightButtonVisible(Window *window, WindowTrafficLight light, bool visible);
;;PLATFORM_EXCLUSIVE("macOS")
bool Window_macOS_isTrafficLightButtonVisible(const Window *window, WindowTrafficLight light);
;;PLATFORM_EXCLUSIVE("macOS")
fn Window_macOS_setTrafficLightHeaderPosition(Window *window, f32 x, f32 y);
;;PLATFORM_EXCLUSIVE("macOS")
fn Window_macOS_getTrafficLightHeaderPosition(const Window *window, f32 *outX, f32 *outY);

// macOS-exclusive Liquid Glass chrome (macOS 26+). The Window_macOS_ infix IS
// the platform lock: these symbols exist only in the Cocoa backend, and Liquid
// Glass has no cross-platform equivalent, so it is never a generic
// Window_setTsomething. Where the OS/SDK lacks it, Window_setBackdropBlur is
// the fallback (the Capability Gating Law). Probe before relying on it.
#define WINDOW_LIQUID_GLASS_STYLE_REGULAR 0
#define WINDOW_LIQUID_GLASS_STYLE_CLEAR   1

typedef struct WindowLiquidGlassDesc {
    bool enabled;        // false = remove the glass (restore the plain backdrop)
    int style;           // WINDOW_LIQUID_GLASS_STYLE_*
    f32 cornerRadius;  // native px
    u32 tintColor;  // RGBA8; 0 = no tint
} WindowLiquidGlassDesc;

;;PLATFORM_EXCLUSIVE("macOS")
bool Window_macOS_hasLiquidGlass();
;;PLATFORM_EXCLUSIVE("macOS")
fn Window_macOS_setLiquidGlass(Window *window, const WindowLiquidGlassDesc *desc);
;;PLATFORM_EXCLUSIVE("macOS")
bool Window_macOS_getLiquidGlass(const Window *window, WindowLiquidGlassDesc *out);

fn Window_setOpacity(Window *window, f32 opacity); // 0.0 to 1.0
fn Window_setTransparentBackground(Window *window, bool transparent); // Makes the window backdrop fully clear so Vulkan can draw holes
fn Window_setBackdropBlur(Window *window, f32 radius); // frosted backdrop behind the draw view; 0 = none
fn Window_setAlwaysOnTop(Window *window, bool onTop);
fn Window_setClickThrough(Window *window, bool clickThrough);
fn Window_setShadow(Window *window, bool shadow);
fn Window_setMovableByBackground(Window *window, bool movable);

// --- Z-Order & Presentation ---
fn Window_bringToFront(Window *window); // Pulls window to the top of its level without stealing keyboard focus
fn Window_attachChild(Window *parent, Window *child); // Glue child above parent: they order, move, and hide as one unit (dialog modality stacking)
fn Window_detachChild(Window *parent, Window *child); // Unglue a child attached above (detach before hide/destroy)


// --- Minimize ---
fn Window_minimize(Window *window);
fn Window_restore(Window *window);
bool Window_isMinimized(Window *window);

// --- Fullscreen ---
bool Window_isFullscreen(Window *window);
fn Window_setFullscreen(Window *window, bool fullscreen);
fn Window_toggleFullscreen(Window *window);

// --- DRM / sharing ---
fn Window_setDRM(Window *window, bool enabled);

// --- Size constraints ---
fn Window_setMinSize(Window *window, int width, int height);
fn Window_setMaxSize(Window *window, int width, int height);

// --- Cursor control ---
typedef enum WindowCursorType {
    WINDOW_CURSOR_DEFAULT       = 0,
    WINDOW_CURSOR_IBEAM         = 1,
    WINDOW_CURSOR_POINTING_HAND = 2,
    WINDOW_CURSOR_CROSSHAIR     = 3,
    WINDOW_CURSOR_RESIZE_EW     = 4,
    WINDOW_CURSOR_RESIZE_NS     = 5,
    WINDOW_CURSOR_NOT_ALLOWED   = 6,
    WINDOW_CURSOR_HIDDEN        = 7,
} WindowCursorType;

fn Window_setCursorType(Window *window, WindowCursorType type);
WindowCursorType Window_getCursorType(const Window *window);

// FPS-style relative cursor: hides the pointer, decouples it from movement,
// and re-warps to the window centre each pump pass while deltas flow into the
// input/mouse stream as move-delta events. (Legacy: macOSWindow.setCursorLock.)
fn Window_setCursorLocked(Window *window, bool locked);

// The AppKit content view behind this window — the surface anchor GPU backends
// need (MoltenVK wraps it in a CAMetalLayer). THREAD CONTRACT: thread 0 only.
any Window_contentView(Window *window);

// Native OS window handle (NSWindow* on macOS, HWND on Win32, wl_surface* on Linux).
any Window_nativeHandle(const Window *window);

// Creates (or reuses) a CAMetalLayer on the content view — the VK_EXT_metal_surface
// path. Returns nullptr off-Apple. THREAD CONTRACT: thread 0 only.
any Window_metalLayer(Window *window);
fn Window_setGravityTopLeft(Window *window);

// Worker present transaction: explicit CoreAnimation commit per present
// walk (Vk_clearPresent; the pane-era VkPane_presentAll walk is retired).
// The present worker
// owns no runloop, so its implicit transaction never commits at idle and
// every presentsWithTransaction=YES drawable would stall behind it —
// Begin/End release YES-presents on worker cadence. Apple-only (impl in
// window/window_cocoa.m, like Window_metalLayer); call from the present
// worker only, guarded by VEX_PLATFORM_APPLE at the call site.
fn Window_workerPresentBegin();
fn Window_workerPresentEnd();

// --- Software frame presentation ---
//
// Stamp an RGBA raster (ColorBuffer layout) into the window's content view,
// scaled to fit. THREAD CONTRACT: call from thread 0 only — this touches
// AppKit, and AppKit owns its _main thread like a landlord.
typedef struct Buffer Buffer;
bool Window_present(Window *window, const Buffer *frame);

// --- Event wiring (adapters) ---
//
// The window is the registration surface for the event contracts: implement
// a KeyHandler/MouseHandler/TouchHandler vtable (with .self = your object) and
// attach it here. Every queued event carries the id of the window the OS
// delivered it to, so an attached adapter only hears events for ITS window
// (broadcast-tagged synthetic events reach every window). Removal is by
// pointer identity. Destroying the window detaches its listeners.
//
// The OS lifecycle contract (resize/move/fullscreen/minimize/restore/press/
// focus/quit/zoom/occlusion) is the WindowEvent class in window/window_event.h — ONE embedded
// per window, fired by the pump pass on Thread 0. It supersedes the former
// WindowEvent adapter list and Window_addWindowAdapter (retired; the old
// adapter struct was macOS-only and lived in the retired Cocoa shim).
// onResized(w,h) and onMoved(x,y) fire EVERY geometry step (per live-resize
// tracking tick), not on settle — the Continuous Real-Time Live Resize Law.
//
// Fill the slots through the single accessor below:
//   WindowEvent_setOnResized(Window_getLifecycle(w), myOnResized);
//   WindowEvent_setOnMoved(Window_getLifecycle(w), myOnMoved);
// Null-safe: nullptr when window is nullptr. This accessor is the ONLY
// outside path to the embedded registry.
fn Window_addKeyAdapter(Window *window, const KeyHandler *adapter);
bool Window_removeKeyAdapter(Window *window, const KeyHandler *adapter);
fn Window_addMouseAdapter(Window *window, const MouseHandler *adapter);
bool Window_removeMouseAdapter(Window *window, const MouseHandler *adapter);
fn Window_addTouchAdapter(Window *window, const TouchHandler *adapter);
bool Window_removeTouchAdapter(Window *window, const TouchHandler *adapter);
WindowEvent *Window_getLifecycle(Window *window);

// The running: drain all three device rings into the registered adapters.
// Call ONCE per frame from the game loop, after Window_pollEvents(). If you
// never call it, polling (Key_isDown/Mouse_x) still works but queued events
// pile up and drop once the rings fill.
fn Window_dispatchEvents(Window *window);

// --- Focus (the spotlight: one focused window per machine) ---
//
// The OS owns focus; we mirror it. After every pump pass the key window's id
// lands in one atomic word any thread can read.
u32 Window_id(Window *window);      // 0 when window is nullptr
fn Window_focus(Window *window);       // ask the OS to make this key
bool Window_isFocused(Window *window);   // is THIS the spotlight right now?
fn Window_setKeyEnabled(Window *window, bool enabled); // false = the OS refuses this window key (modal dialogs holding their parent); render + order unaffected
bool Window_isKeyEnabled(const Window *window);          // false while held by a modal dialog (defaults true)

// --- Monitor identity ---
//
// The OS owns display topology; we mirror it. Every pump pass resolves which
// screen carries the window (greatest intersection) and lands its
// CGDirectDisplayID in one atomic word any thread can read. Primed eagerly
// by Window_show; 0 means unknown (never shown, or headless). When the value
// flips — drag across displays, screen unplugged — WindowEvent's
// onMonitorChanged fires on thread 0.
u32 Window_getMonitorId(const Window *window);

// --- Resize reflection ---
//
// Monotonic counter bumped by Thread 0 whenever Window_pollEvents observes
// the content size actually changed. Renderers, layouts, and scenes compare
// their last-seen generation against this and rebuild when it moved — one
// frame of lag at worst, zero AppKit calls off Thread 0, no locks.
u64 Window_sizeGeneration(Window *window);

// --- Resize-cadence rendering (the c -> objc -> c bridge) --------------------
//
// AppKit resizes windows inside its own nested tracking loop; a decoupled
// present thread always trails that cadence by up to a frame. One hook, two
// bridges, both ON THREAD 0:
//   - windowWillResize/windowDidResize (tracking ticks) route through the
//     SYNC path: they wait out any in-flight frame, then render+present
//     inline, so every border step carries an exactly-sized frame.
//   - the pump pass keeps opportunistic catch-up for move-drags and
//     programmatic changes.
// Hook implementations may still drop internally under contention; the
// regular loop catches up one tick later either way.
typedef fn (*WindowResizeRenderFn)(any userdata);
fn Window_setResizeRenderHook(Window *window, WindowResizeRenderFn fn, any userdata);
// Currently installed resize render hook, or nullptr when none (null = the
// window's event handlers own the geometry renders; non-null = the hook is
// the single renderer per geometry event).
WindowResizeRenderFn Window_getResizeRenderHook(const Window *window);

#endif

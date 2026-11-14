
#if defined(VEX_PLATFORM_WINDOWS)
// window/window_win32.c — the lean Win32 backend for the Window API (draft).
//
// A 1:1 mirror of the AppKit backend (window/window_cocoa.m): the same
// exported C surface, the same state families (POLICY / CONTENT / ADAPTERS),
// the same reflection words, the same WindowEvent lifecycle bridge, the same
// id registry. The only differences are the OS handle that anchors each
// window (HWND instead of NSWindow) and the window-manager dialect that
// serves it (Win32 messages instead of AppKit notifications).
//
// This is a DRAFT translation unit: it is not wired into CMake and has not
// been compiled against a real Win32 SDK. It mirrors the Cocoa backend
// function-for-function so that wiring it in later (WIN32 -> window_win32.c,
// replacing window/window.c) is a build-script edit, not an API migration.
//
// Like the Cocoa backend it is deliberately LEAN: a window with bridges, and
// nothing else. Intrinsic Win32 does the presentation plumbing — the WndProc
// is a pump-fed callback, not a message loop owner; WM_SIZE/WM_MOVE/WM_CLOSE/
// WM_SETFOCUS mirror the OS state into C-visible words exactly the way the
// AppKit delegate notifications do; WM_* input messages route into the
// vexspoke Key/Mouse/Touch rings. There is NO Vulkan, NO D3D, NO swapchain
// and NO present worker here — rendering is the render personal' job and reaches
// the screen through the software present seam (a lean StretchDIBits path
// mirroring the retired raster present) or, once Migrates, the render personal'
// own surfaces. A Window is a dumb surface + callback bridge per the Window
// Decoupling Law.
//
// The GPU-era composite surface (boards, panes, worker present) is retained
// as thin INERT stubs at the bottom of this file exactly as the Cocoa backend
// keeps them, so the still-unmigrated darling compositor links. They retire
// together with their window.h declarations.


// Lean and mean: strip the winsock/DDE/RPC/Shell bloat out of <windows.h>
// before it is pulled in. The lean backend needs USER/GDI only, plus the
// opt-in <dwmapi.h> for the blur seam. If a future render path in this file
// ever needs Winsock, it includes <winsock2.h> explicitly — never by undefining
// the lean flag.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "vex.h"
#include <windows.h>
#include <dwmapi.h>
#include <wingdi.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include "window/window.h"
#include "input/focus.h"
#include "input/key.h"
#include "input/mouse.h"
#include "input/touch.h"
#include "buffer/buffer.h"

;;DEFINITION
/**
 * ============================================================================
 * DEFINITION: Window_win32
 * ============================================================================
 * Win32 desktop window abstraction implementation mirroring Cocoa capabilities.
 * Manages native HWND lifecycle, WndProc message dispatch, and User32 display
 * geometry, feeding OS input directly into vexspoke device rings and driving the
 * embedded WindowEvent callback registry.
 *
 * Conforms to the Window Decoupling Law by treating the window as a dumb display
 * surface and callback seam, supporting software frame presentation via
 * StretchDIBits alongside inert compatibility stubs.
 * ============================================================================
 */

;;OVERVIEW
/**
 * ============================================================================
 * CLASS: Window (window/window_win32.c)
 * ============================================================================
 * SUMMARY:
 *   DRAFT Win32 mirror of the AppKit window backend. One opaque C handle per
 *   HWND; the engine loop constructs it, configures the chrome, shows it, then
 *   pumps Window_pollEvents once per frame while a render path draws through
 *   the software present seam (StretchDIBits) / event bridges. OS input is
 *   routed into the vexspoke device rings (tagged with this window's id); OS
 *   lifecycle (quit, resize, fullscreen, minimize, restore, press, focus) fires
 *   the embedded WindowEvent. Zero Vulkan, zero Metal, zero compositing — a
 *   Window is a dumb surface + callback bridge per the Window Decoupling Law.
 *
 * STRUCT FIELDS (Mirroring window/window.h incomplete tag — completed here):
 * ----------------------------------------------------------------------------
 *   HWND hwnd;                         // Win32 window (we own it)
 *   WindowEvent lifecycle;             // OS lifecycle registry
 *   u32 id;                       // engine window id (1..N, 0 = broadcast)
 *   _Atomic bool shouldClose;          // true once close requested
 *   _Atomic u64 sizeGeneration;   // resize-reflection counter
 *   _Atomic int cachedWidth;           // client width at last WM_SIZE
 *   _Atomic int cachedHeight;          // client height at last WM_SIZE
 *   _Atomic bool liveResizing;         // set during WM_ENTERSIZEMOVE..EXIT
 *   _Atomic int presentMode;           // present pacing (FIFO/IMMEDIATE), stored
 *   _Atomic bool transparent;          // composite transparency request, stored
 *   _Atomic u64 renderGeneration; // policy-reflection counter
 *   _Atomic(any) topLayer;           // content board handle
 *   _Atomic(any) bottomLayer;        // scene board handle
 *   _Atomic bool enabled;              // false mutes ALL OS input
 *   bool lastFocused;                  // focus-flip detection
 *   _Atomic u32 monitorId;        // HMONITOR slot id (0 = unmapped)
 *   WindowCursorType cursorType;       // current cursor request
 *   bool decorated;                    // chrome mode (WS_OVERLAPPED vs WS_POPUP)
 *   bool fullscreen;                   // fullscreen state
 *   WINDOWPLACEMENT prevPlacement;     // restored placement for fullscreen toggle
 *   int minWidth, minHeight;           // WM_GETMINMAXINFO minimums (0 = unset)
 *   int maxWidth, maxHeight;           // WM_GETMINMAXINFO maximums (0 = unbounded)
 *   bool movableByBackground;          // WM_NCHITTEST HTCAPTION toggle
 *   bool fullscreenButton;             // WS_MAXIMIZEBOX gate
 *   bool resizableEnabled;             // WS_THICKFRAME gate
 *   bool closableEnabled;              // SC_CLOSE system-menu gate
 *   bool minimizeEnabled;              // WS_MINIMIZEBOX gate
 *   bool clickThrough;                 // WS_EX_TRANSPARENT toggle
 *   f32 opacity;                     // SetLayeredWindowAttributes alpha
 *   bool shadow;                       // DWM non-client-rendering policy
 *   int frameW, frameH;                // retained bitmap dimensions in px
 *   BITMAPINFO bmi;                    // retained bitmap header for StretchDIBits
 *   WindowResizeRenderFn resizeRenderFn;   // resize-cadence render hook
 *   any resizeRenderUserdata;        // hook userdata
 *
 * PRIVATE HELPERS (kept file-local, no external API):
 * ----------------------------------------------------------------------------
 *   WindowSlot                         — id-registry slot record
 *     HWND hwnd;                       // the window carrying this id
 *     Window *handle;                  // C handle owning that window
 *   Win32Monitor                       — HMONITOR slot record
 *     HMONITOR handle;                 // monitor handle (may be nulled)
 *     u32 id;                     // stable engine id (index + 1)
 *   win32EnsureClass(), windowWndProc()
 *   windowRefreshSize, windowRefreshFocus, windowRefreshMonitor, recenterIfLocked
 *
 * FUNCTION REGISTRY:
 * ----------------------------------------------------------------------------
 * Public Constructors: (.h)
 *   - Window_0()                        : Window_new(nullptr)
 *   - Window_1(title)                       : Window_new(&{ .title })
 *   - Window_3(title, width, height)
 *   - Window_new(desc)                      : descResolve + windowAlloc + show
 *   - Window_create(title, width, height)
 *
 * Private Constructors: (.c static)
 *   - windowAlloc(desc)                     : shared constructor implementation
 *   - descResolve(desc)                     : default parameter resolution
 *
 * Public Core Functions: (.h)
 *   - Window_destroy(window)                : detach, close, free handle
 *   - Window_destroyAll()
 *   - Window_shouldClose(window)
 *   - Window_pollEvents()               : drain the message queue once per frame
 *   - Window_width(window)
 *   - Window_height(window)
 *   - Window_center(window)
 *   - Window_show(window)
 *   - Window_hide(window)
 *   - Window_attachPanes(window, panel, width, height) : inert (;;INTENTION)
 *   - Window_resizePanes(window, panel, width, height) : inert (;;INTENTION)
 *   - Window_compositePanes(window, contentPanel)      : inert (;;INTENTION)
 *   - Window_compositeBoards(window)        : inert (;;INTENTION)
 *   - Window_orderLayers(window)            : inert (;;INTENTION)
 *   - Window_renderGeneration(window)
 *   - Window_bringToFront(window)
 *   - Window_minimize(window)
 *   - Window_restore(window)
 *   - Window_toggleFullscreen(window)
 *   - Window_contentView(window)            : returns the HWND (the surface anchor)
 *   - Window_nativeHandle(window)
 *   - Window_metalLayer(window)             : nullptr — no Metal here (;;INTENTION)
 *   - Window_present(window, frame)         : lean StretchDIBits software path
 *   - Window_workerPresentBegin(window)     : inert (;;INTENTION)
 *   - Window_workerPresentEnd(window)       : inert (;;INTENTION)
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
 *
 * Private Core Functions: (.c static)
 *   - windowWndProc(...)                    : OS message dispatches to device rings + WindowEvent
 *
 * Public Setters: (.h)
 *   - Window_setShouldClose(window, shouldClose)
 *   - Window_setTitle(window, title)
 *   - Window_setSize(window, width, height)
 *   - Window_setLocation(window, x, y)      : SetWindowPos (top-left px)
 *   - Window_setVisible(window, visible)
 *   - Window_setTopLayer(window, layer)
 *   - Window_setBottomLayer(window, layer)
 *   - Window_setPresentMode(window, mode)
 *   - Window_setTransparent(window, transparent)   : WS_EX_LAYERED
 *   - Window_setEnabled(window, enabled)
 *   - Window_setKeyEnabled(window, enabled)
 *   - Window_setResizable(window, resizable)       : WS style bits
 *   - Window_setClosable(window, closable)
 *   - Window_setMiniaturizable(window, miniaturizable)
 *   - Window_setFullscreenButton(window, enabled)   : WS_MAXIMIZEBOX gate
 *   - Window_setUndecorated(window, mode)          : WS_POPUP / WS_OVERLAPPEDWINDOW
 *   - Window_setDecorated(window, decorated)
 *   - Window_setNaked(window, naked)
 *   - Window_setBorderless(window, borderless)
 *   - Window_setViewportFlushToTop(window, flush)       : macOS-only no-op
 *   - Window_setFloatingTrafficLights(window, f32ing) : macOS-only no-op
 *   - Window_macOS_setTrafficLightButtonVisible(window, light, visible)  : macOS-only stub
 *   - Window_macOS_setTrafficLightHeaderPosition(window, x, y)          : macOS-only stub
 *   - Window_setOpacity(window, opacity)          : SetLayeredWindowAttributes LWA_ALPHA
 *   - Window_setTransparentBackground(window, transparent) : WS_EX_LAYERED + clear-color note
 *   - Window_setAlwaysOnTop(window, onTop)        : HWND_TOPMOST / HWND_NOTOPMOST
 *   - Window_setClickThrough(window, clickThrough) : WS_EX_TRANSPARENT
 *   - Window_setShadow(window, shadow)            : DWM non-client-rendering policy
 *   - Window_setMovableByBackground(window, movable) : WM_NCHITTEST HTCAPTION
 *   - Window_setFullscreen(window, fullscreen)
 *   - Window_setDRM(window, enabled)              : no-op (;;INTENTION — macOS concept)
 *   - Window_setMinSize(window, width, height)    : WM_GETMINMAXINFO min track
 *   - Window_setMaxSize(window, width, height)    : WM_GETMINMAXINFO max track
 *   - Window_setCursorType(window, type)
 *   - Window_setCursorLocked(window, locked)      : ClipCursor + ShowCursor(FALSE) + warp
 *   - Window_setResizeRenderHook(window, fn, userdata)
 *   - Window_setGravityTopLeft(window)            : no-op (;;INTENTION)
 *
 * Private Setters: (.c static)
 *   - (none)
 *
 * Public Getters: (.h)
 *   - Window_getLocation(window, outX, outY)
 *   - Window_getContentOrigin(window, outX, outY)
 *   - Window_getTopLayer(window)
 *   - Window_getBottomLayer(window)
 *   - Window_getPresentMode(window)
 *   - Window_isTransparent(window)
 *   - Window_renderGeneration(window)
 *   - Window_isEnabled(window)
 *   - Window_isKeyEnabled(window)
 *   - Window_isLiveResizing(window)
 *   - Window_isResizable(window)
 *   - Window_isClosable(window)
 *   - Window_isMiniaturizable(window)
 *   - Window_isDecorated(window)
 *   - Window_isNaked(window)
 *   - Window_isBorderless(window)
 *   - Window_isViewportFlushToTop(window)             : macOS-only false
 *   - Window_macOS_isTrafficLightButtonVisible(window, light)  : macOS-only stub
 *   - Window_macOS_getTrafficLightHeaderPosition(window, outX, outY) : macOS-only stub
 *   - Window_isMinimized(window)
 *   - Window_isFullscreen(window)
 *   - Window_getCursorType(window)
 *   - Window_id(window)
 *   - Window_isFocused(window)
 *   - Window_getLifecycle(window)
 *   - Window_getMonitorId(window)
 *   - Window_sizeGeneration(window)
 *   - Window_getResizeRenderHook(window)
 *
 * Private Getters: (.c static)
 *   - (none)
 * ============================================================================
 */
;;PLATFORM_EXCLUSIVE("Windows")
;;DRAFT
;;INTENTION("Draft translation unit: wired platform-guarded (WIN32 selects "
         "window/window_win32.c; window.c stays reachable via "
         "-DHOTCWAP_VEX_PLATFORM_WINDOWS_LEGACY=ON), but never compiled against a real Win32 "
         "SDK — the macOS host excludes it. Mirrors the Cocoa backend 1:1; a "
         "Windows-host build is the promotion gate.")
;;INTENTION("GPU-era composite surface (attachPanes/resizePanes/compositePanes/"
         "compositeBoards/orderLayers/metalLayer/setGravityTopLeft/"
         "workerPresentBegin/workerPresentEnd) is retained as inert stubs so "
         "the still-unmigrated darling compositor keeps linking — exactly as "
         "the Cocoa backend keeps them. They retire together with their "
         "window.h declarations.")
;;INTENTION("Window_present is a genuinely lean Win32 path (DIB section + "
         "StretchDIBits on WM_PAINT) — Win32 has a real raster target (the "
         "HWND client area), unlike the Cocoa backend whose inert slice "
         "exists because a pure AppKit view has none. Same signature, same "
         "call contract: software frames land scaled-to-fit into the client.")
;;INTENTION("Per-Monitor-V2 DPI awareness is requested once at register time; "
         "cachedWidth/Height and WM_SIZE speak physical client px, matching "
         "the Native Pixel Law contract (the window reports what its client "
         "really is, so swapchains rebuild at native size).")
;;INTENTION("Window_setDRM has no Win32 analogue (NSWindowSharingNone is an "
         "AppKit concept); stored-and-inert so the surface stays stable, "
         "mirroring the macOS 'no sharing' semantics as closely as the OS "
         "allows. Set member when the guest compositor arrives.")

// Multi-tap window for d64-click style counting (legacy parity: 250ms).
static const u64 kTapThresholdNanos = 250000000ULL;

// Virtual keycode -> KEY_* code. Indexed by the raw Windows virtual-key code
// (0..255); -1 = unmapped. Physical layout maps letters/digits/function rows
// 1:1 like the Cocoa Carbon table, and punctuation rides the OEM VK_ codes.
static int winVkMap[256] = {
   // ASCII-printable codes carried by VK_ values < 0x60 already equal their
   // KEY_ value for ' '..'z' collateral — letters and digits are set below.
   [0x08] = KEY_BACKSPACE,   [0x09] = KEY_TAB,        [0x0D] = KEY_ENTER,
   [0x1B] = KEY_ESCAPE,      [0x20] = KEY_SPACE,
   [0x23] = KEY_END,         [0x24] = KEY_HOME,       [0x25] = KEY_LEFT,
   [0x26] = KEY_UP,          [0x27] = KEY_RIGHT,      [0x28] = KEY_DOWN,
   [0x2C] = KEY_PRINT_SCREEN,[0x2D] = KEY_INSERT,     [0x2E] = KEY_DELETE,
   [0x21] = KEY_PAGE_UP,     [0x22] = KEY_PAGE_DOWN,  [0x13] = KEY_PAUSE,
   [0x14] = KEY_CAPS_LOCK,   [0x5B] = KEY_LEFT_SUPER, [0x5C] = KEY_RIGHT_SUPER,
   [0x5D] = KEY_MENU,
   // Function rows: VK_F1..VK_F24 = 0x70..0x87.
   [0x70] = KEY_F1,  [0x71] = KEY_F2,  [0x72] = KEY_F3,  [0x73] = KEY_F4,
   [0x74] = KEY_F5,  [0x75] = KEY_F6,  [0x76] = KEY_F7,  [0x77] = KEY_F8,
   [0x78] = KEY_F9,  [0x79] = KEY_F10, [0x7A] = KEY_F11, [0x7B] = KEY_F12,
   [0x7C] = KEY_F13, [0x7D] = KEY_F14, [0x7E] = KEY_F15, [0x7F] = KEY_F16,
   [0x80] = KEY_F17, [0x81] = KEY_F18, [0x82] = KEY_F19, [0x83] = KEY_F20,
   [0x84] = KEY_F21, [0x85] = KEY_F22, [0x86] = KEY_F23, [0x87] = KEY_F24,
   // Numpad pad digits (VK_NUMPAD0..9 = 0x60..0x69) ride the top-row digits.
   // Punctuation OEM codes (documented stable VK_ values):
   [0xBA] = KEY_SEMICOLON,   [0xBB] = KEY_EQUAL,      [0xBC] = KEY_COMMA,
   [0xBD] = KEY_MINUS,       [0xBE] = KEY_PERIOD,     [0xBF] = KEY_SLASH,
   [0xC0] = KEY_GRAVE_ACCENT,[0xDB] = KEY_LEFT_BRACKET,
   [0xDC] = KEY_BACKSLASH,   [0xDD] = KEY_RIGHT_BRACKET,
   [0xDE] = KEY_APOSTROPHE,
};

// Cursor-lock state. While locked the pointer is clipped to the window and
// re-warped to the anchor centre every pump pass; WM input deltas feed mouse.
static bool s_cursorLocked = false;
static POINT s_lockCenter = {0, 0};

// The opaque handle handed back to C. Holds the native window (HWND) plus
// every C-visible reflection word — the mirror of the Cocoa struct Window.
// `id` is the engine's small window number (1..N; 0 is the broadcast reserved
// id) used to tag input events and route them to per-window listeners.
// `lifecycle` is the embedded WindowEvent — the app's OS-lifecycle bridge.
struct Window {
   HWND hwnd;
   WindowEvent lifecycle;
   u32 id;
   _Atomic bool shouldClose;
   _Atomic u64 sizeGeneration;
   _Atomic int cachedWidth;
   _Atomic int cachedHeight;
   int cachedX;
   int cachedY;
   int cachedContentX;
   int cachedContentY;
   _Atomic bool liveResizing;
   _Atomic bool miniaturizing;

   // Pure-state reflection words (no GPU calls in this file).
   _Atomic int presentMode;
   _Atomic bool transparent;
   _Atomic u64 renderGeneration;

   // Board slots — opaque layer handles consumed by graphvex/darling; this
   // window stores them, never dereferences them. Panels live on the Frame.
   _Atomic(any) topLayer;
   _Atomic(any) bottomLayer;

   _Atomic bool enabled;
   _Atomic bool keyEnabled; // stored; OS-level key refusal is Cocoa-only for now
   bool lastFocused;
   _Atomic u32 monitorId;
   WindowCursorType cursorType;

   // Chrome: style-mask mirror (WIN32 has no traffic lights; no lightBase).
   bool decorated;
   bool fullscreen;
   RECT savedFrame;                 // normal (pre-fullscreen) outer frame
   int minWidth;
   int minHeight;
   int maxWidth;
   int maxHeight;
   bool movableByBackground;
   bool fullscreenButton;
   bool resizableEnabled;
   bool closableEnabled;
   bool minimizeEnabled;

   // Software present path (mirror of the retired raster present).
   unsigned char *framePixels;
   int frameW;
   int frameH;
   bool frameDirty;

   WindowResizeRenderFn resizeRenderFn;
   any resizeRenderUserdata;
};

// Window id registry: slot i holds the entries for engine id i (index = id,
// slot 0 reserved for FOCUS_BROADCAST). Grows by doubling (the Dynamic
// Scalability & Anti-Hardcoding Law — no fixed window ceiling).
typedef struct WindowSlot {
   HWND hwnd;
   Window *handle;
} WindowSlot;

static WindowSlot *s_refs = nullptr;
static u32 s_refCap = 0;

// Foreground-claim pending: set by Window_show/Window_focus, drained by the
// pump once the OS grants foreground (Win32 foreground-locking rules are the
// mirror of AppKit activation — a bounded re-assert, never a spin).
static HWND sPendingForeground = nullptr;
static u64 sPendingUntilMillis = 0;

// Grow the HWND-to-window registry geometrically and initialize the new slots.
static bool windowRefsGrow() {
   u32 nextCap = (s_refCap == 0) ? 8u : s_refCap * 2u;
   WindowSlot *grown = (WindowSlot*) realloc(s_refs, (bytes) nextCap * sizeof(WindowSlot));
   if (grown == nullptr)
      return false;
   for (u32 i = s_refCap; i < nextCap; i++) {
      grown[i].hwnd = nullptr;
      grown[i].handle = nullptr;
   }
   s_refs = grown;
   s_refCap = nextCap;
   return true;
}

// Resolve an id for a newly created window, or 0 when the table cannot grow.
static u32 windowIdAcquire(HWND hwnd, Window *handle) {
   if (s_refCap == 0 && !windowRefsGrow())
      return FOCUS_BROADCAST;
   for (u32 i = 1; i < s_refCap; i++) {
      if (s_refs[i].hwnd == nullptr) {
         s_refs[i].hwnd = hwnd;
         s_refs[i].handle = handle;
         return i;
      }
   }
   if (!windowRefsGrow())
      return FOCUS_BROADCAST;
   for (u32 i = 1; i < s_refCap; i++) {
      if (s_refs[i].hwnd == nullptr) {
         s_refs[i].hwnd = hwnd;
         s_refs[i].handle = handle;
         return i;
      }
   }
   return FOCUS_BROADCAST;
}

// Clear a registry slot after its native window and C handle are detached.
static fn windowIdRelease(u32 id) {
   if (id != FOCUS_BROADCAST && id < s_refCap) {
      s_refs[id].hwnd = nullptr;
      s_refs[id].handle = nullptr;
   }
}

// Reverse lookup: which engine id does this OS window carry? 0 if unknown.
static u32 windowIdOf(HWND hwnd) {
   if (hwnd == nullptr)
      return FOCUS_BROADCAST;
   for (u32 i = 1; i < s_refCap; i++)
      if (s_refs[i].hwnd == hwnd)
         return i;
   return FOCUS_BROADCAST;
}

static Window *windowHandleOf(HWND hwnd) {
   if (hwnd == nullptr)
      return nullptr;
   for (u32 i = 1; i < s_refCap; i++)
      if (s_refs[i].hwnd == hwnd)
         return s_refs[i].handle;
   return nullptr;
}

// WM_GETMINMAXINFO: clamp the OS to the stored content constraints. Thread 0
// (message pump) — the same contract as the Cocoa min/max reflection.
static fn windowApplyMinMax(Window *window, MINMAXINFO *mmi) {
   RECT frame = {0};
   if ((*window).minWidth > 0 || (*window).minHeight > 0)
      frame.right = (*window).minWidth;
   frame.bottom = (*window).minHeight;
   AdjustWindowRectEx(&frame, (DWORD) GetWindowLongPtr((*window).hwnd, GWL_STYLE), FALSE, 0);
   if ((*window).minWidth > 0)
      (*window).minWidth = frame.right - frame.left;
   if ((*window).minHeight > 0)
      (*window).minHeight = frame.bottom - frame.top;
   frame.left = frame.top = 0;
   frame.right = (*window).maxWidth;
   frame.bottom = (*window).maxHeight;
   // The tracking values are OUTER px: WM_GETMINMAXINFO wants the max frame.
   if ((*window).maxWidth > 0 || (*window).maxHeight > 0) {
      AdjustWindowRectEx(&frame, (DWORD) GetWindowLongPtr((*window).hwnd, GWL_STYLE), FALSE, 0);
      POINT *minTrack = &(*mmi).ptMinTrackSize;
      POINT *maxTrack = &(*mmi).ptMaxTrackSize;
      (*minTrack).x = ((*window).minWidth > 0) ?
         ((*window).minWidth + (frame.right - frame.left)) : 0;
      (*minTrack).y = ((*window).minHeight > 0) ?
         ((*window).minHeight + (frame.bottom - frame.top)) : 0;
      if ((*window).maxWidth > 0)
         (*maxTrack).x = (LONG)(*window).maxWidth
                     + (frame.right - frame.left);
      if ((*window).maxHeight > 0)
         (*maxTrack).y = (LONG)(*window).maxHeight
                     + (frame.bottom - frame.top);
   }
}

// Resize reflection: the message already carries the new client size (cx/cy);
// compare against the cache and bump sizegen + fire onResized + run the resize
// hook only on an actual change. Thread 0 only.
static fn windowRefreshSize(Window *window, int cw, int ch) {
   if (window == nullptr || (*window).hwnd == nullptr)
      return;
   if (cw != atomic_load_explicit(&(*window).cachedWidth, memory_order_relaxed)
      || ch != atomic_load_explicit(&(*window).cachedHeight, memory_order_relaxed)) {
      atomic_store_explicit(&(*window).cachedWidth, cw, memory_order_relaxed);
      atomic_store_explicit(&(*window).cachedHeight, ch, memory_order_relaxed);
      atomic_fetch_add_explicit(&(*window).sizeGeneration, 1, memory_order_release);
      WindowEvent_fireResized(&(*window).lifecycle, window, cw, ch);
      if ((*window).resizeRenderFn)
         (*window).resizeRenderFn((*window).resizeRenderUserdata);
   }
}

// Mirror the OS foreground window into the focus word AND the WindowEvent
// focus slots (flips only). Thread 0 only, called at the end of each pump pass.
static fn windowRefreshFocus(Window *window, HWND fgWindow) {
   if (window == nullptr || (*window).hwnd == nullptr)
      return;
   bool focused = (windowIdOf(fgWindow) == (*window).id);
   if (focused != (*window).lastFocused) {
      (*window).lastFocused = focused;
      if (focused)
         WindowEvent_fireFocusGained(&(*window).lifecycle, window);
      else
         WindowEvent_fireFocusLost(&(*window).lifecycle, window);
   }
}

// Mirror the display assignment into the atomic word; 0 means unmapped. The id
// is the EnumDisplayMonitors traversal index + 1 (stable per session, unlike
// the HMONITOR pointer which recesses between enumerations).
static u32 resolveMonitorId(HWND hwnd) {
   if (hwnd == nullptr)
      return 0;
   HMONITOR mine = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
   if (mine == nullptr)
      return 0;
   u32 idx = 0;
   // Count preceding monitors until EnumDisplayMonitors reaches the target.
   BOOL CALLBACK countCb(HMONITOR m, HDC dc, LPRECT r, LPARAM lp) {
      (fn) dc;
      (fn) r;
      u32 *out = (u32*) lp;
      if (m == mine)
         return FALSE;
      (*out)++;
      return TRUE;
   }
   EnumDisplayMonitors(nullptr, nullptr, countCb, (LPARAM) &idx);
   return idx + 1u;
}

// Refresh the monitor id mirror from the window's nearest display.
static fn windowRefreshMonitor(Window *window) {
   if (window == nullptr)
      return;
   atomic_store_explicit(&(*window).monitorId, resolveMonitorId((*window).hwnd), memory_order_relaxed);
}

// Re-centre the cursor during the pump while locked, so the warp registers
// before the next event loop exit.
static fn recenterIfLocked() {
   if (s_cursorLocked)
      SetCursorPos(s_lockCenter.x, s_lockCenter.y);
}

// Client-area coordinates (top-left origin) from an lParam — Win32 messages
// already carry client px, so no flip is needed (parity with the flipped
// Cocoa content view).
static fn mousePoint(LPARAM lp, d64 *outX, d64 *outY) {
   *outX = (d64) GET_X_LPARAM(lp);
   *outY = (d64) GET_Y_LPARAM(lp);
}

// Map a Win32 mouse message to the MOUSE_* button constant.
static int winButton(UINT msg) {
   if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP)
      return MOUSE_LEFT;
   if (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP)
      return MOUSE_RIGHT;
   if (msg == WM_MBUTTONDOWN || msg == WM_MBUTTONUP)
      return MOUSE_MIDDLE;
   return MOUSE_MIDDLE; // X-button drags ride MOUSE_MIDDLE for parity with Cocoa's "other".
}

// Classify whether a supported Win32 mouse-button message represents a press.
static bool winButtonDown(UINT msg) {
   return msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN
      || msg == WM_MBUTTONDOWN || msg == WM_XBUTTONDOWN;
}

// Modifier mirror: compare the Win32 modifier state against the engine's
// Key ring and emit diffs — the same synthetic down/up pattern the Cocoa
// backend uses for modifier keys. Thread 0 only.
static fn mirrorModifiers(Window *window, WPARAM state, UINT msg) {
   if (window == nullptr)
      return;
   u32 wid = (*window).id;
   bool ctrl = (state & MK_CONTROL) != 0;
   if (ctrl != (Key_isDown(KEY_LEFT_CONTROL) || Key_isDown(KEY_RIGHT_CONTROL)))
      Key_pushEvent(wid, KEY_LEFT_CONTROL, ctrl ? KEY_ACTION_DOWN : KEY_ACTION_UP, kTapThresholdNanos);
   bool shift = (state & MK_SHIFT) != 0;
   if (shift != (Key_isDown(KEY_LEFT_SHIFT) || Key_isDown(KEY_RIGHT_SHIFT)))
      Key_pushEvent(wid, KEY_LEFT_SHIFT, shift ? KEY_ACTION_DOWN : KEY_ACTION_UP, kTapThresholdNanos);
   // Alt/Win are NOT part of MK_*; read the real modifiers for the message.
   LONG_PTR l = (LONG_PTR) msg; // unused here — message modifiers are implicit;
   (fn) l;
}

// Touch route (lean WM_TOUCH): decode each touch into TOUCHINPUT, normalize
// into client px (top-left origin), and push. Identity hash % TOUCH_MAX picks
// the slot, exactly like the Cocoa NSTouch path.
static fn routeTouch(Window *window, WPARAM wp, LPARAM lp) {
   if (window == nullptr)
      return;
   UINT count = (UINT) LOWORD(wp);
   if (count == 0 || count > 64)
      return;
   TOUCHINPUT in[64];
   if (!GetTouchInputInfo((HTOUCHINPUT) lp, count, in, sizeof(TOUCHINPUT)))
      return;
   RECT client = {0};
   GetClientRect((*window).hwnd, &client);
   d64 winW = (d64) (client.right - client.left);
   d64 winH = (d64) (client.bottom - client.top);
   for (UINT i = 0; i < count; i++) {
      if ((in[i].dwFlags & TOUCHEVENTF_DOWN) == 0
         && (in[i].dwFlags & TOUCHEVENTF_UP) == 0
         && (in[i].dwFlags & TOUCHEVENTF_MOVE) == 0)
         continue;
      int action = TOUCH_MOVE;
      if ((in[i].dwFlags & TOUCHEVENTF_DOWN) != 0)
         action = TOUCH_DOWN;
      else if ((in[i].dwFlags & TOUCHEVENTF_UP) != 0)
         action = TOUCH_UP;
      int slot = (int) (in[i].dwID % TOUCH_MAX);
      d64 x = (d64) in[i].x / 100.0;
      d64 y = (d64) in[i].y / 100.0;
      Touch_pushTouchEvent((*window).id, slot, action, x, y, 0.8, kTapThresholdNanos);
   }
   CloseTouchInputHandle((HTOUCHINPUT) lp);
}

// The per-window message sink. Win32 has no delegate object: this is the one
// place OS events meet the C reflection words — the exact role of the Cocoa
// NSWindowDelegate notifications + routeEvent.
static LRESULT CALLBACK windowWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
   Window *window = (Window*) GetWindowLongPtr(hwnd, GWLP_USERDATA);

   switch (msg) {
      case WM_NCCREATE: {
         CREATESTRUCTW *cs = (CREATESTRUCTW*) lp;
         Window *w = (Window*) (*cs).lpCreateParams;
         SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR) w);
         return TRUE;
      }

      case WM_CLOSE: {
         // Quit is the vetoable slot — the same contract as Cocoa's
         // windowShouldClose. A decline keeps the window alive.
         if (window && WindowEvent_fireQuitRequested(&(*window).lifecycle, window))
            DestroyWindow(hwnd);
         return 0;
      }

      case WM_DESTROY: {
         if (window)
            atomic_store_explicit(&(*window).shouldClose, true, memory_order_relaxed);
         return 0;
      }

      case WM_SIZE: {
         if (window) {
            int cw = (int) LOWORD(lp);
            int ch = (int) HIWORD(lp);
            if (wp == SIZE_MINIMIZED) {
               atomic_store_explicit(&(*window).miniaturizing, true, memory_order_relaxed);
               WindowEvent_fireMinimized(&(*window).lifecycle, window);
            } else {
               if (atomic_load_explicit(&(*window).miniaturizing, memory_order_relaxed)) {
                  atomic_store_explicit(&(*window).miniaturizing, false, memory_order_relaxed);
                  WindowEvent_fireRestored(&(*window).lifecycle, window);
               }
               windowRefreshSize(window, cw, ch);
            }
         }
         return 0;
      }

      case WM_MOVE: {
         if (window) {
            RECT rc = {0};
            GetWindowRect(hwnd, &rc);
            (*window).cachedX = (int) rc.left;
            (*window).cachedY = (int) rc.top;
            POINT origin = {0, 0};
            ClientToScreen(hwnd, &origin);
            (*window).cachedContentX = (int) origin.x;
            (*window).cachedContentY = (int) origin.y;
         }
         return 0;
      }

      case WM_ENTERSIZEMOVE:
         if (window)
            atomic_store_explicit(&(*window).liveResizing, true, memory_order_relaxed);
         return 0;

      case WM_EXITSIZEMOVE:
         if (window)
            atomic_store_explicit(&(*window).liveResizing, false, memory_order_relaxed);
         return 0;

      case WM_SETFOCUS:
         if (window) {
            windowRefreshFocus(window, GetForegroundWindow());
            windowRefreshMonitor(window);
         }
         return 0;

      case WM_KILLFOCUS:
         if (window)
            windowRefreshFocus(window, GetForegroundWindow());
         return 0;

      case WM_GETMINMAXINFO:
         if (window)
            windowApplyMinMax(window, (MINMAXINFO*) lp);
         return 0;

      // Movable-by-background: a client-area hit reports as HTCAPTION so the
      // window drags from anywhere while the flag is set (mirror of the Cocoa
      // setMovableByWindowBackground). Resize/menu hits still ride through.
      case WM_NCHITTEST: {
         if (window && (*window).movableByBackground) {
            LRESULT hit = DefWindowProc(hwnd, msg, wp, lp);
            if (hit == HTCLIENT)
               return HTCAPTION;
         }
         break;
      }

      case WM_MOUSEMOVE: {
         if (!window)
            break;
         u32 wid = (*window).id;
         if (atomic_load_explicit(&(*window).enabled, memory_order_relaxed))
            break;
         if (s_cursorLocked) {
            POINT now = {0};
            GetCursorPos(&now);
            Mouse_pushMoveDeltaEvent(wid,
               (d64) (now.x - s_lockCenter.x),
               (d64) (now.y - s_lockCenter.y));
            break;
         }
         d64 x, y;
         mousePoint(lp, &x, &y);
         Mouse_pushMoveEvent(wid, x, y);
         break;
      }

      case WM_LBUTTONDOWN:
      case WM_RBUTTONDOWN:
      case WM_MBUTTONDOWN:
      case WM_XBUTTONDOWN:
      case WM_LBUTTONUP:
      case WM_RBUTTONUP:
      case WM_MBUTTONUP:
      case WM_XBUTTONUP: {
         if (!window)
            break;
         u32 wid = (*window).id;
         d64 x, y;
         mousePoint(lp, &x, &y);
         Mouse_pushMoveEvent(wid, x, y);
         bool down = winButtonDown(msg);
         Mouse_pushButtonEvent(wid, winButton(msg),
                          down ? KEY_ACTION_DOWN : KEY_ACTION_UP,
                          kTapThresholdNanos);
         if (down)
            WindowEvent_firePressed(&(*window).lifecycle, window);
         break;
      }

      case WM_MOUSEWHEEL: {
         if (window) {
            d64 delta = (d64) GET_WHEEL_DELTA_WPARAM(wp) / (d64) WHEEL_DELTA;
            Mouse_pushScrollEvent((*window).id, 0.0, delta);
         }
         return 0;
      }

      case WM_MOUSEHWHEEL: {
         if (window) {
            d64 delta = (d64) GET_WHEEL_DELTA_WPARAM(wp) / (d64) WHEEL_DELTA;
            Mouse_pushScrollEvent((*window).id, delta, 0.0);
         }
         return 0;
      }

      case WM_KEYDOWN:
      case WM_KEYUP:
      case WM_SYSKEYDOWN:
      case WM_SYSKEYUP: {
         if (!window)
            break;
         u32 wid = (*window).id;
         UINT vk = (UINT) wp;
         bool down = (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN);
         // Modifier keys resolve left/right through the extended-key bit of
         // the lParam (bit 24), the same physical distinction AppKit folds
         // into its NSEvent modifier flags.
         bool ext = ((lp & (1L << 24)) != 0);
         int mod = -1;
         if (vk == VK_SHIFT)
            mod = ext ? KEY_RIGHT_SHIFT : KEY_LEFT_SHIFT;
         else if (vk == VK_CONTROL)
            mod = ext ? KEY_RIGHT_CONTROL : KEY_LEFT_CONTROL;
         else if (vk == VK_MENU)
            mod = ext ? KEY_RIGHT_ALT : KEY_LEFT_ALT;
         if (mod != -1) {
            Key_pushEvent(wid, mod, down ? KEY_ACTION_DOWN : KEY_ACTION_UP, kTapThresholdNanos);
            break;
         }
         int stdKey = (vk < 256) ? winVkMap[vk] : -1;
         if (vk >= 'A' && vk <= 'Z')
            stdKey = KEY_A + (int) (vk - 'A');
         else if (vk >= '0' && vk <= '9')
            stdKey = KEY_NUM_0 + (int) (vk - '0');
         if (stdKey != -1)
            Key_pushEvent(wid, stdKey, down ? KEY_ACTION_DOWN : KEY_ACTION_UP, kTapThresholdNanos);
         break;
      }

      case WM_CHAR: {
         if (window) {
            u32 c = (u32) wp;
            if (c > 0)
               Key_pushCharEvent((*window).id, c);
         }
         return 0;
      }

      case WM_TOUCH:
         if (window)
            routeTouch(window, wp, lp);
         return 0;

      case WM_PAINT: {
         if (window && (*window).frameDirty) {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            if (dc && (*window).framePixels && (*window).frameW > 0 && (*window).frameH > 0) {
               BITMAPINFO bi;
               memset(&bi, 0, sizeof(bi));
               BITMAPINFOHEADER *header = &(*bi).bmiHeader;
               (*header).biSize = sizeof(BITMAPINFOHEADER);
               (*header).biWidth = (*window).frameW;
               (*header).biHeight = -(*window).frameH; // top-down
               (*header).biPlanes = 1;
               (*header).biBitCount = 32;
               (*header).biCompression = BI_RGB;
               RECT client = {0};
               GetClientRect(hwnd, &client);
               int cx = client.right - client.left;
               int cy = client.bottom - client.top;
               int mode = (SetStretchBltMode(dc, HALFTONE) != 0) ? HALFTONE : COLORONCOLOR;
               StretchDIBits(dc, 0, 0, cx, cy, 0, 0,
                          (*window).frameW, (*window).frameH,
                          (*window).framePixels, &bi, DIB_RGB_COLORS, SRCCOPY);
               (fn) mode;
            }
            EndPaint(hwnd, &ps);
            (*window).frameDirty = false;
            return 0;
         }
         break;
      }

      default:
         break;
   }

   return DefWindowProc(hwnd, msg, wp, lp);
}

// Drain the OS message queue. Called every frame from the engine loop (the
// "poll" half of poll-then-tick). Returns immediately; never blocks. After
// the pump, mirror the OS's foreground window into the focus word so the rest
// of the engine can ask "who is focused?" without touching USER32.
static fn pumpReflection() {
   HWND fg = GetForegroundWindow();
   Focus_set(windowIdOf(fg));
   for (u32 i = 1; i < s_refCap; i++) {
      Window *handle = s_refs[i].handle;
      if (handle == nullptr)
         continue;
      HWND hwnd = s_refs[i].hwnd;
      windowRefreshFocus(handle, fg);
      windowRefreshMonitor(handle);
      if (hwnd && (*handle).frameDirty)
         InvalidateRect(hwnd, nullptr, FALSE);
   }
   if (sPendingForeground) {
      if (GetForegroundWindow() == sPendingForeground) {
         sPendingForeground = nullptr;
      } else if (GetTickCount64() < sPendingUntilMillis) {
         // Bounded re-assert (the Bounded Wait Law): the OS owns the
         // foreground grant; we ask once per pump while the claim is fresh.
         SetForegroundWindow(sPendingForeground);
      } else {
         sPendingForeground = nullptr;
      }
   }
   recenterIfLocked();
}

// Drain all queued Win32 messages without waiting, then refresh window mirrors.
fn Window_pollEvents() {
   MSG msg;
   // Non-blocking drain, exactly like Cocoa's
   // nextEventMatchingMask:untilDate:[NSDate distantPast]:dequeue:YES.
   while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
   }
   pumpReflection();
}

// Dispatch at most one queued message, refresh mirrors, and report more pending work.
bool Window_pollEventStep() {
   MSG msg;
   if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
   }
   pumpReflection();
   return PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE) != 0;
}

// Traffic-light indices (kept beside the constructors for symmetry with the
// Cocoa backend even though Win32 owns no lights).
enum { LIGHT_CLOSE = 0, LIGHT_MINI = 1, LIGHT_ZOOM = 2 };

// Convert an outer-frame size to the content size the style will produce —
// the mirror of frameRectForContentRect. Used so setters speak CONTENT px and
// the OS gets the frame that matches.
static fn frameForContent(Window *window, int cw, int ch, int *outX, int *outY) {
   RECT rc = {0, 0, cw, ch};
   DWORD style = (DWORD) GetWindowLongPtr((*window).hwnd, GWL_STYLE);
   DWORD ex = (DWORD) GetWindowLongPtr((*window).hwnd, GWL_EXSTYLE);
   AdjustWindowRectEx(&rc, style, FALSE, ex);
   *outX = (int) (rc.right - rc.left);
   *outY = (int) (rc.bottom - rc.top);
}

// Bounded foreground claim: show first, then ask for the foreground grant. If
// the OS denies (Win32 foreground locking), the pump's sPendingForeground
// re-assertion covers it for a bounded window — the mirror of the Cocoa
// activation spin + pending key-window fallback.
static fn winClaimForeground(HWND hwnd) {
   if (hwnd == nullptr)
      return;
   ShowWindow(hwnd, SW_SHOW);
   SetActiveWindow(hwnd);
   SetForegroundWindow(hwnd);
   sPendingForeground = hwnd;
   sPendingUntilMillis = GetTickCount64() + 500;
}

// Apply the WS style bits for the current chrome mode. Computes the frame
// delta so a style switch never resizes the CONTENT area.
static fn windowApplyChrome(Window *window, bool decorated) {
   if (window == nullptr || (*window).hwnd == nullptr)
      return;
   LONG_PTR style = GetWindowLongPtr((*window).hwnd, GWL_STYLE);
   LONG_PTR next;
   if (decorated) {
      next = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
   } else if ((*window).decorated == WINDOW_UNDECORATED_NAKED) {
      // Transparent titlebar with resize affordance kept (the closest Win32
      // dialect of the macOS NAKED mode's "lights over content").
      next = (style & ~(WS_CAPTION | WS_BORDER)) | WS_THICKFRAME | WS_POPUP;
   } else {
      next = WS_POPUP | WS_CLIPCHILDREN;
   }
   if ((*window).fullscreenButton)
      next |= WS_MAXIMIZEBOX;
   if ((*window).minimizeEnabled)
      next |= WS_MINIMIZEBOX;
   if ((*window).closableEnabled)
      next |= WS_SYSMENU;
   if ((*window).resizableEnabled)
      next |= WS_THICKFRAME;
   SetWindowLongPtr((*window).hwnd, GWL_STYLE, next);
   // Frame-changed: re-lay the window so the content keeps its size.
   RECT rc = {0};
   GetClientRect((*window).hwnd, &rc);
   int cw = rc.right - rc.left;
   int ch = rc.bottom - rc.top;
   frameForContent(window, cw, ch, &cw, &ch);
   int x, y, w, h;
   GetWindowRect((*window).hwnd, &rc);
   x = (int) rc.left;
   y = (int) rc.top;
   w = cw;
   h = ch;
   AdjustWindowRectEx(&rc, (DWORD) next, FALSE,
                  (DWORD) GetWindowLongPtr((*window).hwnd, GWL_EXSTYLE));
   SetWindowPos((*window).hwnd, nullptr, x, y,
             (int) (rc.right - rc.left), (int) (rc.bottom - rc.top),
             SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE);
}

// Build the HWND + C handle. Shared by every constructor. The window is
// created HIDDEN — visibility is an explicit Window_show() decision, so
// construct -> mutate -> show never flashes a half-configured window.
static Window *windowAlloc(const WindowDesc *desc) {
   HINSTANCE inst = (HINSTANCE) GetModuleHandleW(nullptr);

   // Register once per process (class name is stable; RegisterClassW is
   // idempotent — a re-register with the same name is simply ignored).
   static bool sRegistered = false;
   if (!sRegistered) {
      SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
      WNDCLASSW wc;
      memset(&wc, 0, sizeof(wc));
      wc.style = CS_HREDRAW | CS_VREDRAW;
      wc.lpfnWndProc = windowWndProc;
      wc.hInstance = inst;
      wc.hCursor = LoadCursorW(nullptr, (LPCWSTR) IDC_ARROW);
      wc.hIcon = LoadIconW(nullptr, (LPCWSTR) IDI_APPLICATION);
      wc.hbrBackground = nullptr;
      wc.lpszClassName = L"VexWindowClass";
      RegisterClassW(&wc);
      sRegistered = true;
   }

   int cw = (*desc).width;
   int ch = (*desc).height;
   int wid = cw;
   int hei = ch;
   // Compute the FRAME (outer) rect from the requested CONTENT size.
   RECT rc = {0, 0, cw, ch};
   AdjustWindowRectEx(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0);
   wid = (int) (rc.right - rc.left);
   hei = (int) (rc.bottom - rc.top);

   int x = CW_USEDEFAULT;
   int y = CW_USEDEFAULT;
   if ((*desc).centered) {
      HMONITOR hm = MonitorFromPoint((POINT){0, 0}, MONITOR_DEFAULTTOPRIMARY);
      MONITORINFO mi;
      mi.cbSize = sizeof(mi);
      GetMonitorInfoW(hm, &mi);
      RECT work = (*mi).rcWork;
      int wx = (int) (work.right - work.left - wid) / 2;
      int wy = (int) (work.bottom - work.top - hei) / 2;
      x = work.left + wx;
      y = work.top + wy;
   } else if ((*desc).x != 0 || (*desc).y != 0) {
      x = (*desc).x;
      y = (*desc).y;
   }

   Window *w = (Window*) calloc(1, sizeof(Window));
   if (w == nullptr)
      return nullptr;

   wchar_t titleBuf[256];
   const char *t = (*desc).title ? (*desc).title : "vex";
   bytes tn = strlen(t);
   if (tn >= 256)
      tn = 255;
   for (bytes i = 0; i < tn; i++)
      titleBuf[i] = (wchar_t) (unsigned char) t[i];
   titleBuf[tn] = 0;

   (*w).hwnd = CreateWindowExW(
      0, L"VexWindowClass", titleBuf,
      WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
      x, y, wid, hei,
      nullptr, nullptr, inst, w);
   if ((*w).hwnd == nullptr) {
      free(w);
      return nullptr;
   }

   // WM_NCCREATE already parked the handle into GWLP_USERDATA.
   WindowEvent_init(&(*w).lifecycle);
   atomic_store_explicit(&(*w).shouldClose, false, memory_order_relaxed);
   atomic_store_explicit(&(*w).sizeGeneration, 0, memory_order_relaxed);
   atomic_store_explicit(&(*w).cachedWidth, cw, memory_order_relaxed);
   atomic_store_explicit(&(*w).cachedHeight, ch, memory_order_relaxed);
   RECT fr = {0};
   GetWindowRect((*w).hwnd, &fr);
   (*w).cachedX = (int) fr.left;
   (*w).cachedY = (int) fr.top;
   POINT origin = {0, 0};
   ClientToScreen((*w).hwnd, &origin);
   (*w).cachedContentX = (int) origin.x;
   (*w).cachedContentY = (int) origin.y;
   atomic_store_explicit(&(*w).presentMode, WINDOW_PRESENT_FIFO, memory_order_relaxed);
   atomic_store_explicit(&(*w).transparent, false, memory_order_relaxed);
   atomic_store_explicit(&(*w).renderGeneration, 0, memory_order_relaxed);
   atomic_store_explicit(&(*w).topLayer, nullptr, memory_order_relaxed);
   atomic_store_explicit(&(*w).bottomLayer, nullptr, memory_order_relaxed);
   atomic_store_explicit(&(*w).enabled, true, memory_order_relaxed);
   atomic_store_explicit(&(*w).keyEnabled, true, memory_order_relaxed);
   (*w).lastFocused = false;
   atomic_store_explicit(&(*w).monitorId, 0, memory_order_relaxed);
   (*w).cursorType = WINDOW_CURSOR_DEFAULT;
   (*w).decorated = true;
   (*w).resizableEnabled = true;
   (*w).closableEnabled = true;
   (*w).minimizeEnabled = true;
   (*w).fullscreenButton = true;
   (*w).resizeRenderFn = nullptr;
   (*w).resizeRenderUserdata = nullptr;
   (*w).id = windowIdAcquire((*w).hwnd, w);
   windowRefreshMonitor(w);

   // Trackpad touches: opt in, mirroring the Cocoa content-view touch mask.
   if (!RegisterTouchWindow((*w).hwnd, TWF_WMFINETOUCH | TWF_WMTWOTOUCH))
      (fn) 0;

   return w;
}

// Merge a caller's Desc over the defaults. Unset (zero) fields fall back —
// this is what makes partial designated initializers behave like overloads.
// Mirror of the Cocoa descResolve verbatim (platform-neutral).
static WindowDesc descResolve(const WindowDesc *desc) {
   WindowDesc d = { .title = "vex", .width = 800, .height = 600, .x = 0, .y = 0, .centered = true };
   if (desc == nullptr)
      return d;
   if ((*desc).title)
      d.title = (*desc).title;
   if ((*desc).width > 0)
      d.width = (*desc).width;
   if ((*desc).height > 0)
      d.height = (*desc).height;
   if ((*desc).x != 0 || (*desc).y != 0) {
      d.x = (*desc).x;
      d.y = (*desc).y;
      d.centered = false;
   }
   d.shown = (*desc).shown;
   return d;
}

// --- Constructors -----------------------------------------------------------

// Construct a default Win32 window through the common descriptor path.
Window *Window_0() {
   return Window_new(nullptr);
}

// Construct a window descriptor using the requested title and default geometry.
Window *Window_1(const char *title) {
   return Window_new(&(WindowDesc){ .title = title });
}

// Construct a window descriptor with explicit title and client dimensions.
Window *Window_3(const char *title, int width, int height) {
   return Window_new(&(WindowDesc){ .title = title, .width = width, .height = height });
}

// Resolve descriptor defaults, allocate the native window, and show if requested.
// Resolve descriptor defaults, allocate the native window, and show on request.
Window *Window_new(const WindowDesc *desc) {
   WindowDesc d = descResolve(desc);
   Window *w = windowAlloc(&d);
   if (w == nullptr)
      return nullptr;
   if (d.shown)
      Window_show(w);
   return w;
}

// Preserve the legacy constructor spelling by forwarding to Window_new.
Window *Window_create(const char *title, int width, int height) {
   return Window_new(&(WindowDesc){ .title = title, .width = width, .height = height });
}

// Tear down the window and free the handle. Safe to call whether the user
// already closed the window or not: if it's still open we close it. The
// WndProc is naturally detatched by DestroyWindow before the handle dies, so
// no callback can touch our freed memory (mirror of the delegate =nil step).
fn Window_destroy(Window *window) {
   if (window == nullptr)
      return;
   if (sPendingForeground == (*window).hwnd)
      sPendingForeground = nullptr;
   if (!atomic_load_explicit(&(*window).shouldClose, memory_order_relaxed)
      && IsWindow((*window).hwnd))
      DestroyWindow((*window).hwnd);
   Key_detachWindowAll((*window).id);
   Mouse_detachWindowAll((*window).id);
   Touch_detachWindowAll((*window).id);
   windowIdRelease((*window).id);
   free((*window).framePixels);
   free(window);
}

// Read the close latch, treating a missing handle as already closed.
bool Window_shouldClose(Window *window) {
   return window ? atomic_load_explicit(&(*window).shouldClose, memory_order_relaxed) : true;
}

// Set the close latch observed by the host lifecycle loop.
fn Window_setShouldClose(Window *window, bool shouldClose) {
   if (window != nullptr)
      atomic_store_explicit(&(*window).shouldClose, shouldClose, memory_order_relaxed);
}

// Destroy the native window and mark its C handle closed.
fn Window_close(Window *window) {
   if (!window) return;
   if (IsWindow((*window).hwnd)) DestroyWindow((*window).hwnd);
   atomic_store_explicit(&(*window).shouldClose, true, memory_order_relaxed);
}

// --- Present policy (pure state; a future render path consumes it) ----------

fn Window_setPresentMode(Window *window, int mode) {
   if (window == nullptr)
      return;
   int prev = atomic_exchange_explicit(&(*window).presentMode, mode, memory_order_relaxed);
   if (prev != mode)
      atomic_fetch_add_explicit(&(*window).renderGeneration, 1, memory_order_release);
}

// Read the stored present policy, defaulting to FIFO for a null handle.
int Window_getPresentMode(const Window *window) {
   if (window == nullptr)
      return WINDOW_PRESENT_FIFO;
   return atomic_load_explicit(&(*window).presentMode, memory_order_relaxed);
}

// Update transparency policy, mark render resources dirty, and toggle layered style.
fn Window_setTransparent(Window *window, bool transparent) {
   if (window == nullptr)
      return;
   bool prev = atomic_exchange_explicit(&(*window).transparent, transparent, memory_order_relaxed);
   if (prev != transparent)
      atomic_fetch_add_explicit(&(*window).renderGeneration, 1, memory_order_release);
   // WS_EX_LAYERED is the Win32 dialect of per-pixel alpha: the swapchain
   // rebuild ticket lands first, then the OS surface follows.
   LONG_PTR ex = GetWindowLongPtr((*window).hwnd, GWL_EXSTYLE);
   if (transparent)
      ex |= WS_EX_LAYERED;
   else
      ex &= ~WS_EX_LAYERED;
   SetWindowLongPtr((*window).hwnd, GWL_EXSTYLE, ex);
   SetWindowPos((*window).hwnd, nullptr, 0, 0, 0, 0,
             SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// Read the stored layered-window transparency policy.
bool Window_isTransparent(const Window *window) {
   return window ? atomic_load_explicit(&(*window).transparent, memory_order_relaxed) : false;
}

// Read the policy generation used by downstream render-resource rebuilds.
u64 Window_renderGeneration(const Window *window) {
   return window ? atomic_load_explicit(&(*window).renderGeneration, memory_order_acquire) : 0;
}

// --- Graphics board slots (stored + ordered; rendering lives elsewhere) -----

// ;;INTENTION("orderLayers on the lean window is a pure no-op — the window no
// longer parents render layers; the still-unmigrated darling compositor
// retains the call. It retires with the composite seam.")
fn Window_orderLayers(Window *window) {
   (fn) window;
}

// Store the opaque lower-layer handle; this backend does not inspect it.
fn Window_setBottomLayer(Window *window, any layer) {
   if (window == nullptr)
      return;
   atomic_store_explicit(&(*window).bottomLayer, layer, memory_order_release);
   Window_orderLayers(window);
}

// Return the stored lower-layer handle without interpreting it.
any Window_getBottomLayer(const Window *window) {
   return window ? atomic_load_explicit(&(*window).bottomLayer, memory_order_acquire) : nullptr;
}

// Store the opaque upper-layer handle; this backend does not inspect it.
fn Window_setTopLayer(Window *window, any layer) {
   if (window == nullptr)
      return;
   atomic_store_explicit(&(*window).topLayer, layer, memory_order_release);
   Window_orderLayers(window);
}

// Return the stored upper-layer handle without interpreting it.
any Window_getTopLayer(const Window *window) {
   return window ? atomic_load_explicit(&(*window).topLayer, memory_order_acquire) : nullptr;
}

// ;;INTENTION("Pane/board compositing is inert on the lean window (the same
// slice the Cocoa backend keeps inert); pane work migrates to the render
// personal' own pass. Retires together with the composite seam.")
bool Window_attachPanes(Window *window, Panel *panel, int width, int height) {
   (fn) window;
   (fn) panel;
   (fn) width;
   (fn) height;
   return false;
}

// Retain the retired pane API as an inert failure on the lean window backend.
bool Window_resizePanes(Window *window, Panel *panel, int width, int height) {
   (fn) window;
   (fn) panel;
   (fn) width;
   (fn) height;
   return false;
}

// Keep the retired pane-compositor entry point linkable without rendering.
fn Window_compositePanes(Window *window, Panel *contentPanel) {
   (fn) window;
   (fn) contentPanel;
}

// Keep the retired board-compositor entry point linkable without rendering.
fn Window_compositeBoards(Window *window) {
   (fn) window;
}

// --- Runtime state ----------------------------------------------------------

fn Window_setEnabled(Window *window, bool enabled) {
   if (window == nullptr)
      return;
   atomic_store_explicit(&(*window).enabled, enabled, memory_order_relaxed);
}

// Read whether native input forwarding is enabled for this window.
bool Window_isEnabled(const Window *window) {
   return window ? atomic_load_explicit(&(*window).enabled, memory_order_relaxed) : false;
}

// Read the flag set between WM_ENTERSIZEMOVE and WM_EXITSIZEMOVE.
bool Window_isLiveResizing(const Window *window) {
   return window ? atomic_load_explicit(&(*window).liveResizing, memory_order_relaxed) : false;
}

// --- Chrome / state API -----------------------------------------------------

fn Window_setTitle(Window *window, const char *title) {
   if (window == nullptr || title == nullptr)
      return;
   wchar_t titleBuf[256];
   bytes tn = strlen(title);
   if (tn >= 256)
      tn = 255;
   for (bytes i = 0; i < tn; i++)
      titleBuf[i] = (wchar_t) (unsigned char) title[i];
   titleBuf[tn] = 0;
   SetWindowTextW((*window).hwnd, titleBuf);
}

// Read the cached client width in native pixels.
int Window_width(Window *window) {
   if (window == nullptr)
      return 0;
   return atomic_load_explicit(&(*window).cachedWidth, memory_order_relaxed);
}

// Read the cached client height in native pixels.
int Window_height(Window *window) {
   if (window == nullptr)
      return 0;
   return atomic_load_explicit(&(*window).cachedHeight, memory_order_relaxed);
}

// Return the unit scale of the Win32 physical-pixel window API.
f32 Window_getScale(const Window *window) {
   (fn) window;
   return 1.0f;
}

// Provide the display-scale revalidation seam; Win32 reports physical pixels directly.
fn Window_revalidate(Window *window) {
   (fn) window;
}

// Convert compatibility point dimensions to the integer client-size request.
fn Window_setSizePoints(Window *window, f32 width, f32 height) {
   Window_setSize(window, (int) width, (int) height);
}

// Return cached client dimensions through the points compatibility outputs.
fn Window_getSizePoints(const Window *window, f32 *outWidth, f32 *outHeight) {
   if (outWidth) *outWidth = (f32) Window_width((Window*) window);
   if (outHeight) *outHeight = (f32) Window_height((Window*) window);
}

// Return cached client width as a f32ing-point compatibility value.
f32 Window_widthPoints(const Window *window) {
   return (f32) Window_width((Window*) window);
}

// Return cached client height as a f32ing-point compatibility value.
f32 Window_heightPoints(const Window *window) {
   return (f32) Window_height((Window*) window);
}

// Alias the client width as the rendering viewport width.
int Window_viewportWidth(const Window *window) {
   return Window_width((Window*) window);
}

// Alias the client height as the rendering viewport height.
int Window_viewportHeight(const Window *window) {
   return Window_height((Window*) window);
}

// Alias the client width through the viewport points compatibility accessor.
f32 Window_viewportWidthPoints(const Window *window) {
   return (f32) Window_width((Window*) window);
}

// Alias the client height through the viewport points compatibility accessor.
f32 Window_viewportHeightPoints(const Window *window) {
   return (f32) Window_height((Window*) window);
}

// Return the full outer-window width in physical pixels.
int Window_windowWidth(const Window *window) {
   return Window_width((Window*) window);
}

// Return the full outer-window height in physical pixels.
int Window_windowHeight(const Window *window) {
   return Window_height((Window*) window);
}

// Return the full outer-window width through the points compatibility API.
f32 Window_windowWidthPoints(const Window *window) {
   return (f32) Window_width((Window*) window);
}

// Return the full outer-window height through the points compatibility API.
f32 Window_windowHeightPoints(const Window *window) {
   return (f32) Window_height((Window*) window);
}

// Resize the HWND so its client area matches the requested physical dimensions.
fn Window_setSize(Window *window, int width, int height) {
   if (window == nullptr)
      return;
   int wid, hei;
   frameForContent(window, width, height, &wid, &hei);
   RECT rc = {0};
   GetWindowRect((*window).hwnd, &rc);
   SetWindowPos((*window).hwnd, nullptr, 0, 0, wid, hei,
             SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// Move the native window to the requested top-left screen pixel coordinates.
fn Window_setLocation(Window *window, int x, int y) {
   if (window == nullptr)
      return;
   RECT rc = {0};
   GetWindowRect((*window).hwnd, &rc);
   SetWindowPos((*window).hwnd, nullptr, x, y,
             (int) (rc.right - rc.left), (int) (rc.bottom - rc.top),
             SWP_NOZORDER | SWP_NOACTIVATE);
}

// Return the cached outer-window top-left coordinates through optional outputs.
fn Window_getLocation(const Window *window, int *outX, int *outY) {
   if (outX)
      *outX = window ? (*window).cachedX : 0;
   if (outY)
      *outY = window ? (*window).cachedY : 0;
}

// Return the cached client-area screen origin through optional outputs.
fn Window_getContentOrigin(const Window *window, int *outX, int *outY) {
   if (outX)
      *outX = window ? (*window).cachedContentX : 0;
   if (outY)
      *outY = window ? (*window).cachedContentY : 0;
}

// Center the outer window in the nearest monitor's usable work area.
fn Window_center(Window *window) {
   if (window == nullptr)
      return;
   HMONITOR hm = MonitorFromWindow((*window).hwnd, MONITOR_DEFAULTTONEAREST);
   MONITORINFO mi;
   mi.cbSize = sizeof(mi);
   GetMonitorInfoW(hm, &mi);
   RECT rc = {0};
   GetWindowRect((*window).hwnd, &rc);
   int wid = (int) (rc.right - rc.left);
   int hei = (int) (rc.bottom - rc.top);
   RECT work = (*mi).rcWork;
   int x = work.left + (work.right - work.left - wid) / 2;
   int y = work.top + (work.bottom - work.top - hei) / 2;
   SetWindowPos((*window).hwnd, nullptr, x, y, 0, 0,
             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// Claim foreground synchronously: Win32 foreground rules are the mirror of
// AppKit activation — the grant is async and may be refused by the OS, so the
// pump's sPendingForeground re-assertion covers the tail (bounded 500ms).
static fn claimForeground(HWND hwnd) {
   if (hwnd == nullptr)
      return;
   winClaimForeground(hwnd);
}

// Show the HWND, request foreground activation, and refresh its monitor mirror.
fn Window_show(Window *window) {
   if (window == nullptr)
      return;
   claimForeground((*window).hwnd);
   windowRefreshMonitor(window);
}

// Hide the HWND and cancel any pending foreground claim for it.
fn Window_hide(Window *window) {
   if (window == nullptr)
      return;
   if (sPendingForeground == (*window).hwnd)
      sPendingForeground = nullptr;
   ShowWindow((*window).hwnd, SW_HIDE);
}

// Select the native show or hide operation from the requested visibility.
fn Window_setVisible(Window *window, bool visible) {
   if (visible)
      Window_show(window);
   else
      Window_hide(window);
}

// Query the current HWND style for the resizable-frame bit.
bool Window_isResizable(Window *window) {
   if (window == nullptr)
      return false;
   LONG_PTR s = GetWindowLongPtr((*window).hwnd, GWL_STYLE);
   return (s & WS_THICKFRAME) != 0;
}

// Update the stored resize capability and rebuild the window chrome style.
fn Window_setResizable(Window *window, bool resizable) {
   if (window == nullptr)
      return;
   (*window).resizableEnabled = resizable;
   windowApplyChrome(window, (*window).decorated);
}

// Query the current HWND style for the system-menu close affordance.
bool Window_isClosable(Window *window) {
   if (window == nullptr)
      return false;
   LONG_PTR s = GetWindowLongPtr((*window).hwnd, GWL_STYLE);
   return (s & WS_SYSMENU) != 0;
}

// Toggle the system-menu close affordance while preserving normal close handling.
fn Window_setClosable(Window *window, bool closable) {
   if (window == nullptr)
      return;
   // Closable on Win32 toggles the system-menu close affordance: remove
   // WS_SYSMENU (mirror of the Cocoa style-bit toggle), so the title-bar
   // closes but destruction still honors a WM_CLOSE properly when asked.
   (*window).closableEnabled = closable;
   windowApplyChrome(window, (*window).decorated);
}

// Query the current HWND style for the minimize-button affordance.
bool Window_isMiniaturizable(Window *window) {
   if (window == nullptr)
      return false;
   LONG_PTR s = GetWindowLongPtr((*window).hwnd, GWL_STYLE);
   return (s & WS_MINIMIZEBOX) != 0;
}

// Update minimize capability and rebuild the HWND chrome style.
fn Window_setMiniaturizable(Window *window, bool miniaturizable) {
   if (window == nullptr)
      return;
   (*window).minimizeEnabled = miniaturizable;
   windowApplyChrome(window, (*window).decorated);
}

// Update maximize-button capability and rebuild the HWND chrome style.
fn Window_setFullscreenButton(Window *window, bool enabled) {
   if (window == nullptr)
      return;
   (*window).fullscreenButton = enabled;
   windowApplyChrome(window, (*window).decorated);
}

// Switch window chrome at runtime: one of WINDOW_UNDECORATED_*. Mirrors the
// Cocoa mask swap — BORDERLESS = zero chrome, NAKED = transparent titlebar
// with the resize affordance kept, DECORATED = standard chrome. DECORATED
// strips any active blur so a frosted caption never renders (the blur ban).
fn Window_setUndecorated(Window *window, int mode) {
   if (window == nullptr)
      return;
   if ((*window).fullscreen)
      return;
   bool decorated = (mode == WINDOW_DECORATED);
   (*window).decorated = mode;
   windowApplyChrome(window, decorated);
}

// Translate the boolean decoration request into the native chrome mode setter.
fn Window_setDecorated(Window *window, bool decorated) {
   Window_setUndecorated(window, decorated ? WINDOW_DECORATED : WINDOW_UNDECORATED_BORDERLESS);
}

// Report whether the current chrome mode is the standard decorated mode.
bool Window_isDecorated(const Window *window) {
   if (window == nullptr)
      return false;
   return (*window).decorated == WINDOW_DECORATED;
}

// Translate the naked-titlebar request into the native chrome mode setter.
fn Window_setNaked(Window *window, bool naked) {
   Window_setUndecorated(window, naked ? WINDOW_UNDECORATED_NAKED : WINDOW_DECORATED);
}

// Report whether the current chrome mode uses the naked titlebar style.
bool Window_isNaked(const Window *window) {
   if (window == nullptr)
      return false;
   return (*window).decorated == WINDOW_UNDECORATED_NAKED;
}

// Translate the borderless request into the native chrome mode setter.
fn Window_setBorderless(Window *window, bool borderless) {
   Window_setUndecorated(window, borderless ? WINDOW_UNDECORATED_BORDERLESS : WINDOW_DECORATED);
}

// Report whether the current chrome mode removes native window decorations.
bool Window_isBorderless(const Window *window) {
   if (window == nullptr)
      return false;
   return (*window).decorated == WINDOW_UNDECORATED_BORDERLESS;
}

// Retain the AppKit-only titlebar flush seam; Win32 does not implement it.
fn Window_setViewportFlushToTop(Window *window, bool flush) {
   if (window == nullptr)
      return;
   if ((*window).decorated != WINDOW_DECORATED)
      return;
   (fn) flush;
}

// Report that viewport flush-to-top is unavailable on this backend.
bool Window_isViewportFlushToTop(const Window *window) {
   (fn) window;
   return false;
}

// Forward the compatibility request to the inert viewport-flush setter.
fn Window_setFloatingTrafficLights(Window *window, bool f32ing) {
   Window_setViewportFlushToTop(window, f32ing);
}

// Map a public traffic-light enum value to its native button slot, if any.
static int lightIndex(WindowTrafficLight light) {
   if (light == WINDOW_TRAFFIC_LIGHT_CLOSE)
      return LIGHT_CLOSE;
   if (light == WINDOW_TRAFFIC_LIGHT_MINIMIZE)
      return LIGHT_MINI;
   if (light == WINDOW_TRAFFIC_LIGHT_ZOOM)
      return LIGHT_ZOOM;
   return -1;
}

;;PLATFORM_EXCLUSIVE("macOS")
fn Window_macOS_setTrafficLightButtonVisible(Window *window, WindowTrafficLight light, bool visible) {
   (fn) window;
   (fn) visible;
   (fn) lightIndex(light);
   fprintf(stderr, "window: Window_macOS_setTrafficLightButtonVisible is macOS-only (needs a Mac machine)\n");
}

;;PLATFORM_EXCLUSIVE("macOS")
bool Window_macOS_isTrafficLightButtonVisible(const Window *window, WindowTrafficLight light) {
   (fn) light;
   fprintf(stderr, "window: Window_macOS_isTrafficLightButtonVisible is macOS-only (needs a Mac machine)\n");
   (fn) window;
   return false;
}

;;PLATFORM_EXCLUSIVE("macOS")
fn Window_macOS_setTrafficLightHeaderPosition(Window *window, f32 x, f32 y) {
   (fn) window;
   (fn) x;
   (fn) y;
   fprintf(stderr, "window: Window_macOS_setTrafficLightHeaderPosition is macOS-only (needs a Mac machine)\n");
}

;;PLATFORM_EXCLUSIVE("macOS")
fn Window_macOS_getTrafficLightHeaderPosition(const Window *window, f32 *outX, f32 *outY) {
   (fn) window;
   fprintf(stderr, "window: Window_macOS_getTrafficLightHeaderPosition is macOS-only (needs a Mac machine)\n");
   if (outX)
      *outX = 0.0f;
   if (outY)
      *outY = 0.0f;
}

// Apply the requested alpha to the layered HWND through User32.
fn Window_setOpacity(Window *window, f32 opacity) {
   if (window == nullptr)
      return;
   LONG_PTR ex = GetWindowLongPtr((*window).hwnd, GWL_EXSTYLE);
   SetWindowLongPtr((*window).hwnd, GWL_EXSTYLE, ex | WS_EX_LAYERED);
   BYTE alpha = (BYTE) (int) (opacity * 255.0f + 0.5f);
   SetLayeredWindowAttributes((*window).hwnd, 0, alpha, LWA_ALPHA);
}

// Configure layered per-pixel transparency through the shared policy setter.
fn Window_setTransparentBackground(Window *window, bool transparent) {
   if (window == nullptr)
      return;
   // A clear backdrop is the Win32 dialect of the Cocoa clear background:
   // the layered window renders per-pixel, and a render path paints holes.
   Window_setTransparent(window, transparent);
}

// Toggle the HWND's topmost placement without moving or resizing it.
fn Window_setAlwaysOnTop(Window *window, bool onTop) {
   if (window == nullptr)
      return;
   HWND insert = onTop ? HWND_TOPMOST : HWND_NOTOPMOST;
   SetWindowPos((*window).hwnd, insert, 0, 0, 0, 0,
             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

// Toggle the extended style that lets pointer input pass through the window.
fn Window_setClickThrough(Window *window, bool clickThrough) {
   if (window == nullptr)
      return;
   LONG_PTR ex = GetWindowLongPtr((*window).hwnd, GWL_EXSTYLE);
   if (clickThrough) {
      // WS_EX_TRANSPARENT also requires WS_EX_LAYERED to take effect.
      ex |= WS_EX_LAYERED | WS_EX_TRANSPARENT;
   } else {
      ex &= ~WS_EX_TRANSPARENT;
   }
   SetWindowLongPtr((*window).hwnd, GWL_EXSTYLE, ex);
}

// Request the native DWM non-client rendering policy for the window.
fn Window_setShadow(Window *window, bool shadow) {
   if (window == nullptr)
      return;
   // ;;INTENTION("DWM-driven drop shadow: DISABLED non-client rendering is
   // the documented low-level switch. Borderless (WS_POPUP) windows get no
   // native shadow regardless — the flag is best-effort for decorated chrome.")
   DWMNCRENDERINGPOLICY policy = shadow ? DWMNCRP_ENABLED : DWMNCRP_DISABLED;
   DwmSetWindowAttribute((*window).hwnd, DWMWA_NCRENDERING_POLICY, &policy, sizeof(policy));
}

// Store whether client-area hit tests should initiate a window drag.
fn Window_setMovableByBackground(Window *window, bool movable) {
   if (window == nullptr)
      return;
   (*window).movableByBackground = movable;
}

// Raise the HWND in the native z-order without claiming foreground focus.
fn Window_bringToFront(Window *window) {
   if (window == nullptr)
      return;
   BringWindowToTop((*window).hwnd);
}

// ;;INTENTION("WIN32 has no child-window ordering contract matching AppKit: attach/detach are link-parity no-ops; stacking stays caller-ordered")
fn Window_attachChild(Window *parent, Window *child) {
   (fn) parent;
   (fn) child;
}

// Retain the child-window ordering seam as a no-op on this backend.
fn Window_detachChild(Window *parent, Window *child) {
   (fn) parent;
   (fn) child;
}

// --- Minimize ---

fn Window_minimize(Window *window) {
   if (window == nullptr)
      return;
   atomic_store_explicit(&(*window).miniaturizing, true, memory_order_relaxed);
   ShowWindow((*window).hwnd, SW_MINIMIZE);
}

// Restore an iconic HWND to its prior placement when it is minimized.
fn Window_restore(Window *window) {
   if (window == nullptr)
      return;
   if (IsIconic((*window).hwnd))
      ShowWindow((*window).hwnd, SW_RESTORE);
}

// Report either an in-progress minimize transition or the OS iconic state.
bool Window_isMinimized(Window *window) {
   if (window == nullptr)
      return false;
   if (atomic_load_explicit(&(*window).miniaturizing, memory_order_relaxed))
      return true;
   return IsIconic((*window).hwnd) != FALSE;
}

// --- Fullscreen ---

bool Window_isFullscreen(Window *window) {
   if (window == nullptr)
      return false;
   return (*window).fullscreen;
}

// Switch between monitor-sized borderless fullscreen and the saved window frame.
fn Window_setFullscreen(Window *window, bool fullscreen) {
   if (window == nullptr)
      return;
   if (fullscreen == (*window).fullscreen)
      return;
   HWND hwnd = (*window).hwnd;
   RECT rc = {0};
   if (fullscreen) {
      GetWindowRect(hwnd, &rc);
      (*window).savedFrame = rc;
      LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
      LONG_PTR ex = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
      SetWindowLongPtr(hwnd, GWL_STYLE, (style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
      SetWindowLongPtr(hwnd, GWL_EXSTYLE, ex & ~WS_EX_DLGMODALFRAME);
      HMONITOR hm = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
      MONITORINFO mi;
      mi.cbSize = sizeof(mi);
      GetMonitorInfoW(hm, &mi);
      (*window).fullscreen = true;
      RECT monitor = (*mi).rcMonitor;
      SetWindowPos(hwnd, HWND_TOPMOST,
                monitor.left, monitor.top,
                (int) (monitor.right - monitor.left),
                (int) (monitor.bottom - monitor.top),
                SWP_FRAMECHANGED | SWP_NOACTIVATE);
   } else {
      LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
      SetWindowLongPtr(hwnd, GWL_STYLE, (style & ~WS_POPUP) | WS_OVERLAPPEDWINDOW);
      RECT saved = (*window).savedFrame;
      (*window).fullscreen = false;
      SetWindowPos(hwnd, HWND_NOTOPMOST,
                (int) saved.left, (int) saved.top,
                (int) (saved.right - saved.left),
                (int) (saved.bottom - saved.top),
                SWP_FRAMECHANGED | SWP_NOACTIVATE);
      WindowEvent_fireRestored(&(*window).lifecycle, window);
   }
}

// Toggle fullscreen using the current mirrored native fullscreen state.
fn Window_toggleFullscreen(Window *window) {
   if (window == nullptr)
      return;
   Window_setFullscreen(window, !Window_isFullscreen(window));
}

// --- DRM / sharing ---

// ;;INTENTION("Window_setDRM has no Win32 analogue (NSWindowSharingNone is an
// AppKit concept). Stored-and-inert; the Win32 surface stays stable.")
fn Window_setDRM(Window *window, bool enabled) {
   (fn) window;
   (fn) enabled;
}

// --- Size constraints ---

fn Window_setMinSize(Window *window, int width, int height) {
   if (window == nullptr)
      return;
   (*window).minWidth = width;
   (*window).minHeight = height;
}

// Store the maximum client dimensions consumed by WM_GETMINMAXINFO.
fn Window_setMaxSize(Window *window, int width, int height) {
   if (window == nullptr)
      return;
   (*window).maxWidth = width;
   (*window).maxHeight = height;
}

// --- Cursor control ---

// FPS-style relative cursor: hides the pointer, decouples it from movement,
// clips it to the window, and re-warps to the anchor centre each pump pass
// while deltas flow into the mouse stream as move-delta events.
fn Window_setCursorLocked(Window *window, bool locked) {
   if (locked == s_cursorLocked)
      return;
   if (locked) {
      HWND anchor = window ? (*window).hwnd : nullptr;
      RECT rc = {0};
      if (anchor)
         GetWindowRect(anchor, &rc);
      s_lockCenter.x = (LONG) (rc.left + (rc.right - rc.left) / 2);
      s_lockCenter.y = (LONG) (rc.top + (rc.bottom - rc.top) / 2);
      if (anchor)
         ClipCursor(&rc);
      SetCursorPos(s_lockCenter.x, s_lockCenter.y);
      while (ShowCursor(FALSE) >= 0) {
      }
      s_cursorLocked = true;
   } else {
      s_cursorLocked = false;
      ClipCursor(nullptr);
      while (ShowCursor(TRUE) < 0) {
      }
   }
}

// Resolve the stored cursor enum to a Win32 cursor and apply its visibility.
static fn windowApplyCursor(Window *window) {
   HCURSOR cursor = LoadCursorW(nullptr, (LPCWSTR) IDC_ARROW);
   switch ((*window).cursorType) {
      case WINDOW_CURSOR_IBEAM:
         cursor = LoadCursorW(nullptr, (LPCWSTR) IDC_IBEAM);
         break;
      case WINDOW_CURSOR_POINTING_HAND:
         cursor = LoadCursorW(nullptr, (LPCWSTR) IDC_HAND);
         break;
      case WINDOW_CURSOR_CROSSHAIR:
         cursor = LoadCursorW(nullptr, (LPCWSTR) IDC_CROSS);
         break;
      case WINDOW_CURSOR_RESIZE_EW:
         cursor = LoadCursorW(nullptr, (LPCWSTR) IDC_SIZEWE);
         break;
      case WINDOW_CURSOR_RESIZE_NS:
         cursor = LoadCursorW(nullptr, (LPCWSTR) IDC_SIZENS);
         break;
      case WINDOW_CURSOR_NOT_ALLOWED:
         cursor = LoadCursorW(nullptr, (LPCWSTR) IDC_NO);
         break;
      case WINDOW_CURSOR_HIDDEN:
         while (ShowCursor(FALSE) >= 0) {
         }
         break;
      case WINDOW_CURSOR_DEFAULT:
      default:
         cursor = LoadCursorW(nullptr, (LPCWSTR) IDC_ARROW);
         break;
   }
   if ((*window).cursorType != WINDOW_CURSOR_HIDDEN)
      SetClassLongPtr((*window).hwnd, GCLP_HCURSOR, (LONG_PTR) cursor);
   SetCursor(cursor);
}

// Store the cursor request and immediately apply the corresponding Win32 cursor.
fn Window_setCursorType(Window *window, WindowCursorType type) {
   if (window == nullptr)
      return;
   (*window).cursorType = type;
   windowApplyCursor(window);
}

// Read the current cursor request, defaulting when no window is supplied.
WindowCursorType Window_getCursorType(const Window *window) {
   if (window == nullptr)
      return WINDOW_CURSOR_DEFAULT;
   return (*window).cursorType;
}

// --- Software frame presentation --------------------------------------------

// The lean Win32 software path: copy the caller's RGBA frame into a staging
// DIB and InvalidateRect so the next WM_PAINT StretchDIBits-s blits it scaled
// to fit the client area. Thread 0 only (BeginPaint/EndPaint are _main-thread).
bool Window_present(Window *window, const Buffer *frame) {
   if (window == nullptr || frame == nullptr)
      return false;
   if (Window_width(window) <= 0 || Window_height(window) <= 0)
      return false;
   u32 fw = (*frame).width;
   u32 fh = (*frame).height;
   if (fw == 0 || fh == 0)
      return false;
   u32 chans = (*frame).channels;
   if (chans != 4 && chans != 3 && chans != 1)
      return false;
   bytes Bytes = (bytes) fw * (bytes) fh * 4u;
   if (Bytes == 0)
      return false;
   // Reuse the staging slot when the frame dimensions are unchanged; grow it
   // otherwise (never in a hot path — present is demand-driven).
   if ((*window).frameW != (int) fw || (*window).frameH != (int) fh) {
      unsigned char *grown = (unsigned char*) realloc((*window).framePixels, Bytes);
      if (grown == nullptr)
         return false;
      (*window).framePixels = grown;
      (*window).frameW = (int) fw;
      (*window).frameH = (int) fh;
   }
   const u64 *data = (*frame).data;
   unsigned char *dst = (*window).framePixels;
   for (u32 y = 0; y < fh; y++) {
      for (u32 x = 0; x < fw; x++) {
         bytes i = (bytes) y * fw + x;
         u64 px = data[i];
         dst[4 * i + 0] = (unsigned char) ((px >> 16) & 0xFFu); // R (ColorBuffer order)
         dst[4 * i + 1] = (unsigned char) ((px >> 8) & 0xFFu);  // G
         dst[4 * i + 2] = (unsigned char) ((px >> 0) & 0xFFu);  // B
         dst[4 * i + 3] = (unsigned char) ((px >> 24) & 0xFFu); // A
      }
   }
   (*window).frameDirty = true;
   InvalidateRect((*window).hwnd, nullptr, FALSE);
   return true;
}

// --- Surface anchors (thread 0) ----------------------------------------------

any Window_contentView(Window *window) {
   if (window == nullptr || (*window).hwnd == nullptr)
      return nullptr;
   return (any) (*window).hwnd;
}

any Window_nativeHandle(const Window *window) {
   if (window == nullptr || (*window).hwnd == nullptr)
      return nullptr;
   return (any) (*window).hwnd;
}

// ;;INTENTION("Window_metalLayer is gone from this file by design (no Metal
// here). Returns nullptr so the VK_EXT_metal_surface path degrades cleanly
// until the render personal create their own surface. Retires with the composite
// seam.")
any Window_metalLayer(Window *window) {
   (fn) window;
   return nullptr;
}

// ;;INTENTION("Gravity ownership moved out of the window (no layer here).
// No-op; retires with the composite seam.")
fn Window_setGravityTopLeft(Window *window) {
   (fn) window;
}

// ;;INTENTION("Worker present transaction was GPU-era (presentsWithTransaction
// drawables). No layer means nothing to commit; no-op. Retires with the
// composite seam.")
fn Window_workerPresentBegin() {
}

// Close the retired worker-present transaction hook; this backend owns none.
fn Window_workerPresentEnd() {
}

// --- Event wiring (rings are engine-global; adapters attach by window id) ----

fn Window_addKeyAdapter(Window *window, const KeyHandler *adapter) {
   if (window == nullptr)
      return;
   Key_attachWindow((*window).id, adapter);
}

// Detach a key callback from the window's engine input slot.
bool Window_removeKeyAdapter(Window *window, const KeyHandler *adapter) {
   if (window == nullptr)
      return false;
   return Key_detachWindow((*window).id, adapter);
}

// Attach a mouse callback to the window's engine input slot.
fn Window_addMouseAdapter(Window *window, const MouseHandler *adapter) {
   if (window == nullptr)
      return;
   Mouse_attachWindow((*window).id, adapter);
}

// Detach a mouse callback from the window's engine input slot.
bool Window_removeMouseAdapter(Window *window, const MouseHandler *adapter) {
   if (window == nullptr)
      return false;
   return Mouse_detachWindow((*window).id, adapter);
}

// Attach a touch callback to the window's engine input slot.
fn Window_addTouchAdapter(Window *window, const TouchHandler *adapter) {
   if (window == nullptr)
      return;
   Touch_attachWindow((*window).id, adapter);
}

// Detach a touch callback from the window's engine input slot.
bool Window_removeTouchAdapter(Window *window, const TouchHandler *adapter) {
   if (window == nullptr)
      return false;
   return Touch_detachWindow((*window).id, adapter);
}

// Drain key, mouse, and touch callback queues for the engine input modules.
fn Window_dispatchEvents(Window *window) {
   (fn) window;
   Key_dispatchEvents();
   Mouse_dispatchEvents();
   Touch_dispatchEvents();
}

// --- Focus (the spotlight: one focused window per machine) -------------------

u32 Window_id(Window *window) {
   return window ? (*window).id : FOCUS_BROADCAST;
}

// Request foreground activation and update the engine focus mirror.
fn Window_focus(Window *window) {
   if (window == nullptr)
      return;
   claimForeground((*window).hwnd);
   Focus_set((*window).id);
}

// Compare this window's id with the engine-wide focused-window identity.
bool Window_isFocused(Window *window) {
   return window && Focus_isFocused((*window).id);
}

// ;;INTENTION("WIN32 stores the key gate without OS enforcement: no WS_CHILD key refusal matches AppKit; the darling redirect remains the enforcer here")
fn Window_setKeyEnabled(Window *window, bool enabled) {
   if (window == nullptr)
      return;
   atomic_store_explicit(&(*window).keyEnabled, enabled, memory_order_relaxed);
}

// Read the key-input gate stored for this native window.
bool Window_isKeyEnabled(const Window *window) {
   if (window == nullptr)
      return false;
   return atomic_load_explicit(&(*window).keyEnabled, memory_order_relaxed);
}

// --- Lifecycle registry ------------------------------------------------------

WindowEvent *Window_getLifecycle(Window *window) {
   if (window == nullptr)
      return nullptr;
   return &(*window).lifecycle;
}

// --- Monitor identity --------------------------------------------------------

u32 Window_getMonitorId(const Window *window) {
   return window ? atomic_load_explicit(&(*window).monitorId, memory_order_acquire) : 0;
}

// --- Resize reflection -------------------------------------------------------

u64 Window_sizeGeneration(Window *window) {
   if (window == nullptr)
      return 0;
   return atomic_load_explicit(&(*window).sizeGeneration, memory_order_acquire);
}

// Register the callback invoked when native resize changes should redraw immediately.
fn Window_setResizeRenderHook(Window *window, WindowResizeRenderFn fn, any userdata) {
   if (window == nullptr)
      return;
   (*window).resizeRenderFn = fn;
   (*window).resizeRenderUserdata = userdata;
}

// Return the currently registered synchronous resize-render callback.
WindowResizeRenderFn Window_getResizeRenderHook(const Window *window) {
   return window ? (*window).resizeRenderFn : nullptr;
}


#elif defined(VEX_PLATFORM_LINUX) && defined(VEX_WINDOW_BACKEND_WAYLAND)
// window/window_wayland.c — the lean Wayland backend for the Window API (draft).
//
// A 1:1 mirror of the AppKit backend (window/window_cocoa.m): the same
// exported C surface, the same state families (POLICY / CONTENT / ADAPTERS),
// the same reflection words, the same WindowEvent lifecycle bridge, the same
// id registry. The only differences are the OS handle that anchors each
// window (wl_surface + xdg_toplevel instead of NSWindow) and the window-manager
// dialect that serves it (xdg-shell events instead of AppKit notifications).
//
// This is a DRAFT translation unit: it is not wired into CMake and has not
// been compiled against a real libwayland. It mirrors the Cocoa backend
// function-for-function so that wiring it in later (UNIX with WAYLAND_DISPLAY
// -> window/window_wayland.c, with window_linux.c kept as the X11 fallback)
// is a build-script edit, not an API migration.
//
// Like the Cocoa backend it is deliberately LEAN: a window with bridges, and
// nothing else. The compositor does the presentation plumbing — the wl_pointer/
// wl_keyboard/wl_touch listeners mirror OS state into C-visible words exactly
// the way the AppKit delegate notifications do, and input routes into the
// vexspoke Key/Mouse/Touch rings. There is NO Vulkan, NO swapchain and NO
// present worker here — rendering is the render personal' job and reaches the
// screen through the software present seam (a lean wl_shm path mirroring the
// retired raster present) or, once Migrates, the render personal' own surfaces.
// A Window is a dumb surface + callback bridge per the Window Decoupling Law.
//
// The GPU-era composite surface (boards, panes, worker present) is retained
// as thin INERT stubs at the bottom of this file exactly as the Cocoa backend
// keeps them, so the still-unmigrated darling compositor links. They retire
// together with their window.h declarations.


#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <wayland-client.h>
#include <wayland-cursor.h>
#include <xdg-shell.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "window/window.h"
#include "input/focus.h"
#include "input/key.h"
#include "input/mouse.h"
#include "input/touch.h"
#include "buffer/buffer.h"

;;DEFINITION
/**
 * ============================================================================
 * DEFINITION: Window_wayland
 * ============================================================================
 * Wayland/XDG desktop window abstraction implementation mirroring Cocoa capabilities.
 * Manages wl_surface, xdg_surface, and xdg_toplevel primitives, routing Wayland
 * seat input into vexspoke device rings and dispatching surface configure events
 * through the embedded WindowEvent lifecycle handlers.
 *
 * Implements the Window Decoupling Law as a presentation surface and event
 * bridge decoupled from GPU drivers, featuring a lean wl_shm software presentation
 * seam alongside inert compatibility stubs.
 * ============================================================================
 */

;;OVERVIEW
/**
 * ============================================================================
 * CLASS: Window (window/window_wayland.c)
 * ============================================================================
 * SUMMARY:
 *   DRAFT Wayland mirror of the AppKit window backend. One opaque C handle per
 *   wl_surface + xdg_toplevel; the engine loop constructs it, configures the
 *   chrome, commits it, then pumps Window_pollEvents once per frame while a
 *   render path draws through the software present seam (wl_shm) / event
 *   bridges. OS input is routed into the vexspoke device rings (tagged with
 *   this window's id); OS lifecycle (quit, resize, fullscreen, minimize,
 *   restore, press, focus) fires the embedded WindowEvent. Zero Vulkan, zero
 *   compositing — a Window is a dumb surface + callback bridge per the Window
 *   Decoupling Law.
 *
 * STRUCT FIELDS (Mirroring window/window.h incomplete tag — completed here):
 * ----------------------------------------------------------------------------
 *   struct wl_surface *surface;        // the surface anchor (we own it)
 *   struct xdg_surface *xdgSurface;    // xdg-shell wrapper (owned)
 *   struct xdg_toplevel *topLevel;     // the window chrome (owned)
 *   char *title;                       // strdup'd title (owned, xdg lifetime)
 *   WindowEvent lifecycle;             // OS lifecycle registry
 *   u32 id;                       // engine window id (1..N, 0 = broadcast)
 *   _Atomic bool shouldClose;          // true once close requested
 *   _Atomic u64 sizeGeneration;   // resize-reflection counter
 *   _Atomic int cachedWidth;           // surface width at last configure
 *   _Atomic int cachedHeight;          // surface height at last configure
 *   _Atomic bool liveResizing;         // set during interactive configure flurries
 *   _Atomic int presentMode;           // present pacing (FIFO/IMMEDIATE), stored
 *   _Atomic bool transparent;          // composite transparency request, stored
 *   _Atomic u64 renderGeneration; // policy-reflection counter
 *   _Atomic(any) topLayer;           // content board handle
 *   _Atomic(any) bottomLayer;        // scene board handle
 *   _Atomic bool enabled;              // false mutes ALL OS input
 *   bool lastFocused;                  // keyboard-enter flip detection
 *   _Atomic u32 monitorId;        // wl_output slot id (0 = unmapped)
 *   WindowCursorType cursorType;       // current cursor request
 *   bool decorated;                    // chrome mode (decoration is SSD-owned)
 *   bool fullscreen;                   // xdg toplevel fullscreen state
 *   bool minimized;                    // last set_minimized request
 *   int minWidth, minHeight;           // xdg_toplevel_set_min_size (0 = unset)
 *   int maxWidth, maxHeight;           // xdg_toplevel_set_max_size (0 = unbounded)
 *   bool movableByBackground;          // stored; no base protocol (;;INTENTION)
 *   bool fullscreenButton;             // stored chrome gate (SSD-owned)
 *   bool resizableEnabled;             // stored chrome gate (SSD-owned)
 *   bool closableEnabled;              // stored chrome gate (SSD-owned)
 *   bool minimizeEnabled;              // stored chrome gate (SSD-owned)
 *   bool clickThrough;                 // empty input region when true
 *   f32 opacity;                     // stored; no base protocol (;;INTENTION)
 *   bool shadow;                       // stored; SSD-owned (;;INTENTION)
 *   int frameW, frameH;                // retained shm staging size in px
 *   struct wl_buffer *shmBuffer;       // retained staging buffer (rebuilt on size)
 *   any shmMap;                      // retained pool mapping (the staging target)
 *   bytes shmSize;                    // current pool capacity in Bytes
 *   WindowResizeRenderFn resizeRenderFn;   // resize-cadence render hook
 *   any resizeRenderUserdata;        // hook userdata
 *
 * PRIVATE HELPERS (kept file-local, no external API):
 * ----------------------------------------------------------------------------
 *   WindowSlot                      — id-registry slot record
 *     struct wl_surface *surface;   // the surface carrying this id
 *     Window *handle;               // C handle owning that surface
 *   WaylandOutput                   — wl_output slot record
 *     struct wl_output *output;     // compositor output (may be nulled)
 *     u32 id;                  // stable engine id (index + 1)
 *   waylandConnect() / waylandDrain()
 *   windowRefreshSize, waylandOutputIdFor, keyFromKeycode, mirrorModifiers
 *   pointerSetCursorFor, routePointer, routeTouch
 *
 * FUNCTION REGISTRY:
 * ----------------------------------------------------------------------------
 * Public Constructors: (.h)
 *   - Window_0()                        : Window_new(nullptr)
 *   - Window_1(title)                       : Window_new(&{ .title })
 *   - Window_3(title, width, height)
 *   - Window_new(desc)                      : descResolve + waylandAlloc + show
 *   - Window_create(title, width, height)
 *
 * Private Constructors: (.c static)
 *   - waylandAlloc(desc)                    : shared internal constructor
 *   - descResolve(desc)                     : default parameter resolution
 *
 * Public Core Functions: (.h)
 *   - Window_destroy(window)                : detach, surface free, handle free
 *   - Window_destroyAll()
 *   - Window_shouldClose(window)
 *   - Window_pollEvents()               : non-blocking wl_display drain
 *   - Window_width(window)
 *   - Window_height(window)
 *   - Window_center(window)
 *   - Window_show(window)
 *   - Window_hide(window)
 *   - Window_attachPanes(window, panel, width, height) : inert (;;INTENTION)
 *   - Window_resizePanes(window, panel, width, height) : inert (;;INTENTION)
 *   - Window_compositePanes(window, contentPanel)      : inert (;;INTENTION)
 *   - Window_compositeBoards(window)        : inert (;;INTENTION)
 *   - Window_orderLayers(window)            : inert (;;INTENTION)
 *   - Window_renderGeneration(window)
 *   - Window_bringToFront(window)           : no base protocol (;;INTENTION)
 *   - Window_minimize(window)               : xdg_toplevel_set_minimized
 *   - Window_restore(window)
 *   - Window_toggleFullscreen(window)
 *   - Window_contentView(window)            : returns the wl_surface anchor
 *   - Window_nativeHandle(window)
 *   - Window_metalLayer(window)             : nullptr — no Metal here
 *   - Window_present(window, frame)         : lean wl_shm software path
 *   - Window_workerPresentBegin(window)     : inert (;;INTENTION)
 *   - Window_workerPresentEnd(window)       : inert (;;INTENTION)
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
 *   - Window_focus(window)                  : no base protocol (;;INTENTION)
 *   - Window_sizeGeneration(window)
 *
 * Private Core Functions: (.c static)
 *   - xdgSurfaceConfigure / toplevelConfigure / pointer* / keyboard* / touch*
 *
 * Public Setters: (.h)
 *   - Window_setShouldClose(window, shouldClose)
 *   - Window_setTitle(window, title)
 *   - Window_setSize(window, width, height)
 *   - Window_setLocation(window, x, y)      : xdg has no shell positioning
 *   - Window_setVisible(window, visible)
 *   - Window_setTopLayer(window, layer)
 *   - Window_setBottomLayer(window, layer)
 *   - Window_setPresentMode(window, mode)
 *   - Window_setTransparent(window, transparent)
 *   - Window_setEnabled(window, enabled)
 *   - Window_setKeyEnabled(window, enabled)
 *   - Window_setResizable(window, resizable) : SSD-owned
 *   - Window_setClosable(window, closable)   : SSD-owned
 *   - Window_setMiniaturizable(window, miniaturizable) : SSD-owned
 *   - Window_setFullscreenButton(window, enabled) : SSD-owned
 *   - Window_setUndecorated(window, mode)
 *   - Window_setDecorated(window, decorated)
 *   - Window_setNaked(window, naked)
 *   - Window_setBorderless(window, borderless)
 *   - Window_setViewportFlushToTop(window, flush) : macOS-only no-op
 *   - Window_setFloatingTrafficLights(window, f32ing) : macOS-only no-op
 *   - Window_macOS_setTrafficLightButtonVisible(window, light, visible) : macOS-only stub
 *   - Window_macOS_setTrafficLightHeaderPosition(window, x, y) : macOS-only stub
 *   - Window_setOpacity(window, opacity)    : stored (;;INTENTION)
 *   - Window_setTransparentBackground(window, transparent)
 *   - Window_setAlwaysOnTop(window, onTop)  : stored (;;INTENTION)
 *   - Window_setClickThrough(window, clickThrough) : empty input region
 *   - Window_setShadow(window, shadow)      : stored (;;INTENTION)
 *   - Window_setMovableByBackground(window, movable) : stored (;;INTENTION)
 *   - Window_setFullscreen(window, fullscreen)
 *   - Window_setDRM(window, enabled)        : no-op (;;INTENTION — macOS concept)
 *   - Window_setMinSize(window, width, height) : xdg min size
 *   - Window_setMaxSize(window, width, height) : xdg max size
 *   - Window_setCursorType(window, type)
 *   - Window_setCursorLocked(window, locked) : hidden cursor + delta warp note
 *   - Window_setResizeRenderHook(window, fn, userdata)
 *   - Window_setGravityTopLeft(window)      : no-op (;;INTENTION)
 *
 * Private Setters: (.c static)
 *   - (none)
 *
 * Public Getters: (.h)
 *   - Window_getLocation(window, outX, outY)     : last known offset (stored)
 *   - Window_getContentOrigin(window, outX, outY)
 *   - Window_getTopLayer(window)
 *   - Window_getBottomLayer(window)
 *   - Window_getPresentMode(window)
 *   - Window_isTransparent(window)
 *   - Window_renderGeneration(window)
 *   - Window_isEnabled(window)
 *   - Window_isKeyEnabled(window)
 *   - Window_isLiveResizing(window)
 *   - Window_isResizable(window)
 *   - Window_isClosable(window)
 *   - Window_isMiniaturizable(window)
 *   - Window_isDecorated(window)
 *   - Window_isNaked(window)
 *   - Window_isBorderless(window)
 *   - Window_isViewportFlushToTop(window) : macOS-only false
 *   - Window_macOS_isTrafficLightButtonVisible(window, light) : macOS-only stub
 *   - Window_macOS_getTrafficLightHeaderPosition(window, outX, outY) : macOS-only stub
 *   - Window_isMinimized(window)
 *   - Window_isFullscreen(window)
 *   - Window_getCursorType(window)
 *   - Window_id(window)
 *   - Window_isFocused(window)
 *   - Window_getLifecycle(window)
 *   - Window_getMonitorId(window)
 *   - Window_sizeGeneration(window)
 *   - Window_getResizeRenderHook(window)
 *
 * Private Getters: (.c static)
 *   - (none)
 * ============================================================================
 */
;;PLATFORM_EXCLUSIVE("Wayland")
;;DRAFT
;;INTENTION("Draft translation unit: wired opt-in (-DHOTCWAP_USE_WAYLAND=ON "
         "resolves wayland-client + xkbcommon via pkgconfig and selects "
         "window/window_wayland.c, auto-falling back to window_linux.c), but "
         "never compiled against a real libwayland — the macOS host excludes "
         "it. Mirrors the Cocoa backend 1:1; a Linux-host build is the "
         "promotion gate.")
;;INTENTION("Wayland has no client-side positioning: the compositor owns "
         "window placement, stacking, chrome, and focus. setLocation, "
         "center, bringToFront, setAlwaysOnTop, setShadow, setOpacity, "
         "setMovableByBackground, setFullscreenButton and the chrome "
         "capability toggles are therefore stored in the mirror fields only "
         "(the values a later xdg_layer/xdg_activation/ext-wm client could "
         "honor), never translated to protocol calls. Focus follows the "
         "wl_keyboard enter/leave/mods mirror, exactly like the Cocoa key "
         "window.")
;;INTENTION("The seat (pointer/keyboard/touch) is process-global — one "
         "connection, one wl_seat — so the listeners resolve their target "
         "window by reverse-looking-up the event's wl_surface in the id "
         "registry (the pointer's focused surface is tracked across enter/"
         "leave, since motion/button/axis carry no surface), the Wayland "
         "dialect of the Cocoa per-window delegate.")
;;INTENTION("Window_present is a genuinely lean Wayland path: a retained "
         "wl_shm pool + buffer rebuilt on size change, stamping the RGBA "
         "frame (Buffer layout, Bytes straight off the uint64 element "
         "array) into a mapped ARGB8888 pool and committing the surface. "
         "Straight alpha: the compositor blends against the desktop, "
         "mirroring the transparent swapchain ask.")
;;INTENTION("Cursor control uses a wl_cursor_theme loaded lazily; HIDDEN and "
         "cursor-lock hide the pointer (wl_pointer_set_cursor with a null "
         "surface) and route motion as move-deltas, the Wayland idiom for "
         "an FPS-style relative cursor — the compositor owns any warp.")
;;INTENTION("xdg-shell's toplevel.configure carries no minimize event, so "
         "isMinimized reads the last set_minimized request and restored "
         "derives from any configure that carries a size. Server-side "
         "decorations mean setUndecorated cannot be enforced from clients; "
         "it is stored like the other chrome gates.")
;;INTENTION("Window_setDRM has no Wayland analogue; stored-and-inert so the "
         "surface stays stable.")

// Multi-tap window for d64-click style counting (legacy parity: 250ms).
static const u64 kTapThresholdNanos = 250000000ULL;

// --- Global compositor state (one connection, many windows) ------------------

static struct wl_display *gDisplay = nullptr;
static struct wl_registry *gRegistry = nullptr;
static struct wl_compositor *gCompositor = nullptr;
static struct xdg_wm_base *gWmBase = nullptr;
static struct wl_shm *gShm = nullptr;
static struct wl_seat *gSeat = nullptr;
static struct wl_pointer *gPointer = nullptr;
static struct wl_keyboard *gKeyboard = nullptr;
static struct wl_touch *gTouch = nullptr;

// The surface the pointer currently hovers (motion/button/axis carry none).
static struct wl_surface *sPointerSurface = nullptr;

// Keyboard mapping state (xkbcommon). One keymap/state per connection.
static struct xkb_context *gXkb = nullptr;
static struct xkb_keymap *gKeymap = nullptr;
static struct xkb_state *gXkbState = nullptr;

// Cursor theme (loaded lazily on first named cursor request).
static struct wl_cursor_theme *sCursorTheme = nullptr;
static struct wl_surface *sCursorSurface = nullptr;

// Window count drives connection teardown: the LAST Window_destroy disconnects.
static int gWindowCount = 0;

// The opaque handle handed back to C. Holds the native surface chain plus
// every C-visible reflection word — the mirror of the Cocoa struct Window.
// `id` is the engine's small window number (1..N; 0 is the broadcast reserved
// id) used to tag input events and route them to per-window listeners.
// `lifecycle` is the embedded WindowEvent — the app's OS-lifecycle bridge.
struct Window {
   struct wl_surface *surface;
   struct xdg_surface *xdgSurface;
   struct xdg_toplevel *topLevel;
   char *title;
   WindowEvent lifecycle;
   u32 id;
   _Atomic bool shouldClose;
   _Atomic u64 sizeGeneration;
   _Atomic int cachedWidth;
   _Atomic int cachedHeight;
   _Atomic bool liveResizing;

   // Pure-state reflection words (no GPU calls in this file).
   _Atomic int presentMode;
   _Atomic bool transparent;
   _Atomic u64 renderGeneration;

   // Board slots — opaque layer handles consumed by graphvex/darling; this
   // window stores them, never dereferences them. Panels live on the Frame.
   _Atomic(any) topLayer;
   _Atomic(any) bottomLayer;

   _Atomic bool enabled;
   _Atomic bool keyEnabled; // stored; OS-level key refusal is Cocoa-only for now
   bool lastFocused;
   _Atomic u32 monitorId;
   WindowCursorType cursorType;

   // Chrome: stored gates (SSD owns the actual decoration).
   bool decorated;
   bool fullscreen;
   bool minimized;
   int minWidth;
   int minHeight;
   int maxWidth;
   int maxHeight;
   bool movableByBackground;
   bool fullscreenButton;
   bool resizableEnabled;
   bool closableEnabled;
   bool minimizeEnabled;
   bool clickThrough;
   f32 opacity;
   bool shadow;

   // Software present path (retained wl_shm staging, mirror of the retired
   // raster present).
   int frameW;
   int frameH;
   struct wl_buffer *shmBuffer;
   any shmMap;
   bytes shmSize;

   WindowResizeRenderFn resizeRenderFn;
   any resizeRenderUserdata;
};

// --- Id registry -------------------------------------------------------------
//
// Slot i holds the entries for engine id i (index = id, slot 0 reserved for
// FOCUS_BROADCAST). Grows by doubling (the Dynamic Scalability &
// Anti-Hardcoding Law — no fixed window ceiling).
typedef struct WindowSlot {
   struct wl_surface *surface;
   Window *handle;
} WindowSlot;

static WindowSlot *sRefs = nullptr;
static u32 sRefCap = 0;

// Grow the surface-to-window registry geometrically and clear the new slots.
static bool windowRefsGrow() {
   u32 nextCap = (sRefCap == 0) ? 8u : sRefCap * 2u;
   WindowSlot *grown = (WindowSlot*) realloc(sRefs, (bytes) nextCap * sizeof(WindowSlot));
   if (grown == nullptr)
      return false;
   for (u32 i = sRefCap; i < nextCap; i++) {
      grown[i].surface = nullptr;
      grown[i].handle = nullptr;
   }
   sRefs = grown;
   sRefCap = nextCap;
   return true;
}

// Assign a reusable non-broadcast id to a surface and its owning C handle.
static u32 windowIdAcquire(struct wl_surface *surface, Window *handle) {
   if (sRefCap == 0 && !windowRefsGrow())
      return FOCUS_BROADCAST;
   for (u32 i = 1; i < sRefCap; i++) {
      if (sRefs[i].surface == nullptr) {
         sRefs[i].surface = surface;
         sRefs[i].handle = handle;
         return i;
      }
   }
   if (!windowRefsGrow())
      return FOCUS_BROADCAST;
   for (u32 i = 1; i < sRefCap; i++) {
      if (sRefs[i].surface == nullptr) {
         sRefs[i].surface = surface;
         sRefs[i].handle = handle;
         return i;
      }
   }
   return FOCUS_BROADCAST;
}

// Clear a live registry slot when its window is destroyed.
static fn windowIdRelease(u32 id) {
   if (id != FOCUS_BROADCAST && id < sRefCap) {
      sRefs[id].surface = nullptr;
      sRefs[id].handle = nullptr;
   }
}

// Reverse lookup: which engine id does this surface carry? 0 if unknown.
static u32 windowIdOf(struct wl_surface *surface) {
   if (surface == nullptr)
      return FOCUS_BROADCAST;
   for (u32 i = 1; i < sRefCap; i++)
      if (sRefs[i].surface == surface)
         return i;
   return FOCUS_BROADCAST;
}

static Window *windowHandleOf(struct wl_surface *surface) {
   if (surface == nullptr)
      return nullptr;
   for (u32 i = 1; i < sRefCap; i++)
      if (sRefs[i].surface == surface)
         return sRefs[i].handle;
   return nullptr;
}

// --- Output tracking (monitor identity mirror) -------------------------------

typedef struct WaylandOutput {
   struct wl_output *output;
   u32 id;
} WaylandOutput;

static WaylandOutput *sOutputs = nullptr;
static u32 sOutputCount = 0;
static u32 sOutputCap = 0;

// Resolve an output proxy to its stable engine-facing monitor id.
static u32 waylandOutputIdFor(struct wl_output *output) {
   for (u32 i = 0; i < sOutputCount; i++)
      if (sOutputs[i].output == output)
         return sOutputs[i].id;
   return 0;
}

// --- Composition helpers -----------------------------------------------------

// Resize reflection: the configure carries the new surface size (px); compare
// against the cache and bump sizegen + fire onResized + run the resize hook
// only on an actual change. Thread 0 only.
static fn windowRefreshSize(Window *window, int cw, int ch) {
   if (window == nullptr)
      return;
   if (cw != atomic_load_explicit(&(*window).cachedWidth, memory_order_relaxed)
      || ch != atomic_load_explicit(&(*window).cachedHeight, memory_order_relaxed)) {
      atomic_store_explicit(&(*window).cachedWidth, cw, memory_order_relaxed);
      atomic_store_explicit(&(*window).cachedHeight, ch, memory_order_relaxed);
      atomic_fetch_add_explicit(&(*window).sizeGeneration, 1, memory_order_release);
      WindowEvent_fireResized(&(*window).lifecycle, window, cw, ch);
      if ((*window).resizeRenderFn)
         (*window).resizeRenderFn((*window).resizeRenderUserdata);
   }
}

// Keyboard focus mirror, driven by wl_keyboard enter/leave (the key window in
// the Wayland dialect). Fires the focus slots only on flips.
static fn windowRefreshFocus(Window *window, bool focused) {
   if (window == nullptr)
      return;
   if (focused != (*window).lastFocused) {
      (*window).lastFocused = focused;
      if (focused)
         WindowEvent_fireFocusGained(&(*window).lifecycle, window);
      else
         WindowEvent_fireFocusLost(&(*window).lifecycle, window);
   }
}

// --- Socket-level pump (the poll half of poll-then-tick) ---------------------
//
// The canonical non-blocking read: reserve the read slot, flush pending
// requests, drain events, then dispatch. If another thread already took the
// slot, just dispatch what is pending. Never blocks — exactly the same
// non-blocking contract as Cocoa's dispatchSource tap.
static fn waylandDrain() {
   if (gDisplay == nullptr)
      return;
   if (wl_display_prepare_read(gDisplay) == -1) {
      wl_display_dispatch_pending(gDisplay);
      return;
   }
   if (wl_display_flush(gDisplay) == -1 && errno != EAGAIN) {
      wl_display_cancel_read(gDisplay);
      return;
   }
   wl_display_read_events(gDisplay);
   wl_display_dispatch_pending(gDisplay);
}

// --- xdg-shell listeners -----------------------------------------------------

// Acknowledge each compositor configure serial for this surface.
static fn xdgSurfaceConfigure(any data, struct xdg_surface *surface, u32 serial) {
   Window *window = (Window*) data;
   if (window == nullptr)
      return;
   xdg_surface_ack_configure(surface, serial);
}

static const struct xdg_surface_listener sXdgSurfaceListener = {
   .configure = xdgSurfaceConfigure,
};

// Reflect compositor state, resize and restore notifications into the C window.
static fn toplevelConfigure(any data, struct xdg_toplevel *topLevel, i32 width,
                       i32 height, struct wl_array *states) {
   Window *window = (Window*) data;
   if (window == nullptr)
      return;
   (fn) topLevel;
   bool fs = false;
   u32 *state = nullptr;
   wl_array_for_each(state, states) {
      if (*state == XDG_TOPLEVEL_STATE_FULLSCREEN)
         fs = true;
      else if (*state == XDG_TOPLEVEL_STATE_MAXIMIZED)
         fs = false;
   }
   if (fs != (*window).fullscreen) {
      (*window).fullscreen = fs;
      if (fs)
         WindowEvent_fireFullscreen(&(*window).lifecycle, window);
      else
         WindowEvent_fireRestored(&(*window).lifecycle, window);
   }
   // A configured size means we are mapped/normal again: restored.
   if ((*window).minimized && width > 0 && height > 0) {
      (*window).minimized = false;
      WindowEvent_fireRestored(&(*window).lifecycle, window);
   }
   if (width > 0 && height > 0)
      windowRefreshSize(window, (int) width, (int) height);
}

// Honor the vetoable quit callback and latch close only when it permits.
static fn toplevelClose(any data, struct xdg_toplevel *topLevel) {
   Window *window = (Window*) data;
   if (window == nullptr)
      return;
   (fn) topLevel;
   // Quit is the vetoable slot — the same contract as Cocoa's
   // windowShouldClose. A decline keeps the window alive (the compositor
   // stays out of it; we simply never tear down).
   if (WindowEvent_fireQuitRequested(&(*window).lifecycle, window))
      atomic_store_explicit(&(*window).shouldClose, true, memory_order_relaxed);
}

static const struct xdg_toplevel_listener sToplevelListener = {
   .configure = toplevelConfigure,
   .close = toplevelClose,
};

// Reply to the compositor's xdg-shell liveness ping.
static fn wmBasePing(any data, struct xdg_wm_base *wmBase, u32 serial) {
   (fn) data;
   xdg_wm_base_pong(wmBase, serial);
}

static const struct xdg_wm_base_listener sWmBaseListener = {
   .ping = wmBasePing,
};

// --- Keyboard (xkbcommon) ----------------------------------------------------

// xkb keycode = evdev scan code + 8. Map evdev codes onto GLFW-style KEY_*
// values. The table answers the physical key; -1 = unmapped (rare keys fall
// back to a keysym lookup below).
static int sEvdevMap[256] = {
   [0 ... 255] = -1,
   [1] = KEY_ESCAPE,
   [2] = KEY_NUM_1, [3] = KEY_NUM_2, [4] = KEY_NUM_3, [5] = KEY_NUM_4,
   [6] = KEY_NUM_5, [7] = KEY_NUM_6, [8] = KEY_NUM_7, [9] = KEY_NUM_8,
   [10] = KEY_NUM_9, [11] = KEY_NUM_0,
   [14] = KEY_BACKSPACE, [15] = KEY_TAB, [28] = KEY_ENTER,
   [16] = KEY_Q, [17] = KEY_W, [18] = KEY_E, [19] = KEY_R, [20] = KEY_T,
   [21] = KEY_Y, [22] = KEY_U, [23] = KEY_I, [24] = KEY_O, [25] = KEY_P,
   [26] = KEY_LEFT_BRACKET, [27] = KEY_RIGHT_BRACKET, [43] = KEY_BACKSLASH,
   [30] = KEY_A, [31] = KEY_S, [32] = KEY_D, [33] = KEY_F, [34] = KEY_G,
   [35] = KEY_H, [36] = KEY_J, [37] = KEY_K, [38] = KEY_L,
   [39] = KEY_SEMICOLON, [40] = KEY_APOSTROPHE, [41] = KEY_GRAVE_ACCENT,
   [44] = KEY_Z, [45] = KEY_X, [46] = KEY_C, [47] = KEY_V, [48] = KEY_B,
   [49] = KEY_N, [50] = KEY_M,
   [51] = KEY_COMMA, [52] = KEY_PERIOD, [53] = KEY_SLASH, [57] = KEY_SPACE,
   [58] = KEY_CAPS_LOCK,
   [59] = KEY_F1, [60] = KEY_F2, [61] = KEY_F3, [62] = KEY_F4,
   [63] = KEY_F5, [64] = KEY_F6, [65] = KEY_F7, [66] = KEY_F8,
   [67] = KEY_F9, [68] = KEY_F10, [87] = KEY_F11, [88] = KEY_F12,
   [69] = KEY_NUM_LOCK, [70] = KEY_SCROLL_LOCK,
   [99] = KEY_PRINT_SCREEN, [119] = KEY_PAUSE,
   [103] = KEY_UP, [105] = KEY_LEFT, [106] = KEY_RIGHT, [108] = KEY_DOWN,
   [110] = KEY_INSERT, [111] = KEY_HOME, [112] = KEY_PAGE_UP,
   [113] = KEY_DELETE, [114] = KEY_END, [115] = KEY_PAGE_DOWN,
   [29] = KEY_LEFT_CONTROL, [42] = KEY_LEFT_SHIFT, [56] = KEY_LEFT_ALT,
   [125] = KEY_LEFT_SUPER,
   [54] = KEY_RIGHT_SHIFT, [97] = KEY_RIGHT_CONTROL, [100] = KEY_RIGHT_ALT,
   [126] = KEY_RIGHT_SUPER, [127] = KEY_MENU,
};

// Fallback path for keys the evdev table does not cover: fold an xkb keysym
// into the ASCII-aligned KEY_* slots (the a-z and punctuation rows all sit at
// their ASCII values by construction).
static int keyFromSym(xkb_keysym_t sym) {
   if (sym >= XKB_KEY_a && sym <= XKB_KEY_z)
      return (int) (KEY_A + (sym - XKB_KEY_a));
   if (sym >= XKB_KEY_A && sym <= XKB_KEY_Z)
      return (int) (KEY_A + (sym - XKB_KEY_A));
   if (sym == XKB_KEY_0)
      return KEY_NUM_0;
   if (sym >= XKB_KEY_1 && sym <= XKB_KEY_9)
      return (int) (KEY_NUM_1 + (sym - XKB_KEY_1));
   if (sym >= 0x20 && sym <= 0x7e) {
      int c = (int) sym;
      if (c == KEY_APOSTROPHE || c == KEY_COMMA || c == KEY_MINUS
         || c == KEY_PERIOD || c == KEY_SLASH || c == KEY_SEMICOLON
         || c == KEY_EQUAL || c == KEY_LEFT_BRACKET || c == KEY_BACKSLASH
         || c == KEY_RIGHT_BRACKET || c == KEY_GRAVE_ACCENT)
         return c;
   }
   return -1;
}

// Mirror the xkb modifier mask into the engine Key ring as left-hand diffs —
// Wayland's modifier state is side-neutral, so the LEFT constant represents
// both hands (the same synthetic down/up the Cocoa backend pushes).
static fn mirrorModifiers(Window *window) {
   if (window == nullptr || gXkbState == nullptr)
      return;
   u32 wid = (*window).id;
   bool shift = xkb_state_mod_name_is_active(gXkbState, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) != 0;
   if (shift != Key_isDown(KEY_LEFT_SHIFT))
      Key_pushEvent(wid, KEY_LEFT_SHIFT, shift ? KEY_ACTION_DOWN : KEY_ACTION_UP, kTapThresholdNanos);
   bool ctrl = xkb_state_mod_name_is_active(gXkbState, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) != 0;
   if (ctrl != Key_isDown(KEY_LEFT_CONTROL))
      Key_pushEvent(wid, KEY_LEFT_CONTROL, ctrl ? KEY_ACTION_DOWN : KEY_ACTION_UP, kTapThresholdNanos);
   bool alt = xkb_state_mod_name_is_active(gXkbState, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE) != 0;
   if (alt != Key_isDown(KEY_LEFT_ALT))
      Key_pushEvent(wid, KEY_LEFT_ALT, alt ? KEY_ACTION_DOWN : KEY_ACTION_UP, kTapThresholdNanos);
   bool super = xkb_state_mod_name_is_active(gXkbState, XKB_MOD_NAME_LOGO, XKB_STATE_MODS_EFFECTIVE) != 0;
   if (super != Key_isDown(KEY_LEFT_SUPER))
      Key_pushEvent(wid, KEY_LEFT_SUPER, super ? KEY_ACTION_DOWN : KEY_ACTION_UP, kTapThresholdNanos);
}

static fn keyboardKeymap(any data, struct wl_keyboard *keyboard, u32 format,
                     i32 fd, u32 size) {
   (fn) data;
   (fn) keyboard;
   if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 || size == 0) {
      close(fd);
      return;
   }
   char *mapStr = (char*) mmap(nullptr, (bytes) size, PROT_READ, MAP_PRIVATE, fd, 0);
   if (mapStr == MAP_FAILED) {
      close(fd);
      return;
   }
   struct xkb_keymap *keymap = xkb_keymap_new_from_string(
      gXkb, mapStr, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
   munmap(mapStr, size);
   close(fd);
   if (keymap == nullptr)
      return;
   if (gKeymap)
      xkb_keymap_unref(gKeymap);
   gKeymap = keymap;
   if (gXkbState)
      xkb_state_unref(gXkbState);
   gXkbState = xkb_state_new(gKeymap);
}

static fn keyboardEnter(any data, struct wl_keyboard *keyboard, u32 serial,
                    struct wl_surface *surface, struct wl_array *keys) {
   (fn) keyboard;
   (fn) serial;
   (fn) surface;
   (fn) keys;
   Window *window = (Window*) data;
   if (window == nullptr)
      return;
   windowRefreshFocus(window, true);
   mirrorModifiers(window);
   Focus_set((*window).id);
}

static fn keyboardLeave(any data, struct wl_keyboard *keyboard, u32 serial,
                    struct wl_surface *surface) {
   (fn) keyboard;
   (fn) serial;
   (fn) surface;
   Window *window = (Window*) data;
   if (window == nullptr)
      return;
   if ((*window).lastFocused)
      Focus_set(FOCUS_BROADCAST);
   windowRefreshFocus(window, false);
}

static fn keyboardKey(any data, struct wl_keyboard *keyboard, u32 serial,
                  u32 time, u32 key, u32 state) {
   (fn) keyboard;
   (fn) serial;
   (fn) time;
   Window *window = (Window*) data;
   if (window == nullptr || gXkbState == nullptr)
      return;
   u32 wid = (*window).id;
   int evdev = (int) key - 8;
   int code = (evdev >= 0 && evdev < 256) ? sEvdevMap[evdev] : -1;
   if (code == -1) {
      xkb_keysym_t sym = xkb_state_key_get_one_sym(gXkbState, key);
      code = keyFromSym(sym);
   }
   if (code == -1)
      return;
   bool down = (state == WL_KEYBOARD_KEY_STATE_PRESSED);
   if (down && Key_isDown(code))
      Key_pushEvent(wid, code, KEY_ACTION_REPEAT, kTapThresholdNanos);
   else
      Key_pushEvent(wid, code, down ? KEY_ACTION_DOWN : KEY_ACTION_UP, kTapThresholdNanos);
   if (down) {
      char utf[8] = {0};
      int n = xkb_state_key_get_utf8(gXkbState, key, utf, (bytes) sizeof(utf) - 1);
      if (n > 0)
         for (int i = 0; i < n; i++)
            Key_pushCharEvent(wid, (u32) (unsigned char) utf[i]);
   }
   xkb_state_update_key(gXkbState, key, down ? XKB_KEY_DOWN : XKB_KEY_UP);
   mirrorModifiers(window);
}

static fn keyboardModifiers(any data, struct wl_keyboard *keyboard, u32 serial,
                       u32 modsDepressed, u32 modsLatched,
                       u32 modsLocked, u32 group) {
   (fn) keyboard;
   (fn) serial;
   Window *window = (Window*) data;
   if (window == nullptr || gXkbState == nullptr)
      return;
   xkb_state_update_mask(gXkbState, modsDepressed, modsLatched, modsLocked, 0, 0, group);
   mirrorModifiers(window);
}

static fn keyboardRepeatInfo(any data, struct wl_keyboard *keyboard,
                        i32 rate, i32 delay) {
   (fn) data;
   (fn) keyboard;
   (fn) rate;
   (fn) delay;
}

static const struct wl_keyboard_listener sKeyboardListener = {
   .keymap = keyboardKeymap,
   .enter = keyboardEnter,
   .leave = keyboardLeave,
   .key = keyboardKey,
   .modifiers = keyboardModifiers,
   .repeat_info = keyboardRepeatInfo,
};

// --- Pointer -----------------------------------------------------------------

// Cursor-theme names per WindowCursorType (the freedesktop cursor theme naming
// that every compositor ships). WINDOW_CURSOR_HIDDEN hides outright.
static const char *cursorName(WindowCursorType type) {
   switch (type) {
      case WINDOW_CURSOR_IBEAM:         return "text";
      case WINDOW_CURSOR_POINTING_HAND: return "pointer";
      case WINDOW_CURSOR_CROSSHAIR:     return "crosshair";
      case WINDOW_CURSOR_RESIZE_EW:     return "ew-resize";
      case WINDOW_CURSOR_RESIZE_NS:     return "ns-resize";
      case WINDOW_CURSOR_NOT_ALLOWED:   return "X_cursor";
      default:                          return "left_ptr";
   }
}

// Apply the requested cursor using the current pointer serial and shared theme.
static fn pointerSetCursorFor(u32 serial, WindowCursorType type) {
   if (gPointer == nullptr)
      return;
   if (type == WINDOW_CURSOR_HIDDEN) {
      wl_pointer_set_cursor(gPointer, serial, nullptr, 0, 0);
      if (sCursorSurface) {
         wl_surface_attach(sCursorSurface, nullptr, 0, 0);
         wl_surface_commit(sCursorSurface);
      }
      return;
   }
   if (sCursorTheme == nullptr && gShm != nullptr)
      sCursorTheme = wl_cursor_theme_load(nullptr, gShm, 24);
   if (sCursorTheme == nullptr) {
      wl_pointer_set_cursor(gPointer, serial, nullptr, 0, 0);
      return;
   }
   struct wl_cursor *cur = wl_cursor_theme_get_cursor(sCursorTheme, cursorName(type));
   if (cur == nullptr)
      cur = wl_cursor_theme_get_cursor(sCursorTheme, "left_ptr");
   if (cur == nullptr || (*cur).image_count == 0)
      return;
   struct wl_cursor_image *img = (*cur).images[0];
   if (sCursorSurface == nullptr)
      sCursorSurface = wl_compositor_create_surface(gCompositor);
   if (sCursorSurface == nullptr)
      return;
   wl_surface_attach(sCursorSurface, (*img).buffer, 0, 0);
   wl_surface_damage(sCursorSurface, 0, 0, (int) (*img).width, (int) (*img).height);
   wl_surface_commit(sCursorSurface);
   wl_pointer_set_cursor(gPointer, serial, sCursorSurface, (int) (*img).hotspot_x, (int) (*img).hotspot_y);
}

static fn pointerEnter(any data, struct wl_pointer *pointer, u32 serial,
                   struct wl_surface *surface, wl_fixed_t sx, wl_fixed_t sy) {
   (fn) data;
   (fn) serial;
   (fn) pointer;
   sPointerSurface = surface;
   Window *window = windowHandleOf(surface);
   if (window == nullptr || !atomic_load_explicit(&(*window).enabled, memory_order_relaxed))
      return;
   Mouse_pushMoveEvent((*window).id, wl_fixed_to_f64(sx), wl_fixed_to_f64(sy));
}

static fn pointerLeave(any data, struct wl_pointer *pointer, u32 serial,
                   struct wl_surface *surface) {
   (fn) data;
   (fn) pointer;
   (fn) serial;
   (fn) surface;
   sPointerSurface = nullptr;
}

static fn pointerMotion(any data, struct wl_pointer *pointer, u32 time,
                    wl_fixed_t sx, wl_fixed_t sy) {
   (fn) data;
   (fn) pointer;
   (fn) time;
   Window *win = windowHandleOf(sPointerSurface);
   if (win == nullptr || !atomic_load_explicit(&(*win).enabled, memory_order_relaxed))
      return;
   d64 x = wl_fixed_to_f64(sx);
   d64 y = wl_fixed_to_f64(sy);
   if ((*win).cursorType == WINDOW_CURSOR_HIDDEN) {
      // Cursor-lock path: the compositor owns the pointer, we only emit
      // deltas from successive absolute positions (Wayland has no warp).
      static d64 sLastX = 0.0;
      static d64 sLastY = 0.0;
      static bool sHasLast = false;
      if (sHasLast)
         Mouse_pushMoveDeltaEvent((*win).id, x - sLastX, y - sLastY);
      sLastX = x;
      sLastY = y;
      sHasLast = true;
      return;
   }
   Mouse_pushMoveEvent((*win).id, x, y);
}

static fn pointerButton(any data, struct wl_pointer *pointer, u32 serial,
                    u32 time, u32 button, u32 state) {
   (fn) data;
   (fn) pointer;
   (fn) time;
   Window *win = windowHandleOf(sPointerSurface);
   if (win == nullptr || !atomic_load_explicit(&(*win).enabled, memory_order_relaxed))
      return;
   int mbutton = -1;
   if (button == BTN_LEFT)
      mbutton = MOUSE_LEFT;
   else if (button == BTN_RIGHT)
      mbutton = MOUSE_RIGHT;
   else if (button == BTN_MIDDLE)
      mbutton = MOUSE_MIDDLE;
   if (mbutton < 0)
      return;
   bool down = (state == WL_POINTER_BUTTON_STATE_PRESSED);
   Mouse_pushButtonEvent((*win).id, mbutton,
                    down ? KEY_ACTION_DOWN : KEY_ACTION_UP, kTapThresholdNanos);
   if (down)
      WindowEvent_firePressed(&(*win).lifecycle, win);
   pointerSetCursorFor(serial, (*win).cursorType);
}

static fn pointerAxis(any data, struct wl_pointer *pointer, u32 time,
                  u32 axis, wl_fixed_t value) {
   (fn) data;
   (fn) pointer;
   (fn) time;
   Window *win = windowHandleOf(sPointerSurface);
   if (win == nullptr || !atomic_load_explicit(&(*win).enabled, memory_order_relaxed))
      return;
   d64 v = wl_fixed_to_f64(value);
   if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
      Mouse_pushScrollEvent((*win).id, 0.0, v);
   else if (axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL)
      Mouse_pushScrollEvent((*win).id, v, 0.0);
}

// Complete a pointer event batch; no additional frame state is retained.
static fn pointerFrame(any data, struct wl_pointer *pointer) {
   (fn) data;
   (fn) pointer;
}

static const struct wl_pointer_listener sPointerListener = {
   .enter = pointerEnter,
   .leave = pointerLeave,
   .motion = pointerMotion,
   .button = pointerButton,
   .axis = pointerAxis,
   .frame = pointerFrame,
};

// --- Touch -------------------------------------------------------------------

static fn touchDown(any data, struct wl_touch *touch, u32 serial,
                 u32 time, struct wl_surface *surface, i32 id,
                 wl_fixed_t sx, wl_fixed_t sy) {
   (fn) data;
   (fn) touch;
   (fn) serial;
   (fn) time;
   Window *win = windowHandleOf(surface);
   if (win == nullptr || !atomic_load_explicit(&(*win).enabled, memory_order_relaxed))
      return;
   int slot = (int) (id % TOUCH_MAX);
   Touch_pushTouchEvent((*win).id, slot, TOUCH_DOWN,
      wl_fixed_to_f64(sx), wl_fixed_to_f64(sy), 1.0, kTapThresholdNanos);
}

static fn touchUp(any data, struct wl_touch *touch, u32 serial, u32 time,
               i32 id) {
   (fn) data;
   (fn) touch;
   (fn) serial;
   (fn) time;
   Window *win = windowHandleOf(wl_touch_get_surface(touch));
   if (win == nullptr)
      return;
   int slot = (int) (id % TOUCH_MAX);
   Touch_pushTouchEvent((*win).id, slot, TOUCH_UP, 0.0, 0.0, 0.0, kTapThresholdNanos);
}

static fn touchMotion(any data, struct wl_touch *touch, u32 time, i32 id,
                  wl_fixed_t sx, wl_fixed_t sy) {
   (fn) data;
   (fn) touch;
   (fn) time;
   Window *win = windowHandleOf(wl_touch_get_surface(touch));
   if (win == nullptr)
      return;
   int slot = (int) (id % TOUCH_MAX);
   Touch_pushTouchEvent((*win).id, slot, TOUCH_MOVE,
      wl_fixed_to_f64(sx), wl_fixed_to_f64(sy), 1.0, kTapThresholdNanos);
}

static fn touchCancel(any data, struct wl_touch *touch, u32 serial, u32 time,
                  i32 id) {
   (fn) data;
   (fn) touch;
   (fn) serial;
   (fn) time;
   Window *win = windowHandleOf(wl_touch_get_surface(touch));
   if (win == nullptr)
      return;
   int slot = (int) (id % TOUCH_MAX);
   Touch_pushTouchEvent((*win).id, slot, TOUCH_CANCEL, 0.0, 0.0, 0.0, kTapThresholdNanos);
}

// Complete a touch event batch; individual events were already forwarded.
static fn touchFrame(any data, struct wl_touch *touch) {
   (fn) data;
   (fn) touch;
}

static const struct wl_touch_listener sTouchListener = {
   .down = touchDown,
   .up = touchUp,
   .motion = touchMotion,
   .frame = touchFrame,
   .cancel = touchCancel,
};

// --- Seat --------------------------------------------------------------------

static fn seatCapabilities(any data, struct wl_seat *seat, u32 capabilities) {
   (fn) data;
   if ((capabilities & WL_SEAT_CAPABILITY_POINTER) != 0 && gPointer == nullptr) {
      gPointer = wl_seat_get_pointer(seat);
      wl_pointer_add_listener(gPointer, &sPointerListener, nullptr);
   } else if ((capabilities & WL_SEAT_CAPABILITY_POINTER) == 0 && gPointer != nullptr) {
      wl_pointer_destroy(gPointer);
      gPointer = nullptr;
   }
   if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) != 0 && gKeyboard == nullptr) {
      gKeyboard = wl_seat_get_keyboard(seat);
      wl_keyboard_add_listener(gKeyboard, &sKeyboardListener, nullptr);
   } else if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) == 0 && gKeyboard != nullptr) {
      wl_keyboard_destroy(gKeyboard);
      gKeyboard = nullptr;
   }
   if ((capabilities & WL_SEAT_CAPABILITY_TOUCH) != 0 && gTouch == nullptr) {
      gTouch = wl_seat_get_touch(seat);
      wl_touch_add_listener(gTouch, &sTouchListener, nullptr);
   } else if ((capabilities & WL_SEAT_CAPABILITY_TOUCH) == 0 && gTouch != nullptr) {
      wl_touch_destroy(gTouch);
      gTouch = nullptr;
   }
}

static const struct wl_seat_listener sSeatListener = {
   .capabilities = seatCapabilities,
};

// --- Output ------------------------------------------------------------------

static fn outputGeometry(any data, struct wl_output *output, i32 x, i32 y,
                     i32 physicalWidth, i32 physicalHeight, i32 subpixel,
                     const char *make, const char *model, i32 transform) {
   (fn) data;
   (fn) output;
   (fn) x;
   (fn) y;
   (fn) physicalWidth;
   (fn) physicalHeight;
   (fn) subpixel;
   (fn) make;
   (fn) model;
   (fn) transform;
}

static fn outputMode(any data, struct wl_output *output, u32 flags,
                  i32 width, i32 height, i32 refresh) {
   (fn) data;
   (fn) output;
   (fn) flags;
   (fn) width;
   (fn) height;
   (fn) refresh;
}

// Consume the output batch terminator; output geometry is not cached here.
static fn outputDone(any data, struct wl_output *output) {
   (fn) data;
   (fn) output;
}

static const struct wl_output_listener sOutputListener = {
   .geometry = outputGeometry,
   .mode = outputMode,
   .done = outputDone,
};

// Register an output proxy and assign its session-stable monitor id.
static fn outputAdd(struct wl_output *output) {
   if (sOutputCount == sOutputCap) {
      u32 nextCap = (sOutputCap == 0) ? 4u : sOutputCap * 2u;
      WaylandOutput *grown = (WaylandOutput*) realloc(sOutputs, (bytes) nextCap * sizeof(WaylandOutput));
      if (grown == nullptr)
         return;
      sOutputs = grown;
      sOutputCap = nextCap;
   }
   sOutputs[sOutputCount].output = output;
   sOutputs[sOutputCount].id = sOutputCount + 1u; // stable per session
   sOutputCount++;
}

// Mark a removed output slot inactive without renumbering other monitors.
static fn outputRemove(struct wl_output *output) {
   for (u32 i = 0; i < sOutputCount; i++)
      if (sOutputs[i].output == output) {
         sOutputs[i].output = nullptr;
         return;
      }
}

// Surface enter/leave: the compositor tells us which output carries the
// window — the mirror of the Cocoa greatest-intersection resolver.
static fn surfaceEnter(any data, struct wl_surface *surface, struct wl_output *output) {
   Window *window = (Window*) data;
   if (window == nullptr)
      return;
   (fn) surface;
   u32 id = waylandOutputIdFor(output);
   if (id != 0)
      atomic_store_explicit(&(*window).monitorId, id, memory_order_relaxed);
}

// Clear the monitor mirror when the surface leaves its currently assigned output.
static fn surfaceLeave(any data, struct wl_surface *surface, struct wl_output *output) {
   Window *window = (Window*) data;
   if (window == nullptr)
      return;
   (fn) surface;
   if (waylandOutputIdFor(output) == atomic_load_explicit(&(*window).monitorId, memory_order_relaxed))
      atomic_store_explicit(&(*window).monitorId, 0, memory_order_relaxed);
}

static const struct wl_surface_listener sSurfaceListener = {
   .enter = surfaceEnter,
   .leave = surfaceLeave,
};

// --- Registry ----------------------------------------------------------------

static fn registryGlobal(any data, struct wl_registry *registry, u32 name,
                     const char *interface, u32 version) {
   (fn) data;
   if (strcmp(interface, wl_compositor_interface.name) == 0) {
      if (gCompositor == nullptr)
         gCompositor = (struct wl_compositor*) wl_registry_bind(registry, name,
            &wl_compositor_interface, version < 4 ? version : 4);
   } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
      if (gWmBase == nullptr)
         gWmBase = (struct xdg_wm_base*) wl_registry_bind(registry, name,
            &xdg_wm_base_interface, 1);
      if (gWmBase)
         xdg_wm_base_add_listener(gWmBase, &sWmBaseListener, nullptr);
   } else if (strcmp(interface, wl_shm_interface.name) == 0) {
      if (gShm == nullptr)
         gShm = (struct wl_shm*) wl_registry_bind(registry, name, &wl_shm_interface, 1);
   } else if (strcmp(interface, wl_seat_interface.name) == 0) {
      if (gSeat == nullptr) {
         gSeat = (struct wl_seat*) wl_registry_bind(registry, name,
            &wl_seat_interface, version < 7 ? version : 7);
         wl_seat_add_listener(gSeat, &sSeatListener, nullptr);
      }
   } else if (strcmp(interface, wl_output_interface.name) == 0) {
      struct wl_output *output = (struct wl_output*) wl_registry_bind(registry, name,
         &wl_output_interface, 1);
      if (output) {
         wl_output_add_listener(output, &sOutputListener, nullptr);
         outputAdd(output);
      }
   }
}

// Ignore registry removals; bound globals remain owned until disconnect.
static fn registryGlobalRemove(any data, struct wl_registry *registry, u32 name) {
   (fn) data;
   (fn) registry;
   (fn) name;
}

static const struct wl_registry_listener sRegistryListener = {
   .global = registryGlobal,
   .global_remove = registryGlobalRemove,
};

// Connect once per process; any later window rides the same socket. Refcounted
// by gWindowCount so the LAST Window_destroy tears the connection down.
static bool waylandConnect() {
   if (gDisplay != nullptr)
      return true;
   gDisplay = wl_display_connect(nullptr);
   if (gDisplay == nullptr)
      return false;
   gRegistry = wl_display_get_registry(gDisplay);
   wl_registry_add_listener(gRegistry, &sRegistryListener, nullptr);
   if (wl_display_roundtrip(gDisplay) == -1)
      return false;
   gXkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
   return true;
}

// Release shared cursor, input, output, and protocol objects after the last window.
static fn waylandDisconnect() {
   if (sCursorSurface) {
      wl_surface_destroy(sCursorSurface);
      sCursorSurface = nullptr;
   }
   if (sCursorTheme) {
      wl_cursor_theme_destroy(sCursorTheme);
      sCursorTheme = nullptr;
   }
   if (gXkbState) {
      xkb_state_unref(gXkbState);
      gXkbState = nullptr;
   }
   if (gKeymap) {
      xkb_keymap_unref(gKeymap);
      gKeymap = nullptr;
   }
   if (gXkb) {
      xkb_context_unref(gXkb);
      gXkb = nullptr;
   }
   if (gRegistry) {
      wl_registry_destroy(gRegistry);
      gRegistry = nullptr;
   }
   if (gDisplay) {
      wl_display_disconnect(gDisplay);
      gDisplay = nullptr;
   }
   gCompositor = nullptr;
   gWmBase = nullptr;
   gShm = nullptr;
   gSeat = nullptr;
   gPointer = nullptr;
   gKeyboard = nullptr;
   gTouch = nullptr;
   sPointerSurface = nullptr;
   free(sOutputs);
   sOutputs = nullptr;
   sOutputCount = 0;
   sOutputCap = 0;
   free(sRefs);
   sRefs = nullptr;
   sRefCap = 0;
}

// --- Software present (retained wl_shm pool + buffer) ------------------------

static u32 sShmNameSeq = 0;

// Rebuild the retained shm buffer + mapping when the frame size changes. A
// fresh pool is created when the size grows so the compositor can still be
// holding the old buffer; the pool object dies right after the buffer is
// created (the buffer keeps the pool's memory alive server-side).
static bool windowEnsureShm(Window *window, int w, int h) {
   bytes Bytes = (bytes) w * (bytes) h * 4u;
   if ((*window).shmBuffer && (*window).frameW == w && (*window).frameH == h)
      return (*window).shmMap != nullptr;
   if ((*window).shmBuffer) {
      wl_buffer_destroy((*window).shmBuffer);
      (*window).shmBuffer = nullptr;
   }
   if ((*window).shmMap) {
      munmap((*window).shmMap, (*window).shmSize);
      (*window).shmMap = nullptr;
   }
   (*window).frameW = w;
   (*window).frameH = h;
   (*window).shmSize = Bytes;

   char name[64];
   snprintf(name, sizeof(name), "/vexgraph-shm-%u", sShmNameSeq++);
   int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
   if (fd == -1)
      fd = shm_open(name, O_RDWR | O_CREAT, 0600);
   if (fd == -1)
      return false;
   shm_unlink(name); // the fd keeps the mapping alive; no name left behind
   if (ftruncate(fd, (off_t) Bytes) == -1) {
      close(fd);
      return false;
   }
   any map = mmap(nullptr, Bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
   if (map == MAP_FAILED) {
      close(fd);
      return false;
   }
   struct wl_shm_pool *pool = wl_shm_create_pool(gShm, fd, (int) Bytes);
   (*window).shmBuffer = wl_shm_pool_create_buffer(pool, 0,
      w, h, w * 4, WL_SHM_FORMAT_ARGB8888);
   wl_shm_pool_destroy(pool); // keep only the buffer alive
   close(fd);
   (*window).shmMap = map;
   return true;
}

// --- Constructors ------------------------------------------------------------

// Merge a caller's Desc over the defaults. Unset (zero) fields fall back —
// this is what makes partial designated initializers behave like overloads.
// Mirror of the Cocoa descResolve verbatim (platform-neutral).
static WindowDesc descResolve(const WindowDesc *desc) {
   WindowDesc d = { .title = "vex", .width = 800, .height = 600, .x = 0, .y = 0, .centered = true };
   if (desc == nullptr)
      return d;
   if ((*desc).title)
      d.title = (*desc).title;
   if ((*desc).width > 0)
      d.width = (*desc).width;
   if ((*desc).height > 0)
      d.height = (*desc).height;
   if ((*desc).x != 0 || (*desc).y != 0) {
      d.x = (*desc).x;
      d.y = (*desc).y;
      d.centered = false;
   }
   d.shown = (*desc).shown;
   return d;
}

// Build the wl_surface + xdg_toplevel + C handle. Shared by every constructor.
// The surface is created MAPPED-HIDDEN: it only ever shows content once a
// first commit carries a buffer (the software present path), so
// construct -> mutate -> show never flashes a half-configured window.
static Window *waylandAlloc(const WindowDesc *desc) {
   if (!waylandConnect())
      return nullptr;
   WindowDesc d = descResolve(desc);
   Window *window = (Window*) calloc(1, sizeof(Window));
   if (window == nullptr)
      return nullptr;

   if (gCompositor == nullptr || gWmBase == nullptr) {
      free(window);
      return nullptr;
   }
   (*window).surface = wl_compositor_create_surface(gCompositor);
   if ((*window).surface == nullptr) {
      free(window);
      return nullptr;
   }
   wl_surface_add_listener((*window).surface, &sSurfaceListener, window);
   (*window).xdgSurface = xdg_wm_base_get_xdg_surface(gWmBase, (*window).surface);
   if ((*window).xdgSurface == nullptr) {
      wl_surface_destroy((*window).surface);
      free(window);
      return nullptr;
   }
   xdg_surface_add_listener((*window).xdgSurface, &sXdgSurfaceListener, window);
   (*window).topLevel = xdg_surface_get_toplevel((*window).xdgSurface);
   if ((*window).topLevel == nullptr) {
      xdg_surface_destroy((*window).xdgSurface);
      wl_surface_destroy((*window).surface);
      free(window);
      return nullptr;
   }
   xdg_toplevel_add_listener((*window).topLevel, &sToplevelListener, window);

   (*window).title = strdup(d.title ? d.title : "vex");
   xdg_toplevel_set_title((*window).topLevel, (*window).title ? (*window).title : "vex");
   xdg_toplevel_set_app_id((*window).topLevel, "vexgraph");

   WindowEvent_init(&(*window).lifecycle);
   atomic_store_explicit(&(*window).shouldClose, false, memory_order_relaxed);
   atomic_store_explicit(&(*window).sizeGeneration, 0, memory_order_relaxed);
   atomic_store_explicit(&(*window).cachedWidth, d.width, memory_order_relaxed);
   atomic_store_explicit(&(*window).cachedHeight, d.height, memory_order_relaxed);
   atomic_store_explicit(&(*window).presentMode, WINDOW_PRESENT_FIFO, memory_order_relaxed);
   atomic_store_explicit(&(*window).transparent, false, memory_order_relaxed);
   atomic_store_explicit(&(*window).renderGeneration, 0, memory_order_relaxed);
   atomic_store_explicit(&(*window).topLayer, nullptr, memory_order_relaxed);
   atomic_store_explicit(&(*window).bottomLayer, nullptr, memory_order_relaxed);
   atomic_store_explicit(&(*window).enabled, true, memory_order_relaxed);
   atomic_store_explicit(&(*window).keyEnabled, true, memory_order_relaxed);
   atomic_store_explicit(&(*window).monitorId, 0, memory_order_relaxed);
   (*window).lastFocused = false;
   (*window).cursorType = WINDOW_CURSOR_DEFAULT;
   (*window).decorated = true;
   (*window).resizableEnabled = true;
   (*window).closableEnabled = true;
   (*window).minimizeEnabled = true;
   (*window).fullscreenButton = true;
   (*window).resizeRenderFn = nullptr;
   (*window).resizeRenderUserdata = nullptr;
   (*window).id = windowIdAcquire((*window).surface, window);

   gWindowCount++;
   return window;
}

// Construct a default Wayland window through the common descriptor path.
Window *Window_0() {
   return Window_new(nullptr);
}

// Construct a Wayland window with the supplied title and default geometry.
Window *Window_1(const char *title) {
   return Window_new(&(WindowDesc){ .title = title });
}

// Construct a Wayland window with a title and explicit pixel dimensions.
Window *Window_3(const char *title, int width, int height) {
   return Window_new(&(WindowDesc){ .title = title, .width = width, .height = height });
}

// Resolve defaults, allocate the protocol surface chain, and show if requested.
Window *Window_new(const WindowDesc *desc) {
   WindowDesc d = descResolve(desc);
   Window *window = waylandAlloc(&d);
   if (window == nullptr)
      return nullptr;
   if (d.shown)
      Window_show(window);
   return window;
}

// Preserve the legacy constructor spelling by forwarding to Window_new.
Window *Window_create(const char *title, int width, int height) {
   return Window_new(&(WindowDesc){ .title = title, .width = width, .height = height });
}

// Tear down the surface + handle. Safe regardless of whether the compositor
// already closed it: object destruction is idempotent here, and every listener
// is detached by destroying the objects before the handle dies — no callback
// can touch our freed memory (mirror of the delegate = nil step).
fn Window_destroy(Window *window) {
   if (window == nullptr)
      return;
   if ((*window).topLevel)
      xdg_toplevel_destroy((*window).topLevel);
   if ((*window).xdgSurface)
      xdg_surface_destroy((*window).xdgSurface);
   if ((*window).surface)
      wl_surface_destroy((*window).surface);
   if ((*window).shmBuffer)
      wl_buffer_destroy((*window).shmBuffer);
   if ((*window).shmMap)
      munmap((*window).shmMap, (*window).shmSize);
   Key_detachWindowAll((*window).id);
   Mouse_detachWindowAll((*window).id);
   Touch_detachWindowAll((*window).id);
   windowIdRelease((*window).id);
   free((*window).title);
   free(window);
   if (gWindowCount > 0)
      gWindowCount--;
   if (gWindowCount == 0)
      waylandDisconnect();
}

// Read the close latch, treating a missing window as already closed.
bool Window_shouldClose(Window *window) {
   return window ? atomic_load_explicit(&(*window).shouldClose, memory_order_relaxed) : true;
}

// Set the close latch observed by the host lifecycle loop.
fn Window_setShouldClose(Window *window, bool shouldClose) {
   if (window != nullptr)
      atomic_store_explicit(&(*window).shouldClose, shouldClose, memory_order_relaxed);
}

// Destroy the native protocol surface chain and mark the handle closed.
fn Window_close(Window *window) {
   if (!window) return;
   if ((*window).topLevel) { xdg_toplevel_destroy((*window).topLevel); (*window).topLevel = nullptr; }
   if ((*window).xdgSurface) { xdg_surface_destroy((*window).xdgSurface); (*window).xdgSurface = nullptr; }
   if ((*window).surface) { wl_surface_destroy((*window).surface); (*window).surface = nullptr; }
   atomic_store_explicit(&(*window).shouldClose, true, memory_order_relaxed);
}

// --- Poll loop ---------------------------------------------------------------

fn Window_pollEvents() {
   if (gDisplay == nullptr)
      return;
   waylandDrain();
}

// Run the batch Wayland pump; this backend does not expose event-by-event stepping.
bool Window_pollEventStep() {
   Window_pollEvents();
   return false; // Wayland's existing batch pump owns its dispatch boundary.
}

// --- Present policy (pure state; a future render path consumes it) ----------

fn Window_setPresentMode(Window *window, int mode) {
   if (window == nullptr)
      return;
   int prev = atomic_exchange_explicit(&(*window).presentMode, mode, memory_order_relaxed);
   if (prev != mode)
      atomic_fetch_add_explicit(&(*window).renderGeneration, 1, memory_order_release);
}

// Return the stored presentation policy, defaulting to FIFO for a null handle.
int Window_getPresentMode(const Window *window) {
   if (window == nullptr)
      return WINDOW_PRESENT_FIFO;
   return atomic_load_explicit(&(*window).presentMode, memory_order_relaxed);
}

// Update transparency policy and advance the render generation when it changes.
fn Window_setTransparent(Window *window, bool transparent) {
   if (window == nullptr)
      return;
   bool prev = atomic_exchange_explicit(&(*window).transparent, transparent, memory_order_relaxed);
   if (prev != transparent)
      atomic_fetch_add_explicit(&(*window).renderGeneration, 1, memory_order_release);
}

// Read the stored transparency policy, returning false for a null handle.
bool Window_isTransparent(const Window *window) {
   return window ? atomic_load_explicit(&(*window).transparent, memory_order_relaxed) : false;
}

// Read the render-policy generation with acquire ordering.
u64 Window_renderGeneration(const Window *window) {
   return window ? atomic_load_explicit(&(*window).renderGeneration, memory_order_acquire) : 0;
}

// --- Graphics board slots (stored + ordered; rendering lives elsewhere) -----

// ;;INTENTION("orderLayers on the lean window is a pure no-op — the window no
// longer parents render layers; the still-unmigrated darling compositor
// retains the call. It retires with the composite seam.")
fn Window_orderLayers(Window *window) {
   (fn) window;
}

// Store the opaque lower-layer handle; this backend does not render it.
fn Window_setBottomLayer(Window *window, any layer) {
   if (window == nullptr)
      return;
   atomic_store_explicit(&(*window).bottomLayer, layer, memory_order_release);
   Window_orderLayers(window);
}

// Return the opaque lower-layer handle without interpreting it.
any Window_getBottomLayer(const Window *window) {
   return window ? atomic_load_explicit(&(*window).bottomLayer, memory_order_acquire) : nullptr;
}

// Store the opaque upper-layer handle; this backend does not render it.
fn Window_setTopLayer(Window *window, any layer) {
   if (window == nullptr)
      return;
   atomic_store_explicit(&(*window).topLayer, layer, memory_order_release);
   Window_orderLayers(window);
}

// Return the opaque upper-layer handle without interpreting it.
any Window_getTopLayer(const Window *window) {
   return window ? atomic_load_explicit(&(*window).topLayer, memory_order_acquire) : nullptr;
}

// ;;INTENTION("Pane/board compositing is inert on the lean window (the same
// slice the Cocoa backend keeps inert); pane work migrates to the render
// personal' own pass. Retires together with the composite seam.")
bool Window_attachPanes(Window *window, Panel *panel, int width, int height) {
   (fn) window;
   (fn) panel;
   (fn) width;
   (fn) height;
   return false;
}

// Retain the legacy pane API as an inert failure on the lean window backend.
bool Window_resizePanes(Window *window, Panel *panel, int width, int height) {
   (fn) window;
   (fn) panel;
   (fn) width;
   (fn) height;
   return false;
}

// Keep the retired pane-compositor entry point linkable without rendering.
fn Window_compositePanes(Window *window, Panel *contentPanel) {
   (fn) window;
   (fn) contentPanel;
}

// Keep the retired board-compositor entry point linkable without rendering.
fn Window_compositeBoards(Window *window) {
   (fn) window;
}

// --- Title / size / position -------------------------------------------------

fn Window_setTitle(Window *window, const char *title) {
   if (window == nullptr || title == nullptr)
      return;
   free((*window).title);
   (*window).title = strdup(title);
   xdg_toplevel_set_title((*window).topLevel, (*window).title ? (*window).title : "");
}

// Read the most recently configured client width in physical pixels.
int Window_width(Window *window) {
   return window ? atomic_load_explicit(&(*window).cachedWidth, memory_order_relaxed) : 0;
}

// Read the most recently configured client height in physical pixels.
int Window_height(Window *window) {
   return window ? atomic_load_explicit(&(*window).cachedHeight, memory_order_relaxed) : 0;
}

// Report unit scale because this Wayland mirror has no fractional-scale support.
f32 Window_getScale(const Window *window) {
   (fn) window;
   return 1.0f;
}

// Provide the display-revalidation seam; this draft backend stores no scale state.
fn Window_revalidate(Window *window) {
   (fn) window;
}

// Forward logical-size requests to the integer pixel sizing seam.
fn Window_setSizePoints(Window *window, f32 width, f32 height) {
   Window_setSize(window, (int) width, (int) height);
}

// Project cached pixel dimensions into the compatibility points outputs.
fn Window_getSizePoints(const Window *window, f32 *outWidth, f32 *outHeight) {
   if (outWidth) *outWidth = (f32) Window_width(window);
   if (outHeight) *outHeight = (f32) Window_height(window);
}

// Return the cached width through the points compatibility accessor.
f32 Window_widthPoints(const Window *window) {
   return (f32) Window_width(window);
}

// Return the cached height through the points compatibility accessor.
f32 Window_heightPoints(const Window *window) {
   return (f32) Window_height(window);
}

// Alias the client width as the viewport width.
int Window_viewportWidth(const Window *window) {
   return Window_width(window);
}

// Alias the client height as the viewport height.
int Window_viewportHeight(const Window *window) {
   return Window_height(window);
}

// Alias the logical width as the viewport width in points.
f32 Window_viewportWidthPoints(const Window *window) {
   return (f32) Window_width(window);
}

// Alias the logical height as the viewport height in points.
f32 Window_viewportHeightPoints(const Window *window) {
   return (f32) Window_height(window);
}

// Alias the client width as the outer-window width in this backend.
int Window_windowWidth(const Window *window) {
   return Window_width(window);
}

// Alias the client height as the outer-window height in this backend.
int Window_windowHeight(const Window *window) {
   return Window_height(window);
}

// Alias the logical client width as the outer-window width in points.
f32 Window_windowWidthPoints(const Window *window) {
   return (f32) Window_width(window);
}

// Alias the logical client height as the outer-window height in points.
f32 Window_windowHeightPoints(const Window *window) {
   return (f32) Window_height(window);
}

// The compositor owns geometry on Wayland; setSize only nudges the configure
// cycle. There is no client-side set_size request on xdg-shell — the toplevel
// configure frames carry the truth and bump sizeGeneration via reflection.
fn Window_setSize(Window *window, int width, int height) {
   if (window == nullptr)
      return;
   (fn) width;
   (fn) height;
   wl_surface_commit((*window).surface);
}

// Accept the placement request; xdg-shell leaves positioning to the compositor.
fn Window_setLocation(Window *window, int x, int y) {
   (fn) window;
   (fn) x;
   (fn) y;
}

// Return zero because Wayland does not expose client-readable toplevel position.
fn Window_getLocation(const Window *window, int *outX, int *outY) {
   if (outX != nullptr)
      *outX = 0;
   if (outY != nullptr)
      *outY = 0;
   (fn) window;
}

// Return zero content origin because compositor placement is not exposed here.
fn Window_getContentOrigin(const Window *window, int *outX, int *outY) {
   if (outX != nullptr)
      *outX = 0;
   if (outY != nullptr)
      *outY = 0;
   (fn) window;
}

// Retain the centering seam; Wayland compositors control window placement.
fn Window_center(Window *window) {
   (fn) window;
}

// Map the surface by committing it without a buffer to start configure exchange.
fn Window_show(Window *window) {
   if (window == nullptr)
      return;
   // Commit with no attached buffer: the surface maps, the compositor runs
   // the configure sequence, and the first present attaches real pixels.
   wl_surface_commit((*window).surface);
}

// Unmap the surface by attaching a null buffer and committing it.
fn Window_hide(Window *window) {
   if (window == nullptr)
      return;
   // Attach a null buffer: the compositor stops compositing the surface.
   wl_surface_attach((*window).surface, nullptr, 0, 0);
   wl_surface_commit((*window).surface);
}

// Select the Wayland show or hide operation from the requested visibility.
fn Window_setVisible(Window *window, bool visible) {
   if (window == nullptr)
      return;
   if (visible)
      Window_show(window);
   else
      Window_hide(window);
}

// --- Runtime state -----------------------------------------------------------

fn Window_setEnabled(Window *window, bool enabled) {
   if (window == nullptr)
      return;
   atomic_store_explicit(&(*window).enabled, enabled, memory_order_relaxed);
}

// Read whether input routing is enabled for this window.
bool Window_isEnabled(const Window *window) {
   return window ? atomic_load_explicit(&(*window).enabled, memory_order_relaxed) : false;
}

// Read the live-resize flag mirrored from compositor configure activity.
bool Window_isLiveResizing(const Window *window) {
   return window ? atomic_load_explicit(&(*window).liveResizing, memory_order_relaxed) : false;
}

// --- Chrome capability toggles (SSD-owned on Wayland; stored for clients) ----

// Read the stored resizable capability; server-side decorations enforce chrome.
bool Window_isResizable(Window *window) {
   return window ? (*window).resizableEnabled : false;
}

// Update the stored resizable capability for clients using server-side chrome.
fn Window_setResizable(Window *window, bool resizable) {
   if (window == nullptr)
      return;
   (*window).resizableEnabled = resizable;
}

// Read the stored closable capability; the compositor owns the titlebar action.
bool Window_isClosable(Window *window) {
   return window ? (*window).closableEnabled : false;
}

// Update the stored closable capability for consumers of the window contract.
fn Window_setClosable(Window *window, bool closable) {
   if (window == nullptr)
      return;
   (*window).closableEnabled = closable;
}

// Read the stored minimize capability maintained for server-side decorations.
bool Window_isMiniaturizable(Window *window) {
   return window ? (*window).minimizeEnabled : false;
}

// Update the stored minimize capability; no client-side chrome is controlled.
fn Window_setMiniaturizable(Window *window, bool miniaturizable) {
   if (window == nullptr)
      return;
   (*window).minimizeEnabled = miniaturizable;
}

// Store whether clients should expose a fullscreen control in their own chrome.
fn Window_setFullscreenButton(Window *window, bool enabled) {
   if (window == nullptr)
      return;
   (*window).fullscreenButton = enabled;
}

// Store the requested chrome mode; the compositor owns server-side decorations.
fn Window_setUndecorated(Window *window, int type) {
   if (window == nullptr)
      return;
   (*window).decorated = (type == WINDOW_DECORATED);
}

// Translate the boolean chrome request to the undecorated-mode setter.
fn Window_setDecorated(Window *window, bool decorated) {
   Window_setUndecorated(window, decorated ? WINDOW_DECORATED : WINDOW_UNDECORATED_BORDERLESS);
}

// Report whether the stored chrome mode is decorated.
bool Window_isDecorated(const Window *window) {
   if (window == nullptr)
      return false;
   return (*window).decorated;
}

// Translate the naked-titlebar request to the undecorated-mode setter.
fn Window_setNaked(Window *window, bool naked) {
   Window_setUndecorated(window, naked ? WINDOW_UNDECORATED_NAKED : WINDOW_DECORATED);
}

// Report the stored undecorated state used as the naked-mode mirror.
bool Window_isNaked(const Window *window) {
   if (window == nullptr)
      return false;
   return !(*window).decorated;
}

// Translate the borderless request to the undecorated-mode setter.
fn Window_setBorderless(Window *window, bool borderless) {
   Window_setUndecorated(window, borderless ? WINDOW_UNDECORATED_BORDERLESS : WINDOW_DECORATED);
}

// Report the stored undecorated state used as the borderless-mode mirror.
bool Window_isBorderless(const Window *window) {
   if (window == nullptr)
      return false;
   return !(*window).decorated;
}

// Retain the AppKit-only flush request as an inert Wayland compatibility seam.
fn Window_setViewportFlushToTop(Window *window, bool flush) {
   (fn) window;
   (fn) flush;
}

// Report that viewport flush-to-top is unavailable on this backend.
bool Window_isViewportFlushToTop(const Window *window) {
   (fn) window;
   return false;
}

// Retain the macOS f32ing-lights request without a Wayland implementation.
fn Window_setFloatingTrafficLights(Window *window, bool f32ing) {
   (fn) window;
   (fn) f32ing;
}

// macOS-only traffic-light chrome (the Window_macOS_ infix IS the platform
// lock; these symbols exist only in the Cocoa backend — stubs make a Wayland
// build that calls them fail loudly instead of silently passing).
fn Window_macOS_setTrafficLightButtonVisible(Window *window, WindowTrafficLight light, bool visible) {
   (fn) window;
   (fn) light;
   (fn) visible;
   fprintf(stderr, "[window] macOS-only traffic-light API called on Wayland\n");
}

// Reject visibility queries for macOS chrome on this non-macOS backend.
bool Window_macOS_isTrafficLightButtonVisible(const Window *window, WindowTrafficLight light) {
   (fn) window;
   (fn) light;
   return false;
}

// Retain the macOS-only traffic-light placement API as an inert stub.
fn Window_macOS_setTrafficLightHeaderPosition(Window *window, f32 x, f32 y) {
   (fn) window;
   (fn) x;
   (fn) y;
}

// Return zero coordinates because this backend has no native traffic lights.
fn Window_macOS_getTrafficLightHeaderPosition(const Window *window, f32 *outX, f32 *outY) {
   if (outX != nullptr)
      *outX = 0.0f;
   if (outY != nullptr)
      *outY = 0.0f;
   (fn) window;
}

// Store opacity for consumers; Wayland core has no per-surface opacity request.
fn Window_setOpacity(Window *window, f32 opacity) {
   if (window == nullptr)
      return;
   (*window).opacity = opacity;
}

// Forward the background transparency preference to the stored policy setter.
fn Window_setTransparentBackground(Window *window, bool transparent) {
   if (window == nullptr)
      return;
   atomic_store_explicit(&(*window).transparent, transparent, memory_order_relaxed);
}

// Retain the stacking preference; Wayland core does not let clients raise windows.
fn Window_setAlwaysOnTop(Window *window, bool onTop) {
   (fn) window;
   (fn) onTop;
}

// Click-through is expressible: an empty input region tells the compositor the
// window never wants input. Resetting hands the full surface back (a resized
// full-region rebuild happens lazily at next pointer/touch entry).
fn Window_setClickThrough(Window *window, bool clickThrough) {
   if (window == nullptr)
      return;
   (*window).clickThrough = clickThrough;
   if (clickThrough) {
      wl_surface_set_input_region((*window).surface, nullptr);
   } else {
      struct wl_region *full = wl_compositor_create_region(gCompositor);
      if (full) {
         wl_region_add(full, 0, 0,
            atomic_load_explicit(&(*window).cachedWidth, memory_order_relaxed),
            atomic_load_explicit(&(*window).cachedHeight, memory_order_relaxed));
         wl_surface_set_input_region((*window).surface, full);
         wl_region_destroy(full);
      }
   }
}

// Store the requested shadow preference for client-side chrome consumers.
fn Window_setShadow(Window *window, bool shadow) {
   if (window == nullptr)
      return;
   (*window).shadow = shadow;
}

// Store whether the backend's hit-test path should treat the surface as draggable.
fn Window_setMovableByBackground(Window *window, bool movable) {
   if (window == nullptr)
      return;
   (*window).movableByBackground = movable;
}

// Retain the stacking seam; activation is compositor-controlled on Wayland.
fn Window_bringToFront(Window *window) {
   (fn) window;
}

// ;;INTENTION("Wayland has no client-side child-window ordering: attach/detach are link-parity no-ops; the compositor owns stacking")
fn Window_attachChild(Window *parent, Window *child) {
   (fn) parent;
   (fn) child;
}

// Keep child-window ordering API linkable; the compositor owns Wayland stacking.
fn Window_detachChild(Window *parent, Window *child) {
   (fn) parent;
   (fn) child;
}

// --- Minimize ----------------------------------------------------------------

fn Window_minimize(Window *window) {
   if (window == nullptr)
      return;
   xdg_toplevel_set_minimized((*window).topLevel);
   (*window).minimized = true;
}

// Clear the remembered minimized state and recommit the surface for remapping.
fn Window_restore(Window *window) {
   if (window == nullptr)
      return;
   (*window).minimized = false;
   wl_surface_commit((*window).surface);
}

// Report the last minimized request because xdg-shell has no minimize-state event.
bool Window_isMinimized(Window *window) {
   return window ? (*window).minimized : false;
}

// --- Fullscreen --------------------------------------------------------------

bool Window_isFullscreen(Window *window) {
   return window ? (*window).fullscreen : false;
}

// Request or release compositor-managed fullscreen for the toplevel.
fn Window_setFullscreen(Window *window, bool fullscreen) {
   if (window == nullptr)
      return;
   if (fullscreen)
      xdg_toplevel_set_fullscreen((*window).topLevel, nullptr);
   else
      xdg_toplevel_unset_fullscreen((*window).topLevel);
}

// Toggle fullscreen based on the last state reflected from the compositor.
fn Window_toggleFullscreen(Window *window) {
   if (window == nullptr)
      return;
   Window_setFullscreen(window, !(*window).fullscreen);
}

// --- DRM / sharing -----------------------------------------------------------

fn Window_setDRM(Window *window, bool enabled) {
   (fn) window;
   (fn) enabled;
}

// --- Size constraints --------------------------------------------------------

fn Window_setMinSize(Window *window, int width, int height) {
   if (window == nullptr)
      return;
   (*window).minWidth = width;
   (*window).minHeight = height;
   xdg_toplevel_set_min_size((*window).topLevel, width, height);
}

// Store and publish maximum toplevel dimensions to the compositor.
fn Window_setMaxSize(Window *window, int width, int height) {
   if (window == nullptr)
      return;
   (*window).maxWidth = width;
   (*window).maxHeight = height;
   xdg_toplevel_set_max_size((*window).topLevel, width, height);
}

// --- Cursor control ----------------------------------------------------------

fn Window_setCursorType(Window *window, WindowCursorType type) {
   if (window == nullptr)
      return;
   (*window).cursorType = type;
   // A named cursor is applied by the compositor on the next pointer enter or
   // button serial; there is no client-owned cursor surface on Wayland.
}

// Read the requested cursor shape, defaulting for a missing window.
WindowCursorType Window_getCursorType(const Window *window) {
   return window ? (*window).cursorType : WINDOW_CURSOR_DEFAULT;
}

// FPS-style relative cursor: on Wayland the pointer itself cannot be warped,
// so lock = hidden pointer + successive absolute positions become move-deltas
// (see pointerMotion). The mirror word is the cursor type itself.
fn Window_setCursorLocked(Window *window, bool locked) {
   if (window == nullptr)
      return;
   if (locked)
      (*window).cursorType = WINDOW_CURSOR_HIDDEN;
   else
      (*window).cursorType = WINDOW_CURSOR_DEFAULT;
}

// --- Surface / Metal accessors -----------------------------------------------

any Window_contentView(Window *window) {
   return window ? (any) (*window).surface : nullptr;
}

any Window_nativeHandle(const Window *window) {
   return window ? (any) (*window).surface : nullptr;
}

// No Metal here: Wayland has no CAMetalLayer ancestor (the lean backend's
// metalLayer is nil, exactly like the Cocoa backend's on non-Apple).
any Window_metalLayer(Window *window) {
   (fn) window;
   return nullptr;
}

// Retain the retired gravity seam as an inert operation on the lean backend.
fn Window_setGravityTopLeft(Window *window) {
   (fn) window;
}

// Keep the retired worker-present transaction hook linkable as a no-op.
fn Window_workerPresentBegin() {
}

// Close the retired worker-present transaction hook; no transaction is owned here.
fn Window_workerPresentEnd() {
}

// --- Software frame presentation --------------------------------------------

bool Window_present(Window *window, const Buffer *frame) {
   if (window == nullptr || frame == nullptr)
      return false;
   const u32 fw = (*frame).width;
   const u32 fh = (*frame).height;
   const u32 ch = (*frame).channels;
   if (fw == 0 || fh == 0 || ch == 0 || ch > 4)
      return false;
   if (!windowEnsureShm(window, (int) fw, (int) fh))
      return false;

   // Stamp the Buffer payload (Bytes straight off the uint64 element array)
   // into the mapped ARGB8888 pool. Straight alpha: the compositor blends it
   // against the desktop — the transparent-swapchain ask, honored for free.
   const unsigned char *src = (const unsigned char*) (*frame).data;
   bytes srcStride = (bytes) fw * ch;
   u32 *dst = (u32*) (*window).shmMap;
   for (u32 y = 0; y < fh; y++) {
      const unsigned char *row = src + (bytes) y * srcStride;
      for (u32 x = 0; x < fw; x++) {
         unsigned char b = row[x * ch + 0];
         unsigned char g = (ch > 1) ? row[x * ch + 1] : 0u;
         unsigned char r = (ch > 2) ? row[x * ch + 2] : 0u;
         unsigned char a = (ch > 3) ? row[x * ch + 3] : 255u;
         dst[(bytes) y * fw + x] = ((u32) a << 24)
            | ((u32) r << 16) | ((u32) g << 8) | (u32) b;
      }
   }

   wl_surface_attach((*window).surface, (*window).shmBuffer, 0, 0);
   wl_surface_damage_buffer((*window).surface, 0, 0, (int) fw, (int) fh);
   wl_surface_commit((*window).surface);
   return true;
}

// --- Event wiring (adapters) -------------------------------------------------

fn Window_addKeyAdapter(Window *window, const KeyHandler *adapter) {
   if (window == nullptr || adapter == nullptr)
      return;
   Key_attachWindow((*window).id, adapter);
}

// Detach a key callback from the window's engine input slot.
bool Window_removeKeyAdapter(Window *window, const KeyHandler *adapter) {
   return (window != nullptr && adapter != nullptr)
      ? Key_detachWindow((*window).id, adapter) : false;
}

// Attach a mouse callback to the window's engine input slot.
fn Window_addMouseAdapter(Window *window, const MouseHandler *adapter) {
   if (window == nullptr || adapter == nullptr)
      return;
   Mouse_attachWindow((*window).id, adapter);
}

// Detach a mouse callback from the window's engine input slot.
bool Window_removeMouseAdapter(Window *window, const MouseHandler *adapter) {
   return (window != nullptr && adapter != nullptr)
      ? Mouse_detachWindow((*window).id, adapter) : false;
}

// Attach a touch callback to the window's engine input slot.
fn Window_addTouchAdapter(Window *window, const TouchHandler *adapter) {
   if (window == nullptr || adapter == nullptr)
      return;
   Touch_attachWindow((*window).id, adapter);
}

// Detach a touch callback from the window's engine input slot.
bool Window_removeTouchAdapter(Window *window, const TouchHandler *adapter) {
   return (window != nullptr && adapter != nullptr)
      ? Touch_detachWindow((*window).id, adapter) : false;
}

WindowEvent *Window_getLifecycle(Window *window) {
   return window ? &(*window).lifecycle : nullptr;
}

// Drain key, mouse, and touch callback queues for the engine input modules.
fn Window_dispatchEvents(Window *window) {
   (fn) window;
   Key_dispatchEvents();
   Mouse_dispatchEvents();
   Touch_dispatchEvents();
}

// --- Focus -------------------------------------------------------------------

u32 Window_id(Window *window) {
   return window ? (*window).id : 0;
}

// Focus on Wayland is entirely compositor-owned; there is no base xdg request
// to force it (that lives in the separate xdg-activation protocol). The
// mirror word updates when wl_keyboard enter/leave lands.
fn Window_focus(Window *window) {
   (fn) window;
}

// Read the focus mirror updated by wl_keyboard enter/leave notifications.
bool Window_isFocused(Window *window) {
   return window ? (*window).lastFocused : false;
}

// ;;INTENTION("Wayland stores the key gate without OS enforcement: the compositor owns activation; the darling redirect remains the enforcer here")
fn Window_setKeyEnabled(Window *window, bool enabled) {
   if (window == nullptr)
      return;
   atomic_store_explicit(&(*window).keyEnabled, enabled, memory_order_relaxed);
}

// Read the stored key-input gate, returning false for a null handle.
bool Window_isKeyEnabled(const Window *window) {
   if (window == nullptr)
      return false;
   return atomic_load_explicit(&(*window).keyEnabled, memory_order_relaxed);
}

// --- Monitor identity --------------------------------------------------------

u32 Window_getMonitorId(const Window *window) {
   return window ? atomic_load_explicit(&(*window).monitorId, memory_order_relaxed) : 0;
}

// --- Resize reflection -------------------------------------------------------

u64 Window_sizeGeneration(Window *window) {
   return window ? atomic_load_explicit(&(*window).sizeGeneration, memory_order_acquire) : 0;
}

// --- Resize-cadence rendering (the c -> compositor -> c bridge) --------------
//
// xdg_toplevel.configure carries the compositor's committed size; the
// configure listener runs the same hook the Cocoa willResize/didResize path
// does — inline render+present on Thread 0 before the frame is composited.

fn Window_setResizeRenderHook(Window *window, WindowResizeRenderFn fn, any userdata) {
   if (window == nullptr)
      return;
   (*window).resizeRenderFn = fn;
   (*window).resizeRenderUserdata = userdata;
}

// Return the callback registered for synchronous configure-size rendering.
WindowResizeRenderFn Window_getResizeRenderHook(const Window *window) {
   return window ? (*window).resizeRenderFn : nullptr;
}


#elif defined(VEX_PLATFORM_LINUX)
// window.c — Win32 backend for the Window API.
//
// The annotations (@PlatformExclusive, @Draft, @Intention, @Incomplete) are
// real markers defined in src/annotation/*.h — C has no annotation feature,
// so they are macros that expand to static asserts embedding the annotation
// text in compiler diagnostics / debug info.
//
// Every function below is an @Incomplete stub returning a safe default. The
// real implementations will use User32: CreateWindowEx / DefWindowProc,
// ShowWindow / SetWindowTextA / SetWindowPos / SetWindowDisplayAffinity,
// and a GetMessage/PeekMessage-TranslateMessage-DispatchMessage event loop.

#include <stdio.h>
#include <stdlib.h>

#include "window/window.h"
#include "window/traffic_light.h"

;;DEFINITION
/**
 * ============================================================================
 * DEFINITION: Window
 * ============================================================================
 * Win32 platform implementation stub of the high-level Window abstraction.
 * Provides fallback function symbols matching the window.h platform-agnostic API
 * seam on Windows environments, returning safe defaults until full Win32 User32
 * and DXGI integration is completed.
 *
 * Implements the Cold-Strict, Hot-Minimal Validation Law and preserves ABI
 * compatibility across targets by completing the Window constructor, core,
 * setter, and getter contracts.
 * ============================================================================
 */

;;OVERVIEW
/**
 * ============================================================================
 * CLASS: Window (window/window.c)
 * ============================================================================
 * SUMMARY:
 *   Win32 stub backend for the platform-agnostic Window API.
 *   Provides complete fallback symbol definitions matching window.h for Windows builds.
 *
 * STRUCT FIELDS:
 * ----------------------------------------------------------------------------
 *   (none — stub backend; real fields live in window/window_cocoa.m)
 *
 * PRIVATE HELPERS:
 * ----------------------------------------------------------------------------
 *   (none)
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
 *   - (none)
 *
 * Public Core Functions: (.h)
 *   - Window_destroy(window)
 *   - Window_destroyAll()
 *   - Window_shouldClose(window)
 *   - Window_pollEvents()
 *   - Window_width(window)
 *   - Window_height(window)
 *   - Window_center(window)
 *   - Window_show(window)
 *   - Window_hide(window)
 *   - Window_attachPanes(window, panel, width, height)
 *   - Window_resizePanes(window, panel, width, height)
 *   - Window_compositePanes(window, contentPanel)
 *   - Window_compositeBoards(window)
 *   - Window_renderGeneration(window)
 *   - Window_bringToFront(window)
 *   - Window_minimize(window)
 *   - Window_restore(window)
 *   - Window_toggleFullscreen(window)
 *   - Window_contentView(window)
 *   - Window_nativeHandle(window)
 *   - Window_metalLayer(window)
 *   - Window_present(window, frame)
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
 *
 * Private Core Functions: (.c static)
 *   - (none)
 *
 * Public Setters: (.h)
 *   - Window_setShouldClose(window, shouldClose)
 *   - Window_setTitle(window, title)
 *   - Window_setSize(window, width, height)
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
 *   - Window_macOS_setTrafficLightButtonVisible(window, light, visible)
 *   - Window_macOS_setTrafficLightHeaderPosition(window, x, y)
 *   - Window_setFloatingTrafficLights(window, f32ing)
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
 *   - Window_setGravityTopLeft(window)
 *   - Window_setResizeRenderHook(window, fn, userdata)
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
 *   - Window_getResizeRenderHook(window)
 *
 * Private Getters: (.c static)
 *   - (none)
 * ============================================================================
 */


#if defined(VEX_PLATFORM_LINUX)

;;PLATFORM_EXCLUSIVE("Windows")
;;INTENTION("Fills the Window API seam (window.h) on Windows so the engine can be built there; mirrors the legacy windowsWindow.java.")
;;DRAFT

;;;;INCOMPLETE // CreateWindowEx; returns nullptr until implemented.
/** Windows draft stub: returns nullptr until native window creation is implemented. */
Window *Window_create(const char *title, int width, int height) {
   (fn) title;
   (fn) width;
   (fn) height;
   return nullptr;
}

;;INCOMPLETE // DestroyWindow; no-op until implemented.
/** Windows draft stub: does not destroy a native window yet. */
fn Window_destroy(Window *window) {
   (fn) window;
}

// Compatibility entry point; this stub backend has no registered windows to destroy.
/** Windows draft stub: has no windows to destroy. */
fn Window_destroyAll() {
}

;;INCOMPLETE // Poll WM_QUIT; false until implemented.
/** Windows draft stub: reports that no close request is pending. */
bool Window_shouldClose(Window *window) {
   (fn) window;
   return false;
}

// Store the requested close state; the incomplete backend has no native window state.
/** Windows draft stub: ignores close-state updates. */
fn Window_setShouldClose(Window *window, bool shouldClose) {
   (fn) window;
   (fn) shouldClose;
}

;;INCOMPLETE // PeekMessage/TranslateMessage/DispatchMessage loop; no-op until implemented.
/** Windows draft stub: does not pump native messages. */
fn Window_pollEvents() {
}

// Perform the available event poll and report that no further event is pending.
/** Pumps the stub event function and reports no event. */
bool Window_pollEventStep() {
   Window_pollEvents();
   return false;
}

;;INCOMPLETE // DXGI swap interval; no-op until implemented.
/** Windows draft stub: ignores presentation mode changes. */
fn Window_setPresentMode(Window *window, int mode) {
   (fn) window;
   (fn) mode;
}

// Return FIFO as the safe present-mode default for the unimplemented backend.
/** Returns FIFO because this backend does not implement presentation modes. */
int Window_getPresentMode(const Window *window) {
   (fn) window;
   return WINDOW_PRESENT_FIFO;
}

;;INCOMPLETE // Layered window alpha; no-op until implemented.
/** Windows draft stub: ignores transparency changes. */
fn Window_setTransparent(Window *window, bool transparent) {
   (fn) window;
   (fn) transparent;
}

// Report the stub backend's default opaque-window state.
/** Returns false because transparency state is not implemented. */
bool Window_isTransparent(const Window *window) {
   (fn) window;
   return false;
}

// Return the unchanged render-policy generation for this stub backend.
/** Returns zero because this stub does not track render generations. */
u64 Window_renderGeneration(const Window *window) {
   (fn) window;
   return 0;
}

;;INCOMPLETE // Input kill switch; no-op until implemented.
/** Windows draft stub: ignores enabled-state changes. */
fn Window_setEnabled(Window *window, bool enabled) {
   (fn) window;
   (fn) enabled;
}

// Report the stub's permissive default input-enabled state.
/** Returns the stub's default enabled state. */
bool Window_isEnabled(const Window *window) {
   (fn) window;
   return true;
}

;;INCOMPLETE // SetWindowTextA; no-op until implemented.
/** Windows draft stub: ignores title changes. */
fn Window_setTitle(Window *window, const char *title) {
   (fn) window;
   (fn) title;
}

;;INCOMPLETE // SetWindowPos; no-op until implemented.
/** Windows draft stub: ignores pixel-size changes. */
fn Window_setSize(Window *window, int width, int height) {
   (fn) window;
   (fn) width;
   (fn) height;
}

// Report the unit scale used by this platform stub.
/** Returns the unit scale used by this unimplemented backend. */
f32 Window_getScale(const Window *window) {
   (fn) window;
   return 1.0f;
}

// Provide the platform revalidation seam without native display state.
/** Windows draft stub: has no native geometry to revalidate. */
fn Window_revalidate(Window *window) {
   (fn) window;
}

// Accept the logical-size API seam; this backend does not retain geometry.
/** Windows draft stub: ignores point-size changes. */
fn Window_setSizePoints(Window *window, f32 width, f32 height) {
   (fn) window;
   (fn) width;
   (fn) height;
}

// Return zero logical dimensions because the stub creates no native window.
/** Writes zero dimensions to supplied outputs. */
fn Window_getSizePoints(const Window *window, f32 *outWidth, f32 *outHeight) {
   (fn) window;
   if (outWidth) *outWidth = 0.0f;
   if (outHeight) *outHeight = 0.0f;
}

// Return the stub window's logical width, which is always zero.
/** Returns zero because point width is unavailable in this stub. */
f32 Window_widthPoints(const Window *window) {
   (fn) window;
   return 0.0f;
}

// Return the stub window's logical height, which is always zero.
/** Returns zero because point height is unavailable in this stub. */
f32 Window_heightPoints(const Window *window) {
   (fn) window;
   return 0.0f;
}

// Return zero width because this stub backend has no physical display window.
/** Returns zero client width because geometry is not tracked. */
int Window_width(Window *window) {
   (fn) window;
   return 0;
}

// Return zero height because this stub backend has no physical display window.
/** Returns zero client height because geometry is not tracked. */
int Window_height(Window *window) {
   (fn) window;
   return 0;
}

// Expose the window width through the viewport compatibility accessor.
/** Returns the window pixel width through the shared accessor. */
int Window_viewportWidth(const Window *window) {
   return Window_width((Window*) window);
}

// Expose the window height through the viewport compatibility accessor.
/** Returns the window pixel height through the shared accessor. */
int Window_viewportHeight(const Window *window) {
   return Window_height((Window*) window);
}

// Expose the logical width through the viewport compatibility accessor.
/** Returns the window point width through the shared accessor. */
f32 Window_viewportWidthPoints(const Window *window) {
   return Window_widthPoints(window);
}

// Expose the logical height through the viewport compatibility accessor.
/** Returns the window point height through the shared accessor. */
f32 Window_viewportHeightPoints(const Window *window) {
   return Window_heightPoints(window);
}

// Alias the client width as the full window width in this stub.
/** Returns the window pixel width through the shared accessor. */
int Window_windowWidth(const Window *window) {
   return Window_width((Window*) window);
}

// Alias the client height as the full window height in this stub.
/** Returns the window pixel height through the shared accessor. */
int Window_windowHeight(const Window *window) {
   return Window_height((Window*) window);
}

// Alias the logical client width as the full window width.
/** Returns the window point width through the shared accessor. */
f32 Window_windowWidthPoints(const Window *window) {
   return Window_widthPoints(window);
}

// Alias the logical client height as the full window height.
/** Returns the window point height through the shared accessor. */
f32 Window_windowHeightPoints(const Window *window) {
   return Window_heightPoints(window);
}

;;INCOMPLETE // SetWindowPos; no-op until implemented.
/** Windows draft stub: ignores location changes. */
fn Window_setLocation(Window *window, int x, int y) {
   (fn) window;
   (fn) x;
   (fn) y;
}

// Return zero content-origin coordinates for the unimplemented backend.
/** Writes zero content-origin coordinates to supplied outputs. */
fn Window_getContentOrigin(const Window *window, int *outX, int *outY) {
   (fn) window;
   if (outX) *outX = 0;
   if (outY) *outY = 0;
}

// Accept but do not retain the resize callback on this stub platform.
/** Windows draft stub: does not retain a resize-render callback. */
fn Window_setResizeRenderHook(Window *window, WindowResizeRenderFn fn, any userdata) {
   (fn) window;
   (fn) fn;
   (fn) userdata;
}

;;INCOMPLETE // No hook slot on the stub platform; nullptr until implemented.
/** Returns nullptr because this stub has no resize-render hook storage. */
WindowResizeRenderFn Window_getResizeRenderHook(const Window *window) {
   (fn) window;
   return nullptr;
}

// Return zero because this backend does not resolve monitor identities.
/** Returns zero because monitor identity is not implemented. */
u32 Window_getMonitorId(const Window *window) {
   (fn) window;
   return 0;
}

;;INCOMPLETE // GetWindowRect top-left; zeros until implemented.
/** Writes zero location coordinates to supplied outputs. */
fn Window_getLocation(const Window *window, int *outX, int *outY) {
   (fn) window;
   if (outX) *outX = 0;
   if (outY) *outY = 0;
}

;;INCOMPLETE // CenterWindow; no-op until implemented.
/** Windows draft stub: does not reposition a native window. */
fn Window_center(Window *window) {
   (fn) window;
}

;;INCOMPLETE // ShowWindow(SW_SHOW)/ShowWindow(SW_HIDE); no-op until implemented.
/** Windows draft stub: ignores visibility changes. */
fn Window_setVisible(Window *window, bool visible) {
   (fn) window;
   (fn) visible;
}

;;INCOMPLETE // GWL_STYLE WS_THICKFRAME; false until implemented.
/** Returns false because resizability is not implemented. */
bool Window_isResizable(Window *window) {
   (fn) window;
   return false;
}

;;INCOMPLETE // SetWindowLong(GWL_STYLE, WS_THICKFRAME); no-op until implemented.
/** Windows draft stub: ignores resizability changes. */
fn Window_setResizable(Window *window, bool resizable) {
   (fn) window;
   (fn) resizable;
}

;;INCOMPLETE // GWL_STYLE WS_SYSMENU; false until implemented.
/** Returns false because closability is not implemented. */
bool Window_isClosable(Window *window) {
   (fn) window;
   return false;
}

;;INCOMPLETE // SetWindowLong(GWL_STYLE, WS_SYSMENU); no-op until implemented.
/** Windows draft stub: ignores closability changes. */
fn Window_setClosable(Window *window, bool closable) {
   (fn) window;
   (fn) closable;
}

;;INCOMPLETE // GWL_STYLE WS_MINIMIZEBOX; false until implemented.
/** Returns false because miniaturization is not implemented. */
bool Window_isMiniaturizable(Window *window) {
   (fn) window;
   return false;
}

;;INCOMPLETE // SetWindowLong(GWL_STYLE, WS_MINIMIZEBOX); no-op until implemented.
/** Windows draft stub: ignores miniaturization changes. */
fn Window_setMiniaturizable(Window *window, bool miniaturizable) {
   (fn) window;
   (fn) miniaturizable;
}

;;INCOMPLETE // WS_MAXIMIZEBOX semantics; no-op until implemented.
/** Windows draft stub: ignores the fullscreen-button setting. */
fn Window_setFullscreenButton(Window *window, bool enabled) {
   (fn) window;
   (fn) enabled;
}

;;INCOMPLETE // Fullscreen/borderless chrome switch; no-op until implemented.
/** Windows draft stub: ignores decoration-mode changes. */
fn Window_setUndecorated(Window *window, int mode) {
   (fn) window;
   (fn) mode;
}

// Map the boolean decoration request onto the undecorated-mode API.
/** Selects decorated or borderless mode through the draft mode setter. */
fn Window_setDecorated(Window *window, bool decorated) {
   Window_setUndecorated(window, decorated ? WINDOW_DECORATED : WINDOW_UNDECORATED_BORDERLESS);
}

// Report the stub's default decorated state.
/** Returns the draft backend's decorated default. */
bool Window_isDecorated(const Window *window) {
   (fn) window;
   return true;
}

// Map the naked-titlebar request onto the undecorated-mode API.
/** Selects naked or decorated mode through the draft mode setter. */
fn Window_setNaked(Window *window, bool naked) {
   Window_setUndecorated(window, naked ? WINDOW_UNDECORATED_NAKED : WINDOW_DECORATED);
}

// Report that the stub does not provide naked chrome.
/** Returns false because naked-mode state is not retained. */
bool Window_isNaked(const Window *window) {
   (fn) window;
   return false;
}

// Map the borderless request onto the undecorated-mode API.
/** Selects borderless or decorated mode through the draft mode setter. */
fn Window_setBorderless(Window *window, bool borderless) {
   Window_setUndecorated(window, borderless ? WINDOW_UNDECORATED_BORDERLESS : WINDOW_DECORATED);
}

// Report that the stub does not provide borderless chrome.
/** Returns false because borderless state is not retained. */
bool Window_isBorderless(const Window *window) {
   (fn) window;
   return false;
}

// Accept the titlebar-flush request; the incomplete backend stores no chrome state.
/** Windows draft stub: ignores viewport flush alignment. */
fn Window_setViewportFlushToTop(Window *window, bool flush) {
   (fn) window;
   (fn) flush;
}

// Report that viewport flush-to-top is unavailable in the stub backend.
/** Returns false because viewport alignment state is not retained. */
bool Window_isViewportFlushToTop(const Window *window) {
   (fn) window;
   return false;
}

// Forward the compatibility request to the viewport-flush setter.
/** Maps the compatibility f32ing setting to viewport flush behavior. */
fn Window_setFloatingTrafficLights(Window *window, bool f32ing) {
   Window_setViewportFlushToTop(window, f32ing);
}

;;PLATFORM_EXCLUSIVE("macOS") // No traffic lights on Win32: needs a Mac to mean anything.
/** Reports the unsupported macOS-only operation to stderr. */
fn Window_macOS_setTrafficLightButtonVisible(Window *window, WindowTrafficLight light, bool visible) {
   (fn) window;
   (fn) light;
   (fn) visible;
   fprintf(stderr, "window: Window_macOS_setTrafficLightButtonVisible is macOS-only (needs a Mac machine)\n");
}

;;PLATFORM_EXCLUSIVE("macOS") // No traffic lights on Win32.
/** Reports the unsupported query and returns false. */
bool Window_macOS_isTrafficLightButtonVisible(const Window *window, WindowTrafficLight light) {
   (fn) window;
   (fn) light;
   fprintf(stderr, "window: Window_macOS_isTrafficLightButtonVisible is macOS-only (needs a Mac machine)\n");
   return false;
}

;;PLATFORM_EXCLUSIVE("macOS") // No traffic lights on Win32.
/** Reports the unsupported macOS-only operation to stderr. */
fn Window_macOS_setTrafficLightHeaderPosition(Window *window, f32 x, f32 y) {
   (fn) window;
   (fn) x;
   (fn) y;
   fprintf(stderr, "window: Window_macOS_setTrafficLightHeaderPosition is macOS-only (needs a Mac machine)\n");
}

;;PLATFORM_EXCLUSIVE("macOS") // No traffic lights on Win32.
/** Reports the unsupported query and writes zero to supplied outputs. */
fn Window_macOS_getTrafficLightHeaderPosition(const Window *window, f32 *outX, f32 *outY) {
   (fn) window;
   fprintf(stderr, "window: Window_macOS_getTrafficLightHeaderPosition is macOS-only (needs a Mac machine)\n");
   if (outX)
      *outX = 0.0f;
   if (outY)
      *outY = 0.0f;
}

;;INCOMPLETE // ShowWindow(SW_MINIMIZE); no-op until implemented.
/** Windows draft stub: does not minimize a native window. */
fn Window_minimize(Window *window) {
   (fn) window;
}

;;INCOMPLETE // ShowWindow(SW_RESTORE); no-op until implemented.
/** Windows draft stub: does not restore a native window. */
fn Window_restore(Window *window) {
   (fn) window;
}

;;INCOMPLETE // IsIconic; false until implemented.
/** Returns false because minimized state is not implemented. */
bool Window_isMinimized(Window *window) {
   (fn) window;
   return false;
}

;;INCOMPLETE // GWL_STYLE WS_MAXIMIZE; false until implemented.
/** Returns false because fullscreen state is not implemented. */
bool Window_isFullscreen(Window *window) {
   (fn) window;
   return false;
}

;;INCOMPLETE // ShowWindow(SW_MAXIMIZE) vs SW_RESTORE; no-op until implemented.
/** Windows draft stub: ignores fullscreen changes. */
fn Window_setFullscreen(Window *window, bool fullscreen) {
   (fn) window;
   (fn) fullscreen;
}

;;INCOMPLETE // Window setFullscreen wrapper; no-op until implemented.
/** Windows draft stub: does not toggle fullscreen state. */
fn Window_toggleFullscreen(Window *window) {
   (fn) window;
}

;;INCOMPLETE // SetWindowDisplayAffinity(WDA_MONITOR/WDA_NONE); no-op until implemented.
/** Windows draft stub: ignores display-recording protection changes. */
fn Window_setDRM(Window *window, bool enabled) {
   (fn) window;
   (fn) enabled;
}

;;INCOMPLETE // AdjustWindowRectEx + GetWindowRect; no-op until implemented.
/** Windows draft stub: ignores minimum-size constraints. */
fn Window_setMinSize(Window *window, int width, int height) {
   (fn) window;
   (fn) width;
   (fn) height;
}

;;INCOMPLETE // AdjustWindowRectEx + GetWindowRect; no-op until implemented.
/** Windows draft stub: ignores maximum-size constraints. */
fn Window_setMaxSize(Window *window, int width, int height) {
   (fn) window;
   (fn) width;
   (fn) height;
}

;;INCOMPLETE // SetCursor; no-op until implemented.
/** Windows draft stub: ignores cursor changes. */
fn Window_setCursorType(Window *window, WindowCursorType type) {
   (fn) window;
   (fn) type;
}

;;INCOMPLETE // GetCursor; returns default until implemented.
/** Returns the default cursor because cursor state is not tracked. */
WindowCursorType Window_getCursorType(const Window *window) {
   (fn) window;
   return WINDOW_CURSOR_DEFAULT;
}

;;INCOMPLETE // Board composite; no-op until implemented.
/** Windows draft stub: does not composite board content. */
fn Window_compositeBoards(Window *window) {
   (fn) window;
}

;;INCOMPLETE
// Return no content-view handle because this platform implementation is a stub.
/** Returns nullptr because the stub has no native content view. */
any Window_contentView(Window *window) {
   (fn) window;
   return nullptr;
}

;;INCOMPLETE
// Return no native handle because the stub never creates an operating-system window.
/** Returns nullptr because the stub has no native window handle. */
any Window_nativeHandle(const Window *window) {
   (fn) window;
   return nullptr;
}

Window *Window_0() {
   return nullptr;
}

Window *Window_1(const char *title) {
   (fn) title;
   return nullptr;
}

Window *Window_3(const char *title, int width, int height) {
   (fn) title;
   (fn) width;
   (fn) height;
   return nullptr;
}

Window *Window_new(const WindowDesc *desc) {
   (fn) desc;
   return nullptr;
}

u32 Window_id(Window *window) {
   (fn) window;
   return 0;
}

u64 Window_sizeGeneration(Window *window) {
   (fn) window;
   return 0;
}

bool Window_isFocused(Window *window) {
   (fn) window;
   return false;
}

fn Window_focus(Window *window) {
   (fn) window;
}

bool Window_isKeyEnabled(const Window *window) {
   (fn) window;
   return true;
}

fn Window_setKeyEnabled(Window *window, bool enabled) {
   (fn) window;
   (fn) enabled;
}

bool Window_isLiveResizing(const Window *window) {
   (fn) window;
   return false;
}

fn Window_show(Window *window) {
   (fn) window;
}

fn Window_hide(Window *window) {
   (fn) window;
}

fn Window_close(Window *window) {
   Window_setShouldClose(window, true);
}

fn Window_waitEvents(Window *window, int timeoutMs) {
   (fn) window;
   (fn) timeoutMs;
}

fn Window_bringToFront(Window *window) {
   (fn) window;
}

fn Window_attachChild(Window *parent, Window *child) {
   (fn) parent;
   (fn) child;
}

fn Window_detachChild(Window *parent, Window *child) {
   (fn) parent;
   (fn) child;
}

fn Window_setAlwaysOnTop(Window *window, bool onTop) {
   (fn) window;
   (fn) onTop;
}

fn Window_setClickThrough(Window *window, bool clickThrough) {
   (fn) window;
   (fn) clickThrough;
}

fn Window_setShadow(Window *window, bool shadow) {
   (fn) window;
   (fn) shadow;
}

fn Window_setMovableByBackground(Window *window, bool movable) {
   (fn) window;
   (fn) movable;
}

fn Window_setOpacity(Window *window, f32 opacity) {
   (fn) window;
   (fn) opacity;
}

fn Window_setTransparentBackground(Window *window, bool transparent) {
   (fn) window;
   (fn) transparent;
}

fn Window_setBackdropBlur(Window *window, f32 radius) {
   (fn) window;
   (fn) radius;
}

fn Window_setGravityTopLeft(Window *window) {
   (fn) window;
}

fn Window_setCursorLocked(Window *window, bool locked) {
   (fn) window;
   (fn) locked;
}

any Window_metalLayer(Window *window) {
   (fn) window;
   return nullptr;
}

fn Window_setBottomLayer(Window *window, any layer) {
   (fn) window;
   (fn) layer;
}

any Window_getBottomLayer(const Window *window) {
   (fn) window;
   return nullptr;
}

fn Window_setTopLayer(Window *window, any layer) {
   (fn) window;
   (fn) layer;
}

any Window_getTopLayer(const Window *window) {
   (fn) window;
   return nullptr;
}

fn Window_orderLayers(Window *window) {
   (fn) window;
}

bool Window_attachPanes(Window *window, Panel *panel, int width, int height) {
   (fn) window;
   (fn) panel;
   (fn) width;
   (fn) height;
   return false;
}

bool Window_resizePanes(Window *window, Panel *panel, int width, int height) {
   (fn) window;
   (fn) panel;
   (fn) width;
   (fn) height;
   return false;
}

fn Window_compositePanes(Window *window, Panel *contentPanel) {
   (fn) window;
   (fn) contentPanel;
}

fn Window_addKeyAdapter(Window *window, const KeyHandler *adapter) {
   (fn) window;
   (fn) adapter;
}

bool Window_removeKeyAdapter(Window *window, const KeyHandler *adapter) {
   (fn) window;
   (fn) adapter;
   return false;
}

fn Window_addMouseAdapter(Window *window, const MouseHandler *adapter) {
   (fn) window;
   (fn) adapter;
}

bool Window_removeMouseAdapter(Window *window, const MouseHandler *adapter) {
   (fn) window;
   (fn) adapter;
   return false;
}

fn Window_addTouchAdapter(Window *window, const TouchHandler *adapter) {
   (fn) window;
   (fn) adapter;
}

bool Window_removeTouchAdapter(Window *window, const TouchHandler *adapter) {
   (fn) window;
   (fn) adapter;
   return false;
}

fn Window_dispatchEvents(Window *window) {
   (fn) window;
}

fn Window_workerPresentBegin() {
}

fn Window_workerPresentEnd() {
}

bool Window_present(Window *window, const Buffer *frame) {
   (fn) window;
   (fn) frame;
   return false;
}

fn Window_presentRGBA(Window *window, const_any pixels, bytes stride, int width, int height) {
   (fn) window;
   (fn) pixels;
   (fn) stride;
   (fn) width;
   (fn) height;
}

any Window_createPresentSurface(Window *window, int widthPx, int heightPx) {
   (fn) window;
   (fn) widthPx;
   (fn) heightPx;
   return nullptr;
}

fn Window_destroyPresentSurface(Window *window, any surface) {
   (fn) window;
   (fn) surface;
}

fn Window_presentSurface(Window *window, any surface) {
   (fn) window;
   (fn) surface;
}

any Window_presentSurfaceContents(const Window *window) {
   (fn) window;
   return nullptr;
}

bool Window_readPresentSurface(Window *window, any surface, any destRGBA, bytes destStride) {
   (fn) window;
   (fn) surface;
   (fn) destRGBA;
   (fn) destStride;
   return false;
}

bool Window_writePNG(const_any pixels, bytes stride, int width, int height, const char *path) {
   (fn) pixels;
   (fn) stride;
   (fn) width;
   (fn) height;
   (fn) path;
   return false;
}

bool Window_macOS_hasLiquidGlass() {
   return false;
}

fn Window_macOS_setLiquidGlass(Window *window, const WindowLiquidGlassDesc *desc) {
   (fn) window;
   (fn) desc;
}

bool Window_macOS_getLiquidGlass(const Window *window, WindowLiquidGlassDesc *out) {
   (fn) window;
   (fn) out;
   return false;
}

struct TrafficLight {
   any window;
   bool lightVisible[TRAFFIC_LIGHT_COUNT];
   f32 lightOX;
   f32 lightOY;
};

/** Fallback stub: creates traffic-light adapter for non-macOS platforms. */
TrafficLight *TrafficLight_create(any nsWindowHandle) {
   TrafficLight *self = (TrafficLight*) calloc(1, sizeof(TrafficLight));
   if (self == nullptr)
      return nullptr;
   (*self).window = nsWindowHandle;
   (*self).lightVisible[TRAFFIC_LIGHT_CLOSE] = true;
   (*self).lightVisible[TRAFFIC_LIGHT_MINIATURIZE] = true;
   (*self).lightVisible[TRAFFIC_LIGHT_ZOOM] = true;
   (*self).lightOX = 0.0f;
   (*self).lightOY = 0.0f;
   return self;
}

/** Fallback stub: destroys traffic-light adapter. */
fn TrafficLight_destroy(TrafficLight *self) {
   if (self != nullptr)
      free(self);
}

/** Fallback stub: resetBase no-op. */
fn TrafficLight_resetBase(TrafficLight *self) {
   (fn) self;
}

/** Fallback stub: refresh no-op. */
fn TrafficLight_refresh(TrafficLight *self) {
   (fn) self;
}

/** Fallback stub: sets button visibility. */
fn TrafficLight_setButtonVisible(TrafficLight *self, TrafficLightButton button, bool visible) {
   if (self == nullptr || (int) button < 0 || button >= TRAFFIC_LIGHT_COUNT)
      return;
   (*self).lightVisible[button] = visible;
}

/** Fallback stub: reports button visibility. */
bool TrafficLight_isButtonVisible(const TrafficLight *self, TrafficLightButton button) {
   if (self == nullptr || (int) button < 0 || button >= TRAFFIC_LIGHT_COUNT)
      return false;
   return (*self).lightVisible[button];
}

/** Fallback stub: setHeaderPosition no-op. */
fn TrafficLight_setHeaderPosition(TrafficLight *self, f32 x, f32 y) {
   (fn) self;
   (fn) x;
   (fn) y;
}

/** Fallback stub: getHeaderPosition writes zero coordinates. */
fn TrafficLight_getHeaderPosition(const TrafficLight *self, f32 *outX, f32 *outY) {
   (fn) self;
   if (outX)
      *outX = 0.0f;
   if (outY)
      *outY = 0.0f;
}

/** Fallback stub: setFloating no-op. */
fn TrafficLight_setFloating(TrafficLight *self, bool f32ing) {
   (fn) self;
   (fn) f32ing;
}

#endif
#endif // VEX_PLATFORM_WINDOWS / _LINUX

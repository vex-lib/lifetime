#ifndef WINDOW_WINDOW_EVENT_H
#define WINDOW_WINDOW_EVENT_H

#include <stdbool.h>

// window/window_event.h — Per-window OS lifecycle event registry.
//
// One WindowEvent per Window. The struct is embedded in the window's platform
// state (never heap-allocated): the app fills the fn-pointer slots once, and
// the pump pass fires them on Thread 0 through the WindowEvent_fire*
// dispatchers. Every slot is nullable — a null slot dispatches as a no-op, so
// a window that ignores an event pays zero cost and never crashes (the
// Cold-Strict, Hot-Minimal Validation Law).
//
// Focus is Application-level, pressed is Window-level: onFocusGained /
// onFocusLost fire only on key-window flips (the pump compares the mirrored
// key-window id against lastFocused); onPressed fires on mouse-down on the
// surface regardless of prior focus. onQuitRequested is the single vetoable
// slot — it covers the close button, Cmd+Q, Cmd+W, and Alt+F4, and its
// return value (true = may quit) lets the app decline for save-before-quit.
//
// Lifecycle:
//   WindowEvent_init(&ev)               // embedded in Window, zeros all slots
//   WindowEvent_setSelf(&ev, owner)
//   WindowEvent_setOnResized(&ev, fn)   // symmetric set per slot
//   ... app runs ...
//   WindowEvent_fireResized(&ev, win, w, h)  // pump pass, Thread 0 only
//
// The fire dispatchers run the slot's fn with the stored self and the Window;
// they are the ONLY path that touches the slot pointers, keeping null-checks
// in one place.

typedef struct Window Window;

// Slot signatures (function-pointer typedefs: types, not classes).
typedef bool (*WindowQuitRequestedFn)(void *self, Window *window);
typedef void (*WindowResizedFn)(void *self, Window *window, int width, int height);
typedef void (*WindowMovedFn)(void *self, Window *window, int x, int y);
typedef void (*WindowNoArgFn)(void *self, Window *window);
typedef void (*WindowOcclusionFn)(void *self, Window *window, bool visible);

typedef struct WindowEvent {
    void *self;                               // opaque owner of every slot
    WindowQuitRequestedFn onQuitRequested;    // close btn / Cmd+Q / Cmd+W / Alt+F4 (true = may quit)
    WindowNoArgFn onAfterQuit;                // fired after window is destroyed
    WindowResizedFn onResized;                // content size changed (pixels)
    WindowMovedFn onMoved;                     // window origin changed (desktop points)
    WindowNoArgFn onFullscreen;               // entered fullscreen
    WindowNoArgFn onMinimized;                // minimized
    WindowNoArgFn onRestored;                 // back to normal (exit fullscreen / unminimize)
    WindowNoArgFn onPressed;                  // mouse pressed on the window surface
    WindowNoArgFn onFocusGained;            // key window became THIS window
    WindowNoArgFn onFocusLost;              // key window left THIS window
    WindowNoArgFn onZoomFilled;             // double-click header zoom-to-fill
    WindowNoArgFn onZoomBack;               // exited zoom-to-fill back to normal
    WindowOcclusionFn onOcclusionChanged;   // occlusion flipped (true = pixels visible)
} WindowEvent;

// --- Constructors ---
// Zero every slot (safe for embedded storage). Null-safe: false on null.
bool WindowEvent_init(WindowEvent *self);

// --- Core — fire dispatchers (null-slot safe; Thread 0 only) ---
// Quit is the vetoable slot: returns true when no slot is set OR the slot
// returns true (may quit); false when the app declines.
bool WindowEvent_fireQuitRequested(WindowEvent *self, Window *window);
void WindowEvent_fireAfterQuit(WindowEvent *self, Window *window);
void WindowEvent_fireResized(WindowEvent *self, Window *window, int width, int height);
void WindowEvent_fireMoved(WindowEvent *self, Window *window, int x, int y);
void WindowEvent_fireFullscreen(WindowEvent *self, Window *window);
void WindowEvent_fireMinimized(WindowEvent *self, Window *window);
void WindowEvent_fireRestored(WindowEvent *self, Window *window);
void WindowEvent_firePressed(WindowEvent *self, Window *window);
void WindowEvent_fireFocusGained(WindowEvent *self, Window *window);
void WindowEvent_fireFocusLost(WindowEvent *self, Window *window);
void WindowEvent_fireZoomFilled(WindowEvent *self, Window *window);
void WindowEvent_fireZoomBack(WindowEvent *self, Window *window);
void WindowEvent_fireOcclusionChanged(WindowEvent *self, Window *window, bool visible);

// --- Setters / Getters (the Symmetric Getter/Setter Completeness Law) ---
// Null-safe: setters no-op on null self/fn, getters return NULL.
void      WindowEvent_setSelf(WindowEvent *self, void *owner);
void     *WindowEvent_getSelf(const WindowEvent *self);

void    WindowEvent_setOnQuitRequested(WindowEvent *self, WindowQuitRequestedFn fn);
WindowQuitRequestedFn WindowEvent_getOnQuitRequested(const WindowEvent *self);

void    WindowEvent_setOnAfterQuit(WindowEvent *self, WindowNoArgFn fn);
WindowNoArgFn WindowEvent_getOnAfterQuit(const WindowEvent *self);

void    WindowEvent_setOnResized(WindowEvent *self, WindowResizedFn fn);
WindowResizedFn WindowEvent_getOnResized(const WindowEvent *self);

void    WindowEvent_setOnMoved(WindowEvent *self, WindowMovedFn fn);
WindowMovedFn WindowEvent_getOnMoved(const WindowEvent *self);

void    WindowEvent_setOnFullscreen(WindowEvent *self, WindowNoArgFn fn);
WindowNoArgFn WindowEvent_getOnFullscreen(const WindowEvent *self);

void    WindowEvent_setOnMinimized(WindowEvent *self, WindowNoArgFn fn);
WindowNoArgFn WindowEvent_getOnMinimized(const WindowEvent *self);

void    WindowEvent_setOnRestored(WindowEvent *self, WindowNoArgFn fn);
WindowNoArgFn WindowEvent_getOnRestored(const WindowEvent *self);

void    WindowEvent_setOnPressed(WindowEvent *self, WindowNoArgFn fn);
WindowNoArgFn WindowEvent_getOnPressed(const WindowEvent *self);

void    WindowEvent_setOnFocusGained(WindowEvent *self, WindowNoArgFn fn);
WindowNoArgFn WindowEvent_getOnFocusGained(const WindowEvent *self);

void    WindowEvent_setOnFocusLost(WindowEvent *self, WindowNoArgFn fn);
WindowNoArgFn WindowEvent_getOnFocusLost(const WindowEvent *self);

void    WindowEvent_setOnZoomFilled(WindowEvent *self, WindowNoArgFn fn);
WindowNoArgFn WindowEvent_getOnZoomFilled(const WindowEvent *self);

void    WindowEvent_setOnZoomBack(WindowEvent *self, WindowNoArgFn fn);
WindowNoArgFn WindowEvent_getOnZoomBack(const WindowEvent *self);

void    WindowEvent_setOnOcclusionChanged(WindowEvent *self, WindowOcclusionFn fn);
WindowOcclusionFn WindowEvent_getOnOcclusionChanged(const WindowEvent *self);

#endif
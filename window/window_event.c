#include "window/window_event.h"

#include <stddef.h>

#include "util/annotation.h"

;;DEFINITION
/**
 * ============================================================================
 * DEFINITION: WindowEvent
 * ============================================================================
 * Lifecycle event callback registry bound to an individual window instance.
 * Embedded directly within the window's platform state rather than heap allocated,
 * providing zero-allocation event dispatch. Every callback slot is nullable and
 * dispatched strictly on Thread 0 during event pump loops; unhandled events
 * incur zero overhead and safely no-op under the Cold-Strict, Hot-Minimal
 * validation rules.
 *
 * Supports vetoable termination requests via onQuitRequested, geometry changes,
 * state flips (fullscreen, minimize, restore), focus transitions, and occlusion
 * visibility updates.
 * ============================================================================
 */

;;OVERVIEW
/**
 * ============================================================================
 * CLASS: WindowEvent (window/window_event.c)
 * ============================================================================
 * SUMMARY:
 *   The fn-pointer lifecycle contract for ONE window. Embedded in the window's
 *   platform state (one instance per Window), filled once by the app, fired by
 *   the pump pass on Thread 0. Every slot is nullable and every fire is a
 *   no-op on a null slot — a window that ignores an event costs nothing and
 *   never crashes (the Cold-Strict, Hot-Minimal Validation Law).
 *
 *   Focus is Application-level, pressed is Window-level, quit is vetoable:
 *     - onFocusGained/onFocusLost fire only on key-window flips.
 *     - onPressed fires on mouse-down regardless of prior focus.
 *     - onQuitRequested (close btn / Cmd+Q / Cmd+W / Alt+F4) returns bool:
 *       true = may quit. A null slot allows the quit (default-proceed).
 *
 *   The fire dispatchers are the ONLY code that reads the slot pointers — the
 *   null-checks live here, nowhere else.
 *
 * STRUCT FIELDS:
 * ----------------------------------------------------------------------------
 *   void *self;                            // opaque owner of every slot
 *   WindowQuitRequestedFn onQuitRequested;  // vetoable quit request
 *   WindowNoArgFn onAfterQuit;              // fired after window destroyed
 *   WindowResizedFn onResized;              // content size changed (pixels)
 *   WindowMovedFn onMoved;                  // window origin changed (desktop points)
 *   WindowNoArgFn onFullscreen;             // entered fullscreen
 *   WindowNoArgFn onMinimized;              // minimized
 *   WindowNoArgFn onRestored;               // back to normal
 *   WindowNoArgFn onPressed;                // mouse pressed on the window
 *   WindowNoArgFn onFocusGained;            // key window became THIS window
 *   WindowNoArgFn onFocusLost;              // key window left THIS window
 *   WindowNoArgFn onZoomFilled;             // d64-click header zoom-to-fill
 *   WindowNoArgFn onZoomBack;               // exited zoom-to-fill
 *   WindowOcclusionFn onOcclusionChanged;   // occlusion flipped (visible flag)
 *
 * PRIVATE HELPERS:
 * ----------------------------------------------------------------------------
 *   (none)
 *
 * FUNCTION REGISTRY:
 * ----------------------------------------------------------------------------
 * Public Constructors: (.h)
 *   - WindowEvent_init(self)                : zeros every slot
 *
 * Private Constructors: (.c static)
 *   - (none)
 *
 * Public Core Functions: (.h)
 *   - WindowEvent_fireQuitRequested(self, window) : Dispatches quit query (returns true if may quit)
 *   - WindowEvent_fireAfterQuit(self, window)     : Dispatches post-destroy notification
 *   - WindowEvent_fireResized(self, window, width, height) : Dispatches resize notification
 *   - WindowEvent_fireMoved(self, window, x, y)    : Dispatches window-move notification
 *   - WindowEvent_fireFullscreen(self, window)    : Dispatches fullscreen transition
 *   - WindowEvent_fireMinimized(self, window)     : Dispatches minimize transition
 *   - WindowEvent_fireRestored(self, window)      : Dispatches restore transition
 *   - WindowEvent_firePressed(self, window)       : Dispatches mouse-down notification
 *   - WindowEvent_fireFocusGained(self, window)   : Dispatches focus gained notification
 *   - WindowEvent_fireFocusLost(self, window)     : Dispatches focus lost notification
 *   - WindowEvent_fireZoomFilled(self, window)    : Dispatches header zoom notification
 *   - WindowEvent_fireZoomBack(self, window)      : Dispatches zoom-exit notification
 *   - WindowEvent_fireOcclusionChanged(self, window, visible) : Dispatches occlusion update
 *
 * Private Core Functions: (.c static)
 *   - (none)
 *
 * Public Setters: (.h)
 *   - WindowEvent_setSelf(self, owner)
 *   - WindowEvent_setOnQuitRequested(self, fn)
 *   - WindowEvent_setOnAfterQuit(self, fn)
 *   - WindowEvent_setOnResized(self, fn)
 *   - WindowEvent_setOnMoved(self, fn)
 *   - WindowEvent_setOnFullscreen(self, fn)
 *   - WindowEvent_setOnMinimized(self, fn)
 *   - WindowEvent_setOnRestored(self, fn)
 *   - WindowEvent_setOnPressed(self, fn)
 *   - WindowEvent_setOnFocusGained(self, fn)
 *   - WindowEvent_setOnFocusLost(self, fn)
 *   - WindowEvent_setOnZoomFilled(self, fn)
 *   - WindowEvent_setOnZoomBack(self, fn)
 *   - WindowEvent_setOnOcclusionChanged(self, fn)
 *
 * Private Setters: (.c static)
 *   - (none)
 *
 * Public Getters: (.h)
 *   - WindowEvent_getSelf(self)
 *   - WindowEvent_getOnQuitRequested(self)
 *   - WindowEvent_getOnAfterQuit(self)
 *   - WindowEvent_getOnResized(self)
 *   - WindowEvent_getOnMoved(self)
 *   - WindowEvent_getOnFullscreen(self)
 *   - WindowEvent_getOnMinimized(self)
 *   - WindowEvent_getOnRestored(self)
 *   - WindowEvent_getOnPressed(self)
 *   - WindowEvent_getOnFocusGained(self)
 *   - WindowEvent_getOnFocusLost(self)
 *   - WindowEvent_getOnZoomFilled(self)
 *   - WindowEvent_getOnZoomBack(self)
 *   - WindowEvent_getOnOcclusionChanged(self)
 *
 * Private Getters: (.c static)
 *   - (none)
 * ============================================================================
 */

// CONSTRUCTORS (PUBLIC & PRIVATE)
/** Clears all callback slots and owner data, returning false for null self. */
bool WindowEvent_init(WindowEvent *self) {
    if(self == nullptr)
        return false;
    (*self).self = nullptr;
    (*self).onQuitRequested = nullptr;
    (*self).onAfterQuit = nullptr;
    (*self).onResized = nullptr;
    (*self).onMoved = nullptr;
    (*self).onFullscreen = nullptr;
    (*self).onMinimized = nullptr;
    (*self).onRestored = nullptr;
    (*self).onPressed = nullptr;
    (*self).onFocusGained = nullptr;
    (*self).onFocusLost = nullptr;
    (*self).onZoomFilled = nullptr;
    (*self).onZoomBack = nullptr;
    (*self).onOcclusionChanged = nullptr;
    return true;
}

// CORE FUNCTIONS (PUBLIC & PRIVATE)
/** Calls the quit-request callback; permits closing when no handler is installed. */
bool WindowEvent_fireQuitRequested(WindowEvent *self, Window *window) {
    if(self == nullptr)
        return true;
    if((*self).onQuitRequested == nullptr)
        return true;
    return (*self).onQuitRequested((*self).self, window);
}

/** Notifies the owner after the window has completed its close path. */
void WindowEvent_fireAfterQuit(WindowEvent *self, Window *window) {
    if(self == nullptr)
        return;
    if((*self).onAfterQuit == nullptr)
        return;
    (*self).onAfterQuit((*self).self, window);
}

/** Notifies the owner of the window's new content dimensions. */
void WindowEvent_fireResized(WindowEvent *self, Window *window, int width, int height) {
    if(self == nullptr)
        return;
    if((*self).onResized == nullptr)
        return;
    (*self).onResized((*self).self, window, width, height);
}

/** Notifies the owner that the window origin changed. */
void WindowEvent_fireMoved(WindowEvent *self, Window *window, int x, int y) {
    if(self == nullptr)
        return;
    if((*self).onMoved == nullptr)
        return;
    (*self).onMoved((*self).self, window, x, y);
}

/** Notifies the owner that the window entered fullscreen. */
void WindowEvent_fireFullscreen(WindowEvent *self, Window *window) {
    if(self == nullptr)
        return;
    if((*self).onFullscreen == nullptr)
        return;
    (*self).onFullscreen((*self).self, window);
}

/** Notifies the owner that the window was minimized. */
void WindowEvent_fireMinimized(WindowEvent *self, Window *window) {
    if(self == nullptr)
        return;
    if((*self).onMinimized == nullptr)
        return;
    (*self).onMinimized((*self).self, window);
}

/** Notifies the owner that the window was restored. */
void WindowEvent_fireRestored(WindowEvent *self, Window *window) {
    if(self == nullptr)
        return;
    if((*self).onRestored == nullptr)
        return;
    (*self).onRestored((*self).self, window);
}

/** Notifies the owner of a mouse press on the window. */
void WindowEvent_firePressed(WindowEvent *self, Window *window) {
    if(self == nullptr)
        return;
    if((*self).onPressed == nullptr)
        return;
    (*self).onPressed((*self).self, window);
}

/** Notifies the owner that this window gained key focus. */
void WindowEvent_fireFocusGained(WindowEvent *self, Window *window) {
    if(self == nullptr)
        return;
    if((*self).onFocusGained == nullptr)
        return;
    (*self).onFocusGained((*self).self, window);
}

/** Notifies the owner that this window lost key focus. */
void WindowEvent_fireFocusLost(WindowEvent *self, Window *window) {
    if(self == nullptr)
        return;
    if((*self).onFocusLost == nullptr)
        return;
    (*self).onFocusLost((*self).self, window);
}

/** Notifies the owner that the window entered zoom-to-fill. */
void WindowEvent_fireZoomFilled(WindowEvent *self, Window *window) {
    if(self == nullptr)
        return;
    if((*self).onZoomFilled == nullptr)
        return;
    (*self).onZoomFilled((*self).self, window);
}

/** Notifies the owner that the window exited zoom-to-fill. */
void WindowEvent_fireZoomBack(WindowEvent *self, Window *window) {
    if(self == nullptr)
        return;
    if((*self).onZoomBack == nullptr)
        return;
    (*self).onZoomBack((*self).self, window);
}

/** Notifies the owner when the window's occlusion visibility changes. */
void WindowEvent_fireOcclusionChanged(WindowEvent *self, Window *window, bool visible) {
    if(self == nullptr)
        return;
    if((*self).onOcclusionChanged == nullptr)
        return;
    (*self).onOcclusionChanged((*self).self, window, visible);
}

// SETTERS (PUBLIC & PRIVATE)

;;SETTER
/** Sets the opaque owner value passed to every registered callback. */
void WindowEvent_setSelf(WindowEvent *self, void *owner) {
    if(self == nullptr)
        return;
    (*self).self = owner;
}

;;SETTER
/** Replaces the vetoable quit-request callback. */
void WindowEvent_setOnQuitRequested(WindowEvent *self, WindowQuitRequestedFn fn) {
    if(self == nullptr)
        return;
    (*self).onQuitRequested = fn;
}

;;SETTER
/** Replaces the post-close callback. */
void WindowEvent_setOnAfterQuit(WindowEvent *self, WindowNoArgFn fn) {
    if(self == nullptr)
        return;
    (*self).onAfterQuit = fn;
}

;;SETTER
/** Replaces the content-resize callback. */
void WindowEvent_setOnResized(WindowEvent *self, WindowResizedFn fn) {
    if(self == nullptr)
        return;
    (*self).onResized = fn;
}

;;SETTER
/** Replaces the window-move callback. */
void WindowEvent_setOnMoved(WindowEvent *self, WindowMovedFn fn) {
    if(self == nullptr)
        return;
    (*self).onMoved = fn;
}

;;SETTER
/** Replaces the fullscreen-entry callback. */
void WindowEvent_setOnFullscreen(WindowEvent *self, WindowNoArgFn fn) {
    if(self == nullptr)
        return;
    (*self).onFullscreen = fn;
}

;;SETTER
/** Replaces the minimize callback. */
void WindowEvent_setOnMinimized(WindowEvent *self, WindowNoArgFn fn) {
    if(self == nullptr)
        return;
    (*self).onMinimized = fn;
}

;;SETTER
/** Replaces the restore callback. */
void WindowEvent_setOnRestored(WindowEvent *self, WindowNoArgFn fn) {
    if(self == nullptr)
        return;
    (*self).onRestored = fn;
}

;;SETTER
/** Replaces the mouse-press callback. */
void WindowEvent_setOnPressed(WindowEvent *self, WindowNoArgFn fn) {
    if(self == nullptr)
        return;
    (*self).onPressed = fn;
}

;;SETTER
/** Replaces the focus-gained callback. */
void WindowEvent_setOnFocusGained(WindowEvent *self, WindowNoArgFn fn) {
    if(self == nullptr)
        return;
    (*self).onFocusGained = fn;
}

;;SETTER
/** Replaces the focus-lost callback. */
void WindowEvent_setOnFocusLost(WindowEvent *self, WindowNoArgFn fn) {
    if(self == nullptr)
        return;
    (*self).onFocusLost = fn;
}

;;SETTER
/** Replaces the zoom-to-fill callback. */
void WindowEvent_setOnZoomFilled(WindowEvent *self, WindowNoArgFn fn) {
    if(self == nullptr)
        return;
    (*self).onZoomFilled = fn;
}

;;SETTER
/** Replaces the zoom-exit callback. */
void WindowEvent_setOnZoomBack(WindowEvent *self, WindowNoArgFn fn) {
    if(self == nullptr)
        return;
    (*self).onZoomBack = fn;
}

;;SETTER
/** Replaces the occlusion-visibility callback. */
void WindowEvent_setOnOcclusionChanged(WindowEvent *self, WindowOcclusionFn fn) {
    if(self == nullptr)
        return;
    (*self).onOcclusionChanged = fn;
}

// GETTERS (PUBLIC & PRIVATE)

;;GETTER
/** Returns the opaque owner supplied to callback handlers. */
void *WindowEvent_getSelf(const WindowEvent *self) {
    if(self == nullptr)
        return nullptr;
    return (*self).self;
}

;;GETTER
/** Returns the currently registered quit-request callback. */
WindowQuitRequestedFn WindowEvent_getOnQuitRequested(const WindowEvent *self) {
    if(self == nullptr)
        return nullptr;
    return (*self).onQuitRequested;
}

;;GETTER
/** Returns the currently registered post-close callback. */
WindowNoArgFn WindowEvent_getOnAfterQuit(const WindowEvent *self) {
    if(self == nullptr)
        return nullptr;
    return (*self).onAfterQuit;
}

;;GETTER
/** Returns the currently registered content-resize callback. */
WindowResizedFn WindowEvent_getOnResized(const WindowEvent *self) {
    if(self == nullptr)
        return nullptr;
    return (*self).onResized;
}

;;GETTER
/** Returns the currently registered window-move callback. */
WindowMovedFn WindowEvent_getOnMoved(const WindowEvent *self) {
    if(self == nullptr)
        return nullptr;
    return (*self).onMoved;
}

;;GETTER
/** Returns the currently registered fullscreen callback. */
WindowNoArgFn WindowEvent_getOnFullscreen(const WindowEvent *self) {
    if(self == nullptr)
        return nullptr;
    return (*self).onFullscreen;
}

;;GETTER
/** Returns the currently registered minimize callback. */
WindowNoArgFn WindowEvent_getOnMinimized(const WindowEvent *self) {
    if(self == nullptr)
        return nullptr;
    return (*self).onMinimized;
}

;;GETTER
/** Returns the currently registered restore callback. */
WindowNoArgFn WindowEvent_getOnRestored(const WindowEvent *self) {
    if(self == nullptr)
        return nullptr;
    return (*self).onRestored;
}

;;GETTER
/** Returns the currently registered mouse-press callback. */
WindowNoArgFn WindowEvent_getOnPressed(const WindowEvent *self) {
    if(self == nullptr)
        return nullptr;
    return (*self).onPressed;
}

;;GETTER
/** Returns the currently registered focus-gained callback. */
WindowNoArgFn WindowEvent_getOnFocusGained(const WindowEvent *self) {
    if(self == nullptr)
        return nullptr;
    return (*self).onFocusGained;
}

;;GETTER
/** Returns the currently registered focus-lost callback. */
WindowNoArgFn WindowEvent_getOnFocusLost(const WindowEvent *self) {
    if(self == nullptr)
        return nullptr;
    return (*self).onFocusLost;
}

;;GETTER
/** Returns the currently registered zoom-to-fill callback. */
WindowNoArgFn WindowEvent_getOnZoomFilled(const WindowEvent *self) {
    if(self == nullptr)
        return nullptr;
    return (*self).onZoomFilled;
}

;;GETTER
/** Returns the currently registered zoom-exit callback. */
WindowNoArgFn WindowEvent_getOnZoomBack(const WindowEvent *self) {
    if(self == nullptr)
        return nullptr;
    return (*self).onZoomBack;
}

;;GETTER
/** Returns the currently registered occlusion-visibility callback. */
WindowOcclusionFn WindowEvent_getOnOcclusionChanged(const WindowEvent *self) {
    if(self == nullptr)
        return nullptr;
    return (*self).onOcclusionChanged;
}

#ifndef LIFETIME_WINDOW_STATUS_BAR_H
#define LIFETIME_WINDOW_STATUS_BAR_H

#include "vex.h"
#include "lang/annotation.h"
#include "window/window.h"

/**
 * window/status_bar.h -- macOS menu bar status item (NSStatusItem) API.
 *
 * Manages a system menu bar status item with title, tooltip, visibility,
 * anchor rect query, click callback, and optional borderless popover Window
 * attachment. Safely degrades in headless mode when NSApp is nil.
 */

#define STATUS_BAR_TITLE_CAP   128u
#define STATUS_BAR_TOOLTIP_CAP 256u

;;PLATFORM_EXCLUSIVE("macOS")
;;CLASSNAME("StatusBarItem")

typedef struct StatusBarItem StatusBarItem;

typedef fn (*StatusBarClickFn)(StatusBarItem *item, any userdata);

/**
 * Creates a macOS status bar item with the given initial title.
 * @param title initial status bar title (may be nullptr)
 * @return new StatusBarItem instance, or nullptr on allocation failure
 */
StatusBarItem *StatusBarItem_1(string title);
#define StatusBarItem(...) CONSTRUCTOR_DISPATCH(StatusBarItem, __VA_ARGS__)

/**
 * Removes the status item from NSStatusBar and frees its handle.
 * @param item status bar item to destroy
 */
fn StatusBarItem_destroy(StatusBarItem *item);

/**
 * Sets the status bar button title text.
 * @param item  status bar item
 * @param title new title string (may be nullptr)
 */
fn StatusBarItem_setTitle(StatusBarItem *item, string title);

/**
 * Sets the hover tooltip text for the status bar button.
 * @param item    status bar item
 * @param tooltip new tooltip string (may be nullptr)
 */
fn StatusBarItem_setTooltip(StatusBarItem *item, string tooltip);

/**
 * Shows or hides the status item in the system menu bar.
 * @param item    status bar item
 * @param visible true to show, false to hide
 */
fn StatusBarItem_setVisible(StatusBarItem *item, bool visible);

/**
 * Queries whether the status item is currently marked visible.
 * @param item status bar item
 * @return true if visible, false otherwise or if item is nullptr
 */
bool StatusBarItem_isVisible(const StatusBarItem *item);

/**
 * Queries the screen-space anchor rectangle of the status item button
 * in top-left desktop point coordinates.
 * @param item status bar item
 * @param outX destination for X coordinate (may be nullptr)
 * @param outY destination for Y coordinate (may be nullptr)
 * @param outW destination for width (may be nullptr)
 * @param outH destination for height (may be nullptr)
 * @return true if a live anchor rect was read, false in headless or invalid state
 */
bool StatusBarItem_getAnchorRect(const StatusBarItem *item,
                                 i32 *outX, i32 *outY, i32 *outW, i32 *outH);

/**
 * Registers a click callback invoked when the status item is clicked.
 * @param item     status bar item
 * @param cb       click callback (nullptr to clear)
 * @param userdata caller context passed to cb
 */
fn StatusBarItem_onClick(StatusBarItem *item, StatusBarClickFn cb, any userdata);

/**
 * Attaches a borderless popover Window whose visibility toggles below the
 * status item button on click. Safely no-ops in headless mode when NSApp is nil.
 * @param item       status bar item
 * @param popoverWin borrowed Window handle (nullptr to detach)
 */
fn StatusBarItem_attachPopoverWindow(StatusBarItem *item, Window *popoverWin);

/**
 * Simulates a click on the status item for headless testing.
 * @param item status bar item
 */
fn StatusBarItem_triggerClickForTest(StatusBarItem *item);

#endif

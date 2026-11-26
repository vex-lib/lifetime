#ifndef LIFETIME_WINDOW_STATUS_BAR_INTERNAL_H
#define LIFETIME_WINDOW_STATUS_BAR_INTERNAL_H

#include "window/status_bar.h"

/**
 * Internal storage for a macOS StatusBarItem allocated from Lifetime_arena.
 *
 * utf8 title[]             - NUL-terminated status bar button title
 * utf8 tooltip[]           - NUL-terminated hover tooltip text
 * bool visible             - visibility state in NSStatusBar
 * bool popoverOpen         - cached toggle state for attached popover
 * StatusBarClickFn onClick - optional click callback
 * any userdata             - caller context pointer for onClick
 * Window *popoverWin       - borrowed popover Window toggled on click
 * any nsStatusItem         - retained NSStatusItem handle (or nullptr)
 * any target               - retained VexStatusBarTarget trampoline
 */
struct StatusBarItem {
    utf8 title[STATUS_BAR_TITLE_CAP];
    utf8 tooltip[STATUS_BAR_TOOLTIP_CAP];
    bool visible;
    bool popoverOpen;
    StatusBarClickFn onClick;
    any userdata;
    Window *popoverWin;
    any nsStatusItem;
    any target;
};

#endif

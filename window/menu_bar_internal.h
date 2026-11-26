#ifndef LIFETIME_WINDOW_MENU_BAR_INTERNAL_H
#define LIFETIME_WINDOW_MENU_BAR_INTERNAL_H

#include "window/menu_bar.h"

/**
 * Single menu or submenu slot stored inside OsMenuBar.
 *
 * utf8 title[]     - NUL-terminated menu title
 * i32 parentMenuId - parent menu index (-1 for top-level menus)
 * any nsMenu       - retained native NSMenu handle (or nullptr before install)
 */
typedef struct OsMenuSlot {
    utf8 title[OS_MENU_TITLE_CAP];
    i32 parentMenuId;
    any nsMenu;
} OsMenuSlot;

/**
 * Single item or separator slot stored inside OsMenuBar.
 *
 * i32 menuId          - owning menu index
 * u32 itemId          - caller-assigned item identifier
 * utf8 title[]        - NUL-terminated item title
 * utf8 keyEquivalent[] - NUL-terminated key equivalent string
 * u32 modifiers       - OS_MENU_MOD_* bitmask
 * OsMenuActionFn cb   - action callback
 * any userdata        - caller context pointer
 * bool isSeparator    - true for separator items
 * bool checked        - checkmark state
 * bool enabled        - interactive enabled state
 * any nsItem          - retained native NSMenuItem handle (or nullptr)
 */
typedef struct OsMenuItemSlot {
    i32 menuId;
    u32 itemId;
    utf8 title[OS_MENU_TITLE_CAP];
    utf8 keyEquivalent[OS_MENU_KEY_CAP];
    u32 modifiers;
    OsMenuActionFn cb;
    any userdata;
    bool isSeparator;
    bool checked;
    bool enabled;
    any nsItem;
} OsMenuItemSlot;

/**
 * Native OS menu bar storage allocated from Lifetime_arena.
 *
 * OsMenuSlot menus[]     - registered top-level menus and submenus
 * u32 menuCount          - active menu slot count
 * OsMenuItemSlot items[] - registered menu items and separators
 * u32 itemCount          - active item slot count
 * any mainMenu           - retained root NSMenu handle
 * any target             - retained ObjC target-action trampoline
 * bool installed         - true after OsMenuBar_install succeeds
 */
struct OsMenuBar {
    OsMenuSlot menus[OS_MENU_MAX_MENUS];
    u32 menuCount;
    OsMenuItemSlot items[OS_MENU_MAX_ITEMS];
    u32 itemCount;
    any mainMenu;
    any target;
    bool installed;
};

#endif

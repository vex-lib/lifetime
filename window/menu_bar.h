#ifndef LIFETIME_WINDOW_MENU_BAR_H
#define LIFETIME_WINDOW_MENU_BAR_H

#include "vex.h"
#include "lang/annotation.h"

/**
 * window/menu_bar.h -- native OS application menu bar API.
 *
 * Provides a platform-agnostic C23 handle for constructing hierarchical
 * application menus (top-level menus, submenus, items with key equivalents
 * and modifier masks, separators, checkmarks, and enable states) and
 * installing them into the host OS menu bar.
 */

#define OS_MENU_MOD_CMD   (1u << 0)
#define OS_MENU_MOD_SHIFT (1u << 1)
#define OS_MENU_MOD_OPT   (1u << 2)
#define OS_MENU_MOD_CTRL  (1u << 3)

#define OS_MENU_MAX_MENUS 32u
#define OS_MENU_MAX_ITEMS 256u
#define OS_MENU_TITLE_CAP 128u
#define OS_MENU_KEY_CAP   32u

;;CLASSNAME("OsMenuBar")

typedef struct OsMenuBar OsMenuBar;

typedef fn (*OsMenuActionFn)(u32 itemId, any userdata);

/**
 * Allocates and initializes an empty OsMenuBar from Lifetime_arena.
 * @return new OsMenuBar instance, or nullptr on allocation failure
 */
OsMenuBar *OsMenuBar_0();
#define OsMenuBar(...) CONSTRUCTOR_DISPATCH(OsMenuBar, __VA_ARGS__)

/**
 * Releases native menu objects and frees the OsMenuBar handle.
 * @param mb menu bar instance to destroy
 */
fn OsMenuBar_destroy(OsMenuBar *mb);

/**
 * Appends a top-level menu to the menu bar.
 * @param mb    menu bar instance
 * @param title display title for the top-level menu
 * @return non-negative menuId on success, or -1 on failure
 */
i32 OsMenuBar_addMenu(OsMenuBar *mb, string title);

/**
 * Appends a nested submenu under an existing menu.
 * @param mb           menu bar instance
 * @param parentMenuId parent menu identifier
 * @param title        display title for the submenu item
 * @return non-negative menuId on success, or -1 on failure
 */
i32 OsMenuBar_addSubmenu(OsMenuBar *mb, i32 parentMenuId, string title);

/**
 * Appends an actionable item to the specified menu.
 * @param mb            menu bar instance
 * @param menuId        target menu identifier
 * @param itemId        caller-assigned unique item identifier
 * @param title         display title for the menu item
 * @param keyEquivalent key equivalent string (may be nullptr or empty)
 * @param modifiers     OS_MENU_MOD_* bitmask
 * @param cb            action callback invoked when triggered
 * @param userdata      caller context passed to cb
 * @return true on success, false on invalid arguments or full capacity
 */
bool OsMenuBar_addItem(OsMenuBar *mb, i32 menuId, u32 itemId, string title,
                       string keyEquivalent, u32 modifiers,
                       OsMenuActionFn cb, any userdata);

/**
 * Appends a visual separator line to the specified menu.
 * @param mb     menu bar instance
 * @param menuId target menu identifier
 * @return true on success, false on invalid arguments or full capacity
 */
bool OsMenuBar_addSeparator(OsMenuBar *mb, i32 menuId);

/**
 * Updates the checked state of a menu item matching itemId.
 * @param mb      menu bar instance
 * @param itemId  target item identifier
 * @param checked true to show checkmark, false to clear
 */
fn OsMenuBar_setItemChecked(OsMenuBar *mb, u32 itemId, bool checked);

/**
 * Updates the enabled state of a menu item matching itemId.
 * @param mb      menu bar instance
 * @param itemId  target item identifier
 * @param enabled true to enable, false to disable
 */
fn OsMenuBar_setItemEnabled(OsMenuBar *mb, u32 itemId, bool enabled);

/**
 * Builds the native OS menu hierarchy and installs it as the application menu.
 * Safely returns true without crashing when NSApp is nil in headless mode.
 * @param mb menu bar instance
 * @return true on success, false on invalid handle
 */
bool OsMenuBar_install(OsMenuBar *mb);

/**
 * Invokes the callback of an enabled item matching itemId for headless testing.
 * @param mb     menu bar instance
 * @param itemId target item identifier
 * @return true if a matching enabled item with callback was invoked
 */
bool OsMenuBar_triggerForTest(OsMenuBar *mb, u32 itemId);

/**
 * Returns the number of registered menus (top-level and submenus).
 * @param mb menu bar instance
 * @return menu count, or 0 if mb is nullptr
 */
u32 OsMenuBar_menuCount(const OsMenuBar *mb);

/**
 * Returns the number of registered items (including separators).
 * @param mb menu bar instance
 * @return item count, or 0 if mb is nullptr
 */
u32 OsMenuBar_itemCount(const OsMenuBar *mb);

#endif

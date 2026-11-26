#import <AppKit/AppKit.h>

#include "vex.h"
#include "nio/lifetime_memory.h"
#include "re/type/re_type.h"
#include "window/menu_bar_internal.h"

#include <string.h>

;;DEFINITION
/**
 * ============================================================================
 * DEFINITION: OsMenuBar
 * ============================================================================
 * Native macOS application menu bar backed by AppKit NSMenu and NSMenuItem.
 * Owned by lifetime/window and allocated from Lifetime_arena. Stores a flat
 * array of menu slots and item slots, constructs the native NSMenu hierarchy
 * with an App menu and target-action trampoline on install, and safely
 * operates in headless mode when NSApp is nil.
 * ============================================================================
 */

;;OVERVIEW
/**
 * ============================================================================
 * CLASS: OsMenuBar (window/menu_bar_cocoa.m)
 * ============================================================================
 * Encapsulates native macOS NSMenu construction, modifier mapping, item state
 * updates, and target-action callback dispatch.
 *
 * --CONSTANTS----------------------------
 * public   OS_MENU_MAX_MENUS           maximum menus per bar (32)
 * public   OS_MENU_MAX_ITEMS           maximum items per bar (256)
 *
 * --FIELDS-------------------------------
 * private  OsMenuSlot menus[]          registered top-level menus and submenus
 * private  u32 menuCount               active menu count
 * private  OsMenuItemSlot items[]      registered items and separators
 * private  u32 itemCount               active item count
 * private  any mainMenu                retained root NSMenu handle
 * private  any target                  retained VexOsMenuTarget trampoline
 * private  bool installed              true once installed
 *
 * --CONSTRUCTORS-------------------------
 * public   OsMenuBar_0()               allocates empty OsMenuBar
 *
 * --FUNCTIONS----------------------------
 * public   OsMenuBar_destroy           releases native handles and frees bar
 * public   OsMenuBar_addMenu           appends top-level menu
 * public   OsMenuBar_addSubmenu        appends nested submenu
 * public   OsMenuBar_addItem           appends actionable menu item
 * public   OsMenuBar_addSeparator      appends separator item
 * public   OsMenuBar_install           builds NSMenu tree and installs on NSApp
 * public   OsMenuBar_triggerForTest    invokes item callback headlessly
 * private  copyBoundedText             copies string into fixed utf8 buffer
 * private  mapModifiers                maps OS_MENU_MOD_* to NSEventModifierFlags
 * private  releaseNativeHandles        releases retained AppKit objects
 *
 * --GETTERS------------------------------
 * public   OsMenuBar_menuCount         returns menu count
 * public   OsMenuBar_itemCount         returns item count
 *
 * --SETTERS------------------------------
 * public   OsMenuBar_setItemChecked    updates item checkmark state
 * public   OsMenuBar_setItemEnabled    updates item enabled state
 * ============================================================================
 */

@interface VexOsMenuTarget : NSObject
@property (nonatomic, assign) OsMenuBar *owner;
- (fn)onMenuItem:(id)sender;
@end

@implementation VexOsMenuTarget
- (fn)onMenuItem:(id)sender
{
    if (!self.owner || !sender)
        return;

    if (![sender isKindOfClass:[NSMenuItem class]])
        return;

    NSMenuItem *item = (NSMenuItem*) sender;
    u32 targetId = (u32) item.tag;
    OsMenuBar_triggerForTest(self.owner, targetId);
}
@end

;;GETTER
u32 OsMenuBar_menuCount(const OsMenuBar *mb)
{
    if (!mb)
        return 0u;

    return (*mb).menuCount;
}

;;GETTER
u32 OsMenuBar_itemCount(const OsMenuBar *mb)
{
    if (!mb)
        return 0u;

    return (*mb).itemCount;
}

;;SETTER
static fn OsMenuBar_setMenuCountField(OsMenuBar *mb, u32 count)
{
    if (mb)
        (*mb).menuCount = count;
}

;;SETTER
static fn OsMenuBar_setItemCountField(OsMenuBar *mb, u32 count)
{
    if (mb)
        (*mb).itemCount = count;
}

;;GETTER
static OsMenuSlot OsMenuBar_menuAt(const OsMenuBar *mb, u32 index)
{
    OsMenuSlot empty;
    memset(&empty, 0, sizeof(OsMenuSlot));
    if (!mb || index >= OS_MENU_MAX_MENUS)
        return empty;

    return (*mb).menus[index];
}

;;SETTER
static fn OsMenuBar_setMenuAt(OsMenuBar *mb, u32 index, OsMenuSlot slot)
{
    if (mb && index < OS_MENU_MAX_MENUS)
        (*mb).menus[index] = slot;
}

;;GETTER
static OsMenuItemSlot OsMenuBar_itemAt(const OsMenuBar *mb, u32 index)
{
    OsMenuItemSlot empty;
    memset(&empty, 0, sizeof(OsMenuItemSlot));
    if (!mb || index >= OS_MENU_MAX_ITEMS)
        return empty;

    return (*mb).items[index];
}

;;SETTER
static fn OsMenuBar_setItemAt(OsMenuBar *mb, u32 index, OsMenuItemSlot slot)
{
    if (mb && index < OS_MENU_MAX_ITEMS)
        (*mb).items[index] = slot;
}

;;GETTER
static any OsMenuBar_mainMenuField(const OsMenuBar *mb)
{
    if (!mb)
        return nullptr;

    return (*mb).mainMenu;
}

;;SETTER
static fn OsMenuBar_setMainMenuField(OsMenuBar *mb, any menu)
{
    if (mb)
        (*mb).mainMenu = menu;
}

;;GETTER
static any OsMenuBar_targetField(const OsMenuBar *mb)
{
    if (!mb)
        return nullptr;

    return (*mb).target;
}

;;SETTER
static fn OsMenuBar_setTargetField(OsMenuBar *mb, any target)
{
    if (mb)
        (*mb).target = target;
}

;;SETTER
static fn OsMenuBar_setInstalledField(OsMenuBar *mb, bool installed)
{
    if (mb)
        (*mb).installed = installed;
}

/**
 * Copies a NUL-terminated string into a bounded utf8 buffer.
 * @param dst destination buffer
 * @param cap destination capacity in bytes
 * @param src source string (may be nullptr)
 */
static fn copyBoundedText(utf8 *dst, bytes cap, string src)
{
    if (!dst || cap == 0u)
        return;

    memset(dst, 0, cap);
    if (!src)
        return;

    bytes len = strlen(src);
    if (len >= cap)
        len = cap - 1u;

    memcpy(dst, src, len);
    dst[len] = 0;
}

/**
 * Maps OS_MENU_MOD_* bitflags to AppKit NSEventModifierFlags.
 * @param modifiers OS_MENU_MOD_* bitmask
 * @return AppKit modifier mask
 */
static NSEventModifierFlags mapModifiers(u32 modifiers)
{
    NSEventModifierFlags mask = 0;
    if ((modifiers & OS_MENU_MOD_CMD) != 0u)
        mask |= NSEventModifierFlagCommand;

    if ((modifiers & OS_MENU_MOD_SHIFT) != 0u)
        mask |= NSEventModifierFlagShift;

    if ((modifiers & OS_MENU_MOD_OPT) != 0u)
        mask |= NSEventModifierFlagOption;

    if ((modifiers & OS_MENU_MOD_CTRL) != 0u)
        mask |= NSEventModifierFlagControl;

    return mask;
}

/**
 * Releases all retained AppKit handles owned by the menu bar.
 * @param mb menu bar instance
 */
static fn releaseNativeHandles(OsMenuBar *mb)
{
    if (!mb)
        return;

    u32 items = OsMenuBar_itemCount(mb);
    for (u32 i = 0u; i < items; i++) {
        OsMenuItemSlot slot = OsMenuBar_itemAt(mb, i);
        if (slot.nsItem) {
            CFBridgingRelease(slot.nsItem);
            slot.nsItem = nullptr;
            OsMenuBar_setItemAt(mb, i, slot);
        }
    }

    u32 menus = OsMenuBar_menuCount(mb);
    for (u32 i = 0u; i < menus; i++) {
        OsMenuSlot slot = OsMenuBar_menuAt(mb, i);
        if (slot.nsMenu) {
            CFBridgingRelease(slot.nsMenu);
            slot.nsMenu = nullptr;
            OsMenuBar_setMenuAt(mb, i, slot);
        }
    }

    any mainMenu = OsMenuBar_mainMenuField(mb);
    if (mainMenu) {
        CFBridgingRelease(mainMenu);
        OsMenuBar_setMainMenuField(mb, nullptr);
    }

    any target = OsMenuBar_targetField(mb);
    if (target) {
        VexOsMenuTarget *t = (__bridge VexOsMenuTarget*) target;
        t.owner = nullptr;
        CFBridgingRelease(target);
        OsMenuBar_setTargetField(mb, nullptr);
    }
}

;;CONSTRUCTOR
OsMenuBar *OsMenuBar_0()
{
    OsMenuBar *mb = (OsMenuBar*) MemoryArena_alloc(Lifetime_arena(), TYPE_BYTE_ARRAY, sizeof(OsMenuBar));
    if (!mb)
        return nullptr;

    memset(mb, 0, sizeof(OsMenuBar));
    return mb;
}

;;DECONSTRUCT
fn OsMenuBar_destroy(OsMenuBar *mb)
{
    if (!mb)
        return;

    releaseNativeHandles(mb);
    MemoryArena_free(Lifetime_arena(), mb);
}

i32 OsMenuBar_addMenu(OsMenuBar *mb, string title)
{
    if (!mb || !title)
        return -1;

    u32 count = OsMenuBar_menuCount(mb);
    if (count >= OS_MENU_MAX_MENUS)
        return -1;

    OsMenuSlot slot;
    memset(&slot, 0, sizeof(OsMenuSlot));
    copyBoundedText(slot.title, OS_MENU_TITLE_CAP, title);
    slot.parentMenuId = -1;
    OsMenuBar_setMenuAt(mb, count, slot);
    OsMenuBar_setMenuCountField(mb, count + 1u);
    return (i32) count;
}

i32 OsMenuBar_addSubmenu(OsMenuBar *mb, i32 parentMenuId, string title)
{
    if (!mb || !title || parentMenuId < 0)
        return -1;

    u32 count = OsMenuBar_menuCount(mb);
    if ((u32) parentMenuId >= count || count >= OS_MENU_MAX_MENUS)
        return -1;

    OsMenuSlot slot;
    memset(&slot, 0, sizeof(OsMenuSlot));
    copyBoundedText(slot.title, OS_MENU_TITLE_CAP, title);
    slot.parentMenuId = parentMenuId;
    OsMenuBar_setMenuAt(mb, count, slot);
    OsMenuBar_setMenuCountField(mb, count + 1u);
    return (i32) count;
}

bool OsMenuBar_addItem(OsMenuBar *mb, i32 menuId, u32 itemId, string title,
                       string keyEquivalent, u32 modifiers,
                       OsMenuActionFn cb, any userdata)
{
    if (!mb || !title || menuId < 0)
        return false;

    if ((u32) menuId >= OsMenuBar_menuCount(mb))
        return false;

    u32 count = OsMenuBar_itemCount(mb);
    if (count >= OS_MENU_MAX_ITEMS)
        return false;

    OsMenuItemSlot slot;
    memset(&slot, 0, sizeof(OsMenuItemSlot));
    slot.menuId = menuId;
    slot.itemId = itemId;
    copyBoundedText(slot.title, OS_MENU_TITLE_CAP, title);
    copyBoundedText(slot.keyEquivalent, OS_MENU_KEY_CAP, keyEquivalent);
    slot.modifiers = modifiers;
    slot.cb = cb;
    slot.userdata = userdata;
    slot.isSeparator = false;
    slot.checked = false;
    slot.enabled = true;
    OsMenuBar_setItemAt(mb, count, slot);
    OsMenuBar_setItemCountField(mb, count + 1u);
    return true;
}

bool OsMenuBar_addSeparator(OsMenuBar *mb, i32 menuId)
{
    if (!mb || menuId < 0)
        return false;

    if ((u32) menuId >= OsMenuBar_menuCount(mb))
        return false;

    u32 count = OsMenuBar_itemCount(mb);
    if (count >= OS_MENU_MAX_ITEMS)
        return false;

    OsMenuItemSlot slot;
    memset(&slot, 0, sizeof(OsMenuItemSlot));
    slot.menuId = menuId;
    slot.isSeparator = true;
    slot.enabled = true;
    OsMenuBar_setItemAt(mb, count, slot);
    OsMenuBar_setItemCountField(mb, count + 1u);
    return true;
}

;;SETTER
fn OsMenuBar_setItemChecked(OsMenuBar *mb, u32 itemId, bool checked)
{
    if (!mb)
        return;

    u32 count = OsMenuBar_itemCount(mb);
    for (u32 i = 0u; i < count; i++) {
        OsMenuItemSlot slot = OsMenuBar_itemAt(mb, i);
        if (!slot.isSeparator && slot.itemId == itemId) {
            slot.checked = checked;
            if (slot.nsItem) {
                NSMenuItem *nsItem = (__bridge NSMenuItem*) slot.nsItem;
                nsItem.state = checked ? NSControlStateValueOn : NSControlStateValueOff;
            }
            OsMenuBar_setItemAt(mb, i, slot);
        }
    }
}

;;SETTER
fn OsMenuBar_setItemEnabled(OsMenuBar *mb, u32 itemId, bool enabled)
{
    if (!mb)
        return;

    u32 count = OsMenuBar_itemCount(mb);
    for (u32 i = 0u; i < count; i++) {
        OsMenuItemSlot slot = OsMenuBar_itemAt(mb, i);
        if (!slot.isSeparator && slot.itemId == itemId) {
            slot.enabled = enabled;
            if (slot.nsItem) {
                NSMenuItem *nsItem = (__bridge NSMenuItem*) slot.nsItem;
                nsItem.enabled = enabled ? YES : NO;
            }
            OsMenuBar_setItemAt(mb, i, slot);
        }
    }
}

bool OsMenuBar_install(OsMenuBar *mb)
{
    if (!mb)
        return false;

    releaseNativeHandles(mb);

    VexOsMenuTarget *target = [[VexOsMenuTarget alloc] init];
    target.owner = mb;
    OsMenuBar_setTargetField(mb, (any) CFBridgingRetain(target));

    NSMenu *mainMenu = [[NSMenu alloc] initWithTitle:@""];
    OsMenuBar_setMainMenuField(mb, (any) CFBridgingRetain(mainMenu));

    NSString *appName = [[NSProcessInfo processInfo] processName];
    if (!appName || appName.length == 0)
        appName = @"Application";

    NSMenuItem *appMenuItem = [[NSMenuItem alloc] initWithTitle:appName action:nil keyEquivalent:@""];
    NSMenu *appMenu = [[NSMenu alloc] initWithTitle:appName];
    NSMenuItem *quitItem = [[NSMenuItem alloc] initWithTitle:[NSString stringWithFormat:@"Quit %@", appName]
                                                      action:@selector(terminate:)
                                               keyEquivalent:@"q"];
    [appMenu addItem:quitItem];
    [appMenuItem setSubmenu:appMenu];
    [mainMenu addItem:appMenuItem];

    u32 mCount = OsMenuBar_menuCount(mb);
    for (u32 i = 0u; i < mCount; i++) {
        OsMenuSlot mSlot = OsMenuBar_menuAt(mb, i);
        NSString *nsTitle = [NSString stringWithUTF8String:mSlot.title];
        if (!nsTitle)
            nsTitle = @"";

        NSMenu *nsMenu = [[NSMenu alloc] initWithTitle:nsTitle];
        [nsMenu setAutoenablesItems:NO];
        mSlot.nsMenu = (any) CFBridgingRetain(nsMenu);
        OsMenuBar_setMenuAt(mb, i, mSlot);

        NSMenuItem *holder = [[NSMenuItem alloc] initWithTitle:nsTitle action:nil keyEquivalent:@""];
        [holder setSubmenu:nsMenu];
        if (mSlot.parentMenuId < 0) {
            [mainMenu addItem:holder];
        } else if ((u32) mSlot.parentMenuId < i) {
            OsMenuSlot parentSlot = OsMenuBar_menuAt(mb, (u32) mSlot.parentMenuId);
            if (parentSlot.nsMenu) {
                NSMenu *parentNs = (__bridge NSMenu*) parentSlot.nsMenu;
                [parentNs addItem:holder];
            }
        }
    }

    u32 iCount = OsMenuBar_itemCount(mb);
    for (u32 i = 0u; i < iCount; i++) {
        OsMenuItemSlot iSlot = OsMenuBar_itemAt(mb, i);
        if (iSlot.menuId < 0 || (u32) iSlot.menuId >= mCount)
            continue;

        OsMenuSlot ownerMenu = OsMenuBar_menuAt(mb, (u32) iSlot.menuId);
        if (!ownerMenu.nsMenu)
            continue;

        NSMenu *nsMenu = (__bridge NSMenu*) ownerMenu.nsMenu;
        if (iSlot.isSeparator) {
            [nsMenu addItem:[NSMenuItem separatorItem]];
            continue;
        }

        NSString *nsTitle = [NSString stringWithUTF8String:iSlot.title];
        if (!nsTitle)
            nsTitle = @"";

        NSString *nsKey = [NSString stringWithUTF8String:iSlot.keyEquivalent];
        if (!nsKey)
            nsKey = @"";

        NSMenuItem *nsItem = [[NSMenuItem alloc] initWithTitle:nsTitle
                                                        action:@selector(onMenuItem:)
                                                 keyEquivalent:nsKey];
        [nsItem setTarget:target];
        [nsItem setTag:(NSInteger) iSlot.itemId];
        [nsItem setKeyEquivalentModifierMask:mapModifiers(iSlot.modifiers)];
        [nsItem setState:iSlot.checked ? NSControlStateValueOn : NSControlStateValueOff];
        [nsItem setEnabled:iSlot.enabled ? YES : NO];
        [nsMenu addItem:nsItem];

        iSlot.nsItem = (any) CFBridgingRetain(nsItem);
        OsMenuBar_setItemAt(mb, i, iSlot);
    }

    if (NSApp != nil)
        [NSApp setMainMenu:mainMenu];

    OsMenuBar_setInstalledField(mb, true);
    return true;
}

bool OsMenuBar_triggerForTest(OsMenuBar *mb, u32 itemId)
{
    if (!mb)
        return false;

    u32 count = OsMenuBar_itemCount(mb);
    for (u32 i = 0u; i < count; i++) {
        OsMenuItemSlot slot = OsMenuBar_itemAt(mb, i);
        if (!slot.isSeparator && slot.itemId == itemId && slot.enabled && slot.cb) {
            slot.cb(slot.itemId, slot.userdata);
            return true;
        }
    }

    return false;
}

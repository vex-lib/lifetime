#import <AppKit/AppKit.h>

#include "vex.h"
#include "nio/lifetime_memory.h"
#include "re/type/re_type.h"
#include "window/status_bar_internal.h"

#include <string.h>

;;PLATFORM_EXCLUSIVE("macOS")
;;DEFINITION
/**
 * ============================================================================
 * DEFINITION: StatusBarItem
 * ============================================================================
 * Native macOS menu bar extra (NSStatusItem) owned by lifetime/window and
 * allocated from Lifetime_arena. Wraps NSStatusBar.systemStatusBar with
 * title, tooltip, visibility, screen anchor rect computation, click dispatch,
 * and borderless popover Window toggling. Degrades safely in headless mode
 * when NSApp is nil.
 * ============================================================================
 */

;;OVERVIEW
/**
 * ============================================================================
 * CLASS: StatusBarItem (window/status_bar_cocoa.m)
 * ============================================================================
 * Encapsulates a macOS NSStatusItem and its optional popover Window binding.
 *
 * --CONSTANTS----------------------------
 * public   STATUS_BAR_TITLE_CAP        max title bytes (128)
 * public   STATUS_BAR_TOOLTIP_CAP      max tooltip bytes (256)
 *
 * --FIELDS-------------------------------
 * private  utf8 title[]                cached title string
 * private  utf8 tooltip[]              cached tooltip string
 * private  bool visible                visibility state
 * private  bool popoverOpen            popover toggle state
 * private  StatusBarClickFn onClick    click callback
 * private  any userdata                callback context
 * private  Window *popoverWin          borrowed popover Window
 * private  any nsStatusItem            retained NSStatusItem handle
 * private  any target                  retained VexStatusBarTarget trampoline
 *
 * --CONSTRUCTORS-------------------------
 * public   StatusBarItem_1(title)      creates status bar item
 *
 * --FUNCTIONS----------------------------
 * public   StatusBarItem_destroy       removes NSStatusItem and frees handle
 * public   StatusBarItem_onClick       registers click callback
 * public   StatusBarItem_attachPopoverWindow binds popover Window
 * public   StatusBarItem_triggerClickForTest simulates click for testing
 * private  copyBoundedStr              copies string into fixed utf8 buffer
 * private  toggleAttachedPopover       toggles attached popover Window
 *
 * --GETTERS------------------------------
 * public   StatusBarItem_isVisible     returns visibility flag
 * public   StatusBarItem_getAnchorRect queries screen-space anchor rect
 *
 * --SETTERS------------------------------
 * public   StatusBarItem_setTitle      updates button title
 * public   StatusBarItem_setTooltip    updates button tooltip
 * public   StatusBarItem_setVisible    updates item visibility
 * ============================================================================
 */

@interface VexStatusBarTarget : NSObject
@property (nonatomic, assign) StatusBarItem *owner;
- (fn)onStatusClick:(id)sender;
@end

@implementation VexStatusBarTarget
- (fn)onStatusClick:(id)sender
{
    (discard) sender;
    if (!self.owner)
        return;

    StatusBarItem_triggerClickForTest(self.owner);
}
@end

;;GETTER
bool StatusBarItem_isVisible(const StatusBarItem *item)
{
    if (!item)
        return false;

    return (*item).visible;
}

;;SETTER
static fn StatusBarItem_setVisibleField(StatusBarItem *item, bool visible)
{
    if (item)
        (*item).visible = visible;
}

;;GETTER
static bool StatusBarItem_popoverOpenField(const StatusBarItem *item)
{
    if (!item)
        return false;

    return (*item).popoverOpen;
}

;;SETTER
static fn StatusBarItem_setPopoverOpenField(StatusBarItem *item, bool open)
{
    if (item)
        (*item).popoverOpen = open;
}

;;GETTER
static StatusBarClickFn StatusBarItem_onClickField(const StatusBarItem *item)
{
    if (!item)
        return nullptr;

    return (*item).onClick;
}

;;GETTER
static any StatusBarItem_userdataField(const StatusBarItem *item)
{
    if (!item)
        return nullptr;

    return (*item).userdata;
}

;;SETTER
static fn StatusBarItem_setClickFields(StatusBarItem *item, StatusBarClickFn cb, any ud)
{
    if (!item)
        return;

    (*item).onClick = cb;
    (*item).userdata = ud;
}

;;GETTER
static Window *StatusBarItem_popoverWinField(const StatusBarItem *item)
{
    if (!item)
        return nullptr;

    return (*item).popoverWin;
}

;;SETTER
static fn StatusBarItem_setPopoverWinField(StatusBarItem *item, Window *win)
{
    if (item)
        (*item).popoverWin = win;
}

;;GETTER
static any StatusBarItem_nsItemField(const StatusBarItem *item)
{
    if (!item)
        return nullptr;

    return (*item).nsStatusItem;
}

;;SETTER
static fn StatusBarItem_setNsItemField(StatusBarItem *item, any nsItem)
{
    if (item)
        (*item).nsStatusItem = nsItem;
}

;;GETTER
static any StatusBarItem_targetField(const StatusBarItem *item)
{
    if (!item)
        return nullptr;

    return (*item).target;
}

;;SETTER
static fn StatusBarItem_setTargetField(StatusBarItem *item, any target)
{
    if (item)
        (*item).target = target;
}

;;GETTER
static string StatusBarItem_titlePtr(const StatusBarItem *item)
{
    if (!item)
        return "";

    return (*item).title;
}

;;GETTER
static utf8 *StatusBarItem_titleBuf(StatusBarItem *item)
{
    if (!item)
        return nullptr;

    return (*item).title;
}

;;GETTER
static utf8 *StatusBarItem_tooltipBuf(StatusBarItem *item)
{
    if (!item)
        return nullptr;

    return (*item).tooltip;
}

static fn StatusBarItem_writeTitle(StatusBarItem *item, string title)
{
    utf8 *buf = StatusBarItem_titleBuf(item);
    if (!buf)
        return;

    memset(buf, 0, STATUS_BAR_TITLE_CAP);
    if (!title)
        return;

    bytes len = strlen(title);
    if (len >= STATUS_BAR_TITLE_CAP)
        len = STATUS_BAR_TITLE_CAP - 1u;

    memcpy(buf, title, len);
    buf[len] = 0;
}

static fn StatusBarItem_writeTooltip(StatusBarItem *item, string tooltip)
{
    utf8 *buf = StatusBarItem_tooltipBuf(item);
    if (!buf)
        return;

    memset(buf, 0, STATUS_BAR_TOOLTIP_CAP);
    if (!tooltip)
        return;

    bytes len = strlen(tooltip);
    if (len >= STATUS_BAR_TOOLTIP_CAP)
        len = STATUS_BAR_TOOLTIP_CAP - 1u;

    memcpy(buf, tooltip, len);
    buf[len] = 0;
}

/**
 * Toggles the attached popover Window anchored below the status item button.
 * Safely no-ops when NSApp is nil in headless mode.
 * @param item status bar item
 */
static fn toggleAttachedPopover(StatusBarItem *item)
{
    if (!item)
        return;

    Window *win = StatusBarItem_popoverWinField(item);
    if (!win)
        return;

    bool nextOpen = !StatusBarItem_popoverOpenField(item);
    StatusBarItem_setPopoverOpenField(item, nextOpen);

    if (NSApp == nil)
        return;

    if (!nextOpen) {
        Window_hide(win);
        return;
    }

    Window_setUndecorated(win, WINDOW_UNDECORATED_BORDERLESS);
    i32 ax = 0;
    i32 ay = 0;
    i32 aw = 0;
    i32 ah = 0;
    if (StatusBarItem_getAnchorRect(item, &ax, &ay, &aw, &ah))
        Window_setLocation(win, ax, ay + ah + 4);

    Window_show(win);
}

;;CONSTRUCTOR
StatusBarItem *StatusBarItem_1(string title)
{
    StatusBarItem *item = (StatusBarItem*) MemoryArena_alloc(Lifetime_arena(), TYPE_BYTE_ARRAY, sizeof(StatusBarItem));
    if (!item)
        return nullptr;

    memset(item, 0, sizeof(StatusBarItem));
    StatusBarItem_setVisibleField(item, true);
    StatusBarItem_writeTitle(item, title);

    if (NSApp != nil) {
        VexStatusBarTarget *target = [[VexStatusBarTarget alloc] init];
        target.owner = item;
        StatusBarItem_setTargetField(item, (any) CFBridgingRetain(target));

        NSStatusItem *nsItem = [[NSStatusBar systemStatusBar] statusItemWithLength:NSVariableStatusItemLength];
        if (nsItem) {
            nsItem.visible = YES;
            if (nsItem.button) {
                NSString *nsTitle = [NSString stringWithUTF8String:StatusBarItem_titlePtr(item)];
                nsItem.button.title = nsTitle ? nsTitle : @"";
                nsItem.button.target = target;
                nsItem.button.action = @selector(onStatusClick:);
            }
            StatusBarItem_setNsItemField(item, (any) CFBridgingRetain(nsItem));
        }
    }

    return item;
}

;;DECONSTRUCT
fn StatusBarItem_destroy(StatusBarItem *item)
{
    if (!item)
        return;

    any nsHandle = StatusBarItem_nsItemField(item);
    if (nsHandle) {
        NSStatusItem *nsItem = (__bridge NSStatusItem*) nsHandle;
        if (NSApp != nil)
            [[NSStatusBar systemStatusBar] removeStatusItem:nsItem];

        CFBridgingRelease(nsHandle);
        StatusBarItem_setNsItemField(item, nullptr);
    }

    any targetHandle = StatusBarItem_targetField(item);
    if (targetHandle) {
        VexStatusBarTarget *target = (__bridge VexStatusBarTarget*) targetHandle;
        target.owner = nullptr;
        CFBridgingRelease(targetHandle);
        StatusBarItem_setTargetField(item, nullptr);
    }

    MemoryArena_free(Lifetime_arena(), item);
}

;;SETTER
fn StatusBarItem_setTitle(StatusBarItem *item, string title)
{
    if (!item)
        return;

    StatusBarItem_writeTitle(item, title);
    any nsHandle = StatusBarItem_nsItemField(item);
    if (nsHandle && NSApp != nil) {
        NSStatusItem *nsItem = (__bridge NSStatusItem*) nsHandle;
        if (nsItem.button) {
            NSString *nsTitle = [NSString stringWithUTF8String:StatusBarItem_titlePtr(item)];
            nsItem.button.title = nsTitle ? nsTitle : @"";
        }
    }
}

;;SETTER
fn StatusBarItem_setTooltip(StatusBarItem *item, string tooltip)
{
    if (!item)
        return;

    StatusBarItem_writeTooltip(item, tooltip);
    any nsHandle = StatusBarItem_nsItemField(item);
    if (nsHandle && NSApp != nil) {
        NSStatusItem *nsItem = (__bridge NSStatusItem*) nsHandle;
        if (nsItem.button) {
            NSString *nsTip = tooltip ? [NSString stringWithUTF8String:tooltip] : @"";
            nsItem.button.toolTip = nsTip ? nsTip : @"";
        }
    }
}

;;SETTER
fn StatusBarItem_setVisible(StatusBarItem *item, bool visible)
{
    if (!item)
        return;

    StatusBarItem_setVisibleField(item, visible);
    any nsHandle = StatusBarItem_nsItemField(item);
    if (nsHandle && NSApp != nil) {
        NSStatusItem *nsItem = (__bridge NSStatusItem*) nsHandle;
        nsItem.visible = visible ? YES : NO;
    }
}

bool StatusBarItem_getAnchorRect(const StatusBarItem *item,
                                 i32 *outX, i32 *outY, i32 *outW, i32 *outH)
{
    if (!item || NSApp == nil)
        return false;

    any nsHandle = StatusBarItem_nsItemField(item);
    if (!nsHandle)
        return false;

    NSStatusItem *nsItem = (__bridge NSStatusItem*) nsHandle;
    NSStatusBarButton *btn = nsItem.button;
    if (!btn || !btn.window)
        return false;

    NSRect screenRect = [btn.window convertRectToScreen:btn.bounds];
    NSArray<NSScreen*> *screens = [NSScreen screens];
    CGFloat primaryH = screens.count > 0 ? screens[0].frame.size.height : 0.0;
    i32 topY = (i32) (primaryH - (screenRect.origin.y + screenRect.size.height));

    if (outX)
        *outX = (i32) screenRect.origin.x;

    if (outY)
        *outY = topY;

    if (outW)
        *outW = (i32) screenRect.size.width;

    if (outH)
        *outH = (i32) screenRect.size.height;

    return true;
}

fn StatusBarItem_onClick(StatusBarItem *item, StatusBarClickFn cb, any userdata)
{
    if (!item)
        return;

    StatusBarItem_setClickFields(item, cb, userdata);
}

fn StatusBarItem_attachPopoverWindow(StatusBarItem *item, Window *popoverWin)
{
    if (!item)
        return;

    StatusBarItem_setPopoverWinField(item, popoverWin);
    StatusBarItem_setPopoverOpenField(item, false);
}

fn StatusBarItem_triggerClickForTest(StatusBarItem *item)
{
    if (!item)
        return;

    StatusBarClickFn cb = StatusBarItem_onClickField(item);
    if (cb)
        cb(item, StatusBarItem_userdataField(item));

    toggleAttachedPopover(item);
}

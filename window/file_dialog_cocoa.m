#import <AppKit/AppKit.h>

#include "vex.h"
#include "window/file_dialog.h"

#include <stdio.h>
#include <string.h>

;;PLATFORM_EXCLUSIVE("macOS")
;;DEFINITION
/**
 * ============================================================================
 * DEFINITION: OsFileDialog
 * ============================================================================
 * Native macOS Open/Save file chooser bridge owned by lifetime/window.
 * Executes out-of-process via /usr/bin/osascript (NSTask + NSPipe) so Apple's
 * Open and Save Panel Service and QuickLookUIService XPC helpers terminate
 * immediately when the picker closes instead of remaining attached to the host
 * process. Returns false immediately without spawning when NSApp is nil so
 * headless CI suites never block or spawn UI panels.
 * ============================================================================
 */

;;OVERVIEW
/**
 * ============================================================================
 * MODULE: OsFileDialog (window/file_dialog_cocoa.m)
 * ============================================================================
 * Out-of-process macOS Open/Save file chooser bridge.
 *
 * --FUNCTIONS----------------------------
 * public   OsFileDialog_open         opens native picker via short-lived helper
 * public   OsFileDialog_save         opens native save dialog via helper
 * private  copyBoundedPath           copies UTF-8 path safely into output buf
 * private  appendEscapedString       escapes quotes/backslashes for AppleScript
 * private  runOsascriptPicker        spawns /usr/bin/osascript and reads path
 * ============================================================================
 */

#if defined(VEX_PLATFORM_APPLE_SILICON) || defined(VEX_PLATFORM_APPLE_X86_64)

static bool copyBoundedPath(utf8 *outPath, bytes cap, string src)
{
    if (!outPath || cap == 0u)
        return false;

    memset(outPath, 0, cap);
    if (!src || src[0] == '\0')
        return false;

    bytes len = strlen(src);
    while (len > 0u && (src[len - 1u] == '\n' || src[len - 1u] == '\r'))
        len--;

    if (len == 0u || len >= cap)
        return false;

    memcpy(outPath, src, len);
    outPath[len] = 0;
    return true;
}

static fn appendEscapedString(NSMutableString *dst, string src)
{
    if (!dst || !src)
        return;

    bytes len = strlen(src);
    for (bytes i = 0u; i < len; i++) {
        utf8 ch = src[i];
        if (ch == '"' || ch == '\\')
            [dst appendFormat:@"\\%c", ch];
        else if (ch >= 32 && ch <= 126)
            [dst appendFormat:@"%c", ch];
    }
}

static fn appendFilterTypes(NSMutableString *script, string filterExt)
{
    if (!script || !filterExt || filterExt[0] == '\0')
        return;

    NSMutableArray *exts = [NSMutableArray array];
    bytes len = strlen(filterExt);
    bytes start = 0u;
    while (start < len) {
        bytes end = start;
        while (end < len && filterExt[end] != ',' && filterExt[end] != ';')
            end++;

        while (start < end && (filterExt[start] == ' ' || filterExt[start] == '*' || filterExt[start] == '.'))
            start++;

        while (end > start && filterExt[end - 1u] == ' ')
            end--;

        if (end > start && (end - start) < 32u) {
            utf8 extBuf[32];
            memset(extBuf, 0, sizeof(extBuf));
            memcpy(extBuf, filterExt + start, end - start);
            NSString *extStr = [NSString stringWithUTF8String:extBuf];
            if (extStr && extStr.length > 0)
                [exts addObject:extStr];
        }
        start = end + 1u;
    }

    if (exts.count == 0u)
        return;

    [script appendString:@" of type {"];
    for (NSUInteger i = 0u; i < exts.count; i++) {
        if (i > 0u)
            [script appendString:@", "];
        [script appendString:@"\""];
        appendEscapedString(script, [[exts objectAtIndex:i] UTF8String]);
        [script appendString:@"\""];
    }
    [script appendString:@"}"];
}

static bool runOsascriptPicker(NSString *script, utf8 *outPath, bytes cap)
{
    if (!script || !outPath || cap == 0u)
        return false;

    NSTask *task = [[NSTask alloc] init];
    NSPipe *outPipe = [NSPipe pipe];
    NSPipe *errPipe = [NSPipe pipe];
    if (!task || !outPipe || !errPipe) {
#if !__has_feature(objc_arc)
        [task release];
#endif
        return false;
    }

    [task setExecutableURL:[NSURL fileURLWithPath:@"/usr/bin/osascript"]];
    [task setArguments:@[@"-e", script]];
    [task setStandardOutput:outPipe];
    [task setStandardError:errPipe];

    NSError *launchErr = nil;
    BOOL launched = [task launchAndReturnError:&launchErr];
    if (!launched || launchErr) {
#if !__has_feature(objc_arc)
        [task release];
#endif
        return false;
    }

    NSData *data = [[outPipe fileHandleForReading] readDataToEndOfFile];
    [task waitUntilExit];
    i32 status = (i32) [task terminationStatus];
#if !__has_feature(objc_arc)
    [task release];
#endif

    if (status != 0 || !data || data.length == 0u)
        return false;

    NSString *result = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
    if (!result)
        return false;

    bool ok = copyBoundedPath(outPath, cap, [result UTF8String]);
#if !__has_feature(objc_arc)
    [result release];
#endif
    return ok;
}

#endif

bool OsFileDialog_open(string title, string filterExt, utf8 *outPath, bytes cap)
{
    if (!outPath || cap == 0u)
        return false;

    outPath[0] = 0;

#if defined(VEX_PLATFORM_APPLE_SILICON) || defined(VEX_PLATFORM_APPLE_X86_64)
    if (!NSApp)
        return false;

    bool picked = false;
    @autoreleasepool {
        NSMutableString *script = [NSMutableString stringWithString:@"POSIX path of (choose file"];
        if (title && title[0] != '\0') {
            [script appendString:@" with prompt \""];
            appendEscapedString(script, title);
            [script appendString:@"\""];
        }
        appendFilterTypes(script, filterExt);
        [script appendString:@")"];
        picked = runOsascriptPicker(script, outPath, cap);
    }
    return picked;
#else
    (discard) title;
    (discard) filterExt;
    return false;
#endif
}

bool OsFileDialog_save(string title, string defaultName, string filterExt,
                       utf8 *outPath, bytes cap)
{
    (discard) filterExt;
    if (!outPath || cap == 0u)
        return false;

    outPath[0] = 0;

#if defined(VEX_PLATFORM_APPLE_SILICON) || defined(VEX_PLATFORM_APPLE_X86_64)
    if (!NSApp)
        return false;

    bool picked = false;
    @autoreleasepool {
        NSMutableString *script = [NSMutableString stringWithString:@"POSIX path of (choose file name"];
        if (title && title[0] != '\0') {
            [script appendString:@" with prompt \""];
            appendEscapedString(script, title);
            [script appendString:@"\""];
        }
        if (defaultName && defaultName[0] != '\0') {
            [script appendString:@" default name \""];
            appendEscapedString(script, defaultName);
            [script appendString:@"\""];
        }
        [script appendString:@")"];
        picked = runOsascriptPicker(script, outPath, cap);
    }
    return picked;
#else
    (discard) title;
    (discard) defaultName;
    return false;
#endif
}

// permission/objc/permission_cocoa.m — the macOS Permission backend.
//
// Implements the four permissionBackend* hooks (permission/permission_backend.h)
// for the host.
//   CoreGraphics / ApplicationServices : screen capture, key listening, event
//       synthesis, accessibility (no bundle needed; queried directly).
//   AVFoundation / Contacts / EventKit / Photos / CoreLocation /
//       UserNotifications : camera, microphone, contacts, calendar, photos,
//       location, notifications. These read a TCC status that is only defined
//       for a signed process with an app identity, so every one of them is
//       GUARDED by hostHasBundle(): a bare CLI (no Info.plist) answers
//       PERMISSION_NOT_DETERMINED and NEVER touches the framework, which also
//       keeps the owner test prompt-free and crash-free.
//   AUTOMATION (needs a per-target decision) and FULL_DISK (no API at all):
//       answer PERMISSION_UNSUPPORTED.
//
// This backend never relaunches or exits the process: it reports what the OS
// says and nothing more (the relaunch is the user's to make).

#include "../permission.h"
#include "../permission_backend.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ApplicationServices/ApplicationServices.h>
#include <CoreServices/CoreServices.h>

#import <Foundation/Foundation.h>
#import <AVFoundation/AVFoundation.h>
#import <Contacts/Contacts.h>
#import <EventKit/EventKit.h>
#import <Photos/Photos.h>
#import <CoreLocation/CoreLocation.h>
#import <UserNotifications/UserNotifications.h>

#include "util/annotation.h"

;;DEFINITION
/**
 * ============================================================================
 * DEFINITION: Permission (macOS backend)
 * ============================================================================
 * The shim backend for permission.c on macOS. Every hook maps one PermissionKind
 * onto the platform's consent machinery. The CoreGraphics quartet is queried
 * directly; the framework kinds (camera, microphone, contacts, calendar, photos,
 * location, notifications) are queried through their frameworks but only when
 * the process has an app identity, so an identity-less CLI never touches them.
 * Requests are fire-and-forget: they raise the OS prompt and return; the answer
 * arrives later, so callers poll the status (the Bounded Wait Law). No hook ever
 * relaunches the process.
 * ============================================================================
 */

;;OVERVIEW
/**
 * ============================================================================
 * CLASS: Permission (permission/objc/permission_cocoa.m)
 * ============================================================================
 * macOS implementation of the permission backend hooks.
 *
 * STRUCT FIELDS: none — procedural (kind in, status/decision out)
 *
 * PRIVATE HELPERS:
 * ----------------------------------------------------------------------------
 *   hostHasBundle()        : true when the process has an app identity (Info.plist)
 *   mapAV/mapCN/mapEK/mapPH/mapCL : framework status -> PermissionStatus
 *
 * FUNCTION REGISTRY (the backend seam; public API lives in permission.c):
 *   - permissionBackendStatus(kind)
 *   - permissionBackendRequest(kind)
 *   - permissionBackendRequiresRestart(kind)
 *   - permissionBackendOpenSettings(kind)
 * ============================================================================
 */
// Status reads that are only defined for an identified app.
static bool hostHasBundle() {
    return [[NSBundle mainBundle] bundleIdentifier] != nil;
}

// --- Framework status -> PermissionStatus -----------------------------------
/** Maps an AVFoundation authorization value to the portable permission status. */
static PermissionStatus mapAV(AVAuthorizationStatus status) {
    switch (status) {
        case AVAuthorizationStatusAuthorized:    return PERMISSION_GRANTED;
        case AVAuthorizationStatusDenied:        return PERMISSION_DENIED;
        case AVAuthorizationStatusRestricted:    return PERMISSION_RESTRICTED;
        case AVAuthorizationStatusNotDetermined:
        default:                                 return PERMISSION_NOT_DETERMINED;
    }
}

/** Maps a Contacts authorization value to the portable permission status. */
static PermissionStatus mapCN(CNAuthorizationStatus status) {
    switch (status) {
        case CNAuthorizationStatusAuthorized:    return PERMISSION_GRANTED;
        case CNAuthorizationStatusDenied:        return PERMISSION_DENIED;
        case CNAuthorizationStatusRestricted:    return PERMISSION_RESTRICTED;
        case CNAuthorizationStatusNotDetermined:
        default:                                 return PERMISSION_NOT_DETERMINED;
    }
}

/** Maps an EventKit authorization value to the portable permission status. */
static PermissionStatus mapEK(EKAuthorizationStatus status) {
    // Named via if-chain: EKAuthorizationStatusAuthorized is deprecated (mac 14)
    // in favor of FullAccess/WriteOnly, so we never name it — anything short of
    // denied/restricted/undetermined is a grant.
    if (status == EKAuthorizationStatusDenied)
        return PERMISSION_DENIED;
    if (status == EKAuthorizationStatusRestricted)
        return PERMISSION_RESTRICTED;
    if (status == EKAuthorizationStatusNotDetermined)
        return PERMISSION_NOT_DETERMINED;
    return PERMISSION_GRANTED;
}

/** Maps a Photos authorization value to the portable permission status. */
static PermissionStatus mapPH(PHAuthorizationStatus status) {
    switch (status) {
        case PHAuthorizationStatusAuthorized:
        case PHAuthorizationStatusLimited:       return PERMISSION_GRANTED;
        case PHAuthorizationStatusDenied:        return PERMISSION_DENIED;
        case PHAuthorizationStatusRestricted:    return PERMISSION_RESTRICTED;
        case PHAuthorizationStatusNotDetermined:
        default:                                 return PERMISSION_NOT_DETERMINED;
    }
}

/** Maps a Core Location authorization value to the portable permission status. */
static PermissionStatus mapCL(CLAuthorizationStatus status) {
    // kCLAuthorizationStatusAuthorizedWhenInUse is iOS-only; on macOS the grant
    // is AuthorizedAlways. kCLAuthorizationStatusAuthorized is deprecated, so it
    // is never named.
    switch (status) {
        case kCLAuthorizationStatusAuthorizedAlways: return PERMISSION_GRANTED;
        case kCLAuthorizationStatusDenied:          return PERMISSION_DENIED;
        case kCLAuthorizationStatusRestricted:      return PERMISSION_RESTRICTED;
        case kCLAuthorizationStatusNotDetermined:
        default:                                    return PERMISSION_NOT_DETERMINED;
    }
}

// Long-lived framework objects for the request calls (the prompt outlives the
// function frame, so these must not be stack locals).
static CNContactStore *g_contactStore = nil;
static EKEventStore    *g_eventStore = nil;
static CLLocationManager *g_locationManager = nil;

// --- Status per kind --------------------------------------------------------
static PermissionStatus statusCamera() {
    if (!hostHasBundle())
        return PERMISSION_NOT_DETERMINED;
    return mapAV([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo]);
}

/** Reads the current microphone authorization status. */
static PermissionStatus statusMicrophone() {
    if (!hostHasBundle())
        return PERMISSION_NOT_DETERMINED;
    return mapAV([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio]);
}

/** Reads the current Contacts authorization status. */
static PermissionStatus statusContacts() {
    if (!hostHasBundle())
        return PERMISSION_NOT_DETERMINED;
    return mapCN([CNContactStore authorizationStatusForEntityType:CNEntityTypeContacts]);
}

/** Reads the current Calendar authorization status. */
static PermissionStatus statusCalendar() {
    if (!hostHasBundle())
        return PERMISSION_NOT_DETERMINED;
    return mapEK([EKEventStore authorizationStatusForEntityType:EKEntityTypeEvent]);
}

/** Reads the current Photos authorization status. */
static PermissionStatus statusPhotos() {
    if (!hostHasBundle())
        return PERMISSION_NOT_DETERMINED;
    if (@available(macOS 11.0, *))
        return mapPH([PHPhotoLibrary authorizationStatusForAccessLevel:PHAccessLevelReadWrite]);
    return PERMISSION_NOT_DETERMINED;
}

/** Reads the current Location authorization status. */
static PermissionStatus statusLocation() {
    if (!hostHasBundle())
        return PERMISSION_NOT_DETERMINED;
    if (g_locationManager == nil)
        g_locationManager = [[CLLocationManager alloc] init];
    return mapCL([g_locationManager authorizationStatus]);
}

// --- Requests (fire-and-forget; the answer arrives later) -------------------
static bool requestCamera() {
    if (!hostHasBundle())
        return false;
    [AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo
                             completionHandler:^(BOOL granted) { (fn) granted; }];
    return true;
}

/** Requests microphone access using AVFoundation's authorization API. */
static bool requestMicrophone() {
    if (!hostHasBundle())
        return false;
    [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio
                             completionHandler:^(BOOL granted) { (fn) granted; }];
    return true;
}

/** Requests Contacts access using the Contacts framework. */
static bool requestContacts() {
    if (!hostHasBundle())
        return false;
    if (g_contactStore == nil)
        g_contactStore = [[CNContactStore alloc] init];
    [g_contactStore requestAccessForEntityType:CNEntityTypeContacts
                             completionHandler:^(BOOL granted, NSError *error) {
                                 (fn) granted;
                                 (fn) error;
                             }];
    return true;
}

/** Requests Calendar access using EventKit. */
static bool requestCalendar() {
    if (!hostHasBundle())
        return false;
    if (g_eventStore == nil)
        g_eventStore = [[EKEventStore alloc] init];
    if (@available(macOS 14.0, *)) {
        [g_eventStore requestFullAccessToEventsWithCompletion:^(BOOL granted, NSError *error) {
            (fn) granted;
            (fn) error;
        }];
    } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        [g_eventStore requestAccessToEntityType:EKEntityTypeEvent
                                     completion:^(BOOL granted, NSError *error) {
                                         (fn) granted;
                                         (fn) error;
                                     }];
#pragma clang diagnostic pop
    }
    return true;
}

/** Requests Photos access using Photos framework authorization. */
static bool requestPhotos() {
    if (!hostHasBundle())
        return false;
    if (@available(macOS 11.0, *)) {
        [PHPhotoLibrary requestAuthorizationForAccessLevel:PHAccessLevelReadWrite
                                                  handler:^(PHAuthorizationStatus status) {
                                                      (fn) status;
                                                  }];
    }
    return true;
}

/** Requests Location authorization from the shared Core Location manager. */
static bool requestLocation() {
    if (!hostHasBundle())
        return false;
    if (g_locationManager == nil)
        g_locationManager = [[CLLocationManager alloc] init];
    [g_locationManager requestWhenInUseAuthorization];
    return true;
}

/** Requests notification authorization through UserNotifications. */
static bool requestNotifications() {
    if (!hostHasBundle())
        return false;
    UNAuthorizationOptions options =
        UNAuthorizationOptionAlert | UNAuthorizationOptionBadge | UNAuthorizationOptionSound;
    [[UNUserNotificationCenter currentNotificationCenter]
        requestAuthorizationWithOptions:options
                      completionHandler:^(BOOL granted, NSError *error) {
                          (fn) granted;
                          (fn) error;
                      }];
    return true;
}

// --- The backend hooks ------------------------------------------------------
PermissionStatus permissionBackendStatus(PermissionKind kind) {
    switch (kind) {
        case PERMISSION_SCREEN_CAPTURE:
            return CGPreflightScreenCaptureAccess() ? PERMISSION_GRANTED : PERMISSION_NOT_DETERMINED;
        case PERMISSION_KEY_LISTEN:
            return CGPreflightListenEventAccess() ? PERMISSION_GRANTED : PERMISSION_NOT_DETERMINED;
        case PERMISSION_POST_EVENT:
            return CGPreflightPostEventAccess() ? PERMISSION_GRANTED : PERMISSION_NOT_DETERMINED;
        case PERMISSION_ACCESSIBILITY:
            return AXIsProcessTrusted() ? PERMISSION_GRANTED : PERMISSION_NOT_DETERMINED;
        case PERMISSION_CAMERA:        return statusCamera();
        case PERMISSION_MICROPHONE:    return statusMicrophone();
        case PERMISSION_CONTACTS:      return statusContacts();
        case PERMISSION_CALENDAR:      return statusCalendar();
        case PERMISSION_PHOTOS:        return statusPhotos();
        case PERMISSION_LOCATION:      return statusLocation();
        case PERMISSION_NOTIFICATIONS: return PERMISSION_NOT_DETERMINED; // async-only, no sync query
        default:                       return PERMISSION_UNSUPPORTED;    // AUTOMATION, FULL_DISK
    }
}

/** Dispatches a permission request to the matching Cocoa authorization flow. */
bool permissionBackendRequest(PermissionKind kind) {
    switch (kind) {
        case PERMISSION_SCREEN_CAPTURE: return CGRequestScreenCaptureAccess();
        case PERMISSION_KEY_LISTEN:     return CGRequestListenEventAccess();
        case PERMISSION_POST_EVENT:     return CGRequestPostEventAccess();
        case PERMISSION_ACCESSIBILITY: {
            const_any keys[] = { (const_any) kAXTrustedCheckOptionPrompt };
            const_any values[] = { (const_any) kCFBooleanTrue };
            CFDictionaryRef options = CFDictionaryCreate(
                nullptr, keys, values, 1,
                &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            bool trusted = AXIsProcessTrustedWithOptions(options);
            CFRelease(options);
            return trusted;
        }
        case PERMISSION_CAMERA:        return requestCamera();
        case PERMISSION_MICROPHONE:    return requestMicrophone();
        case PERMISSION_CONTACTS:      return requestContacts();
        case PERMISSION_CALENDAR:      return requestCalendar();
        case PERMISSION_PHOTOS:        return requestPhotos();
        case PERMISSION_LOCATION:      return requestLocation();
        case PERMISSION_NOTIFICATIONS: return requestNotifications();
        default:                       return false;
    }
}

// macOS: a screen-capture grant binds only after the process relaunches.
// Reported here; the relaunch is the user's to make (never ours).
bool permissionBackendRequiresRestart(PermissionKind kind) {
    return kind == PERMISSION_SCREEN_CAPTURE;
}

/** Opens the appropriate System Settings privacy pane for the permission kind. */
bool permissionBackendOpenSettings(PermissionKind kind) {
    const char *pane = nullptr;
    switch (kind) {
        case PERMISSION_SCREEN_CAPTURE: pane = "Privacy_ScreenCapture"; break;
        case PERMISSION_KEY_LISTEN:
        case PERMISSION_POST_EVENT:     pane = "Privacy_ListenEvent"; break;
        case PERMISSION_ACCESSIBILITY:  pane = "Privacy_Accessibility"; break;
        case PERMISSION_CAMERA:         pane = "Privacy_Camera"; break;
        case PERMISSION_MICROPHONE:     pane = "Privacy_Microphone"; break;
        case PERMISSION_CONTACTS:       pane = "Privacy_Contacts"; break;
        case PERMISSION_CALENDAR:       pane = "Privacy_Calendars"; break;
        case PERMISSION_PHOTOS:         pane = "Privacy_Photos"; break;
        case PERMISSION_LOCATION:       pane = "Privacy_LocationServices"; break;
        case PERMISSION_AUTOMATION:     pane = "Privacy_Automation"; break;
        case PERMISSION_FULL_DISK:      pane = "Privacy_AllFiles"; break;
        default:                        return false;
    }
    char url[192];
    int n = snprintf(url, sizeof url,
                     "x-apple.systempreferences:com.apple.preference.security?%s", pane);
    if (n <= 0 || (bytes) n >= sizeof url)
        return false;
    CFURLRef cfUrl = CFURLCreateWithBytes(nullptr, (const UInt8*) url, (CFIndex) n,
                                          kCFStringEncodingUTF8, nullptr);
    if (!cfUrl)
        return false;
    OSStatus status = LSOpenCFURLRef(cfUrl, nullptr);
    CFRelease(cfUrl);
    return status == noErr;
}

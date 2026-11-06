#include "permission.h"
#include "permission_backend.h"
#include "util/platform.h"

#include "util/annotation.h"

;;DEFINITION
/**
 * ============================================================================
 * DEFINITION: Permission
 * ============================================================================
 * The portable R1 host OS-permission surface: the kind/status vocabulary, the
 * name tables, the bounds checks, and the dispatch to one backend. The macOS
 * backend lives in objc/permission_cocoa.m (the shim backend); hosts without a
 * backend fall back to PERMISSION_UNSUPPORTED here.
 *
 * The class NEVER terminates or relaunches the process: a grant that needs a
 * relaunch is reported by Permission_requiresRestart and left for the user, so
 * the hotcwap hot-reload experience is preserved.
 * ============================================================================
 */

;;OVERVIEW
/**
 * ============================================================================
 * CLASS: Permission (permission/permission.c)
 * ============================================================================
 * OS capability permission requests. Cross-platform contract: the kind exists
 * everywhere, the status is UNSUPPORTED where the host has no concept.
 *
 * STRUCT FIELDS: none — procedural (kind in, status/decision out)
 *
 * PRIVATE HELPERS:
 * ----------------------------------------------------------------------------
 *   (none — the OS hooks live in the backend: objc/permission_cocoa.m)
 *
 * FUNCTION REGISTRY:
 * ----------------------------------------------------------------------------
 * Core Functions:
 *   - Permission_status(kind)
 *   - Permission_request(kind)
 *   - Permission_requiresRestart(kind)
 *   - Permission_openSettings(kind)
 *
 * Getters:
 *   - Permission_kindName(kind)
 *   - Permission_statusName(status)
 * ============================================================================
 */

// NAMES (portable)
;;GETTER
/** Returns the symbolic name for a permission kind, or the unknown label. */
const char *Permission_kindName(PermissionKind kind) {
    switch (kind) {
        case PERMISSION_SCREEN_CAPTURE: return "PERMISSION_SCREEN_CAPTURE";
        case PERMISSION_KEY_LISTEN:     return "PERMISSION_KEY_LISTEN";
        case PERMISSION_POST_EVENT:     return "PERMISSION_POST_EVENT";
        case PERMISSION_ACCESSIBILITY:  return "PERMISSION_ACCESSIBILITY";
        case PERMISSION_CAMERA:         return "PERMISSION_CAMERA";
        case PERMISSION_MICROPHONE:     return "PERMISSION_MICROPHONE";
        case PERMISSION_CONTACTS:       return "PERMISSION_CONTACTS";
        case PERMISSION_CALENDAR:       return "PERMISSION_CALENDAR";
        case PERMISSION_PHOTOS:         return "PERMISSION_PHOTOS";
        case PERMISSION_LOCATION:       return "PERMISSION_LOCATION";
        case PERMISSION_NOTIFICATIONS:  return "PERMISSION_NOTIFICATIONS";
        case PERMISSION_AUTOMATION:     return "PERMISSION_AUTOMATION";
        case PERMISSION_FULL_DISK:      return "PERMISSION_FULL_DISK";
        default:                        return "PERMISSION_UNKNOWN";
    }
}

;;GETTER
/** Returns the symbolic name for a permission status, or the unknown label. */
const char *Permission_statusName(PermissionStatus status) {
    switch (status) {
        case PERMISSION_UNSUPPORTED:    return "UNSUPPORTED";
        case PERMISSION_NOT_DETERMINED: return "NOT_DETERMINED";
        case PERMISSION_GRANTED:        return "GRANTED";
        case PERMISSION_DENIED:         return "DENIED";
        case PERMISSION_RESTRICTED:     return "RESTRICTED";
        default:                        return "UNKNOWN";
    }
}

// PUBLIC API (portable dispatch — bounds-checked once, then the backend decides)
/** Queries a permission through the platform backend, or reports unsupported. */
PermissionStatus Permission_status(PermissionKind kind) {
    if ((int) kind < 0 || kind >= PERMISSION_COUNT)
        return PERMISSION_UNSUPPORTED;
    return permissionBackendStatus(kind);
}

/** Requests the specified permission through the platform backend. */
bool Permission_request(PermissionKind kind) {
    if ((int) kind < 0 || kind >= PERMISSION_COUNT)
        return false;
    return permissionBackendRequest(kind);
}

/** Reports whether a permission grant requires an application restart. */
bool Permission_requiresRestart(PermissionKind kind) {
    if ((int) kind < 0 || kind >= PERMISSION_COUNT)
        return false;
    return permissionBackendRequiresRestart(kind);
}

/** Opens the platform settings surface for the requested permission, if available. */
bool Permission_openSettings(PermissionKind kind) {
    if ((int) kind < 0 || kind >= PERMISSION_COUNT)
        return false;
    return permissionBackendOpenSettings(kind);
}

// PORTABLE BACKEND (non-Apple): the kind exists, the host has no concept yet.
#if !defined(VEX_PLATFORM_APPLE)
    /** Non-Apple fallback: reports every permission as unsupported. */
    PermissionStatus permissionBackendStatus(PermissionKind kind) {
        (void) kind;
        return PERMISSION_UNSUPPORTED;
    }
    /** Non-Apple fallback: no permission request backend is available. */
    bool permissionBackendRequest(PermissionKind kind) {
        (void) kind;
        return false;
    }
    /** Non-Apple fallback: no request can require an OS restart. */
    bool permissionBackendRequiresRestart(PermissionKind kind) {
        (void) kind;
        return false;
    }
    /** Non-Apple fallback: no platform settings surface is provided. */
    bool permissionBackendOpenSettings(PermissionKind kind) {
        (void) kind;
        return false;
    }
#endif

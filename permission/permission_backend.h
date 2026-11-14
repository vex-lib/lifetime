#ifndef PERMISSION_PERMISSION_BACKEND_H
#define PERMISSION_PERMISSION_BACKEND_H

#include "vex.h"
#include "permission.h"

// permission/permission_backend.h — INTERNAL backend seam (not public API).
//
// permission.c owns the portable surface (name tables, bounds checks, dispatch).
// Exactly ONE backend implements these four hooks per host: objc/permission_cocoa.m
// on Apple, or the portable fallback compiled inside permission.c elsewhere.

PermissionStatus permissionBackendStatus(PermissionKind kind);
bool permissionBackendRequest(PermissionKind kind);
bool permissionBackendRequiresRestart(PermissionKind kind);
bool permissionBackendOpenSettings(PermissionKind kind);

#endif

// hotcwap/test/permission/permission_test.c — the Permission class _test.
//
// Contract test (no prompts): because Permission_request raises a real OS
// dialog and Permission_openSettings opens System Settings, THIS test pins only
// the prompt-free surface — enum validity, the name tables, the restart
// contract, out-of-range refusal, and the backend-wired status on macOS. The
// prompting paths are IRL actions and are never thrown at a headless suite (the
// lab/IRL split, applied to permissions).

#include "vex.h"
#include <stdio.h>
#include <string.h>

#include "permission/permission.h"

static int g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);          \
            g_failures++;                                                  \
        }                                                                  \
    } while (0)

int main() {
    // Every kind has a non-empty name and a valid status, and checking a status
    // never prompts.
    for (int k = 0; k < PERMISSION_COUNT; k++) {
        PermissionKind kind = (PermissionKind) k;
        CHECK(Permission_kindName(kind) != nullptr);
        CHECK(Permission_kindName(kind)[0] != '\0');
        PermissionStatus s = Permission_status(kind);
        CHECK(s >= PERMISSION_UNSUPPORTED && s <= PERMISSION_RESTRICTED);
        CHECK(Permission_statusName(s) != nullptr);
        CHECK(Permission_statusName(s)[0] != '\0');
    }

    // Names are exact and stable for the screen/key/accessibility kinds.
    CHECK(strcmp(Permission_kindName(PERMISSION_SCREEN_CAPTURE),
                 "PERMISSION_SCREEN_CAPTURE") == 0);
    CHECK(strcmp(Permission_kindName(PERMISSION_ACCESSIBILITY),
                 "PERMISSION_ACCESSIBILITY") == 0);
    CHECK(strcmp(Permission_statusName(PERMISSION_GRANTED), "GRANTED") == 0);
    CHECK(strcmp(Permission_statusName(PERMISSION_UNSUPPORTED), "UNSUPPORTED") == 0);

    // Out-of-range kinds fail closed, never UB.
    CHECK(Permission_status((PermissionKind) 999) == PERMISSION_UNSUPPORTED);
    CHECK(strcmp(Permission_kindName((PermissionKind) 999), "PERMISSION_UNKNOWN") == 0);
    CHECK(strcmp(Permission_statusName((PermissionStatus) 999), "UNKNOWN") == 0);
    CHECK(!Permission_request((PermissionKind) 999));
    CHECK(!Permission_openSettings((PermissionKind) 999));

    // The restart contract is pure: screen capture is the restart case; the
    // class reports it and never acts on it.
    CHECK(Permission_requiresRestart(PERMISSION_SCREEN_CAPTURE));
    CHECK(!Permission_requiresRestart(PERMISSION_CAMERA));
    CHECK(!Permission_requiresRestart((PermissionKind) 999));

#if defined(VEX_PLATFORM_APPLE)
    // On macOS the CoreGraphics quartet and the framework kinds are all wired:
    // their status is GRANTED / NOT_DETERMINED (a bare CLI has no bundle
    // identity, so the framework kinds answer NOT_DETERMINED without touching
    // their framework), never UNSUPPORTED.
    const PermissionKind wired[] = {
        PERMISSION_SCREEN_CAPTURE, PERMISSION_KEY_LISTEN, PERMISSION_POST_EVENT,
        PERMISSION_ACCESSIBILITY, PERMISSION_CAMERA, PERMISSION_MICROPHONE,
        PERMISSION_CONTACTS, PERMISSION_CALENDAR, PERMISSION_PHOTOS,
        PERMISSION_LOCATION, PERMISSION_NOTIFICATIONS
    };
    for (bytes i = 0; i < sizeof wired / sizeof wired[0]; i++)
        CHECK(Permission_status(wired[i]) != PERMISSION_UNSUPPORTED);
    // The target/API-less kinds stay UNSUPPORTED on every host.
    CHECK(Permission_status(PERMISSION_AUTOMATION) == PERMISSION_UNSUPPORTED);
    CHECK(Permission_status(PERMISSION_FULL_DISK) == PERMISSION_UNSUPPORTED);
#endif

    // An unwired kind refuses a request without prompting.
    CHECK(!Permission_request(PERMISSION_AUTOMATION));
    CHECK(!Permission_request(PERMISSION_FULL_DISK));
    CHECK(!Permission_request((PermissionKind) 999));

    // NOTE (deliberately NOT exercised, IRL only):
    //   Permission_request(PERMISSION_SCREEN_CAPTURE) — raises a TCC dialog
    //   Permission_request(PERMISSION_ACCESSIBILITY)  — raises a TCC dialog
    //   Permission_openSettings(...)                  — opens System Settings

    if (g_failures == 0) {
        printf("permission_test: all assertions held\n");
        return 0;
    }
    printf("permission_test: %d FAILURES\n", g_failures);
    return 1;
}

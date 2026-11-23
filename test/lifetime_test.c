/**
 * Consolidated automated owner test suite for vex/lifetime.
 * Usage: lifetime_test [platform|clock|capability|permission|lifetime|window_event|bridge_seam|console|commandline|process|process_spawn|kernel_function|kernel_lifecycle|application|present_surface|traffic_light|window]
 */

#include "vex.h"
#include "re/primitive/vex-string.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "capability/capability.h"
#include "core/application.h"
#include "core/commandline.h"
#include "core/console.h"
#include "core/kernel.h"
#include "core/process_spawn.h"
#include "vexspoke/engine/loop.h"
#include "vexspoke/input/key.h"
#include "vexspoke/input/mouse.h"
#include "vexspoke/input/touch.h"
#include "lifetime/clock.h"
#include "lifetime/lifetime.h"
#include "permission/permission.h"
#include "re/nio/mem.h"
#include "window/traffic_light.h"
#include "window/window.h"
#include "window/window_event.h"
#include "lifetime_test.h"

#if (defined(VEX_PLATFORM_APPLE_SILICON) + defined(VEX_PLATFORM_APPLE_X86_64) + \
     defined(VEX_PLATFORM_LINUX_ARM64) + defined(VEX_PLATFORM_LINUX_X86_64) +   \
     defined(VEX_PLATFORM_WINDOWS_ARM64) + defined(VEX_PLATFORM_WINDOWS_X86_64)) != 1
#error "lifetime_test: exactly one of the six VEX_PLATFORM_* targets must be defined"
#endif

#if (defined(VEX_PLATFORM_APPLE) + defined(VEX_PLATFORM_WINDOWS) + defined(VEX_PLATFORM_LINUX)) != 1
#error "lifetime_test: exactly one VEX_PLATFORM_{APPLE,WINDOWS,LINUX} must be defined"
#endif

#if (defined(VEX_ARCH_ARM) + defined(VEX_ARCH_X86)) != 1
#error "lifetime_test: exactly one VEX_ARCH_{ARM,X86} family must be defined"
#endif

#if (defined(VEX_ARCH_ARM64) + defined(VEX_ARCH_X86_64)) != 1
#error "lifetime_test: exactly one VEX_ARCH_{ARM64,X86_64} width must be defined"
#endif

#if defined(__APPLE__) && !defined(VEX_PLATFORM_APPLE)
#error "lifetime_test: __APPLE__ did not resolve to VEX_PLATFORM_APPLE"
#endif

#if defined(_WIN32) && !defined(VEX_PLATFORM_WINDOWS)
#error "lifetime_test: _WIN32 did not resolve to VEX_PLATFORM_WINDOWS"
#endif

#if defined(__linux__) && !defined(VEX_PLATFORM_LINUX)
#error "lifetime_test: __linux__ did not resolve to VEX_PLATFORM_LINUX"
#endif

#if (defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)) && !defined(VEX_ARCH_ARM64)
#error "lifetime_test: aarch64 did not resolve to VEX_ARCH_ARM64"
#endif

#if (defined(__x86_64__) || defined(_M_X64)) && !defined(VEX_ARCH_X86_64)
#error "lifetime_test: x86_64 did not resolve to VEX_ARCH_X86_64"
#endif

_Static_assert(sizeof(MemoryHeader) == 32, "MemoryHeader must stay 32 bytes");

;;INTENTION("vexspoke/cli/console.c shares basename with lifetime/core/console.c in b's flat obj dir")
fn Console_init();
fn Console_init()
{
}

static i32 g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            print("FAIL ", __FILE__, ":", __LINE__, " ", #cond);          \
            g_failures++;                                                  \
        }                                                                  \
    } while (0)

/** Proves normalized platform/arch macro consistency and target name spelling. */
static fn suite_platform() {
    CHECK(VEX_PLATFORM_NAME[0] != '\0');
    CHECK(VEX_ARCH_FAMILY[0] != '\0');
    CHECK(VEX_ARCH_NAME[0] != '\0');
    CHECK(VEX_TARGET_NAME[0] != '\0');

    utf8 expected[32];
    snprintf(expected, sizeof expected, "%s-%s", VEX_PLATFORM_NAME, VEX_ARCH_NAME);
    CHECK(!string_compare(expected, VEX_TARGET_NAME));

#if defined(VEX_ARCH_ARM)
    CHECK(!string_compare(VEX_ARCH_FAMILY, "arm"));
    CHECK(!string_compare(VEX_ARCH_NAME, "arm64"));
#else
    CHECK(!string_compare(VEX_ARCH_FAMILY, "x86"));
    CHECK(!string_compare(VEX_ARCH_NAME, "x86_64"));
#endif

#if defined(VEX_PLATFORM_APPLE_SILICON)
    CHECK(!string_compare(VEX_PLATFORM_NAME, "darwin"));
    CHECK(!string_compare(VEX_TARGET_NAME, "darwin-arm64"));
#elif defined(VEX_PLATFORM_APPLE_X86_64)
    CHECK(!string_compare(VEX_PLATFORM_NAME, "darwin"));
    CHECK(!string_compare(VEX_TARGET_NAME, "darwin-x86_64"));
#elif defined(VEX_PLATFORM_LINUX_ARM64)
    CHECK(!string_compare(VEX_PLATFORM_NAME, "linux"));
    CHECK(!string_compare(VEX_TARGET_NAME, "linux-arm64"));
#elif defined(VEX_PLATFORM_LINUX_X86_64)
    CHECK(!string_compare(VEX_PLATFORM_NAME, "linux"));
    CHECK(!string_compare(VEX_TARGET_NAME, "linux-x86_64"));
#elif defined(VEX_PLATFORM_WINDOWS_ARM64)
    CHECK(!string_compare(VEX_PLATFORM_NAME, "windows"));
    CHECK(!string_compare(VEX_TARGET_NAME, "windows-arm64"));
#elif defined(VEX_PLATFORM_WINDOWS_X86_64)
    CHECK(!string_compare(VEX_PLATFORM_NAME, "windows"));
    CHECK(!string_compare(VEX_TARGET_NAME, "windows-x86_64"));
#endif
    CHECK(sizeof(any) * 8 == VEX_ARCH_BITS);
}

/** Proves monotonic nanos, sane unix millis, and seconds-to-nanos agreement. */
static fn suite_clock() {
    u64 n1 = Clock_nowNanos();
    u64 n2 = Clock_nowNanos();
    assert(n2 >= n1);

    u64 m1 = Clock_nowMillis();
    assert(m1 > 1700000000000ull);

    d64 s = Clock_nowSeconds();
    assert(s > 0.0);
    u64 backToNanos = (u64) (s * 1000000000.0);
    u64 n3 = Clock_nowNanos();
    assert(n3 >= backToNanos);
    assert(n3 - backToNanos < 1000000000ull);

    u64 m2 = Clock_nowMillis();
    assert(m2 >= m1);
}

/** Proves host capability probe names, fail-closed out-of-range, and idempotence. */
static fn suite_capability() {
    u64 before = 0;
    for (i32 k = 0; k < CAPABILITY_COUNT; k++) {
        CapabilityKind kind = (CapabilityKind) k;
        CHECK(Capability_kindName(kind) != nullptr);
        CHECK(Capability_kindName(kind)[0] != '\0');
        if (Capability_has(kind))
            before++;
    }
    CHECK(Capability_kindName(CAPABILITY_CPU_DOTPROD)[0] == 'C');
    CHECK(Capability_kindName((CapabilityKind) 999) != nullptr);
    CHECK(!Capability_has((CapabilityKind) 999));
    CHECK(!Capability_has((CapabilityKind) -1));

    i32 again = 0;
    for (i32 k = 0; k < CAPABILITY_COUNT; k++)
        if (Capability_has((CapabilityKind) k))
            again++;
    CHECK((u64) again == before);
    CHECK(Capability_osMajor() == Capability_osMajor());
#if defined(VEX_PLATFORM_APPLE)
    CHECK(Capability_osMajor() >= 14);
#endif
    Capability_probe();
    Capability_probe();
    CHECK((u64) again == before);
}

/** Proves prompt-free Permission status/name tables, restart contract, and out-of-range refusal. */
static fn suite_permission() {
    for (i32 k = 0; k < PERMISSION_COUNT; k++) {
        PermissionKind kind = (PermissionKind) k;
        CHECK(Permission_kindName(kind) != nullptr);
        CHECK(Permission_kindName(kind)[0] != '\0');
        PermissionStatus s = Permission_status(kind);
        CHECK(s >= PERMISSION_UNSUPPORTED && s <= PERMISSION_RESTRICTED);
        CHECK(Permission_statusName(s) != nullptr);
        CHECK(Permission_statusName(s)[0] != '\0');
    }
    CHECK(string_equals(Permission_kindName(PERMISSION_SCREEN_CAPTURE), "PERMISSION_SCREEN_CAPTURE"));
    CHECK(string_equals(Permission_kindName(PERMISSION_ACCESSIBILITY), "PERMISSION_ACCESSIBILITY"));
    CHECK(string_equals(Permission_statusName(PERMISSION_GRANTED), "GRANTED"));
    CHECK(string_equals(Permission_statusName(PERMISSION_UNSUPPORTED), "UNSUPPORTED"));

    CHECK(Permission_status((PermissionKind) 999) == PERMISSION_UNSUPPORTED);
    CHECK(string_equals(Permission_kindName((PermissionKind) 999), "PERMISSION_UNKNOWN"));
    CHECK(string_equals(Permission_statusName((PermissionStatus) 999), "UNKNOWN"));
    CHECK(!Permission_request((PermissionKind) 999));
    CHECK(!Permission_openSettings((PermissionKind) 999));

    CHECK(Permission_requiresRestart(PERMISSION_SCREEN_CAPTURE));
    CHECK(!Permission_requiresRestart(PERMISSION_CAMERA));
    CHECK(!Permission_requiresRestart((PermissionKind) 999));

#if defined(VEX_PLATFORM_APPLE)
    const PermissionKind wired[] = {
        PERMISSION_SCREEN_CAPTURE, PERMISSION_KEY_LISTEN, PERMISSION_POST_EVENT,
        PERMISSION_ACCESSIBILITY, PERMISSION_CAMERA, PERMISSION_MICROPHONE,
        PERMISSION_CONTACTS, PERMISSION_CALENDAR, PERMISSION_PHOTOS,
        PERMISSION_LOCATION, PERMISSION_NOTIFICATIONS
    };
    for (bytes i = 0; i < sizeof wired / sizeof wired[0]; i++)
        CHECK(Permission_status(wired[i]) != PERMISSION_UNSUPPORTED);
    CHECK(Permission_status(PERMISSION_AUTOMATION) == PERMISSION_UNSUPPORTED);
    CHECK(Permission_status(PERMISSION_FULL_DISK) == PERMISSION_UNSUPPORTED);
#endif
    CHECK(!Permission_request(PERMISSION_AUTOMATION));
    CHECK(!Permission_request(PERMISSION_FULL_DISK));
    CHECK(!Permission_request((PermissionKind) 999));
}

;;GETTER
static u64 MemHdr_typeId(const MemoryHeader *h)
{
    return (*h).typeId;
}
;;GETTER
static bytes MemHdr_length(const MemoryHeader *h)
{
    return (*h).length;
}

/** Proves Lifetime arena attestation, pointer legitimacy, header recovery, and teardown. */
static fn suite_lifetime() {
    CHECK(!Lifetime_isLegit(nullptr));
    CHECK(!Lifetime_isLegit((const_any) 0x1000));
    CHECK(!Lifetime_isLegit((const_any) 0x10001));
    CHECK(Lifetime_isLegit((const_any) 0x10000));

    Lifetime lt = Lifetime(64u << 20, 64u << 20);
    CHECK(lt.persistentArena != nullptr);
    CHECK(lt.transientArena != nullptr);
    CHECK(lt.persistentType == VEXSPOKE_TYPE_ARENA);
    CHECK(lt.transientType == VEXSPOKE_TYPE_ARENA);
    CHECK(Lifetime_isLegit(lt.persistentArena));
    CHECK(Lifetime_isLegit(lt.transientArena));
    CHECK(Lifetime_isValid(&lt));

    Lifetime badLt = lt;
    badLt.persistentType = 0;
    CHECK(!Lifetime_isValid(&badLt));
    CHECK(!Lifetime_isValid(nullptr));

    Lifetime bound = Lifetime_bind(lt.persistentArena, lt.transientArena,
                                   lt.persistentType, lt.transientType, nullptr);
    CHECK(Lifetime_isValid(&bound));

    Lifetime badBind = Lifetime_bind((any) 0x10001, lt.transientArena,
                                     lt.persistentType, lt.transientType, nullptr);
    CHECK(badBind.persistentArena == nullptr);

    any p1 = Lifetime_allocPersistent(&lt, 100, 64);
    any p2 = Lifetime_allocPersistent(&lt, 101, 128);
    any t1 = Lifetime_allocTransient(&lt, 200, 48);
    any t2 = Lifetime_allocTransient(&lt, 201, 256);
    CHECK(p1 != nullptr && p2 != nullptr && t1 != nullptr && t2 != nullptr);
    CHECK(Lifetime_isLegit(p1) && Lifetime_isLegit(p2));
    CHECK(Lifetime_isLegit(t1) && Lifetime_isLegit(t2));

    MemoryHeader *h1 = ((MemoryHeader*) p1) - 1;
    MemoryHeader *h2 = ((MemoryHeader*) p2) - 1;
    CHECK(MemHdr_typeId(h1) == 100 && MemHdr_length(h1) == 64);
    CHECK(MemHdr_typeId(h2) == 101 && MemHdr_length(h2) == 128);

    Lifetime_resetTransient(&lt);
    any t3 = Lifetime_allocTransient(&lt, 202, 64);
    CHECK(t3 != nullptr && Lifetime_isLegit(t3));

    CHECK(Lifetime_getPersistentArena(&lt) == lt.persistentArena);
    CHECK(Lifetime_getTransientArena(&lt) == lt.transientArena);
    CHECK(Lifetime_getRelational(&lt) == nullptr);
    any dummyRel = (any) 0x20000;
    Lifetime_setRelational(&lt, dummyRel);
    CHECK(Lifetime_getRelational(&lt) == dummyRel);
    Lifetime_setRelational(&lt, (any) 0x10001);
    CHECK(Lifetime_getRelational(&lt) == dummyRel);

    Lifetime_freePersistent(&lt, p1);
    Lifetime_freePersistent(&lt, p2);
    Lifetime_destroy(&lt);
    CHECK(lt.persistentArena == nullptr && lt.transientArena == nullptr);
    CHECK(lt.persistentType == 0 && lt.transientType == 0);
    Lifetime_destroy(nullptr);
    Lifetime_resetTransient(nullptr);
}

static WeRecorder g_weRec;
static i32 g_weNullOwnerHits;

;;GETTER
static i32 WeRec_fired(const WeRecorder *r)
{
    return (*r).fired;
}
;;SETTER
static fn WeRec_setFired(WeRecorder *r, i32 v)
{
    (*r).fired = v;
}
;;SETTER
static fn WeRec_setLastWindow(WeRecorder *r, Window *w)
{
    (*r).lastWindow = w;
}
;;SETTER
static fn WeRec_setResizedW(WeRecorder *r, i32 w)
{
    (*r).resizedW = w;
}
;;SETTER
static fn WeRec_setResizedH(WeRecorder *r, i32 h)
{
    (*r).resizedH = h;
}
;;SETTER
static fn WeRec_setMovedX(WeRecorder *r, i32 x)
{
    (*r).movedX = x;
}
;;SETTER
static fn WeRec_setMovedY(WeRecorder *r, i32 y)
{
    (*r).movedY = y;
}
;;GETTER
static i32 WeRec_movedCalls(const WeRecorder *r)
{
    return (*r).movedCalls;
}
;;SETTER
static fn WeRec_setMovedCalls(WeRecorder *r, i32 v)
{
    (*r).movedCalls = v;
}
;;SETTER
static fn WeRec_setFocused(WeRecorder *r, bool v)
{
    (*r).focused = v;
}
;;SETTER
static fn WeRec_setLastVisible(WeRecorder *r, bool v)
{
    (*r).lastVisible = v;
}
;;GETTER
static i32 WeRec_occludedCalls(const WeRecorder *r)
{
    return (*r).occludedCalls;
}
;;SETTER
static fn WeRec_setOccludedCalls(WeRecorder *r, i32 v)
{
    (*r).occludedCalls = v;
}
;;GETTER
static i32 WeRec_quitCalls(const WeRecorder *r)
{
    return (*r).quitCalls;
}
;;SETTER
static fn WeRec_setQuitCalls(WeRecorder *r, i32 v)
{
    (*r).quitCalls = v;
}

/** Counts resize callbacks fired with a null stored context. */
static fn onResizedNullOwnerRec(any self, Window *window, i32 width, i32 height) {
    (discard) self;
    (discard) window;
    (discard) width;
    (discard) height;
    g_weNullOwnerHits++;
}

/** Records resize callback arguments in the WindowEvent fixture. */
static fn onResizedRec(any self, Window *window, i32 width, i32 height) {
    WeRecorder *r = (WeRecorder*) self;
    WeRec_setLastWindow(r, window);
    WeRec_setResizedW(r, width);
    WeRec_setResizedH(r, height);
    WeRec_setFired(r, WeRec_fired(r) + 1);
}

/** Records move callback arguments in the WindowEvent fixture. */
static fn onMovedRec(any self, Window *window, i32 x, i32 y) {
    WeRecorder *r = (WeRecorder*) self;
    WeRec_setLastWindow(r, window);
    WeRec_setMovedX(r, x);
    WeRec_setMovedY(r, y);
    WeRec_setMovedCalls(r, WeRec_movedCalls(r) + 1);
}

/** Records generic void window-event delivery. */
static fn onVoidWindowRec(any self, Window *window) {
    WeRecorder *r = (WeRecorder*) self;
    WeRec_setLastWindow(r, window);
    WeRec_setFired(r, WeRec_fired(r) + 1);
}

/** Records focus-gained delivery. */
static fn onFocusGainedRec(any self, Window *window) {
    WeRecorder *r = (WeRecorder*) self;
    WeRec_setLastWindow(r, window);
    WeRec_setFocused(r, true);
    WeRec_setFired(r, WeRec_fired(r) + 1);
}

/** Records focus-lost delivery. */
static fn onFocusLostRec(any self, Window *window) {
    WeRecorder *r = (WeRecorder*) self;
    WeRec_setLastWindow(r, window);
    WeRec_setFocused(r, false);
    WeRec_setFired(r, WeRec_fired(r) + 1);
}

/** Records occlusion visibility changes. */
static fn onOcclusionRec(any self, Window *window, bool visible) {
    WeRecorder *r = (WeRecorder*) self;
    WeRec_setLastWindow(r, window);
    WeRec_setLastVisible(r, visible);
    WeRec_setOccludedCalls(r, WeRec_occludedCalls(r) + 1);
}

/** Records quit request and permits closing. */
static bool onQuitAllowRec(any self, Window *window) {
    WeRecorder *r = (WeRecorder*) self;
    WeRec_setLastWindow(r, window);
    WeRec_setQuitCalls(r, WeRec_quitCalls(r) + 1);
    return true;
}

/** Records quit request and vetoes closing. */
static bool onQuitVetoRec(any self, Window *window) {
    WeRecorder *r = (WeRecorder*) self;
    WeRec_setLastWindow(r, window);
    WeRec_setQuitCalls(r, WeRec_quitCalls(r) + 1);
    return false;
}

/** Proves WindowEvent getter/setter symmetry, dispatch forwarding, vetoable quit, and null safety. */
static fn suite_window_event() {
    memset(&g_weRec, 0, sizeof(g_weRec));
    g_weNullOwnerHits = 0;

    WindowEvent ev;
    (discard) WindowEvent_init(&ev);
    Window *dummyWindow = (Window*) (pointer) 0x1234;

    CHECK(WindowEvent_getSelf(&ev) == nullptr);
    CHECK(WindowEvent_getOnQuitRequested(&ev) == nullptr);
    CHECK(WindowEvent_getOnResized(&ev) == nullptr);
    CHECK(WindowEvent_getOnMoved(&ev) == nullptr);
    CHECK(WindowEvent_getOnOcclusionChanged(&ev) == nullptr);

    WindowEvent_fireResized(&ev, dummyWindow, 800, 600);
    WindowEvent_fireMoved(&ev, dummyWindow, 10, 20);
    WindowEvent_firePressed(&ev, dummyWindow);
    WindowEvent_fireOcclusionChanged(&ev, dummyWindow, false);
    CHECK(g_weRec.fired == 0 && g_weRec.movedCalls == 0 && g_weRec.occludedCalls == 0);
    CHECK(WindowEvent_fireQuitRequested(&ev, dummyWindow) == true);
    CHECK(WindowEvent_fireQuitRequested(nullptr, dummyWindow) == true);

    WindowEvent_setSelf(&ev, &g_weRec);
    WindowEvent_setOnResized(&ev, onResizedRec);
    WindowEvent_setOnMoved(&ev, onMovedRec);
    WindowEvent_setOnFullscreen(&ev, onVoidWindowRec);
    WindowEvent_setOnMinimized(&ev, onVoidWindowRec);
    WindowEvent_setOnRestored(&ev, onVoidWindowRec);
    WindowEvent_setOnPressed(&ev, onVoidWindowRec);
    WindowEvent_setOnFocusGained(&ev, onFocusGainedRec);
    WindowEvent_setOnFocusLost(&ev, onFocusLostRec);
    WindowEvent_setOnZoomFilled(&ev, onVoidWindowRec);
    WindowEvent_setOnOcclusionChanged(&ev, onOcclusionRec);

    CHECK(WindowEvent_getSelf(&ev) == &g_weRec);
    CHECK(WindowEvent_getOnResized(&ev) == onResizedRec);
    CHECK(WindowEvent_getOnMoved(&ev) == onMovedRec);
    CHECK(WindowEvent_getOnOcclusionChanged(&ev) == onOcclusionRec);

    WindowEvent_fireResized(&ev, dummyWindow, 1280, 720);
    CHECK(g_weRec.fired == 1 && g_weRec.resizedW == 1280 && g_weRec.resizedH == 720);
    WindowEvent_fireMoved(&ev, dummyWindow, 42, 99);
    CHECK(g_weRec.movedCalls == 1 && g_weRec.movedX == 42 && g_weRec.movedY == 99);
    WindowEvent_fireFullscreen(&ev, nullptr);
    WindowEvent_fireMinimized(&ev, dummyWindow);
    WindowEvent_fireRestored(&ev, dummyWindow);
    WindowEvent_firePressed(&ev, dummyWindow);
    WindowEvent_fireZoomFilled(&ev, dummyWindow);
    CHECK(g_weRec.fired == 6);
    WindowEvent_fireFocusGained(&ev, dummyWindow);
    CHECK(g_weRec.focused == true);
    WindowEvent_fireFocusLost(&ev, dummyWindow);
    CHECK(g_weRec.focused == false && g_weRec.fired == 8);
    WindowEvent_fireOcclusionChanged(&ev, dummyWindow, false);
    CHECK(g_weRec.occludedCalls == 1 && g_weRec.lastVisible == false);
    WindowEvent_fireOcclusionChanged(&ev, dummyWindow, true);
    CHECK(g_weRec.occludedCalls == 2 && g_weRec.lastVisible == true);

    WindowEvent_setOnQuitRequested(&ev, onQuitAllowRec);
    CHECK(WindowEvent_fireQuitRequested(&ev, dummyWindow) == true);
    WindowEvent_setOnQuitRequested(&ev, onQuitVetoRec);
    CHECK(WindowEvent_fireQuitRequested(&ev, dummyWindow) == false);
    CHECK(g_weRec.quitCalls == 2);

    WindowEvent_setSelf(&ev, nullptr);
    WindowEvent_setOnResized(&ev, onResizedNullOwnerRec);
    WindowEvent_fireResized(&ev, dummyWindow, 2, 4);
    CHECK(g_weNullOwnerHits == 1);

    CHECK(WindowEvent_init(nullptr) == false);
    (discard) WindowEvent_init(&ev);
    CHECK(WindowEvent_getSelf(&ev) == nullptr);
}

/** Proves the inert pixel bridge seam is null-safe and degrades cleanly. */
static fn suite_bridge_seam() {
    CHECK(Window_attachPanes(nullptr, nullptr, 640, 400) == false);
    CHECK(Window_resizePanes(nullptr, nullptr, 800, 600) == false);
    Window_compositePanes(nullptr, nullptr);
    Window_compositeBoards(nullptr);
    CHECK(Window_metalLayer(nullptr) == nullptr);
    Window_setGravityTopLeft(nullptr);
    Window_workerPresentBegin();
    Window_workerPresentEnd();
    CHECK(Window_present(nullptr, nullptr) == false);
}

;;GETTER
static i32 FakeConsole_spawns(const FakeConsoleIo *f)
{
    return (*f).spawns;
}
;;SETTER
static fn FakeConsole_setSpawns(FakeConsoleIo *f, i32 v)
{
    (*f).spawns = v;
}
;;GETTER
static i32 FakeConsole_feeds(const FakeConsoleIo *f)
{
    return (*f).feeds;
}
;;SETTER
static fn FakeConsole_setFeeds(FakeConsoleIo *f, i32 v)
{
    (*f).feeds = v;
}
;;GETTER
static i32 FakeConsole_reaps(const FakeConsoleIo *f)
{
    return (*f).reaps;
}
;;SETTER
static fn FakeConsole_setReaps(FakeConsoleIo *f, i32 v)
{
    (*f).reaps = v;
}
;;GETTER
static i32 FakeConsole_cancels(const FakeConsoleIo *f)
{
    return (*f).cancels;
}
;;SETTER
static fn FakeConsole_setCancels(FakeConsoleIo *f, i32 v)
{
    (*f).cancels = v;
}
;;GETTER
static bool FakeConsole_joined(const FakeConsoleIo *f)
{
    return (*f).joined;
}
;;GETTER
static utf8 *FakeConsole_lastFeedMut(FakeConsoleIo *f)
{
    return (*f).lastFeed;
}
;;SETTER
static fn FakeConsole_setLastFeedLen(FakeConsoleIo *f, bytes v)
{
    (*f).lastFeedLen = v;
}
;;GETTER
static any ConsoleIo_spawnPtr(const ConsoleIo *io)
{
    return (any) (*io).spawn;
}

/** Fake ConsoleIo spawn hook. */
static bool fakeConsoleSpawn(any ctx, string shell, string workDir) {
    (discard) shell;
    (discard) workDir;
    FakeConsoleIo *f = ctx;
    FakeConsole_setSpawns(f, FakeConsole_spawns(f) + 1);
    return true;
}

/** Fake ConsoleIo input feed hook. */
static bool fakeConsoleFeed(any ctx, string data, bytes len) {
    FakeConsoleIo *f = ctx;
    FakeConsole_setFeeds(f, FakeConsole_feeds(f) + 1);
    bytes cap = sizeof(((FakeConsoleIo) { 0 }).lastFeed);
    bytes n = len < cap - 1 ? len : cap - 1;
    utf8 *lastFeed = FakeConsole_lastFeedMut(f);
    memcpy(lastFeed, data, n);
    lastFeed[n] = '\0';
    FakeConsole_setLastFeedLen(f, len);
    return true;
}

/** Fake ConsoleIo output reap hook. */
static bool fakeConsoleReap(any ctx, utf8 *out, bytes outCap, bytes *outLen) {
    FakeConsoleIo *f = ctx;
    FakeConsole_setReaps(f, FakeConsole_reaps(f) + 1);
    string msg = "out";
    bytes n = 3 < outCap ? 3 : outCap;
    if (out && outCap > 0)
        memcpy(out, msg, n);
    if (outLen)
        (*outLen) = n;
    return true;
}

/** Fake ConsoleIo cancel hook. */
static fn fakeConsoleCancel(any ctx) {
    FakeConsoleIo *f = ctx;
    FakeConsole_setCancels(f, FakeConsole_cancels(f) + 1);
}

/** Fake ConsoleIo joined query hook. */
static bool fakeConsoleJoined(const_any ctx) {
    return FakeConsole_joined((const FakeConsoleIo*) ctx);
}

/** Proves Console defaults, clamping, injected IO routing, cancel-on-free, and null safety. */
static fn suite_console() {
    Console *c = Console_0();
    CHECK(c != nullptr);
    CHECK(string_equals(Console_getShell(c), "/bin/sh"));
    CHECK(Console_getWorkDir(c)[0] == '\0');
    CHECK(Console_getIo(c) == nullptr);
    CHECK(!Console_isRunning(c));
    CHECK(!Console_run(c));
    CHECK(!Console_writeInput(c, "x", 1));
    CHECK(Console_getExitStatus(c) == 0);

    utf8 big[2000];
    memset(big, 'a', sizeof big - 1);
    big[sizeof big - 1] = '\0';
    Console_setShell(c, big);
    CHECK(strlen(Console_getShell(c)) == CONSOLE_MAX_SHELL - 1);
    Console_setWorkDir(c, big);
    CHECK(strlen(Console_getWorkDir(c)) == CONSOLE_MAX_WORKDIR - 1);

    FakeConsoleIo f = { 0 };
    ConsoleIo io;
    memset(&io, 0, sizeof(io));
    io.ctx = &f;
    io.spawn = fakeConsoleSpawn;
    io.feed = fakeConsoleFeed;
    io.reap = fakeConsoleReap;
    io.cancel = fakeConsoleCancel;
    io.joined = fakeConsoleJoined;
    Console_setIo(c, &io, &f);
    const ConsoleIo *got = Console_getIo(c);
    CHECK(got != nullptr && ConsoleIo_spawnPtr(got) == (any) fakeConsoleSpawn);

    CHECK(Console_run(c) && f.spawns == 1 && Console_isRunning(c));
    CHECK(!Console_run(c));
    CHECK(Console_writeInput(c, "hello\n", 6) && f.feeds == 1);
    CHECK(string_equals(f.lastFeed, "hello\n") && f.lastFeedLen == 6);

    utf8 out[16];
    bytes outLen = 999;
    CHECK(Console_poll(c, out, sizeof out, &outLen) && f.reaps == 1 && outLen == 3);
    f.joined = true;
    CHECK(Console_poll(c, out, sizeof out, &outLen) && !Console_isRunning(c));
    Console_cancel(c);
    CHECK(f.cancels == 1);
    Console_free(c);
    Console_free(nullptr);

    Console *c4 = Console_1("sh");
    Console_setIo(c4, &io, &f);
    i32 before = f.cancels;
    CHECK(Console_run(c4));
    Console_free(c4);
    CHECK(f.cancels == before + 1);

    Console *c2 = Console("bash");
    CHECK(string_equals(Console_getShell(c2), "bash"));
    Console_free(c2);
}

;;GETTER
static i32 FakeCli_opens(const FakeCli *f)
{
    return (*f).opens;
}
;;SETTER
static fn FakeCli_setOpens(FakeCli *f, i32 v)
{
    (*f).opens = v;
}
;;GETTER
static i32 FakeCli_closes(const FakeCli *f)
{
    return (*f).closes;
}
;;SETTER
static fn FakeCli_setCloses(FakeCli *f, i32 v)
{
    (*f).closes = v;
}
;;GETTER
static i32 FakeCli_presents(const FakeCli *f)
{
    return (*f).presents;
}
;;SETTER
static fn FakeCli_setPresents(FakeCli *f, i32 v)
{
    (*f).presents = v;
}
;;GETTER
static i32 FakeCli_polls(const FakeCli *f)
{
    return (*f).polls;
}
;;SETTER
static fn FakeCli_setPolls(FakeCli *f, i32 v)
{
    (*f).polls = v;
}
;;SETTER
static fn FakeCli_setMouse(FakeCli *f, i32 v)
{
    (*f).mouse = v;
}
;;GETTER
static bytes FakeCli_w(const FakeCli *f)
{
    return (*f).w;
}
;;GETTER
static bytes FakeCli_h(const FakeCli *f)
{
    return (*f).h;
}
;;GETTER
static bool FakeCli_pending(const FakeCli *f)
{
    return (*f).pending;
}
;;SETTER
static fn FakeCli_setPending(FakeCli *f, bool v)
{
    (*f).pending = v;
}
;;GETTER
static CommandLineEvent FakeCli_next(const FakeCli *f)
{
    return (*f).next;
}
;;GETTER
static i32 FakeCliApp_draws(const FakeCliApp *a)
{
    return (*a).draws;
}
;;SETTER
static fn FakeCliApp_setDraws(FakeCliApp *a, i32 v)
{
    (*a).draws = v;
}
;;GETTER
static i32 FakeCliApp_inputs(const FakeCliApp *a)
{
    return (*a).inputs;
}
;;SETTER
static fn FakeCliApp_setInputs(FakeCliApp *a, i32 v)
{
    (*a).inputs = v;
}
;;GETTER
static bool FakeCliApp_stopOnInput(const FakeCliApp *a)
{
    return (*a).stopOnInput;
}

/** Fake CommandLineIo open hook. */
static bool cliOpen(any ctx)
{
    FakeCli *f = ctx;
    FakeCli_setOpens(f, FakeCli_opens(f) + 1);
    return true;
}
/** Fake CommandLineIo close hook. */
static fn cliClose(any ctx)
{
    FakeCli *f = ctx;
    FakeCli_setCloses(f, FakeCli_closes(f) + 1);
}
/** Fake CommandLineIo size query hook. */
static bool cliSize(any ctx, bytes *w, bytes *h) {
    FakeCli *f = ctx;
    bytes fw = FakeCli_w(f);
    bytes fh = FakeCli_h(f);
    *w = fw;
    *h = fh;
    return fw > 0 && fh > 0;
}
/** Fake CommandLineIo present hook. */
static bool cliPresent(any ctx, string cells, bytes w, bytes h) {
    (discard) cells;
    (discard) w;
    (discard) h;
    FakeCli *f = ctx;
    FakeCli_setPresents(f, FakeCli_presents(f) + 1);
    return true;
}
/** Fake CommandLineIo poll hook. */
static bool cliPoll(any ctx, i32 timeoutMs, CommandLineEvent *out) {
    (discard) timeoutMs;
    FakeCli *f = ctx;
    FakeCli_setPolls(f, FakeCli_polls(f) + 1);
    if (!FakeCli_pending(f))
        return false;
    FakeCli_setPending(f, false);
    *out = FakeCli_next(f);
    return true;
}
/** Fake CommandLineIo mouse toggle hook. */
static bool cliMouse(any ctx, bool enabled)
{
    FakeCli_setMouse(ctx, enabled ? 1 : 0);
    return true;
}

/** Fake CommandLineApp draw callback. */
static bool cliAppDraw(any ctx, CommandLine *line) {
    FakeCliApp *a = ctx;
    FakeCliApp_setDraws(a, FakeCliApp_draws(a) + 1);
    utf8 *cells = CommandLine_getCells(line);
    if (cells != nullptr)
        memset(cells, '.', CommandLine_getWidth(line) * CommandLine_getHeight(line));
    return true;
}
/** Fake CommandLineApp input callback. */
static bool cliAppInput(any ctx, CommandLine *line, const CommandLineEvent *event) {
    (discard) event;
    FakeCliApp *a = ctx;
    FakeCliApp_setInputs(a, FakeCliApp_inputs(a) + 1);
    if (FakeCliApp_stopOnInput(a)) {
        CommandLine_stop(line);
        return false;
    }
    return true;
}

/** Builds a CommandLineIo vtable bound to a FakeCli fixture. */
static CommandLineIo makeFakeCliIo(FakeCli *f) {
    CommandLineIo io;
    memset(&io, 0, sizeof(io));
    io.ctx = f;
    io.open = cliOpen;
    io.close = cliClose;
    io.size = cliSize;
    io.present = cliPresent;
    io.poll = cliPoll;
    io.setMouse = cliMouse;
    return io;
}

/** Proves CommandLine headless device seam, resize repaint, run/cancel lifecycle, and Kernel registration. */
static fn suite_commandline() {
    CHECK(CommandLine_getWidth(nullptr) == 0);
    CHECK(CommandLine_getCellsConst(nullptr) == nullptr);
    CHECK(!CommandLine_isOpen(nullptr));

    utf8 buf[256];
    bool trunc = true;
    CHECK(CommandLine_toString(nullptr, buf, sizeof(buf), &trunc) && !trunc && string_equals(buf, "nullptr"));
    CHECK(CommandLine_toStringStruct(nullptr, nullptr, 0, &trunc) == false && trunc);

    CommandLine *def = CommandLine();
    CommandLine *named = CommandLine("Test Terminal");
    CHECK(def != nullptr && named != nullptr);
    CHECK(string_equals(CommandLine_getTitle(def), "vex"));
    CHECK(string_equals(CommandLine_getTitle(named), "Test Terminal"));

    FakeCli fake = { 0 };
    fake.w = 80;
    fake.h = 24;
    CommandLineIo io = makeFakeCliIo(&fake);
    CHECK(CommandLine_setIo(named, &io));
    CHECK(CommandLine_open(named) && fake.opens == 1 && CommandLine_isOpen(named));
    CHECK(CommandLine_getWidth(named) == 80 && CommandLine_getHeight(named) == 24);
    CHECK(CommandLine_getCapacity(named) == 80 * 24 && CommandLine_getCells(named) != nullptr);
    CHECK(!CommandLine_open(named));
    CHECK(CommandLine_clear(named) && CommandLine_getCellsConst(named)[0] == ' ');
    CHECK(CommandLine_present(named) && fake.presents == 1);

    FakeCliApp app = { 0 };
    CommandLineApp seam;
    memset(&seam, 0, sizeof(seam));
    seam.ctx = &app;
    seam.draw = cliAppDraw;
    seam.input = cliAppInput;
    CHECK(CommandLine_setApp(named, &seam));
    CHECK(CommandLine_getGeneration(named) == 1 && fake.opens == 1);
    CommandLineApp bad;
    memset(&bad, 0, sizeof(bad));
    bad.ctx = &app;
    CHECK(!CommandLine_setApp(named, &bad));
    CHECK(CommandLine_getGeneration(named) == 1 && CommandLine_hasApp(named));

    fake.w = 120;
    fake.h = 40;
    i32 drawsBefore = app.draws;
    i32 presentsBefore = fake.presents;
    CHECK(CommandLine_revalidate(named));
    CHECK(CommandLine_getWidth(named) == 120 && CommandLine_getHeight(named) == 40);
    CHECK(app.draws == drawsBefore + 1 && fake.presents == presentsBefore + 1);

    fake.w = 0;
    fake.h = 0;
    CHECK(!CommandLine_revalidate(named) && CommandLine_exhaustionCount(named) == 1);
    CHECK(!CommandLine_revalidate(named) && CommandLine_exhaustionCount(named) == 2);
    fake.w = 100;
    fake.h = 30;

    CommandLineEvent ev;
    CHECK(!CommandLine_poll(named, 0, &ev));
    fake.pending = true;
    fake.next = (CommandLineEvent) { .kind = COMMANDLINE_EVENT_KEY, .key = COMMANDLINE_KEY_ENTER };
    CHECK(CommandLine_poll(named, 10, &ev) && ev.kind == COMMANDLINE_EVENT_KEY && ev.key == COMMANDLINE_KEY_ENTER);
    CHECK(CommandLine_setMouseReporting(named, true) && fake.mouse == 1);
    CHECK(CommandLine_setMouseReporting(named, false) && fake.mouse == 0);
    CHECK(CommandLine_tick(named));

    CHECK(CommandLine_getRunPhase(named) == COMMANDLINE_RUN_IDLE);
    CommandLine_stop(named);
    CHECK(CommandLine_getRunPhase(named) == COMMANDLINE_RUN_IDLE);
    i32 pollsAtRunStart = fake.polls;
    app.stopOnInput = true;
    fake.pending = true;
    fake.next = (CommandLineEvent) { .kind = COMMANDLINE_EVENT_KEY, .key = 3 };
    CHECK(CommandLine_run(named) == 0 && CommandLine_isCancelled(named));
    CHECK(CommandLine_getRunPhase(named) == COMMANDLINE_RUN_CANCELLED && fake.polls > pollsAtRunStart);
    CHECK(CommandLine_run(named) == 1);
    CHECK(CommandLine_detachApp(named) && !CommandLine_hasApp(named));
    CommandLine_close(named);
    CHECK(fake.closes == 1 && !CommandLine_isOpen(named));
    CHECK(CommandLine_open(named) && CommandLine_getRunPhase(named) == COMMANDLINE_RUN_IDLE);
    CommandLine_close(named);

    Kernel *k = Kernel();
    CHECK(k != nullptr);
    CommandLine *cli = CommandLine("kernel-cli");
    FakeCli kf = { 0 };
    kf.w = 60;
    kf.h = 20;
    CommandLineIo kio = makeFakeCliIo(&kf);
    CHECK(CommandLine_setIo(cli, &kio) && CommandLine_open(cli));
    CHECK(Kernel_addCommandLine(k, cli) && Kernel_getCommandLineCount(k) == 1);
    CHECK(Kernel_getCommandLine(k, 0) == cli && Kernel_getCommandLine(k, 5) == nullptr);
    CommandLine *handles[4];
    CHECK(Kernel_getCommandLines(k, handles, 4) == 1 && handles[0] == cli);
    CHECK(!Kernel_free(k));
    CHECK(Kernel_removeCommandLine(k, cli) && Kernel_getCommandLineCount(k) == 0);
    CHECK(Kernel_free(k));
    CommandLine_close(cli);
    CommandLine_free(cli);
    CommandLine_free(def);
    CommandLine_free(named);
}

;;SETTER
static fn ProcCall_setThread(ProcTestCall *call, thread worker)
{
    (*call).worker = worker;
}
;;GETTER
static atomic_bool *ProcCall_enteredMut(ProcTestCall *call)
{
    return &(*call).entered;
}
;;GETTER
static atomic_bool *ProcCall_releaseMut(ProcTestCall *call)
{
    return &(*call).release;
}
;;SETTER
static fn ProcCall_setResult(ProcTestCall *call, ProcessResult result)
{
    (*call).result = result;
}
;;GETTER
static Process *ProcCall_process(const ProcTestCall *call)
{
    return (*call).process;
}
;;GETTER
static i32 *ProcCall_exitStatusMut(ProcTestCall *call)
{
    return &(*call).exitStatus;
}

/** Original process entry fixture. */
static i32 proc_original(any context)
{
    return context ? *(i32*) context : 11;
}
/** Replacement process entry fixture. */
static i32 proc_replacement(any context)
{
    return context ? *(i32*) context + 100 : 22;
}

/** Proves reentrant run, replace, and rename return PROCESS_BUSY. */
static i32 proc_reentrant(any context) {
    Process *process = (Process*) context;
    i32 result = 123;
    CHECK(Process_isRunning(process));
    CHECK(Process_run(process, &result) == PROCESS_BUSY && result == 123);
    CHECK(Process_replace(process, proc_original, nullptr, nullptr) == PROCESS_BUSY);
    CHECK(Process_setName(process, "no") == PROCESS_BUSY);
    return -1;
}

/** Bounded wait for an atomic_bool flag with 5-second timeout. */
static bool waitFlag(atomic_bool *flag) {
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    const struct timespec slice = { 0, 1000000L };
    while (!atomic_load_explicit(flag, memory_order_acquire)) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec - start.tv_sec >= 5)
            return false;
        nanosleep(&slice, nullptr);
    }
    return true;
}

/** Process entry that holds until released by the main test thread. */
static i32 proc_heldEntry(any context) {
    ProcTestCall *call = (ProcTestCall*) context;
    ProcCall_setThread(call, pthread_self());
    atomic_store_explicit(ProcCall_enteredMut(call), true, memory_order_release);
    if (!waitFlag(ProcCall_releaseMut(call)))
        _Exit(2);
    return 77;
}

/** Worker thread body invoking Process_run on the held entry. */
static any proc_worker(any context) {
    ProcTestCall *call = (ProcTestCall*) context;
    ProcCall_setResult(call, Process_run(ProcCall_process(call), ProcCall_exitStatusMut(call)));
    return nullptr;
}

/** Proves Process binding, replacement, busy guards, concurrency, and Kernel dispatch. */
static fn suite_process() {
    i32 result = 123;
    i32 oldContext = -1;
    i32 newContext = 7;
    i32 association = 0;

    CHECK(Process(nullptr) == nullptr);
    CHECK(Process_run(nullptr, &result) == PROCESS_INVALID && result == 123);
    CHECK(Process_replace(nullptr, proc_original, nullptr, nullptr) == PROCESS_INVALID);
    CHECK(Process_setName(nullptr, "x") == PROCESS_INVALID);
    CHECK(!Process_getEntry(nullptr) && !Process_getContext(nullptr) && !Process_getHot(nullptr) && !Process_getName(nullptr));
    CHECK(!Process_isRunning(nullptr) && Process_getInvocationCount(nullptr) == 0 && !Process_free(nullptr));

    Process *process = Process("task", proc_original, &oldContext);
    CHECK(process != nullptr);
    CHECK(string_equals(Process_getName(process), "task"));
    CHECK(Process_run(process, nullptr) == PROCESS_INVALID);
    CHECK(Process_run(process, &result) == PROCESS_OK && result == -1);
    CHECK(Process_replace(process, nullptr, &newContext, &association) == PROCESS_INVALID);
    CHECK(Process_getEntry(process) == proc_original && Process_getContext(process) == &oldContext);
    CHECK(Process_replace(process, proc_replacement, &newContext, &association) == PROCESS_OK);
    CHECK(Process_run(process, &result) == PROCESS_OK && result == 107);
    CHECK(Process_run(process, &result) == PROCESS_OK && result == 107);
    CHECK(Process_getInvocationCount(process) == 3);
    CHECK(Process_setName(process, nullptr) == PROCESS_OK && !Process_getName(process));
    CHECK(Process_replace(process, proc_reentrant, process, nullptr) == PROCESS_OK);
    CHECK(Process_run(process, &result) == PROCESS_OK && result == -1);
    CHECK(Process_free(process));

    ProcTestCall call = { 0 };
    atomic_init(&call.entered, false);
    atomic_init(&call.release, false);
    call.process = Process(proc_heldEntry, &call);
    CHECK(call.process != nullptr);
    thread th;
    CHECK(pthread_create(&th, nullptr, proc_worker, &call) == 0);
    CHECK(waitFlag(&call.entered));
    CHECK(pthread_equal(th, call.worker) && Process_isRunning(call.process));
    result = 123;
    CHECK(Process_run(call.process, &result) == PROCESS_BUSY && result == 123);
    CHECK(Process_replace(call.process, proc_replacement, &newContext, nullptr) == PROCESS_BUSY);
    atomic_store_explicit(&call.release, true, memory_order_release);
    CHECK(pthread_join(th, nullptr) == 0);
    CHECK(call.result == PROCESS_OK && call.exitStatus == 77);
    CHECK(!Process_isRunning(call.process) && Process_getInvocationCount(call.process) == 1);
    CHECK(Process_replace(call.process, proc_replacement, &newContext, nullptr) == PROCESS_OK);
    CHECK(Process_run(call.process, &result) == PROCESS_OK && result == 107);
    CHECK(Process_free(call.process));

    Process *simple = Process(proc_original);
    Kernel *kernel = Kernel();
    CHECK(simple != nullptr && kernel != nullptr);
    CHECK(Kernel_run(kernel, simple) == 11);
    CHECK(Kernel_addProcess(kernel, simple));
    CHECK(Kernel_run(kernel) == 11);
    CHECK(Kernel_removeProcess(kernel, simple));
    CHECK(Process_free(simple) && Kernel_free(kernel));
}

/** Reads monotonic nanoseconds for bounded child-process reaping. */
static u64 spawn_nowNs() {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (u64) t.tv_sec * 1000000000ULL + (u64) t.tv_nsec;
}

/** Reaps spawned child jobs under a 2-second watchdog. */
static fn spawn_reap(ProcessSpawn *owner) {
    u64 start = spawn_nowNs();
    while (!ProcessSpawn_poll(owner, PROCESS_SPAWN_POLL_MAX_NS))
        assert(spawn_nowNs() - start < 2000000000ULL);
}

/** Proves ProcessSpawn job table capacity, reap/reuse, cancel, and null safety. */
static fn suite_process_spawn() {
    ProcessSpawn *a = ProcessSpawn();
    ProcessSpawn *b = ProcessSpawn(20);
    assert(a && b && ProcessSpawn_getTimeoutMs(b) == 20);
    assert(ProcessSpawn_getPid(nullptr) == 0 && ProcessSpawn_getExitCode(nullptr) == 0);
    assert(ProcessSpawn_getTimeoutMs(nullptr) == 0 && ProcessSpawn_getCount(nullptr) == 0);
    assert(ProcessSpawn_isCancelFlag(nullptr));
    assert(ProcessSpawn_getJobPid(nullptr, 0) == 0 && ProcessSpawn_getJobExitCode(nullptr, 0) == 0);
    assert(ProcessSpawn_isJobDone(nullptr, 0) && ProcessSpawn_poll(a, 0));

    ProcessSpawn_setPid(a, 7);
    ProcessSpawn_setTimeoutMs(a, 9);
    ProcessSpawn_setExitCode(a, 11);
    ProcessSpawn_setCancelFlag(a, true);
    assert(ProcessSpawn_getPid(a) == 7 && ProcessSpawn_getTimeoutMs(a) == 9);
    assert(ProcessSpawn_getExitCode(a) == 11 && ProcessSpawn_isCancelFlag(a));

    string args[] = { "/usr/bin/true", nullptr };
    string invalid[] = { "/nonexistent-re-owner-program", nullptr };
    string empty[] = { nullptr };
    assert(!ProcessSpawn_spawn(args, 0, a));
    ProcessSpawn_setCancelFlag(a, false);
    assert(!ProcessSpawn_spawn(nullptr, 0, a) && !ProcessSpawn_spawn(empty, 0, a));
    assert(!ProcessSpawn_spawn(args, 0, nullptr) && !ProcessSpawn_spawn(invalid, 0, a));
    assert(ProcessSpawn_getPid(a) == 7 && ProcessSpawn_getCount(a) == 0);

    for (u32 i = 0; i < PROCESS_SPAWN_JOBS_MAX; ++i)
        assert(ProcessSpawn_spawn(args, 0, a));
    assert(!ProcessSpawn_spawn(args, 0, a));
    assert(ProcessSpawn_getCount(a) == PROCESS_SPAWN_JOBS_MAX);
    spawn_reap(a);
    assert(ProcessSpawn_getCount(a) == 0);
    for (u32 i = 0; i < PROCESS_SPAWN_JOBS_MAX; ++i)
        assert(ProcessSpawn_isJobDone(a, i) && ProcessSpawn_getJobPid(a, i) > 0 && ProcessSpawn_getJobExitCode(a, i) == 0);
    assert(ProcessSpawn_getJobPid(a, UINT32_MAX) == 0 && ProcessSpawn_isJobDone(a, UINT32_MAX));

    assert(ProcessSpawn_spawn(args, 0, a) && ProcessSpawn_getCount(a) == 1);
    spawn_reap(a);
    assert(ProcessSpawn_getCount(a) == 0);

    string pending[] = { "/bin/sleep", "30", nullptr };
    assert(ProcessSpawn_spawn(pending, 0, b));
    ProcessSpawn_cancel(b);
    spawn_reap(b);
    assert(ProcessSpawn_getCount(b) == 0 && ProcessSpawn_isCancelFlag(b));
    ProcessSpawn_free(a);
    ProcessSpawn_free(b);
    ProcessSpawn_free(nullptr);
    Memory_freeAll();
}

;;SETTER
static fn KfRun_setThreadId(KfRunCtx *ctx, thread id)
{
    (*ctx).threadId = id;
}
;;GETTER
static _Atomic bool *KfRun_threadRanMut(KfRunCtx *ctx) { return &(*ctx).threadRan; }
;;GETTER
static _Atomic i32 *KfRun_counterMut(KfRunCtx *ctx) { return &(*ctx).counter; }
;;SETTER
static fn KfEnd_setKernel(KfEndCtx *ctx, Kernel *k)
{
    (*ctx).receivedKernel = k;
}
;;GETTER
static _Atomic i32 *KfEnd_callsMut(KfEndCtx *ctx) { return &(*ctx).endCalls; }

/** Background worker fixture for Kernel_addRunFunction. */
static fn kf_sampleRun(any userdata) {
    KfRunCtx *ctx = (KfRunCtx*) userdata;
    KfRun_setThreadId(ctx, pthread_self());
    atomic_store_explicit(KfRun_threadRanMut(ctx), true, memory_order_relaxed);
    for (i32 i = 0; i < 5; i++) {
        atomic_fetch_add_explicit(KfRun_counterMut(ctx), 1, memory_order_relaxed);
        struct timespec ts = { 0, 10000000L };
        nanosleep(&ts, nullptr);
    }
}

/** End-hook fixture for Kernel_addEndFunction. */
static fn kf_sampleEnd(Kernel *kernel, any userdata) {
    KfEndCtx *ctx = (KfEndCtx*) userdata;
    KfEnd_setKernel(ctx, kernel);
    atomic_fetch_add_explicit(KfEnd_callsMut(ctx), 1, memory_order_relaxed);
}

/** Proves Kernel run-function concurrency, fire-once end hooks, and dynamic capacity growth. */
static fn suite_kernel_function() {
    Kernel *k = Kernel();
    CHECK(Kernel_getRunFunctionCount(k) == 0 && Kernel_getEndFunctionCount(k) == 0);
    KfRunCtx runCtx1 = { 0 };
    KfRunCtx runCtx2 = { 0 };
    KfEndCtx endCtx = { 0 };
    CHECK(Kernel_addRunFunction(k, kf_sampleRun, &runCtx1));
    CHECK(Kernel_addRunFunction(k, kf_sampleRun, &runCtx2));
    CHECK(Kernel_getRunFunctionCount(k) == 2);
    CHECK(Kernel_addEndFunction(k, kf_sampleEnd, &endCtx));
    CHECK(Kernel_getEndFunctionCount(k) == 1);

    thread mainThread = pthread_self();
    CHECK(Kernel_run(k) == KERNEL_EXIT_OK);
    CHECK(atomic_load_explicit(&runCtx1.threadRan, memory_order_relaxed));
    CHECK(atomic_load_explicit(&runCtx2.threadRan, memory_order_relaxed));
    CHECK(!pthread_equal(mainThread, runCtx1.threadId) && !pthread_equal(mainThread, runCtx2.threadId));
    CHECK(atomic_load_explicit(&runCtx1.counter, memory_order_relaxed) == 5);
    CHECK(atomic_load_explicit(&runCtx2.counter, memory_order_relaxed) == 5);
    CHECK(atomic_load_explicit(&endCtx.endCalls, memory_order_relaxed) == 1 && endCtx.receivedKernel == k);
    CHECK(!Kernel_isRunning(k));
    Kernel_free(k);

    Kernel *k2 = Kernel();
    KfEndCtx stopCtx = { 0 };
    Kernel_addEndFunction(k2, kf_sampleEnd, &stopCtx);
    Kernel_stop(k2);
    CHECK(atomic_load_explicit(&stopCtx.endCalls, memory_order_relaxed) == 1);
    Kernel_stop(k2);
    CHECK(atomic_load_explicit(&stopCtx.endCalls, memory_order_relaxed) == 1);
    Kernel_free(k2);

    Kernel *k3 = Kernel();
    KfRunCtx manyRun[10] = { 0 };
    KfEndCtx manyEnd[10] = { 0 };
    for (i32 i = 0; i < 10; i++) {
        CHECK(Kernel_addRunFunction(k3, kf_sampleRun, &manyRun[i]));
        CHECK(Kernel_addEndFunction(k3, kf_sampleEnd, &manyEnd[i]));
    }
    CHECK(Kernel_getRunFunctionCount(k3) == 10 && Kernel_getEndFunctionCount(k3) == 10);
    Kernel_free(k3);
}

;;GETTER
static atomic_bool *Drain_workerDoneMut(DrainProbe *p)
{
    return &(*p).workerDone;
}
;;GETTER
static atomic_bool *Drain_hookSawDoneMut(DrainProbe *p)
{
    return &(*p).hookSawDone;
}
;;GETTER
static atomic_bool *Drain_hookSawDrainingMut(DrainProbe *p)
{
    return &(*p).hookSawDraining;
}
;;GETTER
static atomic_int *Drain_hookCallsMut(DrainProbe *p)
{
    return &(*p).hookCalls;
}

/** Worker that sleeps briefly before marking completion. */
static fn lc_slowWorker(any context) {
    DrainProbe *probe = (DrainProbe*) context;
    struct timespec ts = { 0, 80000000L };
    nanosleep(&ts, nullptr);
    atomic_store_explicit(Drain_workerDoneMut(probe), true, memory_order_release);
}

/** End hook asserting worker quiescence and DRAINING phase. */
static fn lc_observerHook(Kernel *kernel, any userdata) {
    DrainProbe *probe = (DrainProbe*) userdata;
    atomic_store_explicit(Drain_hookSawDoneMut(probe),
                          atomic_load_explicit(Drain_workerDoneMut(probe), memory_order_acquire),
                          memory_order_release);
    atomic_store_explicit(Drain_hookSawDrainingMut(probe),
                          Kernel_getPhase(kernel) == KERNEL_PHASE_DRAINING, memory_order_relaxed);
    atomic_fetch_add_explicit(Drain_hookCallsMut(probe), 1, memory_order_relaxed);
}

/** Verifies registration refusal while RUNNING. */
static fn lc_registrar(any context) {
    Kernel *kernel = (Kernel*) context;
    CHECK(Kernel_getPhase(kernel) == KERNEL_PHASE_RUNNING);
    CHECK(!Kernel_addRunFunction(kernel, lc_slowWorker, nullptr));
    struct timespec hold = { 0, 80000000L };
    nanosleep(&hold, nullptr);
}

/** Thread adapter invoking Kernel_run. */
static any lc_runner(any context)
{
    return (any) (pointer) Kernel_run((Kernel*) context);
}

/** Proves Kernel stop/drain/hook ordering and live-free refusal. */
static fn suite_kernel_lifecycle() {
    Kernel *kernel = Kernel();
    DrainProbe probe = { 0 };
    CHECK(kernel != nullptr);
    atomic_init(&probe.workerDone, false);
    atomic_init(&probe.hookSawDone, false);
    atomic_init(&probe.hookSawDraining, false);
    atomic_init(&probe.hookCalls, 0);
    CHECK(Kernel_addRunFunction(kernel, lc_slowWorker, &probe));
    CHECK(Kernel_addEndFunction(kernel, lc_observerHook, &probe));

    thread th;
    CHECK(pthread_create(&th, nullptr, lc_runner, kernel) == 0);
    struct timespec ts = { 0, 20000000L };
    nanosleep(&ts, nullptr);
    CHECK(Kernel_isRunning(kernel));
    Kernel_stop(kernel);
    CHECK(pthread_join(th, nullptr) == 0);
    CHECK(atomic_load_explicit(&probe.workerDone, memory_order_acquire));
    CHECK(atomic_load_explicit(&probe.hookCalls, memory_order_relaxed) == 1);
    CHECK(atomic_load_explicit(&probe.hookSawDone, memory_order_relaxed));
    CHECK(atomic_load_explicit(&probe.hookSawDraining, memory_order_relaxed));
    CHECK(Kernel_getPhase(kernel) == KERNEL_PHASE_READY && !Kernel_isRunning(kernel));
    CHECK(Kernel_addRunFunction(kernel, lc_slowWorker, &probe));
    CHECK(Kernel_free(kernel));

    Kernel *burnt = Kernel();
    DrainProbe burntProbe = { 0 };
    atomic_init(&burntProbe.hookCalls, 0);
    Kernel_addEndFunction(burnt, lc_observerHook, &burntProbe);
    Kernel_stop(burnt);
    Kernel_stop(burnt);
    CHECK(atomic_load_explicit(&burntProbe.hookCalls, memory_order_relaxed) == 1);
    CHECK(Kernel_free(burnt));

    Kernel *live = Kernel();
    Kernel_addRunFunction(live, lc_registrar, live);
    thread liveThread;
    CHECK(pthread_create(&liveThread, nullptr, lc_runner, live) == 0);
    nanosleep(&ts, nullptr);
    CHECK(!Kernel_free(live));
    Kernel_stop(live);
    CHECK(pthread_join(liveThread, nullptr) == 0);
    CHECK(Kernel_free(live));
}

;;GETTER
static atomic_int *StartGate_enteredMut(StartGate *gate)
{
    return &(*gate).entered;
}
;;GETTER
static atomic_int *StartGate_callsMut(StartGate *gate)
{
    return &(*gate).calls;
}
;;GETTER
static pthread_mutex_t *StartGate_lockMut(StartGate *gate)
{
    return &(*gate).lock;
}
;;GETTER
static pthread_cond_t *StartGate_readyMut(StartGate *gate)
{
    return &(*gate).ready;
}
;;GETTER
static bool StartGate_open(const StartGate *gate)
{
    return (*gate).open;
}
;;SETTER
static fn StartGate_setOpen(StartGate *gate, bool open)
{
    (*gate).open = open;
}
;;GETTER
static i64 Ts_sec(const struct timespec *ts)
{
    return (*ts).tv_sec;
}
;;GETTER
static i64 Ts_nsec(const struct timespec *ts)
{
    return (*ts).tv_nsec;
}

/** No-op reload callback fixture for Application. */
static fn app_hotReloadStub(Application *self, u32 loaded, any userdata) {
    (discard) self;
    (discard) loaded;
    (discard) userdata;
}

/** Gated Application start callback for deterministic STARTING phase observation. */
static fn app_gatedStart(Application *self, any userdata) {
    (discard) self;
    StartGate *gate = userdata;
    atomic_fetch_add_explicit(StartGate_enteredMut(gate), 1, memory_order_release);
    pthread_mutex_lock(StartGate_lockMut(gate));
    while (!StartGate_open(gate))
        pthread_cond_wait(StartGate_readyMut(gate), StartGate_lockMut(gate));
    pthread_mutex_unlock(StartGate_lockMut(gate));
    atomic_fetch_add_explicit(StartGate_callsMut(gate), 1, memory_order_release);
}

/** Releases all waiters parked on a StartGate. */
static fn app_gateOpen(StartGate *gate) {
    pthread_mutex_lock(StartGate_lockMut(gate));
    StartGate_setOpen(gate, true);
    pthread_cond_broadcast(StartGate_readyMut(gate));
    pthread_mutex_unlock(StartGate_lockMut(gate));
}

/** Computes elapsed milliseconds between two monotonic timestamps. */
static i64 app_elapsedMs(const struct timespec *from, const struct timespec *to) {
    return (Ts_sec(to) - Ts_sec(from)) * 1000L + (Ts_nsec(to) - Ts_nsec(from)) / 1000000L;
}

/** Bounded wait for an Application phase transition. */
static bool app_waitPhase(Application *app, AppPhase want, i64 timeoutMs) {
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        if (Application_getPhase(app) == want)
            return true;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (app_elapsedMs(&start, &now) > timeoutMs)
            return false;
        struct timespec nap = { 0, 1000000 };
        nanosleep(&nap, nullptr);
    }
}

/** Bounded wait for an atomic_int counter threshold. */
static bool app_waitCounter(atomic_int *counter, i32 want, i64 timeoutMs) {
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        if (atomic_load_explicit(counter, memory_order_acquire) >= want)
            return true;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (app_elapsedMs(&start, &now) > timeoutMs)
            return false;
        struct timespec nap = { 0, 1000000 };
        nanosleep(&nap, nullptr);
    }
}

/** Proves Application identity, phase machine, gated start, window registry, and completion predicate. */
static fn suite_application() {
    Application *a = Application();
    CHECK(a != nullptr && string_equals(Application_getName(a), "vex"));
    CHECK(Application_getWindowCount(a) == 0 && !Application_isRunning(a));

    Application *a3 = Application("app", "vexgraph", "1.2.3");
    CHECK(string_equals(Application_getName(a3), "app"));
    CHECK(string_equals(Application_getAuthor(a3), "vexgraph"));
    CHECK(string_equals(Application_getVersion(a3), "1.2.3"));

    utf8 big[200];
    memset(big, 'n', sizeof big - 1);
    big[sizeof big - 1] = '\0';
    Application_setName(a, big);
    CHECK(strlen(Application_getName(a)) == APP_MAX_NAME - 1);
    Application_setIconPath(a, "icons/app.png");
    CHECK(string_equals(Application_getIconPath(a), "icons/app.png"));

    CHECK(Application_begin(a) && Application_isRunning(a));
    Application_stop(a);
    Application_finish(a);
    CHECK(!Application_isRunning(a));
    CHECK(Application_isFinished(nullptr) && Application_isFinished(a));

    Application *p = Application();
    CHECK(Application_getPhase(p) == APP_PHASE_IDLE && !Application_isActive(p));
    for (i32 i = 0; i < (i32) APP_PHASE_COUNT; i++)
        CHECK(Application_getPhaseName((AppPhase) i)[0] != '\0');
    CHECK(Application_begin(p) && Application_getPhase(p) == APP_PHASE_RUNNING);
    CHECK(!Application_begin(p) && !Application_addStartEvent(p, app_gatedStart, nullptr));
    Application_close(p);
    CHECK(Application_getPhase(p) == APP_PHASE_CLOSING && !Application_addWindow(p, (Window*) (pointer) 0x77));
    Application_finish(p);
    CHECK(Application_getPhase(p) == APP_PHASE_STOPPED);
    CHECK(Application_begin(p) && Application_getPhase(p) == APP_PHASE_RUNNING);
    Application_finish(p);
    Application_free(p);

    StartGate gate = { 0 };
    pthread_mutex_init(&gate.lock, nullptr);
    pthread_cond_init(&gate.ready, nullptr);
    Application *w = Application();
    CHECK(Application_addStartEvent(w, app_gatedStart, &gate) && Application_begin(w));
    CHECK(Application_getPhase(w) == APP_PHASE_STARTING);
    app_gateOpen(&gate);
    CHECK(app_waitPhase(w, APP_PHASE_RUNNING, 2000));
    CHECK(atomic_load_explicit(&gate.calls, memory_order_acquire) == 1);
    Application_finish(w);
    Application_free(w);
    pthread_cond_destroy(&gate.ready);
    pthread_mutex_destroy(&gate.lock);

    StartGate gate2 = { 0 };
    pthread_mutex_init(&gate2.lock, nullptr);
    pthread_cond_init(&gate2.ready, nullptr);
    Application *c = Application();
    CHECK(Application_addStartEvent(c, app_gatedStart, &gate2) && Application_begin(c));
    CHECK(app_waitCounter(&gate2.entered, 1, 2000));
    Application_close(c);
    app_gateOpen(&gate2);
    CHECK(app_waitCounter(&gate2.calls, 1, 2000));
    struct timespec settle = { 0, 20000000 };
    nanosleep(&settle, nullptr);
    CHECK(Application_getPhase(c) == APP_PHASE_CLOSING);
    Application_finish(c);
    Application_free(c);
    pthread_cond_destroy(&gate2.ready);
    pthread_mutex_destroy(&gate2.lock);

    Window *w1 = (Window*) (pointer) 0x1;
    Window *w2 = (Window*) (pointer) 0x2;
    CHECK(Application_addWindow(a, w1) && !Application_addWindow(a, w1));
    CHECK(!Application_addWindow(a, nullptr) && !Application_addWindow(nullptr, w1));
    CHECK(Application_addWindow(a, w2) && Application_getWindowCount(a) == 2);
    CHECK(Application_getWindow(a, 0) == w1 && Application_getWindow(a, 1) == w2);
    CHECK(Application_getWindow(a, 999) == nullptr);
    Window *out[4];
    CHECK(Application_getWindows(a, out, 4) == 2 && Application_getWindows(a, out, 1) == 1);
    CHECK(Application_removeWindow(a, w1) && !Application_removeWindow(a, w1));
    Application_removeWindow(a, w2);
    for (pointer i = 0; i < APP_MAX_WINDOWS; i++)
        CHECK(Application_addWindow(a, (Window*) (0x100 + i)));
    CHECK(!Application_addWindow(a, (Window*) (pointer) 0x999));

    Application_setHot(a, (any) (pointer) 0x5);
    CHECK(Application_getHot(a) == (any) (pointer) 0x5);
    Application_setHot(a, nullptr);
    Application_onHotReload(a, app_hotReloadStub, a);
    Application_pollHot(a);
    CHECK(Application_getFps(a) == 0 && Application_getFrametimeUs(a) == 0);

    Application *app = Application();
    Window *rw = Window("app_test", 200, 150);
    if (rw) {
        CHECK(Application_addWindow(app, rw) && Application_begin(app));
        CHECK(!Application_isFinished(app));
        Window_setShouldClose(rw, true);
        CHECK(Application_isFinished(app));
        Application_finish(app);
        Application_removeWindow(app, rw);
        Window_destroy(rw);
    }
    Application_free(app);
    Application_free(a3);
    Application_free(a);
    Application_free(nullptr);
}

/** Proves zero-copy IOSurface present seam creation, swap, and null safety. */
static fn suite_present_surface() {
    Window *w = Window("present surface", 320, 240);
    CHECK(w != nullptr);
#if defined(VEX_PLATFORM_APPLE)
    any front = Window_createPresentSurface(w, 320, 240);
    any back = Window_createPresentSurface(w, 320, 240);
    CHECK(front != nullptr && back != nullptr);
    if (front && back) {
        Window_presentSurface(w, front);
        CHECK(Window_presentSurfaceContents(w) == front);
        Window_presentSurface(w, back);
        CHECK(Window_presentSurfaceContents(w) == back);
        Window_presentSurface(w, front);
        CHECK(Window_presentSurfaceContents(w) == front);
    }
    Window_destroyPresentSurface(w, front);
    Window_destroyPresentSurface(w, back);
#else
    CHECK(Window_createPresentSurface(w, 320, 240) == nullptr);
#endif
    Window_presentSurface(nullptr, nullptr);
    Window_presentSurface(w, nullptr);
    Window_destroyPresentSurface(nullptr, nullptr);
    CHECK(Window_presentSurfaceContents(nullptr) == nullptr);
    Window_destroy(w);
}

/** Proves TrafficLight bookkeeping, out-of-range refusal, and live window control. */
static fn suite_traffic_light() {
    TrafficLight *tl = TrafficLight(nullptr);
    CHECK(tl != nullptr);
    CHECK(TrafficLight_isButtonVisible(tl, TRAFFIC_LIGHT_CLOSE));
    CHECK(TrafficLight_isButtonVisible(tl, TRAFFIC_LIGHT_MINIATURIZE));
    CHECK(TrafficLight_isButtonVisible(tl, TRAFFIC_LIGHT_ZOOM));

    TrafficLight_setButtonVisible(tl, TRAFFIC_LIGHT_CLOSE, false);
    CHECK(!TrafficLight_isButtonVisible(tl, TRAFFIC_LIGHT_CLOSE));
    TrafficLight_setButtonVisible(tl, TRAFFIC_LIGHT_CLOSE, true);
    CHECK(TrafficLight_isButtonVisible(tl, TRAFFIC_LIGHT_CLOSE));
    CHECK(!TrafficLight_isButtonVisible(tl, (TrafficLightButton) 99));
    TrafficLight_setButtonVisible(tl, (TrafficLightButton) 99, true);

    f32 x = -1.0f;
    f32 y = -1.0f;
    TrafficLight_getHeaderPosition(tl, &x, &y);
    CHECK(x == 0.0f && y == 0.0f);
    TrafficLight_setHeaderPosition(tl, 10.0f, 20.0f);
    TrafficLight_resetBase(tl);
    TrafficLight_refresh(tl);
    TrafficLight_setFloating(tl, true);
    TrafficLight_destroy(tl);
    TrafficLight_destroy(nullptr);

    Window *w = Window("traffic", 300, 200);
    if (w) {
        TrafficLight *live = TrafficLight(Window_nativeHandle(w));
        CHECK(live != nullptr);
        TrafficLight_refresh(live);
        TrafficLight_setButtonVisible(live, TRAFFIC_LIGHT_ZOOM, false);
        CHECK(!TrafficLight_isButtonVisible(live, TRAFFIC_LIGHT_ZOOM));
        TrafficLight_setButtonVisible(live, TRAFFIC_LIGHT_ZOOM, true);
        CHECK(TrafficLight_isButtonVisible(live, TRAFFIC_LIGHT_ZOOM));
        TrafficLight_setHeaderPosition(live, 12.0f, 8.0f);
        f32 hx = -1.0f;
        f32 hy = -1.0f;
        TrafficLight_getHeaderPosition(live, &hx, &hy);
        CHECK(hx >= 0.0f && hy >= 0.0f);
        TrafficLight_setFloating(live, true);
        TrafficLight_setFloating(live, false);
        TrafficLight_destroy(live);
        Window_destroy(w);
    }
}

;;GETTER
static Window *WinPump_window(const WinPumpCtx *ctx)
{
    return (*ctx).window;
}
;;GETTER
static Loop *WinPump_loopMut(WinPumpCtx *ctx)
{
    return &(*ctx).loop;
}
;;GETTER
static i32 WinPump_frames(const WinPumpCtx *ctx)
{
    return (*ctx).frames;
}
;;SETTER
static fn WinPump_setFrames(WinPumpCtx *ctx, i32 v)
{
    (*ctx).frames = v;
}
;;GETTER
static i32 WinPump_frameBudget(const WinPumpCtx *ctx)
{
    return (*ctx).frameBudget;
}

static const KeyHandler g_winKeyStub = { 0 };
static const MouseHandler g_winMouseStub = { 0 };
static const TouchHandler g_winTouchStub = { 0 };

/** Bounded event-pump tick stopping cleanly at frameBudget. */
static fn win_pumpTick(any userdata) {
    WinPumpCtx *ctx = userdata;
    Window_pollEvents();
    Window *w = WinPump_window(ctx);
    Window_dispatchEvents(w);
    if (Window_shouldClose(w)) {
        Loop_stop(WinPump_loopMut(ctx));
        return;
    }
    i32 frames = WinPump_frames(ctx) + 1;
    WinPump_setFrames(ctx, frames);
    if (WinPump_frameBudget(ctx) > 0 && frames >= WinPump_frameBudget(ctx))
        Loop_stop(WinPump_loopMut(ctx));
}

/** Proves synchronous Window chrome/policy contract, native pixel sizing, and bounded event pump. */
static fn suite_window() {
    Window *w = Window("vex", 640, 480);
    CHECK(w != nullptr);
    if (!w)
        return;
    Window_show(w);

    u64 gen0 = Window_renderGeneration(w);
    CHECK(Window_getPresentMode(w) == WINDOW_PRESENT_FIFO);
    Window_setPresentMode(w, WINDOW_PRESENT_IMMEDIATE);
    CHECK(Window_getPresentMode(w) == WINDOW_PRESENT_IMMEDIATE && Window_renderGeneration(w) > gen0);

    u64 gen1 = Window_renderGeneration(w);
    CHECK(!Window_isTransparent(w));
    Window_setTransparent(w, true);
    CHECK(Window_isTransparent(w) && Window_renderGeneration(w) > gen1);

    CHECK(Window_isEnabled(w));
    Window_setEnabled(w, false);
    CHECK(!Window_isEnabled(w));
    Window_setEnabled(w, true);
    CHECK(Window_isEnabled(w));

    Window_setResizable(w, false);
    CHECK(!Window_isResizable(w));
    Window_setResizable(w, true);
    CHECK(Window_isResizable(w));

    Window_setClosable(w, false);
    CHECK(!Window_isClosable(w));
    Window_setClosable(w, true);
    CHECK(Window_isClosable(w));

    Window_setMiniaturizable(w, false);
    CHECK(!Window_isMiniaturizable(w));
    Window_setMiniaturizable(w, true);
    CHECK(Window_isMiniaturizable(w));

    Window_setUndecorated(w, WINDOW_UNDECORATED_NAKED);
    CHECK(Window_isNaked(w));
    Window_setUndecorated(w, WINDOW_UNDECORATED_BORDERLESS);
    CHECK(Window_isBorderless(w));
    Window_setUndecorated(w, WINDOW_DECORATED);
    CHECK(Window_isDecorated(w));

    CHECK(!Window_isViewportFlushToTop(w));
    Window_setViewportFlushToTop(w, true);
    CHECK(Window_isViewportFlushToTop(w) && Window_isDecorated(w));
    Window_setViewportFlushToTop(w, false);
    CHECK(!Window_isViewportFlushToTop(w));

    Window_setUndecorated(w, WINDOW_UNDECORATED_NAKED);
    Window_setViewportFlushToTop(w, true);
    CHECK(!Window_isViewportFlushToTop(w));
    Window_setUndecorated(w, WINDOW_UNDECORATED_BORDERLESS);
    Window_setViewportFlushToTop(w, true);
    CHECK(!Window_isViewportFlushToTop(w));
    Window_setUndecorated(w, WINDOW_DECORATED);

    Window_setShouldClose(w, true);
    CHECK(Window_shouldClose(w));
    Window_setShouldClose(w, false);
    CHECK(!Window_shouldClose(w));

    Window_addKeyAdapter(w, &g_winKeyStub);
    CHECK(Window_removeKeyAdapter(w, &g_winKeyStub));
    Window_addMouseAdapter(w, &g_winMouseStub);
    CHECK(Window_removeMouseAdapter(w, &g_winMouseStub));
    Window_addTouchAdapter(w, &g_winTouchStub);
    CHECK(Window_removeTouchAdapter(w, &g_winTouchStub));

    CHECK(Window_id(w) != 0u && Window_getLifecycle(w) != nullptr);
    CHECK(Window_width(w) > 0 && Window_height(w) > 0 && Window_getScale(w) >= 1.0f);

    Window_setSize(w, 800, 600);
    CHECK(Window_width(w) == 800 && Window_height(w) == 600);
    CHECK(Window_widthPoints(w) > 0.0f && Window_heightPoints(w) > 0.0f);
    Window_revalidate(w);
    CHECK(Window_width(w) == 800 && Window_height(w) == 600);
    Window_setSize(w, 900, 700);
    CHECK(Window_width(w) == 900 && Window_height(w) == 700);
    Window_setCursorType(w, WINDOW_CURSOR_POINTING_HAND);
    CHECK(Window_getCursorType(w) == WINDOW_CURSOR_POINTING_HAND);

    WinPumpCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.window = w;
    ctx.frameBudget = 15;
    ctx.loop = (Loop) { .tick = win_pumpTick, .userdata = &ctx, .frame_ms = 16, .running = false };
    Loop_run(&ctx.loop);
    CHECK(ctx.frames == 15);
    Window_destroy(w);
}

/** Runs all automated vex/lifetime suites or a single named suite passed as argv[1]. */
i32 main(i32 argc, utf8 **argv) {
    const SuiteEntry suites[] = {
        { "platform", suite_platform },
        { "clock", suite_clock },
        { "capability", suite_capability },
        { "permission", suite_permission },
        { "lifetime", suite_lifetime },
        { "window_event", suite_window_event },
        { "bridge_seam", suite_bridge_seam },
        { "console", suite_console },
        { "commandline", suite_commandline },
        { "process", suite_process },
        { "process_spawn", suite_process_spawn },
        { "kernel_function", suite_kernel_function },
        { "kernel_lifecycle", suite_kernel_lifecycle },
        { "application", suite_application },
        { "present_surface", suite_present_surface },
        { "traffic_light", suite_traffic_light },
        { "window", suite_window },
    };
    string filter = argc > 1 ? argv[1] : nullptr;
    bytes ran = 0;

    for (bytes i = 0; i < sizeof(suites) / sizeof(suites[0]); i++) {
        if (filter && !string_equals(filter, suites[i].name))
            continue;
        suites[i].run();
        ran++;
    }

    if (filter && ran == 0) {
        fprint(stderr, "lifetime_test: unknown suite '", filter, "'");
        return 2;
    }
    if (g_failures != 0) {
        fprint(stderr, "lifetime_test: ", g_failures, " FAILURES");
        return 1;
    }
    puts("lifetime_test: PASS");
    return 0;
}

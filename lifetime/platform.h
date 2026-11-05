#ifndef PLATFORM_PLATFORM_H
#define PLATFORM_PLATFORM_H

// platform/platform.h — the one spelling of "what am I building for" (the One
// File, All Platforms Law, one-spelling clause).
//
// The six canonical targets are named exactly, one macro each, and nothing else
// names a platform:
//
//   VEX_PLATFORM_APPLE_SILICON      darwin-arm64   (macOS 14+, -mcpu=apple-m1)
//   VEX_PLATFORM_APPLE_X86_64       darwin-x86_64  (macOS 13+)
//   VEX_PLATFORM_LINUX_ARM64        linux-arm64    (glibc 2.35+)
//   VEX_PLATFORM_LINUX_X86_64       linux-x86_64   (glibc 2.35+)
//   VEX_PLATFORM_WINDOWS_ARM64      windows-arm64  (Windows 11+)
//   VEX_PLATFORM_WINDOWS_X86_64     windows-x86_64 (Windows 10+)
//
// The coarse axes are DERIVED from the exact target: VEX_PLATFORM_APPLE,
// _WINDOWS, _LINUX, then VEX_ARCH_ARM/_X86 for the family and VEX_ARCH_ARM64/
// _X86_64 for the width. Ask the exact question when the answer differs per
// target; ask a coarse one when it does not. Never the reverse.
//
// Raw __APPLE__ / _WIN32 / __linux__ are resolved HERE and nowhere else. Every
// other file tests VEX_*, so a target is added in one place.
//
// This header answers a COMPILE-TIME question: which implementation exists.
// What a machine can DO is the runtime question, owned by capability/ (the
// Capability Gating Law). A compile-time branch chooses the body; a capability
// bit chooses the feature inside it.

#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64))
#define VEX_PLATFORM_APPLE_SILICON 1
#define VEX_TARGET_NAME "darwin-arm64"
#elif defined(__APPLE__) && (defined(__x86_64__) || defined(_M_X64))
#define VEX_PLATFORM_APPLE_X86_64 1
#define VEX_TARGET_NAME "darwin-x86_64"
#elif defined(__linux__) && (defined(__aarch64__) || defined(_M_ARM64))
#define VEX_PLATFORM_LINUX_ARM64 1
#define VEX_TARGET_NAME "linux-arm64"
#elif defined(__linux__) && (defined(__x86_64__) || defined(_M_X64))
#define VEX_PLATFORM_LINUX_X86_64 1
#define VEX_TARGET_NAME "linux-x86_64"
#elif defined(_WIN32) && (defined(__aarch64__) || defined(_M_ARM64))
#define VEX_PLATFORM_WINDOWS_ARM64 1
#define VEX_TARGET_NAME "windows-arm64"
#elif defined(_WIN32) && (defined(__x86_64__) || defined(_M_X64))
#define VEX_PLATFORM_WINDOWS_X86_64 1
#define VEX_TARGET_NAME "windows-x86_64"
#elif defined(__i386__) || defined(_M_IX86) || defined(__arm__) || defined(_M_ARM)
#error "platform.h: 32-bit x86/arm is outside the six-target floor; every supported target is 64-bit"
#elif (defined(__APPLE__) + defined(_WIN32) + defined(__linux__)) > 1
#error "platform.h: more than one platform family is defined; the floor ships exactly one"
#else
#error "platform.h: no branch for this target; the floor is darwin/linux/windows on arm64/x86_64"
#endif

// Exactly one exact target, by construction. The check is cheap and it catches a
// pasted branch that defines a second one.
#if (defined(VEX_PLATFORM_APPLE_SILICON) + defined(VEX_PLATFORM_APPLE_X86_64) + \
     defined(VEX_PLATFORM_LINUX_ARM64) + defined(VEX_PLATFORM_LINUX_X86_64) +   \
     defined(VEX_PLATFORM_WINDOWS_ARM64) + defined(VEX_PLATFORM_WINDOWS_X86_64)) != 1
#error "platform.h: exactly one of the six VEX_PLATFORM_* targets must be defined"
#endif

// ── derived platform ─────────────────────────────────────────────────────────
#if defined(VEX_PLATFORM_APPLE_SILICON) || defined(VEX_PLATFORM_APPLE_X86_64)
#define VEX_PLATFORM_APPLE 1
#define VEX_PLATFORM_NAME "darwin"
#elif defined(VEX_PLATFORM_LINUX_ARM64) || defined(VEX_PLATFORM_LINUX_X86_64)
#define VEX_PLATFORM_LINUX 1
#define VEX_PLATFORM_NAME "linux"
#else
#define VEX_PLATFORM_WINDOWS 1
#define VEX_PLATFORM_NAME "windows"
#endif

// ── derived architecture ────────────────────────────────────────────────────
// Family answers "arm or x86"; width answers "how wide". Both are derived, so a
// file never has to re-derive them from the exact target.
#if defined(VEX_PLATFORM_APPLE_SILICON) || defined(VEX_PLATFORM_LINUX_ARM64) || \
    defined(VEX_PLATFORM_WINDOWS_ARM64)
#define VEX_ARCH_ARM 1
#define VEX_ARCH_ARM64 1
#define VEX_ARCH_FAMILY "arm"
#define VEX_ARCH_NAME "arm64"
#else
#define VEX_ARCH_X86 1
#define VEX_ARCH_X86_64 1
#define VEX_ARCH_FAMILY "x86"
#define VEX_ARCH_NAME "x86_64"
#endif

// The floor is 64-bit on every target. A 32-bit build would change pointer and
// size_t widths under the same names, so it is refused above and asserted here.
#define VEX_ARCH_BITS 64
_Static_assert(sizeof(void*) * 8 == VEX_ARCH_BITS, "platform.h: the platform floor is 64-bit only");

#endif

// lifetime/test/platform/platform_test.c — the platform branch _test.
//
// Proves the One File, All Platforms Law's one-spelling clause:
//   - exactly one of the six exact VEX_PLATFORM_* targets resolves per build;
//   - the coarse axes (platform, arch family, arch width) are derived from it
//     and agree with it, so a file asking a coarse question gets the same
//     answer as one asking the exact question;
//   - every raw platform macro resolves to the matching normalized branch, so a
//     file testing __APPLE__ cannot disagree with one testing VEX_PLATFORM_*;
//   - a 32-bit x86/arm or unknown target fails to build instead of taking a
//     silent wrong branch.
//
// Almost all of this proof is compile-time: the failing assertion is a build
// that does not complete. The runtime half checks the identity the build claims,
// so a header edited to lie cannot pass by compiling.
//
// This file tests the raw macros on purpose — it is the seam that proves their
// resolution. No other file may.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lifetime/platform.h"

// Exactly one exact target. Duplicated deliberately: the test must fail if the
// header's own guard is removed, not trust the guard it is testing.
#if (defined(VEX_PLATFORM_APPLE_SILICON) + defined(VEX_PLATFORM_APPLE_X86_64) + \
     defined(VEX_PLATFORM_LINUX_ARM64) + defined(VEX_PLATFORM_LINUX_X86_64) +   \
     defined(VEX_PLATFORM_WINDOWS_ARM64) + defined(VEX_PLATFORM_WINDOWS_X86_64)) != 1
#error "platform_test: exactly one of the six VEX_PLATFORM_* targets must be defined"
#endif

// Exactly one coarse axis in each dimension.
#if (defined(VEX_PLATFORM_APPLE) + defined(VEX_PLATFORM_WINDOWS) + defined(VEX_PLATFORM_LINUX)) != 1
#error "platform_test: exactly one VEX_PLATFORM_{APPLE,WINDOWS,LINUX} must be defined"
#endif

#if (defined(VEX_ARCH_ARM) + defined(VEX_ARCH_X86)) != 1
#error "platform_test: exactly one VEX_ARCH_{ARM,X86} family must be defined"
#endif

#if (defined(VEX_ARCH_ARM64) + defined(VEX_ARCH_X86_64)) != 1
#error "platform_test: exactly one VEX_ARCH_{ARM64,X86_64} width must be defined"
#endif

// The exact target must agree with the raw macros it was resolved from.
#if defined(__APPLE__) && !defined(VEX_PLATFORM_APPLE)
#error "platform_test: __APPLE__ did not resolve to VEX_PLATFORM_APPLE"
#endif

#if defined(_WIN32) && !defined(VEX_PLATFORM_WINDOWS)
#error "platform_test: _WIN32 did not resolve to VEX_PLATFORM_WINDOWS"
#endif

#if defined(__linux__) && !defined(VEX_PLATFORM_LINUX)
#error "platform_test: __linux__ did not resolve to VEX_PLATFORM_LINUX"
#endif

#if (defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)) && !defined(VEX_ARCH_ARM64)
#error "platform_test: aarch64 did not resolve to VEX_ARCH_ARM64"
#endif

#if (defined(__x86_64__) || defined(_M_X64)) && !defined(VEX_ARCH_X86_64)
#error "platform_test: x86_64 did not resolve to VEX_ARCH_X86_64"
#endif

static int g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);          \
            g_failures++;                                                  \
        }                                                                  \
    } while (0)

int main(void)
{
    // Every axis and both names must resolve to something non-empty.
    CHECK(VEX_PLATFORM_NAME[0] != '\0');
    CHECK(VEX_ARCH_FAMILY[0] != '\0');
    CHECK(VEX_ARCH_NAME[0] != '\0');
    CHECK(VEX_TARGET_NAME[0] != '\0');

    // The target name is the platform and the width, joined: the floor's own
    // vocabulary ("darwin-arm64"), not a second one.
    char expected[32];
    snprintf(expected, sizeof expected, "%s-%s", VEX_PLATFORM_NAME, VEX_ARCH_NAME);
    CHECK(!strcmp(expected, VEX_TARGET_NAME));

    // Family and width are one answer, not two.
#if defined(VEX_ARCH_ARM)
    CHECK(!strcmp(VEX_ARCH_FAMILY, "arm"));
    CHECK(!strcmp(VEX_ARCH_NAME, "arm64"));
#else
    CHECK(!strcmp(VEX_ARCH_FAMILY, "x86"));
    CHECK(!strcmp(VEX_ARCH_NAME, "x86_64"));
#endif

   // Each exact target must carry its declared platform, family, width and name.
#if defined(VEX_PLATFORM_APPLE_SILICON)
   CHECK(!strcmp(VEX_PLATFORM_NAME, "darwin"));
   CHECK(!strcmp(VEX_ARCH_FAMILY, "arm"));
   CHECK(!strcmp(VEX_ARCH_NAME, "arm64"));
   CHECK(!strcmp(VEX_TARGET_NAME, "darwin-arm64"));
#elif defined(VEX_PLATFORM_APPLE_X86_64)
   CHECK(!strcmp(VEX_PLATFORM_NAME, "darwin"));
   CHECK(!strcmp(VEX_ARCH_FAMILY, "x86"));
   CHECK(!strcmp(VEX_ARCH_NAME, "x86_64"));
   CHECK(!strcmp(VEX_TARGET_NAME, "darwin-x86_64"));
#elif defined(VEX_PLATFORM_LINUX_ARM64)
   CHECK(!strcmp(VEX_PLATFORM_NAME, "linux"));
   CHECK(!strcmp(VEX_ARCH_FAMILY, "arm"));
   CHECK(!strcmp(VEX_ARCH_NAME, "arm64"));
   CHECK(!strcmp(VEX_TARGET_NAME, "linux-arm64"));
#elif defined(VEX_PLATFORM_LINUX_X86_64)
   CHECK(!strcmp(VEX_PLATFORM_NAME, "linux"));
   CHECK(!strcmp(VEX_ARCH_FAMILY, "x86"));
   CHECK(!strcmp(VEX_ARCH_NAME, "x86_64"));
   CHECK(!strcmp(VEX_TARGET_NAME, "linux-x86_64"));
#elif defined(VEX_PLATFORM_WINDOWS_ARM64)
   CHECK(!strcmp(VEX_PLATFORM_NAME, "windows"));
   CHECK(!strcmp(VEX_ARCH_FAMILY, "arm"));
   CHECK(!strcmp(VEX_ARCH_NAME, "arm64"));
   CHECK(!strcmp(VEX_TARGET_NAME, "windows-arm64"));
#elif defined(VEX_PLATFORM_WINDOWS_X86_64)
   CHECK(!strcmp(VEX_PLATFORM_NAME, "windows"));
   CHECK(!strcmp(VEX_ARCH_FAMILY, "x86"));
   CHECK(!strcmp(VEX_ARCH_NAME, "x86_64"));
   CHECK(!strcmp(VEX_TARGET_NAME, "windows-x86_64"));
#endif

    // The floor is 64-bit; the header's _Static_assert must agree with the build.
    CHECK(sizeof(void*) * 8 == VEX_ARCH_BITS);

    if (g_failures) {
        printf("platform_test: %d failure(s)\n", g_failures);
        return 1;
    }

    printf("platform_test: %s\n", VEX_TARGET_NAME);
    return 0;
}

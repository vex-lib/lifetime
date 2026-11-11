// hotcwap/test/kernel/console_test.c — the Console class _test.
//
// Proves the session state machine against an injected ConsoleIo seam (no real
// shell, no threads, no sockets):
//   - constructor defaults (shell "/bin/sh", empty workDir) + the 0/1-arg
//     dispatch macro;
//   - bounded string clamps for shell/workDir (the Truncation clause);
//   - run refuses without a spawn seam, then starts once one is injected;
//   - writeInput / poll route through the seam; joined() ends the session;
//   - cancel fires the seam's cancel; free-while-running cancels first;
//   - every entry point is null-safe.
//
// Headless, deterministic.

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "core/console.h"

static int g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);          \
            g_failures++;                                                  \
        }                                                                  \
    } while (0)

typedef struct {
    int spawns;
    int feeds;
    int reaps;
    int cancels;
    bool joined;
    char lastFeed[64];
    bytes lastFeedLen;
} fake_io_t;

/** Records a successful fake launch using the configured shell and directory. */
static bool fakeSpawn(void *ctx, const char *shell, const char *workDir) {
    (void) shell; (void) workDir;
    fake_io_t *f = ctx;
    (*f).spawns++;
    return true;
}

/** Records input bytes delivered to the console's injected IO seam. */
static bool fakeFeed(void *ctx, const char *Bytes, bytes len) {
    fake_io_t *f = ctx;
    (*f).feeds++;
    bytes n = len < sizeof((*f).lastFeed) - 1 ? len : sizeof((*f).lastFeed) - 1;
    memcpy((*f).lastFeed, Bytes, n);
    (*f).lastFeed[n] = '\0';
    (*f).lastFeedLen = len;
    return true;
}

/** Supplies deterministic child output and its bounded byte count. */
static bool fakeReap(void *ctx, char *out, bytes outCap, bytes *outLen) {
    fake_io_t *f = ctx;
    (*f).reaps++;
    const char *msg = "out";
    bytes n = 3 < outCap ? 3 : outCap;
    if (out && outCap > 0)
        memcpy(out, msg, n);
    if (outLen)
        (*outLen) = n;
    return true;
}

/** Counts cancellation requests made through the fake IO seam. */
static void fakeCancel(void *ctx) {
    fake_io_t *f = ctx;
    (*f).cancels++;
}

/** Returns the fixture's child-joined state for Console polling. */
static bool fakeJoined(const void *ctx) {
    const fake_io_t *f = ctx;
    return (*f).joined;
}

int main(void) {
    // Constructor defaults + the dispatch macro.
    Console *c = Console_0();
    CHECK(c != nullptr);
    CHECK(strcmp(Console_getShell(c), "/bin/sh") == 0);
    CHECK(Console_getWorkDir(c)[0] == '\0');
    CHECK(Console_getIo(c) == nullptr);
    CHECK(!Console_isRunning(c));

    // No seam -> run refuses; input refused while not running.
    CHECK(!Console_run(c));
    CHECK(!Console_writeInput(c, "x", 1));
    CHECK(Console_getExitStatus(c) == 0);

    // Bounded clamps (the buffer must exceed each cap to exercise a cut).
    char big[2000];
    memset(big, 'a', sizeof big - 1);
    big[sizeof big - 1] = '\0';
    Console_setShell(c, big);
    CHECK(strlen(Console_getShell(c)) == CONSOLE_MAX_SHELL - 1);
    Console_setWorkDir(c, big);
    CHECK(strlen(Console_getWorkDir(c)) == CONSOLE_MAX_WORKDIR - 1);

    // Inject the fake seam.
    fake_io_t f = { .spawns = 0, .feeds = 0, .reaps = 0, .cancels = 0, .joined = false };
    ConsoleIo io = { .ctx = &f, .spawn = fakeSpawn, .feed = fakeFeed,
                     .reap = fakeReap, .cancel = fakeCancel, .joined = fakeJoined };
    Console_setIo(c, &io, &f);
    const ConsoleIo *got = Console_getIo(c);
    CHECK(got != nullptr);
    CHECK((*got).spawn == fakeSpawn);

    // Run: spawn fires once; a second run while running is refused.
    CHECK(Console_run(c));
    CHECK(f.spawns == 1);
    CHECK(Console_isRunning(c));
    CHECK(!Console_run(c));

    // Input routes to the seam.
    CHECK(Console_writeInput(c, "hello\n", 6));
    CHECK(f.feeds == 1);
    CHECK(strcmp(f.lastFeed, "hello\n") == 0);
    CHECK(f.lastFeedLen == 6);

    // Poll drains through the seam.
    char out[16];
    bytes outLen = 999;
    CHECK(Console_poll(c, out, sizeof out, &outLen));
    CHECK(f.reaps == 1);
    CHECK(outLen == 3);

    // joined() ends the session on the next poll.
    f.joined = true;
    CHECK(Console_poll(c, out, sizeof out, &outLen));
    CHECK(!Console_isRunning(c));

    // Cancel fires the seam.
    Console_cancel(c);
    CHECK(f.cancels == 1);

    Console_free(c);
    Console_free(nullptr); // null-safe

    // free-while-running cancels first (the Teardown Order Law).
    Console *c4 = Console_1("sh");
    Console_setIo(c4, &io, &f);
    int before = f.cancels;
    CHECK(Console_run(c4));
    Console_free(c4);
    CHECK(f.cancels == before + 1);

    // Dispatch macro, 1-arg form.
    Console *c2 = Console("bash");
    CHECK(strcmp(Console_getShell(c2), "bash") == 0);
    Console_free(c2);

    if (g_failures == 0) {
        printf("console_test: all assertions held\n");
        return 0;
    }
    printf("console_test: %d FAILURES\n", g_failures);
    return 1;
}

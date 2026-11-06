/* hotcwap/test/kernel/commandline_test.c
 *
 * Owner test for the R1 terminal-application host (kernel/commandline.{c,h}).
 * A hand-written fake CommandLineIo backend drives the device seam headlessly,
 * so no real terminal, ncurses screen or TTY is acquired. Proves: arity
 * constructors, open/close, the ALWAYS-honored resize surface (grid growth +
 * one repaint), counted resize refusal, swappable app seam without a device
 * reset, rejection preservation, bounded poll delivery, mouse toggle,
 * projections, null safety, and Kernel registration as the fourth kind.
 */
#include <stdio.h>
#include <string.h>

#include "core/commandline.h"
#include "core/kernel.h"
#include "util/test_support.h"

typedef struct Fake {
    int opens, closes, presents, mouse, polls;
    size_t w, h;
    bool pending;
    CommandLineEvent next;
} Fake;

typedef struct FakeApp {
    int draws, inputs;
    bool stopOnInput;
} FakeApp;

static bool fOpen(void *ctx) { (*((Fake*) ctx)).opens++; return true; }
static void fClose(void *ctx) { (*((Fake*) ctx)).closes++; }
static bool fSize(void *ctx, size_t *w, size_t *h) {
    Fake *f = (Fake*) ctx;
    *w = (*f).w; *h = (*f).h;
    return (*f).w > 0 && (*f).h > 0;
}
static bool fPresent(void *ctx, const char *cells, size_t w, size_t h) {
    Fake *f = (Fake*) ctx;
    (void) cells; (void) w; (void) h;
    (*f).presents++;
    return true;
}
static bool fPoll(void *ctx, int timeoutMs, CommandLineEvent *out) {
    Fake *f = (Fake*) ctx;
    (void) timeoutMs;
    (*f).polls++;
    if (!(*f).pending)
        return false;
    (*f).pending = false;
    *out = (*f).next;
    return true;
}
static bool fMouse(void *ctx, bool enabled) { (*((Fake*) ctx)).mouse = enabled ? 1 : 0; return true; }

static bool aDraw(void *ctx, CommandLine *line) {
    FakeApp *a = (FakeApp*) ctx;
    (*a).draws++;
    char *cells = CommandLine_getCells(line);
    if (cells != nullptr)
        memset(cells, '.', CommandLine_getWidth(line) * CommandLine_getHeight(line));
    return true;
}
static bool aInput(void *ctx, CommandLine *line, const CommandLineEvent *event) {
    FakeApp *a = (FakeApp*) ctx;
    (void) event;
    (*a).inputs++;
    if ((*a).stopOnInput) {
        CommandLine_stop(line);
        return false;
    }
    return true;
}

static CommandLineIo fakeIo(Fake *f) {
    CommandLineIo io;
    memset(&io, 0, sizeof(io));
    io.ctx = f;
    io.open = fOpen;
    io.close = fClose;
    io.size = fSize;
    io.present = fPresent;
    io.poll = fPoll;
    io.setMouse = fMouse;
    return io;
}

int main(void) {
    int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } } while (0)

    // Null safety / projections.
    CHECK(CommandLine_getWidth(nullptr) == 0, "null width");
    CHECK(CommandLine_getCellsConst(nullptr) == nullptr, "null cells");
    CHECK(!CommandLine_isOpen(nullptr), "null isOpen");
    char buf[256];
    bool trunc = true;
    CHECK(CommandLine_toString(nullptr, buf, sizeof(buf), &trunc) && !trunc &&
          strcmp(buf, "nullptr") == 0, "null toString");
    CHECK(CommandLine_toStringStruct(nullptr, nullptr, 0, &trunc) == false && trunc,
          "null destination truncation flag");

    // Arity constructors.
    CommandLine *def = CommandLine();
    CommandLine *named = CommandLine("Test Terminal");
    CHECK(def != nullptr && named != nullptr, "constructors");
    CHECK(strcmp(CommandLine_getTitle(def), "vex") == 0, "default title");
    CHECK(strcmp(CommandLine_getTitle(named), "Test Terminal") == 0, "named title");

    // Open through the fake backend.
    Fake fake;
    memset(&fake, 0, sizeof(fake));
    fake.w = 80; fake.h = 24;
    CommandLineIo io = fakeIo(&fake);
    CHECK(CommandLine_setIo(named, &io), "setIo");
    CHECK(CommandLine_open(named), "open");
    CHECK(fake.opens == 1 && CommandLine_isOpen(named), "device acquired");
    CHECK(CommandLine_getWidth(named) == 80 && CommandLine_getHeight(named) == 24, "geometry");
    CHECK(CommandLine_getCapacity(named) == 80 * 24, "grid capacity");
    CHECK(CommandLine_getCells(named) != nullptr, "grid exists");

    // Reopen is refused (no device reset).
    CHECK(!CommandLine_open(named), "second open refused");

    // Present / clear.
    CHECK(CommandLine_clear(named), "clear");
    const char *cells = CommandLine_getCellsConst(named);
    CHECK(cells != nullptr && cells[0] == ' ', "cleared to blanks");
    CHECK(CommandLine_present(named) && fake.presents == 1, "present");

    // App seam: swap WITHOUT a device reset; generation advances.
    FakeApp app; memset(&app, 0, sizeof(app));
    CommandLineApp seam; memset(&seam, 0, sizeof(seam));
    seam.ctx = &app; seam.draw = aDraw; seam.input = aInput;
    CHECK(CommandLine_setApp(named, &seam), "setApp");
    CHECK(CommandLine_getGeneration(named) == 1, "generation after setApp");
    CHECK(fake.opens == 1, "setApp did not re-acquire the device");

    // Rejection preserves the previous app + generation.
    CommandLineApp bad; memset(&bad, 0, sizeof(bad)); bad.ctx = &app; // draw == NULL
    CHECK(!CommandLine_setApp(named, &bad), "reject setApp without draw");
    CHECK(CommandLine_getGeneration(named) == 1 && CommandLine_hasApp(named), "app preserved");

    // THE resize surface: geometry change grows the grid and repaints once.
    fake.w = 120; fake.h = 40;
    int drawsBefore = app.draws, presentsBefore = fake.presents;
    CHECK(CommandLine_revalidate(named), "revalidate grow");
    CHECK(CommandLine_getWidth(named) == 120 && CommandLine_getHeight(named) == 40, "geometry grew");
    CHECK(CommandLine_getCapacity(named) >= 120 * 40, "grid grew");
    CHECK(app.draws == drawsBefore + 1 && fake.presents == presentsBefore + 1, "resize repaints in one step");

    // Same-size revalidate just presents.
    drawsBefore = app.draws; presentsBefore = fake.presents;
    CHECK(CommandLine_revalidate(named), "revalidate same size");
    CHECK(app.draws == drawsBefore && fake.presents == presentsBefore + 1, "same-size present only");

    // Counted resize refusal: a degenerate device size is rejected loudly once.
    fake.w = 0; fake.h = 0;
    CHECK(!CommandLine_revalidate(named), "revalidate rejects 0x0");
    CHECK(CommandLine_exhaustionCount(named) == 1, "first refusal counted");
    CHECK(!CommandLine_revalidate(named), "second refusal");
    CHECK(CommandLine_exhaustionCount(named) == 2, "second refusal counted");
    fake.w = 100; fake.h = 30;

    // Input poll delivery.
    CommandLineEvent ev;
    CHECK(!CommandLine_poll(named, 0, &ev), "no pending event");
    fake.pending = true;
    fake.next = (CommandLineEvent) { .kind = COMMANDLINE_EVENT_KEY, .key = COMMANDLINE_KEY_ENTER };
    CHECK(CommandLine_poll(named, 10, &ev) && ev.kind == COMMANDLINE_EVENT_KEY &&
          ev.key == COMMANDLINE_KEY_ENTER, "event delivered");

    // Mouse toggle.
    CHECK(CommandLine_setMouseReporting(named, true) && fake.mouse == 1, "mouse on");
    CHECK(CommandLine_setMouseReporting(named, false) && fake.mouse == 0, "mouse off");

    // tick: draw + present, no event pending.
    drawsBefore = app.draws; presentsBefore = fake.presents;
    CHECK(CommandLine_tick(named), "tick");
    CHECK(app.draws == drawsBefore + 1 && fake.presents == presentsBefore + 1, "tick draws + presents");
    CHECK(!CommandLine_isCancelled(named), "not cancelled");

    // run: one stopping input ends the loop with exit code 0.
    app.stopOnInput = true;
    fake.pending = true;
    fake.next = (CommandLineEvent) { .kind = COMMANDLINE_EVENT_KEY, .key = 3 };
    int rc = CommandLine_run(named);
    CHECK(rc == 0 && CommandLine_isCancelled(named), "run exits on app stop");

    // detach + close restore the device.
    CHECK(CommandLine_detachApp(named) && !CommandLine_hasApp(named), "detachApp");
    CommandLine_close(named);
    CHECK(fake.closes == 1 && !CommandLine_isOpen(named), "device restored");

    // Kernel registration: the fourth kind.
    Kernel *k = Kernel();
    CHECK(k != nullptr, "kernel create");
    CommandLine *cli = CommandLine("kernel-cli");
    Fake kf; memset(&kf, 0, sizeof(kf)); kf.w = 60; kf.h = 20;
    CommandLineIo kio = fakeIo(&kf);
    CHECK(CommandLine_setIo(cli, &kio), "kernel cli io");
    CHECK(CommandLine_open(cli), "kernel cli open");
    CHECK(Kernel_addCommandLine(k, cli), "kernel add");
    CHECK(Kernel_getCommandLineCount(k) == 1 && Kernel_getCommandLine(k, 0) == cli, "kernel get");
    CHECK(Kernel_getCommandLine(k, 5) == nullptr, "kernel out of range");
    CommandLine *handles[4];
    CHECK(Kernel_getCommandLines(k, handles, 4) == 1 && handles[0] == cli, "kernel handles");
    CHECK(!Kernel_free(k), "free refuses while a commandline is registered");
    CHECK(Kernel_removeCommandLine(k, cli), "kernel remove");
    CHECK(Kernel_getCommandLineCount(k) == 0, "kernel empty");
    CHECK(Kernel_free(k), "kernel free");
    CommandLine_close(cli);
    CommandLine_free(cli);

    CommandLine_free(def);
    CommandLine_free(named);

    if (failures == 0)
        printf("commandline_test: PASS\n");
    return failures == 0 ? 0 : 1;
}

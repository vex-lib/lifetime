#include "commandline.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/annotation.h"
#include "util/throw.h"

;;DEFINITION
/**
 * ============================================================================
 * DEFINITION: CommandLine
 * ============================================================================
 * The R1 terminal-application host — the fourth process kind. CommandLine owns
 * the TTY device for a full-screen terminal program: acquisition (raw mode,
 * alternate screen, colors), ONE owned cell grid, the single resize surface,
 * bounded normalized input, one present flush, and teardown. It is the
 * terminal analog of Window: a device resource, never widget semantics.
 *
 * Determinant (R3) owns cells, widgets and the reloadable view/command module;
 * TerminalApp (R3) composes it. This class includes no R3 header — the seam is
 * an injectable CommandLineIo backend plus the owned grid, mirroring how Console
 * borrows ProcessSpawn through ConsoleIo. The R5 application wires the toolkit.
 *
 * MEMORY LAYOUT & LIFECYCLE:
 *   Fixed title buffer plus a heap-owned width*height cell grid, allocated on
 *   open and grown on revalidate. The CommandLine itself is cold calloc.
 *
 * OPERATIONAL INVARIANTS:
 *   - Resize is ALWAYS honored by CommandLine_revalidate; the table stakes for a
 *     live terminal UI, never deferred to a "settle".
 *   - CommandLine_setApp swaps the borrowed app WITHOUT re-acquiring the device,
 *     so a hot-loaded view can be published in the same process.
 *   - The host owns loaded-code lifetime (HotModule); CommandLine owns the device.
 * ============================================================================
 */

;;OVERVIEW
/**
 * ============================================================================
 * CLASS: CommandLine (kernel/commandline.c)
 * ============================================================================
 * STRUCT FIELDS (Mirroring kernel/commandline.h):
 * ----------------------------------------------------------------------------
 *   char title[128]        device title
 *   CommandLineIo io       injected device backend
 *   bool explicitIo        false => default (ncurses) backend
 *   CommandLineApp app     borrowed swappable application seam
 *   char *cells            owned device framebuffer (width*height)
 *   bytes capacity        admitted grid byte capacity
 *   bytes width, height   cell geometry
 *   u64 generation    successful app publications
 *   bytes refusalCount    epoch grid-capacity refusals
 *   atomic_uint run        CommandLineRunPhase: IDLE/RUNNING/CANCELLED
 *   bool opened            device acquired
 *   bool active            callback exclusion guard
 *
 * PRIVATE HELPERS:
 *   cmdIo(self)      : the effective backend (explicit, else default)
 *   cmdAllocGrid(self, w, h) : (re)allocate the owned grid, false on overflow/OOM
 *
 * FUNCTION REGISTRY (exports; see kernel/commandline.h for signatures):
 *   Constructors: CommandLine_0, CommandLine_1, CommandLine(...)
 *   Backend:      CommandLine_setIo, CommandLine_getIo, CommandLine_defaultIo
 *   Lifecycle:    CommandLine_open, CommandLine_close, CommandLine_revalidate,
 *                 CommandLine_present
 *   App seam:     CommandLine_setApp, CommandLine_hasApp, CommandLine_detachApp
 *   Loop:         CommandLine_tick, CommandLine_run, CommandLine_stop,
 *                 CommandLine_requestCancel, CommandLine_isRunning
 *   Geometry:     CommandLine_getWidth/Height/Capacity, CommandLine_getCells,
 *                 CommandLine_getCellsConst, CommandLine_clear
 *   Input:        CommandLine_poll, CommandLine_setMouseReporting
 *   Identity:     CommandLine_setTitle/getTitle/getGeneration/
 *                 exhaustionCount/isOpen/isActive/isCancelled
 *   Projections:  CommandLine_toString/toStringStruct
 *
 * CONTRACT: cold invalid operations THROW once; null queries return safe
 * defaults. A resize refusal is counted and reported once per epoch. Rejection
 * preserves the prior app and the owned grid. All calls require host external
 * serialization; the device is not internally thread-safe. The host excludes
 * active callers before setApp/detach. No loader, socket or worker is owned.
 * ============================================================================
 */

;;INTENTION("R1 terminal-application host: device ownership + one resize surface + swappable app seam; R3 toolkit binds via CommandLineIo, never an R1 include")

static const bytes COMMANDLINE_DEFAULT_COLUMNS = 80;
static const bytes COMMANDLINE_DEFAULT_ROWS = 24;

// The effective backend: an injected io wins; otherwise the default (ncurses).
static CommandLineIo cmdIo(const CommandLine *self) {
    return (*self).explicitIo ? (*self).io : CommandLine_defaultIo();
}

// (Re)allocate the owned grid for width*height cells. Overflow-checked cold path.
static bool cmdAllocGrid(CommandLine *self, bytes width, bytes height) {
    if (width == 0 || height == 0 || height > SIZE_MAX / width)
        return false;
    bytes bytes = width * height;
    if (bytes > (*self).capacity) {
        char *next = (char*) realloc((*self).cells, bytes);
        if (next == nullptr)
            return false;
        (*self).cells = next;
        (*self).capacity = bytes;
    }
    (*self).width = width;
    (*self).height = height;
    memset((*self).cells, ' ', bytes);
    return true;
}

CommandLine *CommandLine_0() {
    return CommandLine_1("vex");
}

CommandLine *CommandLine_1(const char *title) {
    CommandLine *self = (CommandLine*) calloc(1, sizeof(CommandLine));
    if (self == nullptr)
        return nullptr;
    CommandLine_setTitle(self, title);
    return self;
}

fn CommandLine_free(CommandLine *self) {
    if (self == nullptr)
        return;
    if ((*self).opened)
        CommandLine_close(self);
    free((*self).cells);
    free(self);
}

bool CommandLine_setIo(CommandLine *self, const CommandLineIo *io) {
    if (self == nullptr || (*self).opened || (*self).active) {
        THROW("CommandLine setIo: null or busy");
        return false;
    }
    if (io == nullptr) {
        memset(&(*self).io, 0, sizeof((*self).io));
        (*self).explicitIo = false;
        return true;
    }
    (*self).io = *io;
    (*self).explicitIo = true;
    return true;
}

const CommandLineIo *CommandLine_getIo(const CommandLine *self) {
    return self ? &(*self).io : nullptr;
}

bool CommandLine_open(CommandLine *self) {
    if (self == nullptr || (*self).opened) {
        THROW("CommandLine open: null or already open");
        return false;
    }
    CommandLineIo io = cmdIo(self);
    if (io.open == nullptr || io.size == nullptr) {
        THROW("CommandLine open: backend lacks open/size");
        return false;
    }
    if (!io.open(io.ctx)) {
        THROW("CommandLine open: device refused");
        return false;
    }
    (*self).io = io;
    (*self).explicitIo = true;   // freeze the resolved backend for this session
    bytes w = COMMANDLINE_DEFAULT_COLUMNS, h = COMMANDLINE_DEFAULT_ROWS;
    if (!io.size(io.ctx, &w, &h) || !cmdAllocGrid(self, w, h)) {
        io.close(io.ctx);
        THROW("CommandLine open: grid admission failed");
        return false;
    }
    (*self).opened = true;
    // A device session owns one loop history: opening (never legal while a loop
    // is live) starts the word fresh, so a cancelled session is revived only by
    // an explicit close/reopen, never silently mid-session.
    atomic_store_explicit(&(*self).run, COMMANDLINE_RUN_IDLE, memory_order_release);
    return true;
}

fn CommandLine_close(CommandLine *self) {
    if (self == nullptr || !(*self).opened)
        return;
    CommandLineIo io = (*self).io;
    if (io.close != nullptr)
        io.close(io.ctx);
    free((*self).cells);
    (*self).cells = nullptr;
    (*self).capacity = 0;
    (*self).width = 0;
    (*self).height = 0;
    (*self).opened = false;
    (*self).active = false;
}

bool CommandLine_revalidate(CommandLine *self) {
    if (self == nullptr || !(*self).opened) {
        THROW("CommandLine revalidate: not open");
        return false;
    }
    CommandLineIo io = (*self).io;
    if (io.size == nullptr) {
        THROW("CommandLine revalidate: not open");
        return false;
    }
    bytes w = (*self).width, h = (*self).height;
    if (!io.size(io.ctx, &w, &h) || w == 0 || h == 0) {
        if ((*self).refusalCount == 0)
            THROW("CommandLine resized: device reported %zux%zu", w, h);
        if ((*self).refusalCount != SIZE_MAX)
            ++(*self).refusalCount;
        return false;
    }
    if (w == (*self).width && h == (*self).height)
        return CommandLine_present(self);
    if (!cmdAllocGrid(self, w, h)) {
        if ((*self).refusalCount == 0)
            THROW("CommandLine resized: grid %zux%zu exceeds backing", w, h);
        if ((*self).refusalCount != SIZE_MAX)
            ++(*self).refusalCount;
        return false;
    }
    // A resize repaints in one step: redraw the app, then present once.
    CommandLineApp app = (*self).app;
    if (app.draw != nullptr) {
        (*self).active = true;
        bool ok = app.draw(app.ctx, self);
        (*self).active = false;
        if (!ok)
            return false;
    }
    return CommandLine_present(self);
}

bool CommandLine_present(CommandLine *self) {
    if (self == nullptr || !(*self).opened || (*self).cells == nullptr)
        return false;
    CommandLineIo io = (*self).io;
    if (io.present == nullptr)
        return false;
    return io.present(io.ctx, (*self).cells, (*self).width, (*self).height);
}

bool CommandLine_setApp(CommandLine *self, const CommandLineApp *app) {
    if (self == nullptr || app == nullptr || (*app).draw == nullptr || (*self).active
        || (*self).generation == UINT64_MAX) {
        THROW("CommandLine setApp: null draw, busy or generation overflow");
        return false;
    }
    (*self).app = *app;
    ++(*self).generation;
    return true;
}

bool CommandLine_hasApp(const CommandLine *self) {
    return self && (*self).app.draw != nullptr;
}

bool CommandLine_detachApp(CommandLine *self) {
    if (self == nullptr || (*self).app.draw == nullptr)
        return false;
    CommandLineApp app = (*self).app;
    if (app.detach != nullptr)
        app.detach(app.ctx);
    memset(&(*self).app, 0, sizeof((*self).app));
    return true;
}

/** Requests cancel; only a live loop consumes it (IDLE stops are discarded). */
static fn cmdRequestCancel(CommandLine *self) {
    u32 expected = COMMANDLINE_RUN_RUNNING;
    atomic_compare_exchange_strong_explicit(&(*self).run, &expected,
        COMMANDLINE_RUN_CANCELLED, memory_order_acq_rel, memory_order_relaxed);
}

bool CommandLine_tick(CommandLine *self) {
    if (self == nullptr || !(*self).opened || (*self).app.draw == nullptr) {
        THROW("CommandLine tick: not open or no app");
        return false;
    }
    CommandLineEvent ev;
    CommandLineApp app = (*self).app;
    if (CommandLine_poll(self, 0, &ev)) {
        if (ev.kind == COMMANDLINE_EVENT_RESIZE)
            return CommandLine_revalidate(self);   // revalidate draws + presents
        if (app.input != nullptr) {
            (*self).active = true;
            bool ok = app.input(app.ctx, self, &ev);
            (*self).active = false;
            if (!ok) {
                cmdRequestCancel(self);
                return false;
            }
        }
    }
    (*self).active = true;
    bool drew = app.draw(app.ctx, self);
    (*self).active = false;
    if (!drew)
        return false;
    return CommandLine_present(self);
}

int CommandLine_run(CommandLine *self) {
    if (self == nullptr || !(*self).opened || (*self).app.draw == nullptr) {
        THROW("CommandLine run: not open or no app");
        return 1;
    }
    u32 expected = COMMANDLINE_RUN_IDLE;
    if (!atomic_compare_exchange_strong_explicit(&(*self).run, &expected,
            COMMANDLINE_RUN_RUNNING, memory_order_acq_rel, memory_order_relaxed)) {
        if (expected == COMMANDLINE_RUN_CANCELLED)
            THROW("CommandLine run: session was cancelled; reopen the device for a new loop");
        else
            THROW("CommandLine run: loop already active");
        return 1;
    }
    int rc = 0;
    CommandLineApp app = (*self).app;
    while (atomic_load_explicit(&(*self).run, memory_order_acquire) == COMMANDLINE_RUN_RUNNING) {
        CommandLineEvent ev;
        if (CommandLine_poll(self, 50, &ev)) {
            if (ev.kind == COMMANDLINE_EVENT_RESIZE) {
                if (!CommandLine_revalidate(self)) { rc = 1; break; }
                continue;   // revalidate already drew + presented
            }
            if (app.input != nullptr) {
                (*self).active = true;
                bool ok = app.input(app.ctx, self, &ev);
                (*self).active = false;
                if (!ok) {
                    cmdRequestCancel(self);
                    break;
                }
            }
        }
        (*self).active = true;
        bool drew = app.draw(app.ctx, self);
        (*self).active = false;
        if (!drew || !CommandLine_present(self)) {
            rc = 1;
            break;
        }
    }
    // A stop leaves CANCELLED visible; a failure exit returns to IDLE so the
    // same device can be run again.
    expected = COMMANDLINE_RUN_RUNNING;
    atomic_compare_exchange_strong_explicit(&(*self).run, &expected,
        COMMANDLINE_RUN_IDLE, memory_order_acq_rel, memory_order_relaxed);
    return rc;
}

fn CommandLine_stop(CommandLine *self) {
    if (self != nullptr)
        cmdRequestCancel(self);
}

fn CommandLine_requestCancel(CommandLine *self) {
    CommandLine_stop(self);
}

bool CommandLine_isRunning(const CommandLine *self) {
    return self && atomic_load_explicit(&(*self).run, memory_order_acquire) == COMMANDLINE_RUN_RUNNING;
}

CommandLineRunPhase CommandLine_getRunPhase(const CommandLine *self) {
    if (self == nullptr)
        return COMMANDLINE_RUN_IDLE;
    return (CommandLineRunPhase) atomic_load_explicit(&(*self).run, memory_order_acquire);
}

bytes CommandLine_getWidth(const CommandLine *self) { return self ? (*self).width : 0; }
bytes CommandLine_getHeight(const CommandLine *self) { return self ? (*self).height : 0; }
bytes CommandLine_getCapacity(const CommandLine *self) { return self ? (*self).capacity : 0; }
char *CommandLine_getCells(CommandLine *self) { return self ? (*self).cells : nullptr; }
const char *CommandLine_getCellsConst(const CommandLine *self) { return self ? (*self).cells : nullptr; }

bool CommandLine_clear(CommandLine *self) {
    if (self == nullptr || (*self).cells == nullptr)
        return false;
    memset((*self).cells, ' ', (*self).width * (*self).height);
    return true;
}

bool CommandLine_poll(CommandLine *self, int timeoutMs, CommandLineEvent *out) {
    if (out != nullptr)
        *out = (CommandLineEvent) { .kind = COMMANDLINE_EVENT_NONE };
    if (self == nullptr || out == nullptr || !(*self).opened)
        return false;
    CommandLineIo io = (*self).io;
    if (io.poll == nullptr)
        return false;
    return io.poll(io.ctx, timeoutMs, out);
}

bool CommandLine_setMouseReporting(CommandLine *self, bool enabled) {
    if (self == nullptr || !(*self).opened)
        return false;
    CommandLineIo io = (*self).io;
    if (io.setMouse == nullptr)
        return false;
    return io.setMouse(io.ctx, enabled);
}

fn CommandLine_setTitle(CommandLine *self, const char *title) {
    if (self == nullptr)
        return;
    const char *src = title ? title : "vex";
    bytes n = strlen(src);
    if (n >= sizeof((*self).title))
        n = sizeof((*self).title) - 1;
    memcpy((*self).title, src, n);
    (*self).title[n] = '\0';
}

const char *CommandLine_getTitle(const CommandLine *self) { return self ? (*self).title : "vex"; }
u64 CommandLine_getGeneration(const CommandLine *self) { return self ? (*self).generation : 0; }
bytes CommandLine_exhaustionCount(const CommandLine *self) { return self ? (*self).refusalCount : 0; }
bool CommandLine_isOpen(const CommandLine *self) { return self && (*self).opened; }
bool CommandLine_isActive(const CommandLine *self) { return self && (*self).active; }
bool CommandLine_isCancelled(const CommandLine *self) {
    return self && atomic_load_explicit(&(*self).run, memory_order_acquire) == COMMANDLINE_RUN_CANCELLED;
}

bool CommandLine_toString(const CommandLine *self, char *dest, bytes cap, bool *outTruncated) {
    if (outTruncated != nullptr)
        *outTruncated = false;
    if (dest == nullptr || cap == 0) {
        if (outTruncated != nullptr)
            *outTruncated = true;
        THROW("CommandLine projection: missing destination");
        return false;
    }
    int n = self ? snprintf(dest, cap, "CommandLine{title=\"%s\",%zux%zu,capacity=%zu,generation=%llu,refusalCount=%zu,run=%u,opened=%d,active=%d,hasApp=%d}",
        (*self).title, (*self).width, (*self).height, (*self).capacity,
        (unsigned long long) (*self).generation, (*self).refusalCount,
        (unsigned) atomic_load_explicit(&(*self).run, memory_order_acquire),
        (*self).opened, (*self).active, (*self).app.draw != nullptr)
        : snprintf(dest, cap, "nullptr");
    bool cut = n < 0 || (bytes) n >= cap;
    if (outTruncated != nullptr)
        *outTruncated = cut;
    if (cut)
        THROW("CommandLine projection: truncated at %zu bytes", cap);
    return !cut;
}

bool CommandLine_toStringStruct(const CommandLine *self, char *dest, bytes cap, bool *outTruncated) {
    return CommandLine_toString(self, dest, cap, outTruncated);
}

#include "vex.h"
#include "commandline.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


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

;;GETTER
static bool CommandLine_explicitIo(const CommandLine *self) { return (*self).explicitIo; }
;;SETTER
static fn CommandLine_setExplicitIo(CommandLine *self, bool v) { (*self).explicitIo = v; }
;;GETTER
static CommandLineIo CommandLine_ioVal(const CommandLine *self) { return (*self).io; }
;;GETTER
static CommandLineIo *CommandLine_ioMut(CommandLine *self) { return &(*self).io; }
;;GETTER
static const CommandLineIo *CommandLine_ioPtr(const CommandLine *self) { return &(*self).io; }
;;SETTER
static fn CommandLine_setIoVal(CommandLine *self, CommandLineIo io) { (*self).io = io; }
;;GETTER
static bytes CommandLine_capacity(const CommandLine *self) { return (*self).capacity; }
;;SETTER
static fn CommandLine_setCapacity(CommandLine *self, bytes v) { (*self).capacity = v; }
;;GETTER
static char *CommandLine_cells(const CommandLine *self) { return (*self).cells; }
;;SETTER
static fn CommandLine_setCells(CommandLine *self, char *v) { (*self).cells = v; }
;;GETTER
static bytes CommandLine_width(const CommandLine *self) { return (*self).width; }
;;SETTER
static fn CommandLine_setWidth(CommandLine *self, bytes v) { (*self).width = v; }
;;GETTER
static bytes CommandLine_height(const CommandLine *self) { return (*self).height; }
;;SETTER
static fn CommandLine_setHeight(CommandLine *self, bytes v) { (*self).height = v; }
;;GETTER
static bool CommandLine_opened(const CommandLine *self) { return (*self).opened; }
;;SETTER
static fn CommandLine_setOpened(CommandLine *self, bool v) { (*self).opened = v; }
;;GETTER
static bool CommandLine_active(const CommandLine *self) { return (*self).active; }
;;SETTER
static fn CommandLine_setActive(CommandLine *self, bool v) { (*self).active = v; }
;;GETTER
static _Atomic u32 *CommandLine_runMut(CommandLine *self) { return &(*self).run; }
;;GETTER
static const _Atomic u32 *CommandLine_runPtr(const CommandLine *self) { return &(*self).run; }
;;GETTER
static bytes CommandLine_refusalCount(const CommandLine *self) { return (*self).refusalCount; }
;;SETTER
static fn CommandLine_setRefusalCount(CommandLine *self, bytes v) { (*self).refusalCount = v; }
;;GETTER
static CommandLineApp CommandLine_appVal(const CommandLine *self) { return (*self).app; }
;;GETTER
static CommandLineApp *CommandLine_appMut(CommandLine *self) { return &(*self).app; }
;;SETTER
static fn CommandLine_setAppVal(CommandLine *self, CommandLineApp app) { (*self).app = app; }
;;GETTER
static u64 CommandLine_generation(const CommandLine *self) { return (*self).generation; }
;;SETTER
static fn CommandLine_setGeneration(CommandLine *self, u64 v) { (*self).generation = v; }
;;GETTER
static char *CommandLine_titleMut(CommandLine *self) { return (*self).title; }
;;GETTER
static const char *CommandLine_titlePtr(const CommandLine *self) { return (*self).title; }

// The effective backend: an injected io wins; otherwise the default (ncurses).
static CommandLineIo cmdIo(const CommandLine *self) {
    return CommandLine_explicitIo(self) ? CommandLine_ioVal(self) : CommandLine_defaultIo();
}

// (Re)allocate the owned grid for width*height cells. Overflow-checked cold path.
static bool cmdAllocGrid(CommandLine *self, bytes width, bytes height) {
    if (width == 0 || height == 0 || height > SIZE_MAX / width)
        return false;
    bytes bytes = width * height;
    if (bytes > CommandLine_capacity(self)) {
        char *next = (char*) realloc(CommandLine_cells(self), bytes);
        if (next == nullptr)
            return false;
        CommandLine_setCells(self, next);
        CommandLine_setCapacity(self, bytes);
    }
    CommandLine_setWidth(self, width);
    CommandLine_setHeight(self, height);
    memset(CommandLine_cells(self), ' ', bytes);
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
    if (CommandLine_opened(self))
        CommandLine_close(self);
    free(CommandLine_cells(self));
    free(self);
}

bool CommandLine_setIo(CommandLine *self, const CommandLineIo *io) {
    if (self == nullptr || CommandLine_opened(self) || CommandLine_active(self)) {
        THROW("CommandLine setIo: null or busy");
        return false;
    }
    if (io == nullptr) {
        memset(CommandLine_ioMut(self), 0, sizeof(CommandLineIo));
        CommandLine_setExplicitIo(self, false);
        return true;
    }
    CommandLine_setIoVal(self, *io);
    CommandLine_setExplicitIo(self, true);
    return true;
}

const CommandLineIo *CommandLine_getIo(const CommandLine *self) {
    return self ? CommandLine_ioPtr(self) : nullptr;
}

bool CommandLine_open(CommandLine *self) {
    if (self == nullptr || CommandLine_opened(self)) {
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
    CommandLine_setIoVal(self, io);
    CommandLine_setExplicitIo(self, true);   // freeze the resolved backend for this session
    bytes w = COMMANDLINE_DEFAULT_COLUMNS, h = COMMANDLINE_DEFAULT_ROWS;
    if (!io.size(io.ctx, &w, &h) || !cmdAllocGrid(self, w, h)) {
        io.close(io.ctx);
        THROW("CommandLine open: grid admission failed");
        return false;
    }
    CommandLine_setOpened(self, true);
    // A device session owns one loop history: opening (never legal while a loop
    // is live) starts the word fresh, so a cancelled session is revived only by
    // an explicit close/reopen, never silently mid-session.
    atomic_store_explicit(CommandLine_runMut(self), COMMANDLINE_RUN_IDLE, memory_order_release);
    return true;
}

fn CommandLine_close(CommandLine *self) {
    if (self == nullptr || !CommandLine_opened(self))
        return;
    CommandLineIo io = CommandLine_ioVal(self);
    if (io.close != nullptr)
        io.close(io.ctx);
    free(CommandLine_cells(self));
    CommandLine_setCells(self, nullptr);
    CommandLine_setCapacity(self, 0);
    CommandLine_setWidth(self, 0);
    CommandLine_setHeight(self, 0);
    CommandLine_setOpened(self, false);
    CommandLine_setActive(self, false);
}

bool CommandLine_revalidate(CommandLine *self) {
    if (self == nullptr || !CommandLine_opened(self)) {
        THROW("CommandLine revalidate: not open");
        return false;
    }
    CommandLineIo io = CommandLine_ioVal(self);
    if (io.size == nullptr) {
        THROW("CommandLine revalidate: not open");
        return false;
    }
    bytes curW = CommandLine_width(self);
    bytes curH = CommandLine_height(self);
    bytes w = curW, h = curH;
    if (!io.size(io.ctx, &w, &h) || w == 0 || h == 0) {
        bytes rc = CommandLine_refusalCount(self);
        if (rc == 0)
            THROW("CommandLine resized: device reported %zux%zu", w, h);
        if (rc != SIZE_MAX)
            CommandLine_setRefusalCount(self, rc + 1);
        return false;
    }
    if (w == curW && h == curH)
        return CommandLine_present(self);
    if (!cmdAllocGrid(self, w, h)) {
        bytes rc = CommandLine_refusalCount(self);
        if (rc == 0)
            THROW("CommandLine resized: grid %zux%zu exceeds backing", w, h);
        if (rc != SIZE_MAX)
            CommandLine_setRefusalCount(self, rc + 1);
        return false;
    }
    // A resize repaints in one step: redraw the app, then present once.
    CommandLineApp app = CommandLine_appVal(self);
    if (app.draw != nullptr) {
        CommandLine_setActive(self, true);
        bool ok = app.draw(app.ctx, self);
        CommandLine_setActive(self, false);
        if (!ok)
            return false;
    }
    return CommandLine_present(self);
}

bool CommandLine_present(CommandLine *self) {
    char *cells = self ? CommandLine_cells(self) : nullptr;
    if (self == nullptr || !CommandLine_opened(self) || cells == nullptr)
        return false;
    CommandLineIo io = CommandLine_ioVal(self);
    if (io.present == nullptr)
        return false;
    return io.present(io.ctx, cells, CommandLine_width(self), CommandLine_height(self));
}

bool CommandLine_setApp(CommandLine *self, const CommandLineApp *app) {
    if (self == nullptr || app == nullptr || CommandLineApp_draw(app) == nullptr || CommandLine_active(self)
        || CommandLine_generation(self) == UINT64_MAX) {
        THROW("CommandLine setApp: null draw, busy or generation overflow");
        return false;
    }
    CommandLine_setAppVal(self, *app);
    CommandLine_setGeneration(self, CommandLine_generation(self) + 1);
    return true;
}

typedef bool (*CommandLine_DrawFn)(any ctx, CommandLine *line);

;;GETTER
static const CommandLineApp *CommandLine_app(const CommandLine *self) {
    return &(*self).app;
}

;;GETTER
static CommandLine_DrawFn CommandLineApp_draw(const CommandLineApp *app) {
    return (*app).draw;
}

bool CommandLine_hasApp(const CommandLine *self) {
    return self != nullptr && CommandLineApp_draw(CommandLine_app(self)) != nullptr;
}

bool CommandLine_detachApp(CommandLine *self) {
    if (!CommandLine_hasApp(self))
        return false;
    CommandLineApp app = CommandLine_appVal(self);
    if (app.detach != nullptr)
        app.detach(app.ctx);
    memset(CommandLine_appMut(self), 0, sizeof(CommandLineApp));
    return true;
}

/** Requests cancel; only a live loop consumes it (IDLE stops are discarded). */
static fn cmdRequestCancel(CommandLine *self) {
    u32 expected = COMMANDLINE_RUN_RUNNING;
    atomic_compare_exchange_strong_explicit(CommandLine_runMut(self), &expected,
        COMMANDLINE_RUN_CANCELLED, memory_order_acq_rel, memory_order_relaxed);
}

bool CommandLine_tick(CommandLine *self) {
    if (self == nullptr || !CommandLine_opened(self) || !CommandLine_hasApp(self)) {
        THROW("CommandLine tick: not open or no app");
        return false;
    }
    CommandLineEvent ev;
    CommandLineApp app = CommandLine_appVal(self);
    if (CommandLine_poll(self, 0, &ev)) {
        if (ev.kind == COMMANDLINE_EVENT_RESIZE)
            return CommandLine_revalidate(self);   // revalidate draws + presents
        if (app.input != nullptr) {
            CommandLine_setActive(self, true);
            bool ok = app.input(app.ctx, self, &ev);
            CommandLine_setActive(self, false);
            if (!ok) {
                cmdRequestCancel(self);
                return false;
            }
        }
    }
    CommandLine_setActive(self, true);
    bool drew = app.draw(app.ctx, self);
    CommandLine_setActive(self, false);
    if (!drew)
        return false;
    return CommandLine_present(self);
}

i32 CommandLine_run(CommandLine *self) {
    if (self == nullptr || !CommandLine_opened(self) || !CommandLine_hasApp(self)) {
        THROW("CommandLine run: not open or no app");
        return 1;
    }
    u32 expected = COMMANDLINE_RUN_IDLE;
    if (!atomic_compare_exchange_strong_explicit(CommandLine_runMut(self), &expected,
            COMMANDLINE_RUN_RUNNING, memory_order_acq_rel, memory_order_relaxed)) {
        if (expected == COMMANDLINE_RUN_CANCELLED)
            THROW("CommandLine run: session was cancelled; reopen the device for a new loop");
        else
            THROW("CommandLine run: loop already active");
        return 1;
    }
    i32 rc = 0;
    CommandLineApp app = CommandLine_appVal(self);
    while (atomic_load_explicit(CommandLine_runPtr(self), memory_order_acquire) == COMMANDLINE_RUN_RUNNING) {
        CommandLineEvent ev;
        if (CommandLine_poll(self, 50, &ev)) {
            if (ev.kind == COMMANDLINE_EVENT_RESIZE) {
                if (!CommandLine_revalidate(self)) { rc = 1; break; }
                continue;   // revalidate already drew + presented
            }
            if (app.input != nullptr) {
                CommandLine_setActive(self, true);
                bool ok = app.input(app.ctx, self, &ev);
                CommandLine_setActive(self, false);
                if (!ok) {
                    cmdRequestCancel(self);
                    break;
                }
            }
        }
        CommandLine_setActive(self, true);
        bool drew = app.draw(app.ctx, self);
        CommandLine_setActive(self, false);
        if (!drew || !CommandLine_present(self)) {
            rc = 1;
            break;
        }
    }
    // A stop leaves CANCELLED visible; a failure exit returns to IDLE so the
    // same device can be run again.
    expected = COMMANDLINE_RUN_RUNNING;
    atomic_compare_exchange_strong_explicit(CommandLine_runMut(self), &expected,
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
    return self && atomic_load_explicit(CommandLine_runPtr(self), memory_order_acquire) == COMMANDLINE_RUN_RUNNING;
}

CommandLineRunPhase CommandLine_getRunPhase(const CommandLine *self) {
    if (self == nullptr)
        return COMMANDLINE_RUN_IDLE;
    return (CommandLineRunPhase) atomic_load_explicit(CommandLine_runPtr(self), memory_order_acquire);
}

bytes CommandLine_getWidth(const CommandLine *self) { return self ? CommandLine_width(self) : 0; }
bytes CommandLine_getHeight(const CommandLine *self) { return self ? CommandLine_height(self) : 0; }
bytes CommandLine_getCapacity(const CommandLine *self) { return self ? CommandLine_capacity(self) : 0; }
char *CommandLine_getCells(CommandLine *self) { return self ? CommandLine_cells(self) : nullptr; }
const char *CommandLine_getCellsConst(const CommandLine *self) { return self ? CommandLine_cells(self) : nullptr; }

bool CommandLine_clear(CommandLine *self) {
    char *cells = self ? CommandLine_cells(self) : nullptr;
    if (self == nullptr || cells == nullptr)
        return false;
    memset(cells, ' ', CommandLine_width(self) * CommandLine_height(self));
    return true;
}

bool CommandLine_poll(CommandLine *self, i32 timeoutMs, CommandLineEvent *out) {
    if (out != nullptr)
        *out = (CommandLineEvent) { .kind = COMMANDLINE_EVENT_NONE };
    if (self == nullptr || out == nullptr || !CommandLine_opened(self))
        return false;
    CommandLineIo io = CommandLine_ioVal(self);
    if (io.poll == nullptr)
        return false;
    return io.poll(io.ctx, timeoutMs, out);
}

bool CommandLine_setMouseReporting(CommandLine *self, bool enabled) {
    if (self == nullptr || !CommandLine_opened(self))
        return false;
    CommandLineIo io = CommandLine_ioVal(self);
    if (io.setMouse == nullptr)
        return false;
    return io.setMouse(io.ctx, enabled);
}

fn CommandLine_setTitle(CommandLine *self, const char *title) {
    if (self == nullptr)
        return;
    const char *src = title ? title : "vex";
    bytes n = strlen(src);
    bytes cap = sizeof(((CommandLine) {0}).title);
    if (n >= cap)
        n = cap - 1;
    char *buf = CommandLine_titleMut(self);
    memcpy(buf, src, n);
    buf[n] = '\0';
}

const char *CommandLine_getTitle(const CommandLine *self) { return self ? CommandLine_titlePtr(self) : "vex"; }
u64 CommandLine_getGeneration(const CommandLine *self) { return self ? CommandLine_generation(self) : 0; }
bytes CommandLine_exhaustionCount(const CommandLine *self) { return self ? CommandLine_refusalCount(self) : 0; }
bool CommandLine_isOpen(const CommandLine *self) { return self && CommandLine_opened(self); }
bool CommandLine_isActive(const CommandLine *self) { return self && CommandLine_active(self); }
bool CommandLine_isCancelled(const CommandLine *self) {
    return self && atomic_load_explicit(CommandLine_runPtr(self), memory_order_acquire) == COMMANDLINE_RUN_CANCELLED;
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
    i32 n = self ? snprintf(dest, cap, "CommandLine{title=\"%s\",%zux%zu,capacity=%zu,generation=%llu,refusalCount=%zu,run=%u,opened=%d,active=%d,hasApp=%d}",
        CommandLine_titlePtr(self), CommandLine_width(self), CommandLine_height(self), CommandLine_capacity(self),
        (u64) CommandLine_generation(self), CommandLine_refusalCount(self),
        (u32) atomic_load_explicit(CommandLine_runPtr(self), memory_order_acquire),
        CommandLine_opened(self), CommandLine_active(self), CommandLine_hasApp(self))
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

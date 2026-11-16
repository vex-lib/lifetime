#include "vex.h"
#include "console.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


;;DEFINITION
/**
 * ============================================================================
 * DEFINITION: Console
 * ============================================================================
 * Session state machine for shell scripts, CLI child execution, and REPL interactions.
 * Owns session lifecycle (shell command, working directory, running/cancel state,
 * exit status) while delegating process execution across the ConsoleIo function-table
 * seam to avoid direct thread or socket ownership in hotcwap.
 *
 * memory consists of fixed-capacity string buffers for command and directory, an
 * injectable ConsoleIo function table, and an atomic cancel flag. Allocation is
 * cold heap via calloc; child process operations run non-blocking in slices bounded
 * to at most 100 milliseconds per the Bounded Wait Law.
 * ============================================================================
 */

;;OVERVIEW
/**
 * ============================================================================
 * CLASS: Console (kernel/console.c)
 * ============================================================================
 * SUMMARY:
 *   Session state machine for shell scripts and REPL-style interactions. Owns
 *   the session lifecycle (running/cancel/exitStatus) and borrows execution
 *   through a ConsoleIo fn-table seam (the Conflict Triage Law managed exception — wired to
 *   R2 ProcessSpawn when that class lands).
 *
 *   No threads, no sockets, no blocking waits are owned by Console. The
 *   supervising Kernel thread drives Console_poll in bounded <=100ms slices
 *   with a cancel flag (the Bounded Wait Law). Console never touches a window.
 *
 * STRUCT FIELDS (Mirroring kernel/console.h — exactly this file's class):
 * ----------------------------------------------------------------------------
 *   char shell[CONSOLE_MAX_SHELL];          // command to run (default "/bin/sh")
 *   char workDir[CONSOLE_MAX_WORKDIR];      // working directory (default "")
 *   ConsoleIo io;                           // injectable execution seam
 *   _Atomic bool cancel;                    // cancel flag (the Bounded Wait Law)
 *   bool running;                           // session active
 *   i32 exitStatus;                         // exit code of last session
 *
 * PRIVATE HELPERS:
 * ----------------------------------------------------------------------------
 *   console_clamp_copy(dst, cap, src) : Safe bounded string copy with null terminator
 *
 * FUNCTION REGISTRY:
 * ----------------------------------------------------------------------------
 * Public Constructors: (.h)
 *   - Console()                               : Console_0()
 *   - Console(shell)                          : Console_1(shell)
 *
 * Private Constructors: (.c static)
 *   - (none)
 *
 * Public Core Functions: (.h)
 *   - Console_run(self)                       : Launch session via io seam
 *   - Console_writeInput(self, line, len)     : Feed stdin pipe
 *   - Console_poll(self, out, outCap, outLen) : Non-blocking <=100ms output drain
 *   - Console_cancel(self)                    : Request graceful SIGTERM
 *   - Console_isRunning(self)                 : Query session active flag
 *   - Console_getExitStatus(self)             : Query child process exit code
 *   - Console_free(self)                      : Cancel session and release memory
 *
 * Private Core Functions: (.c static)
 *   - console_clamp_copy(dst, cap, src)       : Bounded string clamp helper
 *
 * Public Setters: (.h)
 *   - Console_setShell(self, shell)
 *   - Console_setWorkDir(self, workDir)
 *   - Console_setIo(self, io, ctx)
 *
 * Private Setters: (.c static)
 *   - (none)
 *
 * Public Getters: (.h)
 *   - Console_getShell(self)
 *   - Console_getWorkDir(self)
 *   - Console_getIo(self)
 *
 * Private Getters: (.c static)
 *   - (none)
 * ============================================================================
 */

// ;;INTENTION("ConsoleIo seam wiring: when R2 ProcessSpawn lands,
// Console_run calls io.spawn(shell, workDir).  Console_poll drives
// io.reap in <=100ms non-blocking slices, with io.cancel firing
// SIGTERM and the supervising thread observing completion via io.joined.
// All io.fn-pointers are nullable — NULL degrades gracefully per the Cold-Strict, Hot-Minimal Validation Law.")

/** Copies a string into a bounded buffer and always terminates nonempty output. */
static fn console_clamp_copy(char *dst, bytes cap, const char *src) {
    if (!dst || cap == 0)
        return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, cap, "%s", src);
}

// CONSTRUCTORS (PUBLIC & PRIVATE)
/** Creates a console configured to use the default shell. */
Console *Console_0() {
    return Console_1("/bin/sh");
}

;;GETTER
static char *Console_shellMut(Console *self) { return (*self).shell; }
;;GETTER
static char *Console_workDirMut(Console *self) { return (*self).workDir; }
;;GETTER
static ConsoleIo *Console_ioMut(Console *self) { return &(*self).io; }
;;GETTER
static const ConsoleIo *Console_ioPtr(const Console *self) { return &(*self).io; }
;;GETTER
static atomic_bool *Console_cancelMut(Console *self) { return &(*self).cancel; }
;;GETTER
static bool Console_running(const Console *self) { return (*self).running; }
;;SETTER
static fn Console_setRunning(Console *self, bool v) { (*self).running = v; }
;;GETTER
static i32 Console_exitStatus(const Console *self) { return (*self).exitStatus; }
;;SETTER
static fn Console_setExitStatus(Console *self, i32 v) { (*self).exitStatus = v; }

;;GETTER
static any ConsoleIo_ctx(const ConsoleIo *io) { return (*io).ctx; }
;;SETTER
static fn ConsoleIo_setCtx(ConsoleIo *io, any ctx) { (*io).ctx = ctx; }
;;GETTER
static bool (*ConsoleIo_spawn(const ConsoleIo *io))(any, const char*, const char*) { return (*io).spawn; }
;;GETTER
static bool (*ConsoleIo_feed(const ConsoleIo *io))(any, const char*, bytes) { return (*io).feed; }
;;GETTER
static bool (*ConsoleIo_reap(const ConsoleIo *io))(any, char*, bytes, bytes*) { return (*io).reap; }
;;GETTER
static fn (*ConsoleIo_cancel(const ConsoleIo *io))(any) { return (*io).cancel; }
;;GETTER
static bool (*ConsoleIo_joined(const ConsoleIo *io))(const_any) { return (*io).joined; }

/** Allocates a stopped console and copies the supplied shell name. */
Console *Console_1(const char *shell) {
    Console *self = (Console*) calloc(1, sizeof(Console));
    if (!self)
        return nullptr;
    console_clamp_copy(Console_shellMut(self), CONSOLE_MAX_SHELL, shell);
    Console_workDirMut(self)[0] = '\0';
    memset(Console_ioMut(self), 0, sizeof(ConsoleIo));
    atomic_init(Console_cancelMut(self), false);
    Console_setRunning(self, false);
    Console_setExitStatus(self, 0);
    return self;
}

// CORE FUNCTIONS (PUBLIC & PRIVATE)
/** Starts a session through the configured spawn callback if one is available. */
bool Console_run(Console *self) {
    if (!self)
        return false;
    if (Console_running(self))
        return false;
    ConsoleIo *io = Console_ioMut(self);
    bool (*spawnFn)(any, const char*, const char*) = ConsoleIo_spawn(io);
    if (!spawnFn)
        return false;

    atomic_store_explicit(Console_cancelMut(self), false, memory_order_relaxed);
    Console_setExitStatus(self, 0);
    bool ok = spawnFn(ConsoleIo_ctx(io), Console_shellMut(self), Console_workDirMut(self));
    Console_setRunning(self, ok);
    return ok;
}

/** Sends a byte span to the active session through its feed callback. */
bool Console_writeInput(Console *self, const char *line, bytes len) {
    if (!self)
        return false;
    if (!Console_running(self))
        return false;
    ConsoleIo *io = Console_ioMut(self);
    bool (*feedFn)(any, const char*, bytes) = ConsoleIo_feed(io);
    if (!feedFn)
        return false;
    return feedFn(ConsoleIo_ctx(io), line, len);
}

/** Drains one output slice and clears running state when the seam reports joined. */
bool Console_poll(Console *self, char *out, bytes outCap, bytes *outLen) {
    if (!self)
        return false;
    if (outLen)
        *outLen = 0;
    if (!Console_running(self)) {
        if (out && outCap > 0)
            out[0] = '\0';
        return true;
    }
    ConsoleIo *io = Console_ioMut(self);
    bool (*reapFn)(any, char*, bytes, bytes*) = ConsoleIo_reap(io);
    if (!reapFn) {
        if (out && outCap > 0)
            out[0] = '\0';
        return true;
    }
    bytes n = 0;
    bool ok = reapFn(ConsoleIo_ctx(io), out, outCap, &n);
    if (outLen)
        *outLen = n;
    bool (*joinedFn)(const_any) = ConsoleIo_joined(io);
    if (joinedFn && joinedFn(ConsoleIo_ctx(io))) {
        Console_setRunning(self, false);
    }
    return ok;
}

/** Sets the cancellation flag and invokes the optional seam cancellation callback. */
fn Console_cancel(Console *self) {
    if (!self)
        return;
    atomic_store_explicit(Console_cancelMut(self), true, memory_order_relaxed);
    ConsoleIo *io = Console_ioMut(self);
    fn (*cancelFn)(any) = ConsoleIo_cancel(io);
    if (cancelFn)
        cancelFn(ConsoleIo_ctx(io));
}

/** Requests cancellation if active, clears the borrowed IO table, and frees self. */
fn Console_free(Console *self) {
    if (!self)
        return;
    if (Console_running(self))
        Console_cancel(self);
    memset(Console_ioMut(self), 0, sizeof(ConsoleIo));
    free(self);
}

/** Returns whether a session is currently active. */
bool Console_isRunning(const Console *self) {
    if (!self)
        return false;
    return Console_running(self);
}

/** Returns the last session exit status, or -1 for a null console. */
i32 Console_getExitStatus(const Console *self) {
    if (!self)
        return -1;
    return Console_exitStatus(self);
}

// SETTERS (PUBLIC & PRIVATE)
;;SETTER
/** Copies the shell string into the console's bounded shell buffer. */
fn Console_setShell(Console *self, const char *shell) {
    if (!self)
        return;
    console_clamp_copy(Console_shellMut(self), CONSOLE_MAX_SHELL, shell);
}

;;SETTER
/** Copies the working directory into the console's bounded path buffer. */
fn Console_setWorkDir(Console *self, const char *workDir) {
    if (!self)
        return;
    console_clamp_copy(Console_workDirMut(self), CONSOLE_MAX_WORKDIR, workDir);
}

;;SETTER
/** Copies the IO callbacks and replaces their context with ioCtx. */
fn Console_setIo(Console *self, const ConsoleIo *io, any ioCtx) {
    if (!self)
        return;
    if (!io) {
        memset(Console_ioMut(self), 0, sizeof(ConsoleIo));
        return;
    }
    ConsoleIo *slot = Console_ioMut(self);
    (*slot) = *io;
    ConsoleIo_setCtx(slot, ioCtx);
}

// GETTERS (PUBLIC & PRIVATE)
;;GETTER
/** Returns the configured shell buffer, or nullptr for a null console. */
const char *Console_getShell(const Console *self) {
    if (!self)
        return nullptr;
    return (*self).shell;
}

;;GETTER
/** Returns the configured working-directory buffer, or nullptr for null. */
const char *Console_getWorkDir(const Console *self) {
    if (!self)
        return nullptr;
    return (*self).workDir;
}

;;GETTER
/** Returns the IO table only when its context is non-null. */
const ConsoleIo *Console_getIo(const Console *self) {
    if (!self)
        return nullptr;
    const ConsoleIo *slot = Console_ioPtr(self);
    return ConsoleIo_ctx(slot) ? slot : nullptr;
}

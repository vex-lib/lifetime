#ifndef HOT_PROCESS_CONSOLE_H
#define HOT_PROCESS_CONSOLE_H

#include "vex.h"

// kernel/console.h — Session state machine for shell scripts / REPL sessions.
//
// Console owns the session lifecycle (shell/workDir, running flag, cancel
// flag, exit status) and borrows execution through an injectable ConsoleIo
// fn-table (the Conflict Triage Law — the R2 ProcessSpawn seam).  The session is polled
// in bounded ≤100ms slices with a cancel flag (the Bounded Wait Law); no threads, no
// sockets, no blocking waits are owned by Console itself.
//
// A terminal-in-a-window is composition — an Application hosting a child
// Console via ProcessSpawn — never a hybrid type.  Console never touches a
// window (keeps darling off its allowlist).
//
// Lifecycle:
//   Console(shell) -> Console_setIo(io, ctx) -> Console_run
//     -> Console_poll (bounded slices) -> Console_cancel -> Console_free

#define CONSOLE_MAX_SHELL   128
#define CONSOLE_MAX_WORKDIR 1024

typedef struct Console Console;

// Injectable execution seam (the Conflict Triage Law managed exception — ProcessSpawn in R2
// implements this).  All fn-pointers are optional: NULL means unimplemented;
// the Console guards every call and degrades (returns false, keeps old state).
typedef struct ConsoleIo {
    any ctx;                                              // borrowed: R2 ProcessSpawn job
    bool (*spawn)(any ctx, const char *shell, const char *workDir);
    bool (*feed)(any ctx, const char *Bytes, bytes len); // write to stdin pipe
    bool (*reap)(any ctx, char *out, bytes outCap,
                 bytes *outLen);                           // non-blocking ≤100ms
    fn (*cancel)(any ctx);                              // SIGTERM; never blocks
    bool (*joined)(const_any ctx);                        // true once child is reaped
} ConsoleIo;

struct Console {
    char shell[CONSOLE_MAX_SHELL];          // command to run (default "/bin/sh")
    char workDir[CONSOLE_MAX_WORKDIR];      // working directory (default "")
    ConsoleIo io;                           // injectable execution seam
    _Atomic bool cancel;                    // cancel flag (the Bounded Wait Law)
    bool running;                           // session active
    int exitStatus;                         // exit code of last session
};

// --- Overloaded constructors ---
//
//   Console()             -> default shell "/bin/sh"
//   Console(shell)        -> named shell
//
Console *Console_0();
Console *Console_1(const char *shell);

#define CONSOLE_CHOOSER(_0, _1, NAME, ...) NAME

#define Console(...) CONSOLE_CHOOSER( \
    dummy __VA_OPT__(,) __VA_ARGS__, \
    Console_1, Console_0 \
)(__VA_ARGS__)

// Free the session.  If still running: cancel first (the Teardown Order Law / the Bounded Wait Law), never blocks.
// Null-safe.
fn Console_free(Console *self);

// --- Core ---
// Start a session: spawn via io.seam.  False if io not set, io.spawn is NULL,
// or spawn itself fails (the Cold-Strict, Hot-Minimal Validation Law cold-seam check).
bool Console_run(Console *self);

// Write a line into the session's stdin pipe.  False if not running or feed
// not implemented.
bool Console_writeInput(Console *self, const char *line, bytes len);

// Non-blocking output drain (≤100ms slice contract, the Bounded Wait Law).  Writes up to
// outCap Bytes into out, sets *outLen to actual Bytes written.  Returns false
// only on null self.  Reads 0 Bytes is a valid (non-error) empty poll.
bool Console_poll(Console *self, char *out, bytes outCap, bytes *outLen);

// Request graceful termination (SIGTERM via io.cancel).  Does not wait;
// the supervising thread calls Console_poll + joined() to observe completion
// per the Bounded Wait Law.
fn Console_cancel(Console *self);

bool     Console_isRunning(const Console *self);
int      Console_getExitStatus(const Console *self);

// --- Setters / Getters (the Symmetric Getter/Setter Completeness Law) ---
// Clamp policy (the Cold-Strict, Hot-Minimal Validation Law): strings that exceed CONSOLE_MAX_SHELL or
// CONSOLE_MAX_WORKDIR are clamped (truncated with NUL); the setter is a
// fn function per house style (Application_setName) and the overview
// documents the clamp behavior.  Callers needing truncation feedback copy
// via snprintf + manual length check against cap.
fn        Console_setShell(Console *self, const char *shell);
const char *Console_getShell(const Console *self);

fn        Console_setWorkDir(Console *self, const char *workDir);
const char *Console_getWorkDir(const Console *self);

// Io seam injection (nullable — NULL disables execution until re-set).
fn          Console_setIo(Console *self, const ConsoleIo *io, any ioCtx);
const ConsoleIo *Console_getIo(const Console *self);

#endif

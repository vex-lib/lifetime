#ifndef LIFETIME_TEST_TYPES_H
#define LIFETIME_TEST_TYPES_H

#include "vex.h"
#include "core/commandline.h"
#include "core/kernel.h"
#include "vexspoke/engine/loop.h"
#include "core/process.h"
#include "window/window.h"
#include <pthread.h>

/**
 * Context for the interactive chrome tour manual test.
 *
 * Window *window - window handle under test
 * Loop loop      - frame loop driving tour steps
 * i32 frames     - total frames elapsed
 */
typedef struct TourCtx {
    Window *window;
    Loop loop;
    i32 frames;
} TourCtx;

/**
 * Single chrome configuration step for the interactive window tour.
 *
 * string label      - human-readable step description
 * bool closable          - enable close button
 * bool miniaturizable    - enable miniaturize button
 * bool resizable         - enable zoom/resize button
 * bool fullscreen_button - enable fullscreen behavior
 * i32 undecorated        - -1 leave unchanged, 0 decorated, 1 undecorated
 */
typedef struct ChromeStep {
    string label;
    bool closable;
    bool miniaturizable;
    bool resizable;
    bool fullscreen_button;
    i32 undecorated;
} ChromeStep;

/**
 * Window event callback recorder for automated window_event tests.
 *
 * Window *lastWindow - most recent window handle passed to a callback
 * i32 fired          - total callbacks invoked
 * i32 resizedW       - last resize width
 * i32 resizedH       - last resize height
 * i32 movedX         - last move X coordinate
 * i32 movedY         - last move Y coordinate
 * i32 movedCalls     - number of move callbacks observed
 * i32 quitCalls      - number of quit callbacks observed
 * bool focused       - last focus state observed
 * i32 occludedCalls  - number of occlusion callbacks observed
 * bool lastVisible   - last visibility flag observed
 */
typedef struct WeRecorder {
    Window *lastWindow;
    i32 fired;
    i32 resizedW;
    i32 resizedH;
    i32 movedX;
    i32 movedY;
    i32 movedCalls;
    i32 quitCalls;
    bool focused;
    i32 occludedCalls;
    bool lastVisible;
} WeRecorder;

/**
 * Fake Console I/O seam recorder for headless console tests.
 *
 * i32 spawns          - spawn invocation count
 * i32 feeds           - feed invocation count
 * i32 reaps           - reap invocation count
 * i32 cancels         - cancel invocation count
 * bool joined         - joined state flag
 * utf8 lastFeed[64]   - last fed input buffer
 * bytes lastFeedLen   - byte length of last fed input
 */
typedef struct FakeConsoleIo {
    i32 spawns;
    i32 feeds;
    i32 reaps;
    i32 cancels;
    bool joined;
    utf8 lastFeed[64];
    bytes lastFeedLen;
} FakeConsoleIo;

/**
 * Fake terminal backend state for CommandLine tests.
 *
 * i32 opens              - open call count
 * i32 closes             - close call count
 * i32 presents           - present call count
 * i32 mouse              - mouse toggle count
 * i32 polls              - poll call count
 * bytes w                - simulated viewport width
 * bytes h                - simulated viewport height
 * bool pending           - true when next event is queued
 * CommandLineEvent next  - queued synthetic event
 */
typedef struct FakeCli {
    i32 opens, closes, presents, mouse, polls;
    bytes w, h;
    bool pending;
    CommandLineEvent next;
} FakeCli;

/**
 * Fake CommandLineApp state for headless CLI seam tests.
 *
 * i32 draws        - draw callback count
 * i32 inputs       - input callback count
 * bool stopOnInput - true to halt the loop on first input
 */
typedef struct FakeCliApp {
    i32 draws, inputs;
    bool stopOnInput;
} FakeCliApp;

/**
 * Worker state for concurrent Process lifecycle tests.
 *
 * Process *process     - process handle under test
 * atomic_bool entered  - set once worker enters execution
 * atomic_bool release  - signals worker to complete
 * thread worker        - worker pthread handle
 * ProcessResult result - captured process return status
 * i32 exitStatus       - captured exit code
 */
typedef struct ProcTestCall {
    Process *process;
    atomic_bool entered;
    atomic_bool release;
    thread worker;
    ProcessResult result;
    i32 exitStatus;
} ProcTestCall;

/**
 * Kernel run-hook context for automated kernel tests.
 *
 * _Atomic i32 counter    - invocation counter
 * _Atomic bool threadRan - true once worker thread ran
 * thread threadId        - captured worker thread id
 */
typedef struct KfRunCtx {
    _Atomic i32 counter;
    _Atomic bool threadRan;
    thread threadId;
} KfRunCtx;

/**
 * Kernel shutdown-hook context for automated kernel tests.
 *
 * _Atomic i32 endCalls   - shutdown callback invocation count
 * Kernel *receivedKernel - kernel pointer delivered to shutdown hook
 */
typedef struct KfEndCtx {
    _Atomic i32 endCalls;
    Kernel *receivedKernel;
} KfEndCtx;

/**
 * Synchronization probe for kernel drain ordering tests.
 *
 * atomic_bool workerDone      - set when worker finishes
 * atomic_bool hookSawDone     - true if shutdown hook observed workerDone
 * atomic_bool hookSawDraining - true if shutdown hook observed draining state
 * atomic_int hookCalls        - number of hook invocations
 */
typedef struct DrainProbe {
    atomic_bool workerDone;
    atomic_bool hookSawDone;
    atomic_bool hookSawDraining;
    atomic_int hookCalls;
} DrainProbe;

/**
 * Condition-variable barrier for concurrent Kernel start tests.
 *
 * pthread_mutex_t lock - mutex guarding gate state
 * pthread_cond_t ready - condvar waking blocked callers
 * bool open            - true once gate is released
 * atomic_int entered   - count of threads waiting at the gate
 * atomic_int calls     - count of completed start calls
 */
typedef struct StartGate {
    pthread_mutex_t lock;
    pthread_cond_t ready;
    bool open;
    atomic_int entered;
    atomic_int calls;
} StartGate;

/**
 * Context for the automated window pump test.
 *
 * Window *window  - window handle under test
 * Loop loop       - frame loop driving the pump
 * i32 frames      - frames rendered so far
 * i32 frameBudget - target frame count before stopping
 */
typedef struct WinPumpCtx {
    Window *window;
    Loop loop;
    i32 frames;
    i32 frameBudget;
} WinPumpCtx;

/**
 * Named test suite entry for the lifetime_test runner.
 *
 * string name - suite identifier string
 * fn (*run)()      - suite entry point function pointer
 */
typedef struct SuiteEntry {
    string name;
    fn (*run)();
} SuiteEntry;

#endif

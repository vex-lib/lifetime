# lifetime — Repo-Local Living Preferences
> Repo-local preferences governed by the Living Documentation Law.
> Universal constitution: `gist.md` is the compressed law list; `PREFERENCES.md` is its rationale.

## 0. Constitution Link (supreme)
- [gist.md](https://gist.github.com/vex-graph/4132a6c45cb6d3797c3e8eff2e94035a#file-gist-md) — the compressed law list, mirrored in this workspace at `../../metadata/gists/preferences/gist.md`. It is the only normative law text; `../../../metadata/gists/preferences/PREFERENCES.md` carries the rationale and is never ingested whole. Numbers are slice addresses inside one file, never law identity: cite by Title.
- All universal laws bind this repository. Read one on demand from the workspace root: `./tools/prefs law "<title>"`, `./tools/prefs section "<section>"`, `./tools/prefs repo <name>`.
- Exclusive `lifetime` (R0 Kernel Host) preferences. R0 owns lifetimes, native OS windowing, process/application lifecycle, capability gating, and memory substrate attestation. R1 comprises Relational Engine memory/storage/cache and Hotcwap dynamic module reloading. R2 comprises Vexspoke CPU computation/behavior. R0 owns residency and hardware ground truth.

## 1. Repo-Local Law Index (Binding Matrix)

Universal laws are inherited from the compressed constitution (`../../metadata/gists/preferences/gist.md`); this table indexes the additional laws specific to this repository.

| Law Title | Scope | Enforcement |
| :--- | :--- | :--- |
| **One-Seam Canvas Window Architecture Law** | R0 Kernel Host | Mandatory for `lifetime` |
| **Continuous Real-Time Live Resize & Presentation Law (Abolishing "Freeze-Exact")** | R0 Kernel Host | Mandatory for `lifetime` |
| **Present-On-Demand Law (composite ≠ render)** | R0 Kernel Host | Mandatory for `lifetime` |
| **R0 Freeze Law (always in, never reloaded)** | R0 Kernel Host | Mandatory for `lifetime` |

## 2. Exclusive Repo-Local Laws (FULL PROSE RESTATEMENT)

### One-Seam Canvas Window Architecture Law

#### Definition:
Exactly ONE on-screen Metal layer belongs to the host composite: seam canvas
(`CAMetalLayer`), beneath optional transparent `NSVisualEffectView` blur.
Vulkan boards (scene backdrop, content UI) are retained OFFSCREEN targets
composited into the seam image by the render repo; ordering is fixed by this
seam canvas, not by per-scene layers — no per-scene/widget `CALayer` or Vulkan
window presentation path.

#### The Why:
One on-screen Metal layer gives the window one frame cadence. Its drawable
tracks live window-content backing size, not a fixed monitor buffer; board
rectangles resolve against that size.

#### The Rule:
1. **Bottom Layer:** window blur substrate; UI widgets own no display layer.
2. **Top Layer:** sole on-screen seam `CAMetalLayer` (`geometryFlipped = YES`,
   TopLeft-pinned, native-pixel `drawableSize`); renderer composites retained
   board targets into it; lifetime owns window and event routing.

---

### Continuous Real-Time Live Resize & Presentation Law (Abolishing "Freeze-Exact")

Freezing canvas extent or early-returning from layout during mouse drags
(`Window_isLiveResizing`) is **strictly abolished**; deferring until "settle"
freezes windows/animations and tears. Render continuously during every drag:

1. **Dynamic Extent:** `CAMetalLayer.drawableSize` tracks the live
   window-content backing bounds on every resize event; resize retained GPU
   targets without creating a Vulkan presentation object.
2. **Live Layout Recalculation = Real-Time Anchor Feel:** Recalculate layout and
   anchors against live bounds each drag frame. Pinned elements (e.g.,
   right-/bottom-anchored) update offsets and re-present, staying at their edges.
   One vsync (60/120Hz) is native granularity: CoreAnimation commits each frame,
   so no content is stale beyond one frame. The sub-frame gap is covered by the
   instantly-moved layer frame (Single-Transaction Live Coordination Law) and
   `gravity = kCAGravityTopLeft` anchor pin.
3. **No Settle Deferral:** Live resize must not wait for mouse-up to resize the
   canvas, clear backbuffers, or reposition anchors. Doing so makes the window
   feel frozen and unresponsive.

---

### Present-On-Demand Law (composite ≠ render)

Windows present when the scene is dirty, not on a timer or fixed poll:

### R0 Freeze Law (always in, never reloaded)

R0 is `lifetime` + `relational-engine`: statically linked, never `dlopen`ed,
never hot-swapped. Arena addresses stay put across every dylib generation, so
modules borrow the same bytes in turn. R0 changes only by versioned cold
release (launcher stages, then `exec`s) with ABI check — no in-process R0
swap, ever. `Kernel → Lifetime → {arenas, clock, relational}`; Kernel owns
start/stop + registries + teardown order, never tick/draw.

`Application` holds reload watchers opaque (`void *hot` + `AppHotPollFn`
seam): R0 never names a hotcwap type. Hotcwap adapts to R0, never the
reverse.

## 3. Current State (Living Documentation Law)

- `lifetime/clock.h/.c` — the R0 ground-truth clock: `Clock_nowNanos()`
  (CLOCK_MONOTONIC_RAW, never backwards) + `Clock_nowMillis()` (unix wall)
  + `Clock_nowSeconds()` (single conversion for module frames). No
  pause/scale. `test/clock/clock_test.c` proves monotonic + range.
- Host owns `Transient_reset`: `appParkSlice` (parked loop) resets per pass
  via the app's bound `hostLifetime`; `Kernel_runAll` reactor resets per
  pass; `kernelGfxAppContinues` (GfxLoop seat) resets per frame. Scratch
  borrowed during a pass dies at its boundary — never across frames/swaps.
  A module stashing a transient pointer in its save blob fails restore.
- `lifetime/platform.h` — the single spelling of the platform: six exact target macros
  (`VEX_PLATFORM_APPLE_SILICON`, `_APPLE_X86_64`, `_LINUX_ARM64`, `_LINUX_X86_64`,
  `_WINDOWS_ARM64`, `_WINDOWS_X86_64`) with coarse axes (`VEX_PLATFORM_APPLE/_WINDOWS/_LINUX`,
  `VEX_ARCH_ARM/_X86`, `VEX_ARCH_ARM64/_X86_64`) derived from them; 32-bit x86/arm
  refused with their own diagnostic. Home is R0, so every tier includes it downward.
- `src/window/window.c` — the sole C owner of the Window API (One File, All Platforms Law):
  Windows branch from the former `window_win32.c`, Wayland branch gated on
  `VEX_PLATFORM_LINUX && VEX_WINDOW_BACKEND_WAYLAND`, Linux X11 branch from the former
  generic stub. `window_win32.c`, `window_linux.c` and `window_wayland.c` no longer exist.
- `src/window/window_cocoa.m` and `src/window/traffic_light_cocoa.m` remain `.m` — the
  only permitted exception (Objective-C cannot be a branch inside a `.c`).
- `src/window/window_event.c` and `src/window/window.h` are unchanged; the `.h` is the seam.

## 4. Scope and Limitations

- The Windows and Linux C branches are not compiled on this host (darwin-arm64); the
  merged file's preprocessor is balanced and its Darwin build passes, but the bodies
  are unproven on those targets.
- The Linux branch still carries the generic stub body and a stale "Win32 backend"
  annotation; a real Linux/Windows compile is required to validate it.
- `tests/lifetime/platform/platform_test.c` proves the platform resolution on
  darwin-arm64 only.

## 5. Readiness Cross-Reference (Living Documentation Law)

- Feature readiness matrix: [lifetime](https://gist.github.com/vex-graph/6943f92acb931b25dad1073c46da6ce7#file-lifetime-md).
- Open blockers and deferred decisions: [ecosystem blockers Gist](https://gist.github.com/vex-graph/e921fa188eebbd0c68c4e59646109887).
- Platform owner test: `tests/lifetime/platform/platform_test.c` (✅, darwin-arm64).
- Window tests: `tests/lifetime/window/window_test.c`, `present_surface_test.c` (✅, darwin-arm64, Cocoa path).

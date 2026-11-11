# lifetime — Repo-Local Living Preferences

## Constitution Link

gist.md (`../../metadata/gists/preferences/gist.md`) is the compressed law list; all universal laws bind this repository. Read one on demand: `./tools/prefs law "<title>"`. Universal laws are inherited, never restated.

Tier: lifetime (R0 Kernel Host). Owns: lifetimes, native OS windowing, process/application lifecycle, capability gating, memory substrate attestation, clock, window API. Borrows: nothing above itself.

## Exclusive Laws

| Law Title | Scope | Enforcement |
| **One-Seam Canvas Window Architecture Law** | R0 Kernel Host | Mandatory for `lifetime` |
| **Continuous Real-Time Live Resize & Presentation Law** | R0 Kernel Host | Mandatory for `lifetime` |
| **Present-On-Demand Law** | R0 Kernel Host | Mandatory for `lifetime` |
| **R0 Freeze Law** | R0 Kernel Host | Mandatory for `lifetime` |

### One-Seam Canvas Window Architecture Law

* Exactly one on-screen Metal layer belongs to the host composite: seam canvas (`CAMetalLayer`), beneath optional transparent `NSVisualEffectView` blur.
* Vulkan boards (scene backdrop, content UI) are retained offscreen targets composited into the seam image by the render repo; ordering fixed by the seam canvas, not per-scene layers.
* No per-scene/widget `CALayer` or Vulkan window presentation path.
* Window blur substrate is the bottom layer; UI widgets own no display layer.
* Sole on-screen seam `CAMetalLayer` (`geometryFlipped = YES`, TopLeft-pinned, native-pixel `drawableSize`) is the top layer; renderer composites retained board targets into it; lifetime owns window and event routing.

### Continuous Real-Time Live Resize & Presentation Law

* Freezing canvas extent or early-returning from layout during mouse drags (`Window_isLiveResizing`) is strictly abolished.
* `CAMetalLayer.drawableSize` tracks the live window-content backing bounds on every resize event; resize retained GPU targets without creating a Vulkan presentation object.
* Recalculate layout and anchors against live bounds each drag frame; pinned elements update offsets and re-present.
* No settle deferral: live resize must not wait for mouse-up to resize the canvas, clear backbuffers, or reposition anchors.

### Present-On-Demand Law

* Windows present when the scene is dirty, not on a timer or fixed poll.

### R0 Freeze Law

* R0 (`lifetime` + `relational-engine`) is statically linked, never `dlopen`ed, never hot-swapped.
* Arena addresses stay put across every dylib generation, so modules borrow the same bytes in turn.
* R0 changes only by versioned cold release (launcher stages, then `exec`s) with ABI check — no in-process R0 swap, ever.
* Kernel owns start/stop + registries + teardown order, never tick/draw.

## Repo-Local Extensions

* None — no managed exceptions to universal laws.

## Readiness Cross-Reference

* Feature readiness: lifetime readiness gist.
* Open blockers: ecosystem blockers gist.
* Proof lawbook: `tests/test-preferences.md` — `./tools/prefs test "<title>"`.
* Evidence ledger: `metadata/gists/test-checklist.md` — `python3 tools/test_checklist.py check`.

# lifetime

R1 host kernel for Vexgraph: native OS windowing, application lifecycle, TTY/process control, monotonic clock, permissions, and hardware capability probes.

Remote: [vex-lib/lifetime](https://github.com/vex-lib/lifetime).

## About

`lifetime` isolates OS-specific host integration into a single R1 layer so upper libraries (`determinant`, `graphvex`, `darling`, and suite apps) do not call Cocoa, Win32, or POSIX platform APIs directly.

## Layout

- **`window/`**, Native OS window creation, event loops, macOS traffic lights, and Win32/Wayland/Linux backends.
- **`core/`**, OS application entry points, `Process`, `Console`, and `CommandLine` TTY handling.
- **`lifetime/`**, Host memory substrate wiring and R0 monotonic `Clock`.
- **`permission/`**, OS and Cocoa permission checks and prompts.
- **`capability/`**, Hardware, display, and OS feature probing.

## Build and test

From the `worktree/` root:

```bash
./tools/b build lifetime
./tools/shell/test.sh lifetime
```

## References

- [`lifetime-preferences.md`](lifetime-preferences.md), Repo-specific engineering rules.
- [`../../../metadata/gists/preferences/gist.md`](../../../metadata/gists/preferences/gist.md), Universal Vexgraph preferences.

## Future

- Reach full parity between the macOS Cocoa window backend and the Linux (Wayland) / Win32 backends.
- Wire every `lifetime` constructor into its dedicated per-library `MemoryArena`.

## Scope and Limitations

- **macOS arm64 proven**: Cocoa windowing, traffic lights, and permissions are the primary tested path. Win32 and Wayland paths are in progress.
- **Host layer only**: `lifetime` owns OS handles and raw events; widget layout, rendering, and terminal UI belong in higher layers (`graphvex`, `determinant`, `darling`).

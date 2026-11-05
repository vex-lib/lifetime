# lifetime

R1 Host Kernel for the Vexgraph ecosystem.

Lifetime owns native OS windowing, process/application lifecycles, command line TTY,
system permissions, hardware capability detection, and memory substrate attestation.

## Structure

- `window/`: Native OS window (`NSWindow`, events, traffic lights, Win32/Wayland/Linux backends).
- `core/`: OS Application entry, Process management, Console, CommandLine TTY.
- `lifetime/`: Lifetime memory substrate + R0 ground-truth Clock (nanos/millis).
- `permission/`: OS permissions backend & Cocoa permissions.
- `capability/`: Hardware and system capability probing.

## Architecture & Preferences

See `lifetime-preferences.md` for exclusive repo-local laws and rules.
Universal constitution: `metadata/gists/preferences/gist.md`.

# mtk-core

The native input core: interception, suppression, pointer synthesis, window
management, and the JSON Lines IPC server the overlay connects to.

## Status

**Interfaces only.** This directory currently contains the headers that define
the seam described in [../docs/SPEC.md](../docs/SPEC.md) sections 6 and 8, plus
the build definition. The implementation is milestones M2-M4:

| Milestone | Scope |
|-----------|-------|
| M2 | Engine, motion integrator, action dispatcher, IPC server, unit tests. Driven by a synthetic input backend -- no OS calls. |
| M3 | Windows backend: `WH_KEYBOARD_LL`, `SendInput`, Win32 windows and monitors. |
| M4 | Linux/X11 backend: evdev grab, uinput, EWMH, XRandR, udev rule. |

The overlay runs today without any of this, on the mock backend
(`mousetrapkeys --backend mock`), and connects to the core automatically once it
starts listening. Nothing in the Python side needs to change when it does.

## Why the engine is a separate library

`mtk_engine` touches no OS API, opens no device and starts no thread. It takes
events and a clock and returns decisions. That is the only way the logic
inherited from the original prototype -- the CapsLock race window, the
forwarded-release invariant -- can be tested without real hardware, and it is
why `mtk_engine_tests` needs no display and no privileges.

## Building

```sh
cmake -S core -B core/build
cmake --build core/build
ctest --test-dir core/build
```

Linux additionally needs `libevdev`, `libx11` and `libxrandr` development
packages.

## The prototype

[`../legacy/kbd_layer.c`](../legacy/kbd_layer.c) is where this started. It is
kept for reference, not built. The ideas that carry forward are documented in
[OUTLINE.md](../docs/OUTLINE.md) section 9.

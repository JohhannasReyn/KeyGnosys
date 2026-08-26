# MouseTrapKeys — Software Outline

> This is the **outline**: what the software is, the pieces it is made of, and why
> the seams fall where they do. The [SPEC](SPEC.md) turns each piece below into
> testable, implementable detail. Read this first.

---

## 1. What it is

MouseTrapKeys is a **keyboard-first desktop input layer** with a **visual on-screen
keyboard that explains itself**.

Two things are happening at once, and the whole design follows from keeping them
separate:

1. **An input layer.** CapsLock stops being CapsLock. While the layer is engaged,
   the alphanumeric keys drive the mouse pointer, click, scroll, jump between
   monitors, and switch between running applications — so the hands never leave
   the home row.
2. **A heads-up map.** A translucent, pinnable, click-through on-screen keyboard
   that lights up as you type and **relabels its own keys to match whatever mode
   you are in**. Base layer shows characters. Shift shows uppercase. Holding Ctrl
   shows the shortcuts *for the app you are actually focused on*. CapsLock shows
   your cursor-control bindings.

The map exists to make the layer learnable. Once it is muscle memory, the map can
be hidden entirely and the layer keeps working. **The overlay is optional; the
input layer is not.**

---

## 2. Design principles

These are the constraints every later decision is measured against.

| # | Principle | Consequence |
|---|-----------|-------------|
| P1 | **Data, not code, defines the keyboard.** | Layouts, bindings, themes and app shortcut packs are JSON files discovered at runtime. Adding a keyboard layout means dropping in a file — never a code change, never a recompile. |
| P2 | **One key vocabulary everywhere.** | A single platform-neutral key identifier is used by layouts, bindings, both OS backends and the UI. No translation tables scattered through the codebase. |
| P3 | **Privilege lives in one small place.** | Only the native core touches raw input devices. The GUI never needs elevation and never runs as root. |
| P4 | **The UI is a viewer, not a source of truth.** | The core owns all state. The overlay renders what it is told. Killing the overlay must not disturb typing. |
| P5 | **Platform differences are backends, not `#ifdef` thickets.** | Each OS capability sits behind an interface with one implementation per platform. Adding a backend never edits the engine. |
| P6 | **Degrade loudly, never silently.** | A malformed layout is skipped with a named diagnostic. An unavailable capability is reported and disabled, not faked. |
| P7 | **Never strand a key down.** | Any suppressed or synthesized key press has a guaranteed matching release, on every exit path — including crash, layer change, and config reload. |

---

## 3. Two processes, one seam

```
┌──────────────────────────────────────────────────────────────────────┐
│  mtk-core        (C++17, native, privileged)                         │
│                                                                      │
│   Input backend  →  Layer engine  →  Action dispatcher               │
│   (evdev/hook)      (state machine)   (cursor · scroll · windows)    │
│                            │                                         │
│                            └──→  Output backend (uinput / SendInput) │
│                                                                      │
│                         IPC server  ◄────────────────┐               │
└─────────────────────────────────────┬────────────────┼───────────────┘
                                      │ events         │ commands
                        JSON Lines over a local socket │
                                      │                │
┌─────────────────────────────────────▼────────────────┴───────────────┐
│  mousetrapkeys   (Python 3.11+ / PySide6, unprivileged)              │
│                                                                      │
│   Core client  →  App state  →  Overlay window   +  Control bar      │
│                        │         (click-through)    (always live)    │
│                        │                                             │
│   Config store · Layout registry · Theme engine · Settings UI        │
└──────────────────────────────────────────────────────────────────────┘
```

**Why two processes and not one.**

- On Linux the input path needs `/dev/uinput` and an exclusive device grab — root
  or a privileged group. Running a Qt GUI with that privilege is unacceptable (P3).
- The core must keep working when the GUI is closed, crashed, or hidden (P4).
  Typing is not allowed to depend on a repaint loop.
- The core is testable headlessly, in CI, with no display server.
- A future alternative front-end (tray-only, CLI, a different toolkit) plugs into
  the same socket without touching input handling.

**Why the seam lands here and not elsewhere.** Everything latency-critical —
interception, suppression, pointer motion — is on the core side of the line. The
IPC carries only *notifications* and *configuration*. A slow or stalled GUI can
never add latency to a keystroke, because no keystroke ever waits on one.

---

## 4. The native core

### 4.1 Backend interfaces

Three capability interfaces, each with one implementation per platform (P5):

| Interface | Responsibility | Linux | Windows | Later |
|-----------|----------------|-------|---------|-------|
| `InputBackend` | Observe and *suppress* physical key events | `libevdev` grab | `WH_KEYBOARD_LL` hook | Interception driver |
| `OutputBackend` | Synthesize keys, pointer motion, buttons, scroll | `uinput` | `SendInput` | — |
| `WindowBackend` | Enumerate/focus windows and monitors, move windows | X11 EWMH | Win32 | Wayland portals |

The Windows input path ships as a **low-level hook**, with the interface shaped so
a signed **Interception driver** backend can be added later without touching the
engine. The hook's known limits (no interception over elevated windows, never over
`Ctrl+Alt+Del`) are documented and surfaced in the UI rather than papered over (P6).

### 4.2 The layer engine

A single state machine owning **mode** and **modifier** state.

- **Modes:** `NORMAL` (keys behave normally) and `CURSOR` (the CapsLock layer).
- **Activation policy** is user-selectable; all three ship:
  - `toggle` — tap CapsLock to latch the layer on and off.
  - `hold` — layer is live only while CapsLock is physically down.
  - `hybrid` — tap latches, hold is momentary. The default.
- **Real CapsLock is not lost.** It moves to a configurable escape gesture
  (default `Shift+CapsLock`).
- The **grace window** from the original prototype is preserved and generalised: a
  mapped key pressed *just before* CapsLock registers is held in limbo for a few
  milliseconds, so near-simultaneous presses resolve as intent rather than as typos.

### 4.3 Action dispatch

Bindings map a key to a named **action**, not to hardcoded behaviour. Action
families:

- **Pointer** — 8-direction movement with an acceleration ramp and a
  hold-to-slow precision modifier.
- **Buttons** — left/right/middle, double-click, and **drag lock** (press to hold
  the button down, move freely, press again to release). Dragging is unusable
  without it.
- **Scroll** — vertical and horizontal, independently tuned.
- **Warp** — jump the pointer instantly to screen regions (3×3 grid, monitor
  corners, next/previous monitor) instead of travelling there.
- **Windows** — cycle focus forward/back, jump directly to a numbered window slot,
  move focus between monitors, and **move the focused window between monitors**.
- **System** — release the layer, reload config, toggle overlay visibility.

### 4.4 Motion model

A fixed ~60 Hz tick integrates held direction keys into pointer displacement.
Speed follows a ramp — slow at first press for precision, accelerating to a cap
while held — so one binding serves both "nudge two pixels" and "cross three
monitors". The precision modifier clamps the ramp to its floor.

---

## 5. The overlay

### 5.1 Two windows, one illusion

Click-through with a still-clickable control bar cannot be done with a single
window on either platform. So there are two, docked and moved together:

- **Keyboard window** — frameless, translucent, always-on-top, and *input-shaped
  to nothing* when click-through is on. Mouse events pass straight through to
  whatever is underneath. This is the map.
- **Control bar** — small, always interactive. Pin, opacity, layout picker,
  visibility, settings. This is the handle.

Turning click-through off simply restores the keyboard window's input region.

### 5.2 Legend layers

The same physical key shows different text depending on state. This is the feature
that makes the overlay teach rather than merely decorate:

| State | What each key shows |
|-------|---------------------|
| Base | The normal character |
| Shift held | The shifted character |
| Modifier held (Ctrl/Alt/…) | **The shortcut for the currently focused application** |
| CapsLock layer engaged | The user's cursor-control binding for that key |

The modifier legend is driven by **app shortcut profiles** matched against the
focused window — Chrome shows Chrome's shortcuts, an image editor shows its own.
Curated packs ship for common applications; each is a plain JSON file the user can
override or replace.

### 5.3 Feedback and theming

Keys light on press and fade on release, in a user-chosen accent colour. Themes are
`dark` / `light` / `system`, defined as colour-token documents rather than
stylesheets, so a new theme is another JSON drop-in (P1).

---

## 6. Data model — the four file kinds

Everything user-facing is one of four JSON document types. Each is versioned by a
`schema` field, validated on load, and skipped-with-a-diagnostic if malformed (P6).

| Kind | Answers | Shipped examples |
|------|---------|------------------|
| **Layout** | Where are the keys, and what shape are they? | `us-105-ansi`, `thinkpad-compact`, `asus-compact` |
| **Bindings** | What does each key do in the cursor layer? | `default` |
| **Theme** | What colours? | `dark`, `light` |
| **App profile** | Which app is this, and what are its shortcuts? | Chrome, VS Code, file manager, terminal |

Bundled files live with the package; user files live in the platform config
directory and **shadow bundled ones by id** — so customising a shipped layout never
means editing an installed file that the next upgrade will overwrite.

---

## 7. Key identity

The single vocabulary of P2 is **W3C UI Events `code` values** — `KeyA`,
`Digit1`, `ShiftLeft`, `CapsLock`, `F1`, `Numpad7`.

They are chosen because they are: already standardised and documented; *physical*
(position-based rather than character-based, so they survive the user's OS keyboard
language); human-readable in a JSON file a person is expected to hand-edit; and
already possessed of published mappings to both Linux evdev codes and Windows
scancodes. Each backend owns exactly one translation table, at the edge.

---

## 8. What ships in v1 — and what does not

**In:** Windows and Linux/X11. The three layouts. All cursor, scroll, warp, window
and monitor actions. All three CapsLock modes. The overlay with its four legend
layers, feedback, themes, click-through, pinning and opacity. Config persistence
and a settings UI. Bundled app profiles with user override.

**Out, deliberately:**

- **Wayland** — the overlay's click-through, global always-on-top, focused-window
  identification and pointer warping are all restricted there by design, and no
  single approach covers GNOME, KDE and wlroots. The backend seam exists; the
  implementation does not. Documented, not faked (P6).
- **macOS** — no backend. The interfaces do not preclude one.
- **Shortcut capture ("learn") mode** — profiles are hand-authored in v1.
- **Layout editor GUI** — layouts are hand-authored JSON in v1.

---

## 9. Relationship to the prototype

`legacy/kbd_layer.c` is the origin of this project and is kept for reference. It
established the ideas that survive into the spec: **grab-and-reinject**, the
**CapsLock race grace window**, **live config reload**, and the
**forwarded-release invariant** (P7) that stops keys sticking down.

What changes: it is Linux-only, hold-only, and knows only four directions at a
fixed speed with a compile-time key cap. The layer engine described above is its
generalisation — same invariants, arbitrary actions, two platforms, and a UI that
can see what it is doing.

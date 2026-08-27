# KeyGnosys — Software Outline

> This is the **outline**: what the software is, the pieces it is made of, and why
> the seams fall where they do. The [SPEC](SPEC.md) turns each piece below into
> testable, implementable detail. Read this first.

---

## 1. What it is

KeyGnosys is an **application-aware keyboard productivity and learning system**.

Its purpose is to make the keyboard shortcuts you *could* be using visible at the
moment you would use them, and to give you somewhere to put the ones you invent.
Everything below follows from that.

It does this through two components, and the whole design follows from keeping
them separate:

1. **A heads-up map.** A translucent, pinnable, click-through on-screen keyboard
   that lights up as you type and **relabels its own keys to match whatever mode
   you are in**. Base layer shows characters. Shift shows uppercase. Holding Ctrl
   shows the shortcuts *for the application you are actually focused on*. This is
   the learning surface, and it is what the product is named for.
2. **An input layer.** CapsLock stops being CapsLock and becomes a configurable
   control layer. While engaged, the alphanumeric keys drive the mouse pointer,
   click, scroll, jump between monitors, and switch between running applications
   — so the hands never leave the home row.

The map makes the layer learnable, and makes shortcuts you already had
discoverable. Once either is muscle memory the map can be hidden entirely and
the layer keeps working. **The overlay is optional; the input layer is not.**

Cursor control is one major capability rather than the whole identity: a user who
never engages the CapsLock layer still gets application-aware shortcut guidance
and live key feedback, and that alone is the point for them.

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
│  keygnosys-core        (C++17, native, privileged)                         │
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
│  keygnosys   (Python 3.11+ / PySide6, unprivileged)              │
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

### 5.4 The layout editor

The same `KeyboardView` that draws the overlay also backs the layout editor, in an
editing mode — two renderers would drift, and a user editing against a picture
that does not match the overlay is being lied to.

See §6.2 for what it does and why it exists.

---

## 6. Data model — the four file kinds

Everything user-facing is one of four JSON document types. Each is versioned by a
`schema` field, validated on load, and skipped-with-a-diagnostic if malformed (P6).

| Kind | Answers | Shipped examples |
|------|---------|------------------|
| **Layout** | Where are the keys, and what shape are they? | `us-ansi-104`, `us-iso-105`, `thinkpad-compact`, `asus-vivobook-s`, `asus-zenbook` |
| **Bindings** | What does each key do in the cursor layer? | `default` |
| **Theme** | What colours? | `dark`, `light` |
| **App profile** | Which app is this, and what are its shortcuts? | Chrome, VS Code, file manager, terminal |

Bundled files live with the package; user files live in the platform config
directory and **shadow bundled ones by id** — so customising a shipped layout never
means editing an installed file that the next upgrade will overwrite.

### 6.1 Logical keys and segments

A layout is a list of **logical keys**, and each is drawn as one or more
rectangular **segments**. Most keys are one segment. An ISO Enter is two, forming
its L-shape.

This matters more than it sounds. The segments are a drawing detail and nothing
else: a logical key has one identity, one label, one highlight, one hit-test
target. Press Enter on an ISO board and the whole L lights up as one key, because
it *is* one key. Segments become individually addressable only inside the layout
editor, and only when the user has deliberately entered segment-edit mode.

Arbitrary SVG paths were rejected. They solve one key shape at the cost of a node
editor, path hit-testing and path overlap detection. Rectangles keep dragging,
snapping, aligning and overlap-checking trivial, and they survive a round-trip
through hand-edited JSON in a way a path string does not.

*(The ISO left Shift, incidentally, is not L-shaped -- it is a short rectangle
with a separate key beside it, and the two are modelled as two logical keys.)*

### 6.2 Editing without JSON

Hand-editing JSON is a fine way to *author* a layout and a poor way to *fix* one.
Since the laptop layouts are representative templates rather than model-exact
reproductions, correcting a board to match the machine in front of you is an
ordinary user task -- so there is a **visual layout editor**: duplicate a
template, drag keys around, resize with handles, snap to a grid, align and
distribute, edit the segments of an L-shaped key, save under your own name, or
reset back to the template.

JSON stays the persistence and interchange format -- exporting a layout produces
a file anyone can drop into their own `layouts/` directory. It simply stops being
the only way in.

The editor introduces **no schema of its own**. It reads and writes exactly the
documents the renderer consumes and the bundled layouts use, which is why
segments, stable ids and metadata are in the format from the start even though
the editor itself arrives later.

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

**Planned, and specified now:**

- **Visual layout editor** (§6.2). The *format* support — segments, stable ids,
  metadata — ships from the start, because a format is expensive to change once
  people have authored against it. The editing UI follows in a later milestone,
  because an editor is additive and can arrive whenever.

**Out, deliberately:**

- **Wayland** — **X11 is the supported Linux display environment for v1.** The
  overlay's click-through, global always-on-top, focused-window identification and
  pointer warping are all restricted under Wayland by design, and no single
  mechanism covers GNOME, KDE and wlroots. The backend seam exists; the
  implementation does not, and no compositor-specific code ships. Documented, not
  faked (P6).
- **macOS** — no backend. The interfaces do not preclude one.
- **Shortcut capture ("learn") mode** — profiles are hand-authored in v1.
- **Arbitrary key shapes** — SVG paths and polygons. Segmented rectangles cover
  every board we support, and cost far less in editor and renderer complexity.
- **Per-device layouts** — all keyboards are accepted as input, but the overlay
  draws the one layout the user selected. A map that redraws itself when the
  hands move between two boards cannot be memorised, which is the whole point.

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

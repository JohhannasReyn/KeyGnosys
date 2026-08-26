# MouseTrapKeys — Technical Specification

**Version:** 1.0-draft · **Status:** design · **Targets:** Windows 10/11, Linux (X11)

Read [OUTLINE.md](OUTLINE.md) first for the shape of the system and the rationale
behind the seams. This document specifies the contracts: file formats, the wire
protocol, the state machine, the action catalog, and the platform backends.

Requirement keywords **MUST**, **SHOULD**, **MAY** are used in the RFC 2119 sense.

---

## Table of contents

1. [Terminology](#1-terminology)
2. [Key identity](#2-key-identity)
3. [Configuration files](#3-configuration-files)
4. [File formats](#4-file-formats)
5. [IPC protocol](#5-ipc-protocol)
6. [Native core](#6-native-core)
7. [Action catalog](#7-action-catalog)
8. [Platform backends](#8-platform-backends)
9. [Overlay UI](#9-overlay-ui)
10. [Diagnostics and failure behaviour](#10-diagnostics-and-failure-behaviour)
11. [Security and permissions](#11-security-and-permissions)
12. [Testing strategy](#12-testing-strategy)
13. [Milestones](#13-milestones)
14. [Open questions](#14-open-questions)

---

## 1. Terminology

| Term | Meaning |
|------|---------|
| **Core** | `mtk-core`, the native C++ process that intercepts and synthesizes input. |
| **Overlay** | The Python/PySide6 GUI process. Comprises the *keyboard window* and the *control bar*. |
| **Cursor layer** | The alternate key mode engaged via CapsLock, in which keys drive the pointer and window manager. |
| **Legend layer** | Which set of labels the overlay is currently drawing (`base`, `shift`, `modifier`, `cursor`). |
| **Binding** | A mapping from a key code to an action, active in the cursor layer. |
| **Profile** | An app-shortcut document, matched against the focused window. |
| **Unit (`u`)** | Layout geometry unit. `1u` is the width of one standard alphanumeric key. |
| **Suppression** | The core consuming a physical key event so the OS never sees it. |

---

## 2. Key identity

### 2.1 The vocabulary

All key references — in layouts, bindings, profiles, and on the wire — **MUST**
use **W3C UI Events `code` values**.

```
Letters      KeyA … KeyZ
Digits       Digit0 … Digit9
Function     F1 … F24
Modifiers    ShiftLeft ShiftRight ControlLeft ControlRight
             AltLeft AltRight MetaLeft MetaRight CapsLock
Whitespace   Space Tab Enter Backspace
Punctuation  Minus Equal BracketLeft BracketRight Backslash
             Semicolon Quote Backquote Comma Period Slash IntlBackslash
Navigation   Insert Delete Home End PageUp PageDown
             ArrowUp ArrowDown ArrowLeft ArrowRight
Numpad       Numpad0 … Numpad9 NumpadAdd NumpadSubtract NumpadMultiply
             NumpadDivide NumpadDecimal NumpadEnter NumLock
System       Escape PrintScreen ScrollLock Pause ContextMenu
Vendor       Fn FnLock  (observed on laptop keyboards; frequently not
                         visible to software — see §8.4)
```

An unknown code string is **not** an error at parse time; it is retained verbatim,
rendered if a layout references it, and reported as a diagnostic if a *binding*
references a code that no loaded layout contains.

### 2.2 Translation

Each input/output backend owns exactly one bidirectional table between the
vocabulary and its native representation. **No other module may hold a key
translation table.**

| Platform | Native representation | Table location |
|----------|----------------------|----------------|
| Linux | evdev `KEY_*` constants (`linux/input-event-codes.h`) | `core/src/platform/linux/evdev_keymap.cpp` |
| Windows | Scancode set 1 (from the hook's `scanCode` + `LLKHF_EXTENDED`) | `core/src/platform/windows/scancode_keymap.cpp` |
| Overlay | Qt `QKeyEvent::nativeScanCode` (mock backend only) | `python/mousetrapkeys/coreclient/mock.py` |

Windows mapping **MUST** key off the *scancode*, not the virtual key code.
Virtual key codes are affected by the user's active keyboard layout; scancodes are
positional, which is what the vocabulary is defined to be.

---

## 3. Configuration files

### 3.1 Locations

| Role | Windows | Linux |
|------|---------|-------|
| **Bundled** (read-only) | `<package>/data/` | `<package>/data/` |
| **User config** | `%APPDATA%\MouseTrapKeys\` | `$XDG_CONFIG_HOME/mousetrapkeys/` (default `~/.config/mousetrapkeys/`) |
| **Runtime/socket** | `\\.\pipe\mousetrapkeys` | `$XDG_RUNTIME_DIR/mousetrapkeys/core.sock` |
| **Logs** | `%LOCALAPPDATA%\MouseTrapKeys\logs\` | `$XDG_STATE_HOME/mousetrapkeys/logs/` |

Both roots contain the same four subdirectories: `layouts/`, `bindings/`,
`themes/`, `profiles/`.

### 3.2 Shadowing rule

Documents are identified by their `id` field, **not** by filename. The registry
loads bundled documents first, then user documents; **a user document with the same
`id` as a bundled one replaces it entirely.** There is no field-level merging —
merging makes it impossible for a user to *remove* something, and makes upgrade
behaviour unpredictable.

Consequence: customising a shipped layout means copying it to the user directory
and editing the copy. An upgrade that changes the bundled original will not
silently overwrite the user's work, and will not silently take effect either. The
settings UI **MUST** offer a one-click "fork this to my config" action.

### 3.3 Live reload

The user config directory **SHOULD** be watched. On change, the affected registry
reloads and emits `bindings_changed` / `layouts_changed` over IPC. A reload that
produces zero valid documents **MUST** keep the previous set and emit a diagnostic
(inherited from the prototype's behaviour).

Reloading bindings **MUST** first release every held key and every locked drag
button (P7).

---

## 4. File formats

Every document carries a `schema` field of the form
`mousetrapkeys/<kind>/<major>`. A consumer **MUST** reject a document whose major
version it does not recognise, with a diagnostic naming the file.

### 4.1 Layout

Describes physical key geometry. Geometry is expressed in **units** from the
top-left corner. All keys are axis-aligned rectangles in v1 (see §14).

```jsonc
{
  "schema": "mousetrapkeys/layout/1",
  "id": "us-ansi-104",
  "name": "US Full-Size (ANSI 104)",
  "description": "Standard US full-size keyboard.",
  "size": { "w": 22.5, "h": 6.5 },     // total extent in units
  "keys": [
    {
      "code": "Escape",                 // §2.1 vocabulary
      "x": 0, "y": 0,                   // top-left, in units
      "w": 1, "h": 1,                   // defaults 1, 1
      "legend": {
        "base": "Esc",                  // required
        "shift": null,                  // optional; falls back to base
        "sub": null                     // optional small corner text
      },
      "role": "system"                  // see below; default "normal"
    }
  ]
}
```

**`role`** informs rendering and hit-testing, not behaviour:

| role | Meaning | Rendering hint |
|------|---------|----------------|
| `normal` | Ordinary character key | Standard |
| `modifier` | Shift/Ctrl/Alt/Meta/Fn | Dimmed until held, then latched-bright |
| `system` | Esc, F-keys, PrtSc | Standard, smaller label |
| `nav` | Arrows, Home/End/PgUp/PgDn | Standard |
| `numpad` | Numpad cluster | Grouped, may be hidden by a setting |
| `toggle` | CapsLock, NumLock, ScrollLock | Renders an LED dot |

**Validation.** A layout is rejected if: `schema` major is unknown; `id` is
missing or not `^[a-z0-9][a-z0-9-]*$`; `keys` is empty; any key lacks `code` or
`legend.base`; or two keys with the same `code` overlap geometrically. Duplicate
`code` values are otherwise **permitted** — `ShiftLeft` and `ShiftRight` are
distinct codes, but a layout may legitimately draw e.g. two `Fn` keys.

**Adding a layout** is: write the file, drop it in `layouts/`, restart or wait for
the watcher. No code change. This is principle P1 and is a hard requirement — any
change that makes a new layout require a code edit is a spec violation.

### 4.2 Bindings

Maps key codes to cursor-layer actions.

```jsonc
{
  "schema": "mousetrapkeys/bindings/1",
  "id": "default",
  "name": "Default cursor layer",
  "settings": {
    "pointer_base_speed": 2,        // px per tick at ramp floor
    "pointer_max_speed": 28,        // px per tick at ramp ceiling
    "pointer_ramp_ms": 420,         // time from floor to ceiling
    "precision_factor": 0.25,       // multiplier while precision held
    "scroll_base_speed": 1,         // notches per tick
    "scroll_max_speed": 6,
    "scroll_ramp_ms": 500
  },
  "bindings": {
    "KeyH": { "action": "pointer.move",  "params": { "dir": "left"  } },
    "KeyJ": { "action": "pointer.move",  "params": { "dir": "down"  } },
    "KeyK": { "action": "pointer.move",  "params": { "dir": "up"    } },
    "KeyL": { "action": "pointer.move",  "params": { "dir": "right" } },
    "Space":{ "action": "button.click",  "params": { "button": "left" },
              "legend": "Click" }
  }
}
```

- `action` **MUST** name an action from §7. Unknown actions are skipped with a
  diagnostic; the rest of the document still loads.
- `params` are validated per-action; an invalid param set skips that one binding.
- `legend` is optional display text. When absent, the overlay derives one from the
  action catalog's default legend.
- Diagonal movement is **not** a binding. It emerges from two direction keys held
  simultaneously, as vectors sum in the motion integrator (§6.4).

### 4.3 Theme

Colour tokens, not stylesheets — so themes stay renderer-agnostic.

```jsonc
{
  "schema": "mousetrapkeys/theme/1",
  "id": "dark",
  "name": "Dark",
  "base": "dark",                    // "dark" | "light" — used by "system" mode
  "tokens": {
    "surface":        "#12141aE6",   // #RRGGBBAA; alpha is multiplied by the
    "key_face":       "#1e2129F2",   //   user's global opacity setting
    "key_face_alt":   "#171a21F2",   // modifier/system keys
    "key_border":     "#2c313d",
    "key_text":       "#e6e9ef",
    "key_text_dim":   "#8b93a7",
    "key_sub_text":   "#6b7285",
    "accent":         "#4c9aff",     // default feedback colour
    "accent_text":    "#0b0d12",
    "latched":        "#f0a020",     // held modifier / latched layer
    "led_on":         "#37d67a",
    "led_off":        "#333845",
    "bar_surface":    "#181b22F2",
    "bar_text":       "#e6e9ef"
  }
}
```

The user's **accent colour** setting overrides `tokens.accent` when set. Theme
mode `system` selects the bundled theme whose `base` matches the OS preference,
re-evaluated when the OS reports a change.

### 4.4 App profile

Identifies an application and supplies its shortcut legends.

```jsonc
{
  "schema": "mousetrapkeys/profile/1",
  "id": "chrome",
  "name": "Google Chrome",
  "priority": 50,                        // higher wins ties; default 50
  "match": {
    "process": ["chrome.exe", "chrome", "google-chrome", "google-chrome-stable"],
    "wm_class": ["Google-chrome", "Chromium"],   // X11 only
    "title_regex": null
  },
  "shortcuts": {
    "Control": {                         // modifier combination, see below
      "KeyT": "New tab",
      "KeyW": "Close tab",
      "KeyL": "Address bar"
    },
    "Control+Shift": {
      "KeyT": "Reopen tab",
      "KeyN": "Incognito"
    },
    "Alt": {
      "ArrowLeft": "Back",
      "ArrowRight": "Forward"
    }
  }
}
```

**Modifier combination keys** are a `+`-joined, canonically ordered string drawn
from `Control`, `Alt`, `Shift`, `Meta` — in exactly that order. The overlay
computes the currently-held combination in the same canonical order and looks it
up directly. `Shift` alone is **never** a shortcut combination; a lone Shift
selects the `shift` legend layer instead.

**Matching.** The core reports the focused window's process name, WM class (X11)
and title. The overlay selects the highest-`priority` profile where *any*
`match` field matches. Comparison is case-insensitive. On no match, the modifier
legend layer shows the base legend dimmed rather than showing nothing — an empty
keyboard reads as a bug.

### 4.5 Settings

Single document, user config root, written by the settings UI.

```jsonc
{
  "schema": "mousetrapkeys/settings/1",
  "appearance": {
    "theme": "system",              // "dark" | "light" | "system" | <theme id>
    "accent": null,                 // "#RRGGBB" overrides theme accent
    "opacity": 0.85,                // 0.15 – 1.0, global multiplier
    "scale": 1.0,                   // 0.4 – 2.0, units-to-pixels
    "show_numpad": true,
    "feedback_fade_ms": 220,
    "overlay_visible": true,
    "position": { "x": null, "y": null }   // null = bottom-centre of primary
  },
  "behavior": {
    "activation_mode": "hybrid",    // "toggle" | "hold" | "hybrid"
    "hybrid_tap_ms": 200,           // press shorter than this = tap/latch
    "grace_ms": 50,                 // CapsLock race window (§6.3)
    "real_capslock_gesture": "Shift+CapsLock",   // or "double-tap" | "none"
    "layout": "us-ansi-104",
    "bindings": "default",
    "start_minimized": false
  },
  "window": {
    "pinned": true,                 // always-on-top
    "click_through": true
  }
}
```

Unknown keys are preserved on rewrite (forward compatibility). Out-of-range values
are clamped, and the clamp is reported as a diagnostic.

---

## 5. IPC protocol

### 5.1 Transport

Newline-delimited JSON (**JSON Lines**), UTF-8, one message per line. Chosen over
a binary protocol because the message rate is low (key events at human typing
speed), and because a developer being able to `nc`/`cat` the socket during
debugging is worth more than the bytes saved.

| Platform | Endpoint | Access control |
|----------|----------|----------------|
| Linux | `$XDG_RUNTIME_DIR/mousetrapkeys/core.sock` (`AF_UNIX`, `SOCK_STREAM`) | Socket mode `0600`, owner-only |
| Windows | `\\.\pipe\mousetrapkeys` (named pipe, message mode) | DACL restricted to the creating user |

The core is the server and **MUST** accept multiple concurrent clients. Events are
broadcast to all; replies go only to the requesting client.

A client that stops reading **MUST NOT** block the core. Each client has a bounded
outbound queue (default 256 messages); on overflow the core drops the oldest
*event* messages, emits one `diagnostic`, and keeps running. Replies are never
dropped.

### 5.2 Envelope

```jsonc
{ "v": 1, "t": "event",   "n": "key",      "seq": 1042, "d": { … } }
{ "v": 1, "t": "command", "n": "set_mode", "id": "c17",  "d": { … } }
{ "v": 1, "t": "reply",   "id": "c17", "ok": true,  "d": { … } }
{ "v": 1, "t": "reply",   "id": "c17", "ok": false, "e": { "code": "…", "message": "…" } }
```

`seq` increases monotonically per connection, letting a client detect dropped
events.

### 5.3 Events (core → overlay)

| `n` | Payload | Emitted when |
|-----|---------|--------------|
| `hello` | `{core_version, protocol, platform, backends:{input,output,window}, capabilities:[…], limitations:[…]}` | Immediately on connect |
| `key` | `{code, state:"down"\|"up"\|"repeat", suppressed:bool}` | Every physical key event, suppressed or not |
| `modifiers` | `{shift, control, alt, meta, caps_layer}` — all bool | Any modifier state change |
| `mode` | `{mode:"normal"\|"cursor", latched:bool, activation:"toggle"\|"hold"\|"hybrid"}` | Layer engaged/released |
| `focus` | `{app_id, process, wm_class, title, window_id}` | Focused window changes |
| `windows` | `{slots:[{index, window_id, process, title, monitor}]}` | Window list changes (debounced ≥250 ms) |
| `monitors` | `{monitors:[{index, x, y, w, h, primary, name}]}` | Monitor topology changes |
| `pointer` | `{x, y, monitor}` | Only while cursor layer is engaged, ≤20 Hz |
| `drag_lock` | `{button, active}` | Drag lock engaged/released |
| `config_changed` | `{kinds:["bindings","layouts",…]}` | Core reloaded config |
| `diagnostic` | `{level:"info"\|"warn"\|"error", code, message, file?}` | §10 |
| `shutdown` | `{reason}` | Core is exiting |

`key` events are emitted for **all** keys including suppressed ones — this is what
makes visual feedback work in the cursor layer, where no character reaches the OS.

The core **MUST NOT** include character/text content in `key` events. Only
positional codes are transmitted. See §11.

### 5.4 Commands (overlay → core)

| `n` | Payload | Reply |
|-----|---------|-------|
| `get_state` | `{}` | Full snapshot: mode, modifiers, focus, windows, monitors |
| `set_activation_mode` | `{mode}` | `{}` |
| `set_enabled` | `{enabled:bool}` | `{}` — master switch; disables interception entirely |
| `reload_config` | `{}` | `{loaded:{layouts:n, bindings:n, profiles:n}}` |
| `set_bindings` | `{id}` | `{}` |
| `set_setting` | `{path, value}` | `{}` — dotted path into §4.5 `behavior` |
| `release_all` | `{}` | `{}` — panic button: release every held key, button and drag lock |
| `ping` | `{}` | `{pong:true, uptime_ms}` |

### 5.5 Versioning

`v` is the envelope version; `hello.protocol` is the semantic protocol version.
A client **MUST** refuse to operate against a core whose `protocol` major differs
from its own and **MUST** surface that as a user-visible error naming both
versions — a silent partial-compatibility mode is worse than a clear refusal.

---

## 6. Native core

### 6.1 Module layout

```
core/
├── include/mtk/
│   ├── keycode.hpp          Key vocabulary + code<->string
│   ├── input_backend.hpp    InputBackend interface
│   ├── output_backend.hpp   OutputBackend interface
│   ├── window_backend.hpp   WindowBackend interface
│   ├── layer_engine.hpp     Mode/modifier state machine
│   ├── motion.hpp           Pointer/scroll integrator
│   ├── actions.hpp          Action catalog + dispatch
│   ├── config.hpp           Bindings/settings loading
│   └── ipc.hpp              JSON Lines server
└── src/
    ├── platform/linux/      evdev_input, uinput_output, x11_window
    └── platform/windows/    hook_input, sendinput_output, win32_window
```

### 6.2 Interfaces

```cpp
class InputBackend {
public:
  // Returns true if the event should be suppressed (not passed to the OS).
  using Handler = std::function<bool(KeyCode, KeyState)>;
  virtual ~InputBackend() = default;
  virtual bool start(Handler) = 0;
  virtual void stop() = 0;
  virtual Capabilities capabilities() const = 0;
};

class OutputBackend {
public:
  virtual void moveCursorBy(int dx, int dy) = 0;
  virtual void moveCursorTo(int x, int y) = 0;
  virtual Point cursorPosition() = 0;
  virtual void button(MouseButton, bool down) = 0;
  virtual void scroll(int dx, int dy) = 0;
  virtual void sendKey(KeyCode, bool down) = 0;
  virtual void releaseAll() = 0;             // P7
};

class WindowBackend {
public:
  virtual std::vector<WindowInfo> windows() = 0;
  virtual std::optional<WindowInfo> focused() = 0;
  virtual bool focus(WindowId) = 0;
  virtual std::vector<MonitorInfo> monitors() = 0;
  virtual bool moveWindowToMonitor(WindowId, int monitorIndex) = 0;
};
```

The engine depends only on these. Adding a backend **MUST NOT** require editing
any file outside `src/platform/<os>/` plus one line in the factory.

### 6.3 Layer engine state machine

States: `NORMAL`, `CURSOR`. Plus a per-key sub-state for the grace window.

**CapsLock handling by activation mode:**

| Mode | CapsLock down | CapsLock up |
|------|---------------|-------------|
| `hold` | → `CURSOR` | → `NORMAL`, release all |
| `toggle` | Toggle `NORMAL`↔`CURSOR` | (ignored) |
| `hybrid` | → `CURSOR`, start tap timer | If held < `hybrid_tap_ms` → latch `CURSOR`. Else → `NORMAL`, release all. |

In `hybrid` and `toggle`, a latched `CURSOR` is released by tapping CapsLock again
or by any binding with action `layer.release`.

**Real CapsLock.** When `real_capslock_gesture` is `Shift+CapsLock`, a CapsLock
press while Shift is physically held is not a layer gesture: the core synthesizes
a genuine CapsLock press/release to the OS and does not change mode.

**The grace window (inherited from the prototype).** A key bound in the cursor
layer, pressed while in `NORMAL`, is *ambiguous* — the user may be typing it, or
may be a few milliseconds ahead of the CapsLock that was meant to precede it.
Such a press is **buffered**, not forwarded, for up to `grace_ms`. It resolves as:

| Event within the window | Resolution |
|-------------------------|------------|
| CapsLock arrives | Promote to a cursor-layer press. Nothing reaches the OS. |
| The key is released | Ordinary tap. Forward press+release together, in order. |
| `grace_ms` elapses | Ordinary hold. Forward the press, mark **passthrough**. |

**The passthrough invariant (P7).** A key whose press was forwarded to the OS is
flagged. Its release **MUST** also be forwarded, unconditionally, regardless of
what the mode has become in the meantime. Violating this leaves a key stuck down
in the compositor. This invariant applies equally to: mode changes, config
reloads, `release_all`, client disconnects, and process shutdown — every exit path
**MUST** run `OutputBackend::releaseAll()`.

Buffering adds up to `grace_ms` of latency, but **only** to keys that are bound in
the cursor layer, and **only** when they are typed while the layer is off. Keys
with no cursor-layer binding are never buffered and never delayed.

### 6.4 Motion integrator

A monotonic timer ticks at **60 Hz** (16.67 ms). Each tick:

1. Sum the unit direction vectors of all currently-held `pointer.move` bindings.
   This is where diagonals come from — no diagonal binding exists.
2. Compute ramp position `p = clamp(held_ms / pointer_ramp_ms, 0, 1)`, eased
   (`p² `, quadratic-in) so the first moments are precise and the tail is fast.
3. `speed = base + (max - base) · p`, then `× precision_factor` if a
   `pointer.precision` binding is held.
4. Normalise the direction vector so a diagonal is not √2 times faster than a
   cardinal.
5. Accumulate fractional pixels across ticks and emit only whole-pixel deltas —
   otherwise slow speeds truncate to zero and the pointer never moves.

Ramp time is measured from the **first** direction key pressed in a continuous
motion, not per key, so changing direction mid-travel does not reset to a crawl.

Scroll uses the same structure with its own settings and its own accumulator.

### 6.5 Window slots

`windows` slot ordering **MUST** be stable across emissions, or the number-key
bindings become unusable. Ordering rule:

1. Windows are keyed by their OS window id.
2. A window keeps its slot index for as long as it exists.
3. A new window takes the lowest free index.
4. Indices are 1-based; the event carries at most 9 slots (mapped to `Digit1`…
   `Digit9`); additional windows are reported without an index.

Reordering by recency would be more "useful" and is explicitly rejected: a map
that changes under the user's fingers cannot be learned, which defeats the entire
purpose of the overlay.

---

## 7. Action catalog

Every action, its parameters, and its default legend. This table is the contract
between bindings files, the dispatcher, and the overlay's legend renderer.

### 7.1 Pointer

| Action | Params | Default legend | Behaviour |
|--------|--------|----------------|-----------|
| `pointer.move` | `dir`: `up`\|`down`\|`left`\|`right` | `◀ ▶ ▲ ▼` | Held; contributes to the motion integrator (§6.4) |
| `pointer.precision` | — | `Slow` | While held, multiply pointer and scroll speed by `precision_factor` |

### 7.2 Buttons

| Action | Params | Default legend | Behaviour |
|--------|--------|----------------|-----------|
| `button.click` | `button`: `left`\|`right`\|`middle` | `Click` / `R-Click` / `M-Click` | Press on key-down, release on key-up (so click-and-hold works naturally) |
| `button.double_click` | `button` | `Dbl Click` | Two press/release pairs, separated by the OS double-click interval |
| `button.drag_lock` | `button` | `Drag` | Toggle. First press holds the button down; second releases. Emits `drag_lock`. **MUST** auto-release on layer exit (P7) |

### 7.3 Scroll

| Action | Params | Default legend | Behaviour |
|--------|--------|----------------|-----------|
| `scroll.scroll` | `dir`: `up`\|`down`\|`left`\|`right` | `Scroll ▲` etc. | Held; ramped like pointer motion |
| `scroll.page` | `dir`: `up`\|`down` | `Page ▲` | One large discrete scroll per press |

### 7.4 Warp

| Action | Params | Default legend | Behaviour |
|--------|--------|----------------|-----------|
| `warp.grid` | `cell`: 1–9 | `⌗1`…`⌗9` | Jump to the centre of that cell of a 3×3 grid over the **current** monitor |
| `warp.corner` | `corner`: `tl`\|`tr`\|`bl`\|`br`\|`center` | `↖ ↗ ↙ ↘ ⊙` | Jump to that point of the current monitor |
| `warp.monitor` | `target`: `next`\|`prev`\|integer | `Mon ▶` | Move the pointer to the centre of that monitor |

### 7.5 Windows

| Action | Params | Default legend | Behaviour |
|--------|--------|----------------|-----------|
| `window.cycle` | `dir`: `next`\|`prev` | `App ▶` / `App ◀` | Focus the next/previous window in slot order |
| `window.slot` | `index`: 1–9 | Slot's app name | Focus the window in that slot |
| `window.focus_monitor` | `target`: `next`\|`prev`\|integer | `Screen ▶` | Focus the topmost window on that monitor, and warp the pointer there |
| `window.move_to_monitor` | `target`: `next`\|`prev`\|integer | `Send ▶` | Move the **focused window** to that monitor, preserving relative position and restoring from maximised as needed |

### 7.6 System

| Action | Params | Default legend | Behaviour |
|--------|--------|----------------|-----------|
| `layer.release` | — | `Exit` | Leave the cursor layer, release everything |
| `overlay.toggle` | — | `Map` | Show/hide the keyboard window |
| `system.reload` | — | `Reload` | Reload configuration |
| `key.passthrough` | `code` | The key's own legend | Send a literal key even while the layer is engaged — the escape hatch for keys you still need in the layer |

Unknown action names are a **binding-level** failure, never a document-level one:
the binding is skipped, a diagnostic names the file and the key, and every other
binding in the file still loads.

---

## 8. Platform backends

### 8.1 Linux input — evdev grab

Open the keyboard `/dev/input/event*` node, `libevdev_grab(LIBEVDEV_GRAB)` for
exclusive access, and re-inject non-suppressed events through a `uinput` virtual
device. This is the prototype's proven approach.

- Device selection: auto-detect nodes advertising `EV_KEY` with `KEY_A` and
  `KEY_Z`, preferring `/dev/input/by-path/*-event-kbd`. Overridable by config and
  by CLI flag. Multiple keyboards **SHOULD** all be grabbed.
- The virtual device **MUST** advertise every `EV_KEY` code the real device has,
  plus `EV_REL` `REL_X`/`REL_Y`/`REL_WHEEL`/`REL_HWHEEL` and the mouse buttons.
- Requires membership in the `input` group and write access to `/dev/uinput`. A
  shipped udev rule grants this without running as root.
- Hot-plug: watch for device add/remove via udev and re-grab.

### 8.2 Windows input — low-level hook

`SetWindowsHookExW(WH_KEYBOARD_LL, …)` on a dedicated thread with its own message
pump. Returning non-zero from the hook proc suppresses the event.

**Hard constraints, to be surfaced in `hello.limitations` and in the UI:**

- The hook proc **MUST** return within the `LowLevelHooksTimeout` window
  (default 300 ms) or Windows silently unhooks it. Therefore the hook proc
  **MUST** do nothing but decide suppression and enqueue; all dispatch, IPC and
  logging happen on other threads.
- The core **MUST** detect having been unhooked and re-install automatically.
- Keys cannot be intercepted while an **elevated** window has focus unless the
  core itself runs elevated. Running unelevated is the default; the UI states
  plainly when the layer is inert for this reason.
- `Ctrl+Alt+Del` and the Secure Attention Sequence are **never** interceptable.
  This is not a bug and **MUST NOT** be presented as one.
- Injected events (`LLKHF_INJECTED`) **MUST** be ignored, or the core's own
  `SendInput` output feeds back into its hook.

**Driver seam.** `InputBackend` is deliberately narrow so an
`InterceptionInputBackend` can be added later. It **MUST NOT** be a build-time
requirement, and the app **MUST** run fully without it.

### 8.3 Output

| | Linux | Windows |
|-|-------|---------|
| Pointer motion | `uinput` `EV_REL` | `SendInput` `MOUSEEVENTF_MOVE` |
| Absolute warp | `EV_ABS` on a second virtual device, or `XWarpPointer` | `MOUSEEVENTF_ABSOLUTE\|VIRTUALDESK`, normalised to 0–65535 across the virtual desktop |
| Buttons | `EV_KEY` `BTN_LEFT`… | `MOUSEEVENTF_*DOWN/UP` |
| Scroll | `REL_WHEEL` / `REL_HWHEEL` | `MOUSEEVENTF_WHEEL` / `HWHEEL`, `WHEEL_DELTA` units |

Windows absolute positioning across multiple monitors **MUST** use
`MOUSEEVENTF_VIRTUALDESK` and normalise against `SM_XVIRTUALSCREEN` /
`SM_CXVIRTUALSCREEN` — normalising against the primary monitor alone is the
classic bug that makes warp land on the wrong screen.

### 8.4 Window management

**Windows.** `EnumWindows` filtered to visible, non-tool, non-cloaked top-level
windows (`DWMWA_CLOAKED` **MUST** be checked, or UWP ghost windows appear in the
slot list). Focus via `SetForegroundWindow` with the standard
`AttachThreadInput` workaround for foreground lock. Monitors via
`EnumDisplayMonitors` / `GetMonitorInfo`. Moving a window between monitors:
restore if maximised, `SetWindowPos` to the scaled relative position on the target
monitor, re-maximise if it was maximised. **MUST** account for per-monitor DPI.

**Linux/X11.** EWMH throughout: `_NET_CLIENT_LIST` for enumeration,
`_NET_ACTIVE_WINDOW` for focus (via a client message, not `XSetInputFocus`, so the
WM cooperates), `_NET_WM_NAME` / `WM_CLASS` for identification. Monitors via
XRandR. Moving between monitors: remove `_NET_WM_STATE_MAXIMIZED_*`, `XMoveWindow`
accounting for frame extents (`_NET_FRAME_EXTENTS`), restore the state.

**Fn keys.** On most laptops — ThinkPad and Asus included — the `Fn` key is
handled in keyboard firmware and never produces a scancode the OS can see. The
layouts render it for visual fidelity; the core **MUST NOT** promise to bind or
report it, and the UI **SHOULD** show it as permanently unbindable rather than
letting the user assign an action that will never fire.

---

## 9. Overlay UI

### 9.1 Windows and flags

**Keyboard window** — `Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint |
Qt.Tool`, `WA_TranslucentBackground`, `WA_ShowWithoutActivating`.
`Qt.Tool` keeps it out of the taskbar and the Alt-Tab list, which matters because
this app's whole job is to make Alt-Tab-like switching legible.

The window **MUST NOT** take focus. `WA_ShowWithoutActivating` plus, on Windows,
`WS_EX_NOACTIVATE`.

**Control bar** — same flags minus click-through, docked to the keyboard window's
top edge and moved with it. Dragging it moves both. It is the only surface that
accepts mouse input while click-through is on.

### 9.2 Click-through

Whole-window pass-through, toggled at runtime:

| Platform | Mechanism |
|----------|-----------|
| Windows | Add `WS_EX_LAYERED \| WS_EX_TRANSPARENT` via `SetWindowLongPtrW(GWL_EXSTYLE)`. Remove `WS_EX_TRANSPARENT` to restore. |
| X11 | `XShapeCombineRectangles(dpy, win, ShapeInput, 0, 0, nullptr, 0, ShapeSet, Unsorted)` — an empty input region. Restore with a full-window rectangle. |

Both are reached through `ui/platform/clickthrough.py`, which exposes
`set_click_through(window, enabled) -> bool`. It returns `False` on a platform
where it cannot be done; the caller **MUST** then disable the toggle in the UI and
show why, rather than presenting a control that silently does nothing (P6).

Per-widget click-through (`WA_TransparentForMouseEvents`) is **not** sufficient —
it routes events within the Qt window, but the OS still delivers them to that
window rather than to the application beneath. This is the entire reason for the
two-window design.

### 9.3 Rendering

The keyboard is drawn by a **single custom-painted widget**, not 105 child
widgets. Rationale: repaint cost on every keystroke, uniform theming, and
sub-pixel geometry from the units model.

- Layout units → pixels via `scale × base_key_px` (base 44 px at scale 1.0).
- Each key: rounded rect, face + border from theme tokens, centred legend,
  optional `sub` text in the top-right corner, optional LED dot for `toggle` role.
- Painted in one pass with clipping to the damaged region; a key press repaints
  only that key's rect plus its glow margin.
- Hit-testing exists only for the non-click-through mode (used by the binding
  editor, where clicking a key assigns it).

### 9.4 Legend resolution

For each key, in order — the first rule that produces text wins:

1. **Cursor layer engaged** → the binding's `legend`, else the action catalog's
   default legend, else blank-and-dimmed for unbound keys.
2. **A non-Shift modifier is held** → look up the canonical modifier combination
   (§4.4) in the active profile. On a hit, show the shortcut description. On a
   miss, show the base legend **dimmed**.
3. **Shift held** → `legend.shift` if present, else `legend.base` upper-cased.
4. Otherwise → `legend.base`.

Long shortcut descriptions are elided with `…` and shown in full on hover — but
only when click-through is off, since hover is impossible otherwise.

### 9.5 Feedback

On `key` down: that key enters a highlight state, painted with the accent colour.
On `key` up: it fades out over `feedback_fade_ms` on a shared animation timer.
Suppressed keys are highlighted identically — in the cursor layer nothing reaches
the OS, so the overlay is the *only* feedback the user gets.

Modifier keys held and latched toggles use the `latched` token rather than the
fade animation, since they persist.

### 9.6 The mock backend

`coreclient/mock.py` implements the same client interface as the real IPC client
but sources events from Qt's own key handling in the overlay window. It exists so
the UI can be developed, demoed and tested with no native core, no elevation and
no platform-specific anything — including in CI.

Its limits are inherent and **MUST** be stated in the UI when active: it sees keys
only while the overlay has focus, it cannot suppress anything, and it has no
window or monitor information. It is a development tool, not a degraded mode of
the product.

---

## 10. Diagnostics and failure behaviour

Every diagnostic carries a stable machine-readable `code`.

| Code | Level | Meaning |
|------|-------|---------|
| `layout.invalid` | warn | Layout file failed validation; skipped |
| `layout.duplicate_id` | warn | Two documents claim one id; later wins |
| `binding.unknown_action` | warn | Binding skipped |
| `binding.unknown_key` | warn | Binding references a code absent from the active layout |
| `profile.invalid` | warn | Profile skipped |
| `config.clamped` | info | A setting was outside its range |
| `input.permission_denied` | error | Cannot open the input device / install the hook |
| `input.hook_lost` | warn | Windows unhooked us; re-installing |
| `input.elevated_window` | info | Interception inert; an elevated window has focus |
| `window.unsupported` | warn | A window operation is unavailable on this backend |
| `ipc.client_overflow` | warn | A client's queue overflowed; events dropped |
| `ipc.version_mismatch` | error | Client and core protocol majors differ |

**The governing rule (P6):** one bad file never prevents the rest from loading,
and a missing capability is always reported and disabled — never emulated with
something that merely looks similar.

---

## 11. Security and permissions

This software is, structurally, a keylogger with a GUI. That obliges some
explicit commitments.

1. **No key content is ever persisted.** No keystroke log file, at any log level.
   Key *codes* may appear in debug-level logs only when a debug flag is passed
   explicitly at launch; never by default, never in a config file.
2. **No network access, ever.** The core and overlay open no sockets other than
   the local IPC endpoint. There is no telemetry, no update check, no crash
   reporting upload.
3. **The IPC endpoint is owner-only** (§5.1). Any other process reading it would
   see every keystroke the user types, so this is a correctness requirement, not
   a nicety.
4. **Positional codes only on the wire** (§5.3). The core does not resolve
   keystrokes to characters, so the IPC stream cannot reconstruct typed text
   without independently knowing the user's keyboard layout.
5. **Least privilege.** The core takes exactly the privileges its backend needs —
   the `input` group on Linux via a shipped udev rule, and *no* elevation on
   Windows by default. Elevation is opt-in, and the UI explains what it buys
   (interception over elevated windows) and what it costs.

---

## 12. Testing strategy

| Layer | Approach |
|-------|----------|
| Key vocabulary | Round-trip every code through each backend's table; assert bijection and no unmapped entries |
| Layout registry | Validate every bundled layout against the schema; assert no geometric overlaps; assert every bound key exists in every bundled layout |
| Layer engine | **Pure unit tests over a synthetic event trace.** The engine takes events and a clock and returns decisions — no OS involved. This is where the grace window and P7 are proven. |
| Grace window | Table-driven: for each of the three resolutions in §6.3, assert the exact output event sequence |
| P7 invariant | Property test: for any random event sequence with random mode changes, assert every forwarded press has a matching forwarded release |
| Motion | Assert diagonal speed equals cardinal speed; assert fractional accumulation never loses pixels; assert the ramp is monotonic |
| IPC | Golden-file tests of serialised messages; a fake client that stops reading, asserting the core does not block |
| Overlay | `pytest-qt` against the mock backend; render each bundled layout at several scales and assert no key overlaps and no clipped legends |
| Platform backends | Manual test matrix, documented in `docs/manual-tests.md` — these cannot be meaningfully automated without a real display and real hardware, and pretending otherwise produces tests that pass while the product is broken |

The layer engine being pure — a function of `(events, clock)` — is a deliberate
design constraint, not an accident. It is the only way the concurrency-sensitive
logic inherited from the prototype can be tested at all.

---

## 13. Milestones

| # | Milestone | Contents |
|---|-----------|----------|
| **M0** | Foundation | Docs, repo layout, schemas, the three layouts, default bindings, themes, IPC protocol definition |
| **M1** | Overlay on the mock backend | Renders any layout, all four legend layers, feedback, themes, click-through, pin, opacity, persistence |
| **M2** | Core skeleton + IPC | Engine, motion integrator, action dispatcher, IPC server, unit tests. No OS backends — driven by a synthetic input backend |
| **M3** | Windows backend | Hook, `SendInput`, Win32 windows/monitors. End-to-end on Windows |
| **M4** | Linux/X11 backend | evdev, uinput, EWMH/XRandR, udev rule. End-to-end on Linux |
| **M5** | Configuration UI | Settings dialog, visual binding editor, profile editor |
| **M6** | Packaging | Windows installer, Linux packages, first release |

M0 and M1 are the scope of the initial commit.

---

## 14. Open questions

1. **Non-rectangular keys.** ISO Enter and the ISO left-Shift are L-shaped. v1
   draws rectangles. Options: approximate (ship now, looks slightly wrong on ISO
   boards), or add an optional `path` field to the key schema (correct, more
   renderer work). *Proposed: approximate in v1, add `path` in layout schema v2 —
   the `schema` version field exists precisely to make this a non-breaking change.*
2. **Layout naming — "104" vs "105".** "105-key" conventionally denotes the ISO
   variant; the standard US full-size board is ANSI 104. The repo ships
   `us-ansi-104` as the default and `us-iso-105` alongside it, so both readings of
   the original request are satisfied and the difference is visible rather than
   argued about.
3. **Laptop layout fidelity.** ThinkPad and Asus keyboards vary by model and year.
   The bundled files are modelled on current mainstream models and are explicitly
   a starting point — correcting them is a JSON edit, which is the point of P1.
4. **Multiple keyboards on Linux.** Grabbing all keyboards is specified, but an
   external keyboard plus a laptop keyboard with different physical layouts means
   the overlay can only draw one of them. *Proposed: draw the configured layout,
   accept input from all devices.*
5. **Wayland.** Deferred (Outline §8). Revisit when `wlr-layer-shell` or an
   equivalent portal is available uniformly enough to implement without
   per-compositor branches.

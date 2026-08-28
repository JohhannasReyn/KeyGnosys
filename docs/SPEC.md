# KeyGnosys — Technical Specification

*Master Keys to Your System*

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
10. [Editors](#10-editors)
11. [Diagnostics and failure behaviour](#11-diagnostics-and-failure-behaviour)
12. [Security and permissions](#12-security-and-permissions)
13. [Testing strategy](#13-testing-strategy)
14. [Milestones](#14-milestones)
15. [Resolved design decisions](#15-resolved-design-decisions)

---

## 1. Terminology

| Term | Meaning |
|------|---------|
| **Core** | `keygnosys-core`, the native C++ process that intercepts and synthesizes input. |
| **Overlay** | The Python/PySide6 GUI process. Comprises the *keyboard window* and the *control bar*. |
| **Cursor layer** | The alternate key mode engaged via CapsLock, in which keys drive the pointer and window manager. |
| **Legend layer** | Which set of labels the overlay is currently drawing (`base`, `shift`, `modifier`, `cursor`). |
| **Binding** | A mapping from a key code to an action, active in the cursor layer. |
| **Profile** | An app-shortcut document, matched against the focused window. |
| **Unit (`u`)** | Layout geometry unit. `1u` is the width of one standard alphanumeric key. |
| **Suppression** | The core consuming a physical key event so the OS never sees it. |
| **Logical key** | One key as the user experiences it: one identity, one label, one highlight, one hit-test target. Carries a stable `id` and a physical key `code`. |
| **Segment** | One axis-aligned rectangle of a logical key's drawn shape. Most keys have one; an ISO Enter has two. Segments have no identity of their own outside the editor. |

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
| Overlay | Qt `QKeyEvent::nativeScanCode` (mock backend only) | `python/keygnosys/coreclient/mock.py` |

Windows mapping **MUST** key off the *scancode*, not the virtual key code.
Virtual key codes are affected by the user's active keyboard layout; scancodes are
positional, which is what the vocabulary is defined to be.

---

## 3. Configuration files

### 3.1 Locations

| Role | Windows | Linux |
|------|---------|-------|
| **Bundled** (read-only) | `<package>/data/` | `<package>/data/` |
| **User config** | `%APPDATA%\KeyGnosys\` | `$XDG_CONFIG_HOME/keygnosys/` (default `~/.config/keygnosys/`) |
| **Runtime/socket** | `\\.\pipe\keygnosys` | `$XDG_RUNTIME_DIR/keygnosys/core.sock`, with a fallback when that is unset — §5.1.1 |
| **Logs** | `%LOCALAPPDATA%\KeyGnosys\logs\` | `$XDG_STATE_HOME/keygnosys/logs/` |

Both config roots contain the same four subdirectories: `layouts/`, `bindings/`,
`themes/`, `profiles/`.

The runtime endpoint is the one location in this table that is not merely a
place to put files — it carries every keystroke the user types. Its resolution,
its ownership requirements and the rules for recovering one left behind by a
crashed core are specified in [§5.1.1](#51-transport) to §5.1.3, which are
normative for the core, the overlay and the launcher alike.

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

Reloading bindings **MUST** release every held action whose binding disappeared,
and every locked drag button (P7).

A reload **MUST NOT** discard physical input the user has already committed. A
press sitting in the grace window was delayed only to resolve *layer intent*,
not to await permission:

- if the key is still bound to an action, the press stays buffered, **retaining
  its original press time** — restarting the clock would silently extend the
  grace window;
- if the key is no longer action-bound, there is nothing left to disambiguate,
  so the press resolves immediately as the ordinary forwarded keystroke it
  turned out to be, and its release obligation is recorded;
- forwarded presses are untouched. P7 outranks a config reload.

Multiple keys resolved by one reload are emitted in press order (§6.3.3).

### 3.4 Update and migration policy

**An update adds capability. It never resets, discards, or silently rewrites
anything the user has configured.** This is a guarantee, not an aspiration, and
the rules below exist to make it structurally true rather than a matter of care
at release time.

#### 3.4.1 The installer never touches user configuration

An update writes **only** to the bundled data directory and the program files. It
**MUST NOT** write to, move, or delete anything under the user config root
(§3.1) — not layouts, not bindings, not themes, not profiles, not `settings.json`.

Uninstallation **MUST** leave the user config root in place by default, and
**MUST** ask before removing it if it offers to at all. A curated layout is
potentially hours of work.

This is why the two roots are separate directories rather than one directory with
a naming convention. There is no code path in the installer that can reach user
data, because it never looks there.

#### 3.4.2 A user document always wins

The shadowing rule (§3.2) does the rest. A user layout with the same `id` as a
bundled one replaces it entirely and permanently. If a later release improves the
bundled `thinkpad-compact`, a user who forked it keeps their own version and is
unaffected.

The consequence is deliberate and cuts both ways: **a fork stops receiving
upstream improvements.** That is the correct trade — it is the only way to
guarantee the first half — but the UI **MUST** make it visible rather than
letting a user wonder why a documented fix never arrived:

- A user document whose `id` matches a bundled one **SHOULD** be marked as
  overriding it, naming what it shadows.
- When the bundled original changes in an update, the UI **SHOULD** offer a
  side-by-side "the built-in version changed — review?" affordance. It **MUST
  NOT** apply anything automatically.

#### 3.4.3 Settings gain defaults, never lose values

`settings.json` is deep-merged over the current defaults on load (§4.5):

- A key **added** in a new release appears with its default. The user's file need
  not mention it.
- A key **removed** from the defaults but still present in the user's file is
  **preserved verbatim** on rewrite, not stripped. Downgrading a release, or
  a feature returning later, must not cost the value.
- A key whose value is out of range is **clamped and reported**, never reset to
  default silently.

#### 3.4.4 Schema migrations are additive and in-memory

Every document kind is versioned (§4). When a schema gains a major version:

- The loader **MUST** continue to read every prior major version it has ever
  shipped, upgrading **in memory** — as layout schema 1 is upgraded to 2 in
  §4.1.7 and bindings schema 1 to 2 in §4.2.
- Upgrading **MUST NOT** rewrite the file on disk. A document is rewritten only
  when the user explicitly saves it from an editor.
- New fields **MUST** have defaults that reproduce the old behaviour exactly. A
  document written before a field existed must render and behave identically
  after the field is introduced.
- Dropping support for an old schema version is a breaking change and requires a
  major release, a migration tool, and a release note. It is not something to do
  quietly.

#### 3.4.5 New actions reach existing users

A command added to the action catalog (§7) in a later release **MUST** become
visible to a user whose binding document predates it, without them re-forking
anything.

This is why the editor's unassigned list has a derived half (§10.2.3): commands
are offered from the live catalog, not from a stored inventory. A document that
stored the complete list of available commands would freeze the feature set at
the moment it was written, and "updates only add features" would quietly become
false for exactly the users who had customised the most.

---

## 4. File formats

Every document carries a `schema` field of the form
`keygnosys/<kind>/<major>`. A consumer **MUST** reject a document whose major
version it does not recognise, with a diagnostic naming the file.

### 4.1 Layout

Describes physical key geometry. Geometry is expressed in **units** from the
top-left corner, never in pixels — the renderer scales units to pixels, so one
layout serves every display scale and every zoom level.

A layout is a list of **logical keys**. Each logical key owns its identity, its
physical key code, its label and its style, and it is drawn as **one or more
rectangular segments**. Most keys have exactly one segment. An ISO Enter has two.

#### 4.1.1 Why segments rather than paths

The alternative was an arbitrary SVG path or polygon per key. Segments were
chosen because:

- they cover every shape the supported layouts actually need — in practice that
  is the ISO Enter and nothing else;
- they are directly editable with drag handles, whereas a path requires a node
  editor, which is a substantially larger piece of UI for one key shape;
- hit-testing, snapping, alignment and overlap detection are all trivial on
  rectangles and all fiddly on paths;
- a rectangle list survives a round-trip through hand-edited JSON in a way a
  path string does not.

Arbitrary paths are **out of scope**. If a future layout genuinely needs one, the
`schema` version field exists to introduce it without breaking anything.

#### 4.1.2 Document

```jsonc
{
  "schema": "keygnosys/layout/2",
  "id": "us-iso-105",
  "name": "US-International Full-Size (ISO 105)",
  "description": "Full-size ISO variant: tall L-shaped Enter, short left Shift.",

  "metadata": {
    "author": "KeyGnosys",
    "source_template": null,        // id of the layout this was duplicated from
    "model": "Generic ISO full-size",
    "revision": 1                   // bumped by the editor on every save
  },

  "size": { "w": 22.5, "h": 6.25 }, // total extent in units

  "keys": [
    {
      "id": "escape",               // stable, unique within the layout
      "code": "Escape",             // physical key identifier, §2.1 vocabulary
      "legend": {
        "base": "Esc",              // required
        "shift": null,              // optional; falls back to base
        "sub": null                 // optional small corner text
      },
      "role": "system",             // see below; default "normal"
      "style": null,                // optional per-key overrides, §4.1.4
      "segments": [
        { "id": "s0", "x": 0, "y": 0, "w": 1, "h": 1 }
      ]
    },
    {
      "id": "enter",
      "code": "Enter",
      "legend": { "base": "Enter" },
      "role": "system",
      "segments": [
        { "id": "s0", "x": 13.5,  "y": 2.25, "w": 1.5,  "h": 1 },
        { "id": "s1", "x": 13.75, "y": 3.25, "w": 1.25, "h": 1 }
      ]
    }
  ]
}
```

That two-segment Enter is the real ISO shape: the upper part reaches `0.25u`
further left than the lower part, because the row above it holds one fewer key.

**Identity fields.**

| Field | Scope | Purpose |
|-------|-------|---------|
| `id` (layout) | Global | What settings and the shadowing rule (§3.2) refer to |
| `id` (key) | Unique within a layout | Lets the editor track a key across moves, resizes and relabels, and lets a future per-key override refer to one |
| `id` (segment) | Unique within a key | Lets the editor address one rectangle of a multi-segment key |
| `code` | — | The physical key. Independent of `id`, `legend` and geometry: renaming a label or moving a key never changes which physical key it represents |

Key and segment ids **MUST** match `^[A-Za-z0-9][A-Za-z0-9_-]*$`. When a document
omits them, the loader **MUST** synthesise stable ones (`k0`, `k1`, … and
`s0`, `s1`, …) by index, so hand-written files stay easy to author and the editor
still has something to hold on to.

**`role`** informs rendering and hit-testing, not behaviour:

| role | Meaning | Rendering hint |
|------|---------|----------------|
| `normal` | Ordinary character key | Standard |
| `modifier` | Shift/Ctrl/Alt/Meta/Fn | Dimmed until held, then latched-bright |
| `system` | Esc, F-keys, PrtSc | Standard, smaller label |
| `nav` | Arrows, Home/End/PgUp/PgDn | Standard |
| `numpad` | Numpad cluster | Grouped, may be hidden by a setting |
| `toggle` | CapsLock, NumLock, ScrollLock | Renders an LED dot |

#### 4.1.3 The one-key rule

A logical key's segments are a **drawing detail, not a structural one**. Every
consumer treats the key as a single thing:

- **Rendering** — one fill, one border, one label. The border follows the outline
  of the union of the segments; interior edges where segments meet are not drawn.
- **Highlighting** — a key press lights every segment at once, and the fade
  animation runs once for the key, not once per segment.
- **Hit-testing** — a point inside *any* segment hits the key. The editor selects
  the whole key.
- **Moving** — dragging a key in the editor translates all of its segments
  together, preserving their relative offsets.
- **Legend** — drawn once, centred on the **largest** segment. Centring on the
  bounding box of an L-shape puts the text in the notch, where it can fall
  outside the key entirely.

Only inside the editor's **segment-edit mode** (§10.5) may segments be resized or
repositioned individually.

#### 4.1.4 Style overrides

`style` is an optional object on a logical key, overriding theme tokens for that
key alone. All fields optional; anything absent falls through to the theme.

```jsonc
"style": {
  "face": "#2a2f3aF2",     // #RRGGBB or #RRGGBBAA
  "text": "#e6e9ef",
  "border": "#3a4150",
  "accent": "#ff7043"      // this key's feedback colour
}
```

Style overrides are geometry- and label-independent, and they survive relabelling
and moving. They exist so a user can mark up their own board — tinting the keys
they are still learning, say — without forking a whole theme.

#### 4.1.5 Validation

Failures are graded, because a layout is a document a human edits by hand and one
mistake must never blank the whole keyboard (principle P6).

**Document rejected** — the layout does not load at all:

| Condition |
|-----------|
| `schema` major version is unrecognised |
| `id` missing or not matching `^[a-z0-9][a-z0-9-]*$` |
| `keys` missing, not a list, or empty |
| `keys` is not a list of objects |

**Key dropped, document still loads** — diagnostic `layout.key_invalid`:

| Condition |
|-----------|
| Missing `code` or `legend.base` |
| `segments` missing or empty |
| Any segment dimension non-positive, non-numeric, or non-finite |
| Duplicate key `id` within the layout (the later one is dropped) |

**Warning, key still loads** — the layout renders, and the editor surfaces these
before allowing a save:

| Code | Condition |
|------|-----------|
| `layout.duplicate_code` | Two logical keys claim the same `code`. Almost always a mistake — both would light up together on one keypress — but not structurally invalid, so it warns rather than drops. |
| `layout.unknown_code` | `code` is not in the §2.1 vocabulary. The key renders but can never highlight, because no backend will ever emit that code. |
| `layout.overlap` | Two **different** logical keys overlap geometrically. |
| `layout.out_of_bounds` | A segment extends beyond the declared `size`. |

**Overlap is a warning, never an automatic correction.** The editor reports it
and highlights the offending pair; it does not nudge, resize, or reject the
user's work. Some overlap may be deliberate — a board with a raised key drawn
over its neighbour — and silently rewriting someone's layout is worse than
letting them ship an odd one.

**Segments of the same logical key are exempt.** They may touch, share edges, or
overlap outright without producing `layout.overlap`. Sharing an edge is the
normal case: that is how an L-shape is assembled.

#### 4.1.6 Adding a layout

Three supported routes, all producing the same document:

1. **The editor** (§10) — duplicate a bundled template, drag it into shape, save.
   This is the route ordinary users take.
2. **Drop in a file** — write JSON, put it in `layouts/`, restart or wait for the
   watcher.
3. **Generate it** — `tools/gen_layouts.py` for boards that are more easily
   described in code than placed by hand.

No route involves a code change. This is principle P1 and is a hard requirement:
any change that makes a new layout require a code edit is a spec violation.

#### 4.1.7 Schema 1 compatibility

Schema 1 described flat keys with `x`/`y`/`w`/`h` directly on the key and no
identity fields. The loader **MUST** still accept it, upgrading in memory:

- each key becomes a logical key with a single segment carrying its geometry;
- `id` fields are synthesised by index;
- `metadata` defaults to empty.

Schema 1 documents are never rewritten in place. The editor saves as schema 2,
and offers to upgrade a schema 1 document on first edit.

### 4.2 Bindings

Maps key codes to cursor-layer actions.

```jsonc
{
  "schema": "keygnosys/bindings/2",
  "id": "default",
  "name": "Default cursor layer",

  "metadata": {
    "author": "KeyGnosys",
    "source_template": null,        // id this was duplicated from
    "revision": 1
  },

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
  },

  // Commands the user configured that currently sit on no key. See §10.2.3.
  "unassigned": [
    { "action": "window.move_to_monitor", "params": { "target": 2 },
      "legend": "Send to left screen" }
  ]
}
```

- `action` **MUST** name an action from §7. Unknown actions are skipped with a
  diagnostic; the rest of the document still loads.
- `params` are validated per-action; an invalid param set skips that one binding.
- `legend` is optional display text. When absent, the overlay derives one from the
  action catalog's default legend.
- Diagonal movement is **not** a binding. It emerges from two direction keys held
  simultaneously, as vectors sum in the motion integrator (§6.4).

**Binding a command to more than one key is legal and expected.** The shipped
defaults bind left-click to both `KeyD` and `Space`. Only the *key* side is
exclusive: a key holds at most one command.

**`unassigned`** holds commands the user has configured but that are currently
bound to nowhere — the persistent half of the editor's unassigned list (§10.2.3).
It exists so that a command displaced by reassignment keeps its `params` and its
custom `legend` across a restart instead of silently reverting to a catalog
default. It is validated exactly like a binding; entries that fail validation are
dropped with a diagnostic.

The list is **not** a complete inventory of unbound commands. Everything in the
action catalog that is neither bound nor listed here is offered by the editor as
an *available* command, derived at runtime. Storing the whole catalog in every
document would mean a new action in a later release never appearing for existing
users — the derived half is what keeps upgrades additive (§3.4).

An entry that is also present in `bindings` is a contradiction; the loader
**MUST** drop it from `unassigned`, keep the binding, and emit
`binding.unassigned_conflict`. This can only arise from hand-editing.

**Schema 1 compatibility.** Schema 1 had no `metadata` and no `unassigned`. Both
default to empty on load. Schema 1 documents are never rewritten in place; the
editor saves as schema 2.

### 4.3 Theme

Colour tokens, not stylesheets — so themes stay renderer-agnostic.

```jsonc
{
  "schema": "keygnosys/theme/1",
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
  "schema": "keygnosys/profile/1",
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
  "schema": "keygnosys/settings/1",
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
| Linux | `$XDG_RUNTIME_DIR/keygnosys/core.sock` (`AF_UNIX`, `SOCK_STREAM`), or the §5.1.1 fallback | Socket mode `0600`, in a directory verified per §5.1.2 |
| Windows | `\\.\pipe\keygnosys` (named pipe, message mode) | DACL restricted to the creating user |

The core is the server and **MUST** accept multiple concurrent clients. Events are
broadcast to all; replies go only to the requesting client.

A client that stops reading **MUST NOT** block the core. Each client has a bounded
outbound queue (default 256 messages); on overflow the core drops the oldest
*event* messages, emits one `diagnostic`, and keeps running. Replies are never
dropped.

**When the queue is all replies.** Dropping the oldest event is an *event*
policy, and it cannot bound a queue that holds none. A client is free to send
commands indefinitely while never reading, and "one reply per command" bounds
nothing when the commands do not stop, so the two rules above — a bounded queue,
and replies that are never dropped — cannot both hold by shedding messages.

They are reconciled by bounding the **client** rather than the memory:

1. A client whose outbound queue has reached the bound **MUST** have its input
   left unread until the queue drains. Consuming a command that cannot be
   answered is what turns a slow client into an unbounded one, and declining to
   read it is ordinary backpressure — the client's own writes block instead.
2. A client whose outbound queue nevertheless passes a hard ceiling (**four
   times** the bound) **MUST** be disconnected, with a diagnostic. No reply is
   dropped, because there is no longer a client to deliver one to; the queue
   stays bounded; and the cost falls on the connection that caused it.

The ceiling is a backstop, not the mechanism. Reaching it means backpressure
was outrun within a single read.

#### 5.1.1 Endpoint resolution

**One rule, derived the same way by everyone.** The core, the overlay and the
launcher **MUST** resolve the endpoint by the rule below, and **MUST NOT** each
carry a private variant of it. `keygnosys.paths.ipc_endpoint()` is that rule on
the Python side; the core implements the identical rule natively. A component
that resolved it differently would report a running core its peers cannot find,
or connect to a path nothing is listening on, and either failure looks from the
outside like the core is broken.

**Windows.** `\\.\pipe\keygnosys`. Constant.

**Linux.** `<runtime base>/keygnosys/core.sock`, where:

| Condition | Runtime base | Resulting endpoint |
|-----------|--------------|--------------------|
| `$XDG_RUNTIME_DIR` is set | its value | `/run/user/1000/keygnosys/core.sock` |
| `$XDG_RUNTIME_DIR` is unset | `/tmp/keygnosys-<uid>`, `<uid>` being the real uid | `/tmp/keygnosys-1000/keygnosys/core.sock` |

The `keygnosys` component appears twice in the fallback form. That is the
intended consequence of there being one composition rule rather than two, and it
is left as it is: a second rule that existed only to tidy up a path is a second
rule that can be implemented inconsistently by the core and the overlay.

**Why there is a fallback.** `$XDG_RUNTIME_DIR` is set by `pam_systemd` or
`elogind` at login, and is absent on non-systemd installations without elogind,
in bare `startx` sessions, and in minimal containers. Refusing to run in those
environments would be an unforced platform restriction, so the fallback exists
and is specified rather than left to chance.

**Both forms are per-user, by construction.** `$XDG_RUNTIME_DIR` is per-user by
definition; the fallback embeds the uid in its name. Two users on one machine
never resolve to the same endpoint.

#### 5.1.2 Ownership and permissions

The endpoint carries every keystroke the user types. Anything able to substitute
its own socket at the endpoint path receives them, so the directory containing
the endpoint is part of the security boundary and not merely a location.

`$XDG_RUNTIME_DIR` is safe by construction: the XDG Base Directory
specification requires it to be owned by the user, accessible to nobody else
(mode `0700`), and created and destroyed with the session.

`/tmp` is not. It is world-writable with the sticky bit set, and while the
sticky bit prevents another user from deleting entries the core created, **it
does not prevent another user from creating `/tmp/keygnosys-<uid>` first.** A
directory belongs to whoever creates it, and the owner of a directory may unlink
what is inside it. An attacker who wins that race owns the directory the core is
about to bind in, and can replace the core's socket with one of their own — to
which the overlay would then connect, and into which it would read every
keystroke. That is the worst failure available to this system. The fallback is
therefore never merely *used*; it is verified.

**Every component the core owns is verified, not merely the last one.** The
fallback endpoint has two directory components beneath `/tmp`, and securing only
the innermost would secure nothing: if another user owns
`/tmp/keygnosys-<uid>`, they control the directory entry for the `keygnosys`
child and may rename or remove it whatever its own mode says.

| Component | `$XDG_RUNTIME_DIR` form | Fallback form | Created by the core | Verified |
|-----------|-------------------------|---------------|---------------------|----------|
| Fallback root | — | `/tmp` | No | Sticky bit only — see below |
| Runtime base | `$XDG_RUNTIME_DIR` | `/tmp/keygnosys-<uid>` | Only in the fallback form | **Fully** |
| Runtime directory | `$XDG_RUNTIME_DIR/keygnosys` | `…/keygnosys-<uid>/keygnosys` | Yes | **Fully** |
| Endpoint | `core.sock` | `core.sock` | Yes | Created mode `0600` |
| Startup lock | `core.lock` (§5.1.3) | `core.lock` (§5.1.3) | Yes | Created mode `0600` |

**Fully verified** means all of: it is a real directory and **not** a symbolic
link; it is owned by the uid the core is running as; and its mode grants no
access to group or other. A directory the core creates is created with mode
`0700` and then verified anyway, because creating it and winning the race to
create it are not the same thing.

**The fallback root is the one exception, and its sticky bit is load-bearing.**
`/tmp` is world-writable and **MUST NOT** be treated as private — requiring that
would fail on every conforming system. But the core **MUST** require it to have
the sticky bit set. That bit is the only thing stopping another user renaming
`/tmp/keygnosys-<uid>` aside and putting their own directory at the name clients
resolve; without it the core would carry on safely inside its own verified
inode while every client was directed somewhere else entirely.

**Verification is anchored to descriptors, not to paths.** The core **MUST**
descend one component at a time, opening each with `O_DIRECTORY | O_NOFOLLOW |
O_CLOEXEC` relative to the descriptor of the component above it, and **MUST**
`fstat` the *descriptor* rather than `stat` the path. Every later operation —
creating the child directory, the lock and the socket, and unlinking a stale
endpoint — is performed relative to the descriptor that was verified. Checking a
path and then re-resolving it reintroduces exactly the race the checks exist to
close.

`bind()` has no `*at` form, so it is the one step where that discipline is easy
to abandon by accident. The core **MUST** bind through the verified descriptor
— by `fchdir` to it and binding a relative name, or through
`/proc/self/fd/<dirfd>/core.sock` — and **MUST NOT** bind by reassembling the
absolute path.

**What this does and does not reach.** The scope is the components in the table
above: the runtime base and the runtime directory, plus the fallback root's
sticky bit. Components *above* the runtime base are deliberately out of scope.
On a conforming system they are root-owned and not private — `/run` and
`/run/user` are `0755` — so they cannot be required to be private, and
`/var/run` is a symbolic link to `/run` on Debian and Ubuntu, so refusing an
intermediate link would refuse a legitimate `$XDG_RUNTIME_DIR` outright.

The practical consequence is that `O_NOFOLLOW` on the runtime base is applied
to its **final component**, which is exactly what "not a symbolic link" in the
table means and all it can mean. It is not a weaker check than the fallback
form gets: there, the same flag reaches every component the core owns, because
there the core owns one more of them.

Verifying an inode does not, and cannot, guarantee that the *pathname* still
resolves to it later. That is precisely why the fallback root's sticky bit is
required (above), and why nothing beyond it is claimed here.

**The uid is unambiguous.** The core **MUST NOT** be installed setuid or setgid;
§12.5 grants it access by group membership, not by identity change. Real and
effective uid therefore coincide, and the `<uid>` naming the fallback directory
is the same uid the ownership checks require. Were they allowed to differ, the
endpoint could be named for one user and owned by another.

**Nothing unsafe is ever repaired.** On any failed check the core **MUST** emit
`ipc.endpoint_unsafe` (§11) naming the component and the property that failed,
and **MUST NOT** start the input backend. It **MUST NOT** `chmod`, `chown`, or
delete and recreate any component to make it acceptable. The core cannot
distinguish a hostile directory from one it merely does not understand, and the
cost of guessing wrong is silent keystroke interception. Refusing loudly is the
specified behaviour (P6).

These checks apply to **both** forms, not only the fallback.
`$XDG_RUNTIME_DIR` is expected to pass them trivially, and verifying it anyway
costs nothing and removes the need to trust an environment variable that a
process able to set the core's environment could have pointed anywhere.

#### 5.1.3 Binding, liveness and stale endpoints

**Only the core creates, binds, renames or removes an endpoint.** The overlay
connects and nothing more. The launcher connects, to probe for a running core,
and nothing more ([LAUNCHING.md §5](LAUNCHING.md#5-detecting-an-existing-instance)).
Neither **MUST** ever delete an endpoint, on any code path, including error and
cleanup paths.

**Cross-user removal is structurally impossible, not merely forbidden.** The
endpoint always lives inside a directory the core has verified to be owned by
itself and inaccessible to anyone else (§5.1.2), and the core operates only
within that directory. There is therefore no path by which one user's core
reaches another user's endpoint — the guarantee does not rest on a check at the
point of deletion.

**The governing invariant: at no point may two cores both conclude that they own
the canonical endpoint.** Leaving one pathname behind is not the same property
and is not sufficient. A core is not made to cease existing by having its
pathname taken away from it, and a core that survives with its device grab
intact but unreachable by any client is a worse outcome than a clean refusal to
start.

How that invariant is met differs by platform, because the two platforms
disagree about whether an endpoint can be stale at all, and therefore about
whether claiming one can be a single atomic act.

**Linux.** A bound `AF_UNIX` socket leaves a filesystem entry that outlives the
process, so `bind()` on it fails with `EADDRINUSE` and the core must decide
whether the endpoint is live or abandoned. `connect()` is the only test:

| `connect()` result | Meaning | The core **MUST** |
|--------------------|---------|-------------------|
| succeeds | A core is listening | Refuse to start, report that a core is already running, leave the endpoint untouched |
| `ECONNREFUSED` | Nothing is listening; the socket outlived its process | Treat as stale; it **MAY** then recover it |
| `ENOENT` | Nothing is there | Bind normally |
| anything else | Staleness is not proven | Refuse to start, emit `ipc.endpoint_in_use`, leave the endpoint untouched |

`ECONNREFUSED` is the **only** admissible evidence of staleness. A process
listing, a PID file, and the socket's age are not evidence and **MUST NOT** be
used: each of them can be wrong in the direction that removes a live core's
endpoint.

**That test alone is not enough, and no lock-free arrangement of it is.** Two
independent facts about Linux make probe-then-act unsafe, both confirmed against
a running kernel rather than inferred:

- **`rename()` onto the canonical path does not exclude anyone.** It is atomic
  with respect to the pathname, so exactly one directory entry survives — but
  the core it displaces is untouched. Its listening socket is bound to an
  *inode*, and the pathname is consulted only when a client calls `connect()`.
  Two cores that both observe `ECONNREFUSED` will both bind, both rename, and
  both continue running; the second simply wins the name. The first stays alive
  and listening, holding a device grab, reachable by nobody. Unlinking and then
  binding fails the same way and adds a window in which the path is missing.
- **`ECONNREFUSED` is also what a perfectly healthy core returns between
  `bind()` and `listen()`.** A socket that is bound but not yet listening
  refuses connections. So the probe can declare a core stale that is merely
  starting up, and the race does not even require a genuinely stale socket to
  occur.

Claiming the endpoint therefore **MUST** be serialized. The core **MUST**
acquire an exclusive **startup lock** before it probes, and **MUST** hold it
across the probe, any recovery, `bind()` and `listen()`:

1. Verify the runtime directory and obtain its descriptor (§5.1.2). Everything
   below happens relative to that descriptor.
2. Open `core.lock` in that directory, creating it with mode `0600` if absent,
   with `O_CLOEXEC`.
3. Take `flock(LOCK_EX | LOCK_NB)`. On `EWOULDBLOCK` another core owns or is
   claiming this endpoint: the core **MUST** refuse to start, emit
   `ipc.endpoint_in_use`, and **MUST NOT** block or retry in a loop. A core that
   waits on startup is indistinguishable from a core that has hung.
4. **Now** probe the endpoint, and act on the table above.
5. Recover if and only if the probe returned `ECONNREFUSED`.
6. `bind()`, then `listen()`.
7. **Retain the lock for the lifetime of the process.**

Holding the lock for the process lifetime, rather than releasing it after
`bind()`, is what makes it a single-instance token rather than a critical
section: the kernel releases a `flock` when the holding process dies by any
means, `SIGKILL` included, so unlike a PID file it can never be stale and never
needs cleaning up.

Three details decide whether this works:

- **`flock`, not `fcntl(F_SETLK)`.** An `fcntl` record lock is dropped when
  *any* descriptor to that file is closed anywhere in the process, so an
  unrelated open-and-close of the lock file silently surrenders the lock.
- **`O_CLOEXEC`.** Otherwise a process the core execs inherits the descriptor
  and holds the lock open past the core's own death.
- **The lock file is never unlinked** — not by the core, not on shutdown, not by
  the launcher, not by anyone. Removing a lock file is itself a race: two
  processes can hold exclusive locks on two different inodes that were briefly
  reachable by the same name. An empty `core.lock` persisting in the runtime
  directory is correct, not litter.

Because the lock lives in the runtime directory, which is derived from the
endpoint rule of §5.1.1, one endpoint has exactly one lock by construction —
two cores resolving different endpoints never contend, and one endpoint can
never be claimed through two different locks.

With the lock held there is no competing core, so recovery is simply unlinking
the stale endpoint relative to the verified descriptor and then binding.
Recovery **SHOULD** still bind a uniquely named temporary socket in the same
directory and `rename()` it onto the canonical path, but for one narrow reason
only: it leaves no interval in which the pathname is absent, and a client
polling during recovery — the overlay retries every two seconds, and the
launcher probes — would otherwise read that absence as the core not existing.
**`rename()` contributes nothing to mutual exclusion.** The lock is the whole of
it.

**Windows.** A named pipe is a kernel object, not a filesystem entry; its name
ceases to exist when the last handle to it closes. **There is no such thing as a
stale named pipe**, so on Windows there is nothing to recover and nothing to
delete.

The hazard there is the opposite one. By default `CreateNamedPipe` on a name
that already exists *succeeds*, creating an additional **instance** of the same
pipe and silently joining whoever created it — so a second core, or a process
that pre-created the name, would begin accepting connections intended for the
first. The core **MUST** therefore create the endpoint with
`FILE_FLAG_FIRST_PIPE_INSTANCE`, which fails if any instance already exists.
That one flag delivers both required properties: a live endpoint cannot be
stolen, and a name squatted in advance is detected rather than joined.

**Windows therefore needs no startup lock.** Creating the endpoint there is a
single kernel operation that either grants exclusive ownership or fails, so the
probe-then-act sequence that makes Linux unsafe does not exist: there is nothing
to observe between deciding and claiming. The invariant is identical on both
platforms; only Linux has to build it, because only Linux has an endpoint that
persists after its owner dies and a claim that cannot be made in one step.

On failure the core **MUST** refuse to start and emit `ipc.endpoint_in_use`. It
**MUST NOT** retry under a different name — a core on an endpoint its clients do
not resolve to is worse than no core — and there is nothing for it to remove.

**The scope of the guarantee.** It is exclusion over *one resolved endpoint*.
On Windows the endpoint is a constant, so that is the whole machine per user. On
Linux two cores started under environments that resolve differently — one with
`$XDG_RUNTIME_DIR` set and one without — resolve to two endpoints and two locks,
and neither is stealing anything from the other. What stops the second one there
is the input backend rather than the IPC layer: an exclusive evdev grab is
already held, so it fails and reports rather than quietly doubling up. Recording
the boundary is the point; nothing above should be read as promising more.

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
├── include/kgn/
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

**What the layer does to keys it has no binding for.** While `CURSOR` is
engaged, each physical key falls into exactly one of three cases:

| Key | Treatment |
|-----|-----------|
| Bound to `key.passthrough` | **Forwarded.** The escape hatch (§7.6), for a key that must still reach the OS inside the layer. |
| Bound to any other action | Runs its action. Suppressed; nothing reaches the OS. |
| A modifier (`Shift`/`Control`/`Alt`/`Meta`) | **Forwarded.** Ctrl+click and Shift+drag must keep working, and a layer that broke them would be useless for the pointer control it exists to provide. |
| Anything else | **Suppressed.** |

Suppressing the last case is deliberate: the cursor layer is a *mode*, not an
overlay on normal typing. The overlay draws those keys blank and dimmed (§9.4)
precisely to say they do nothing, and a key that silently typed a character
while the map showed it as empty would be the worst of both.

**The escape hatch requires an explicit binding.** A key reaches the OS inside
the layer only because it is bound to `key.passthrough`. Having no binding is
never treated as permission to type — an unbound key is suppressed, without
exception.

This is enforced structurally rather than defended. The engine receives **one**
classification — a map from key to `Action` or `Passthrough` — and *having an
entry* is what being bound means. There is no second set that could list a key
as passthrough while the first omits it, and a reload that removes a binding
cannot leave stale behaviour behind, because the whole map is replaced in a
single call with no observable intermediate state.

**A passthrough key is never delayed by the grace window.** It does the same
thing whether the layer is engaged or not, so there is nothing to disambiguate,
and buffering it would cost latency and buy nothing.

**The grace window (inherited from the prototype).** A key bound in the cursor
layer, pressed while in `NORMAL`, is *ambiguous* — the user may be typing it, or
may be a few milliseconds ahead of the CapsLock that was meant to precede it.
Such a press is **buffered**, not forwarded, for up to `grace_ms`. It resolves as:

| Event within the window | Resolution |
|-------------------------|------------|
| CapsLock arrives | Promote to a cursor-layer press. Nothing reaches the OS. |
| The key is released | Ordinary tap. Forward press+release together, in order. |
| `grace_ms` elapses | Ordinary hold. Forward the press, mark **passthrough**. |

**The forwarded-release invariant (P7).** A key whose press was forwarded to the
OS is flagged. Its release **MUST** also be forwarded, unconditionally,
regardless of what the mode has become in the meantime. Violating this leaves a
key stuck down in the compositor. The invariant applies equally to mode changes,
config reloads, `release_all`, client disconnects, and process shutdown — every
exit path **MUST** run `OutputBackend::releaseAll()`.

It covers **every** press that reaches the OS, not only the grace-window replay:

| Forwarded press | Why it is at risk |
|-----------------|-------------------|
| Ordinary typing while the layer is off | The layer may engage before the key is released |
| A **modifier** forwarded inside the layer | The layer may deactivate, or change mode, while the modifier is still physically held |
| A **`key.passthrough`** binding | Same: the layer may drop while the key is held |
| A key replayed when the **grace window** lapses | The layer may engage immediately afterwards |
| A **real CapsLock** from the escape gesture | Shift is commonly released first, changing the modifier state mid-press |

There is exactly one code path by which a press reaches the OS, and it records
the key as it goes. That is what makes the invariant checkable rather than
merely intended.

#### 6.3.1 State capacity and the key domain

Per-key state **MUST** cover the entire `KeyCode` id space, and the storage
domain **MUST** agree with the validity contract at both ends: every id that
`valid()` admits has a slot, including the largest one. A disagreement between
the two is an out-of-bounds access, not a policy question. There is deliberately
no "untracked" class of key: a key the engine cannot hold state for is a key
whose invariants it cannot maintain, and forwarding such events blindly breaks
both P7 and its mirror — an orphan release is forwarded, and a forwarded press
cannot be unwound by `release_all`. One byte of packed flags per key covers the
whole domain in 64 KiB, allocated once at construction, so the question does not
arise.

The remaining bounds are on **simultaneity** — how many keys can be buffered,
held, or forwarded at one time — and each **MUST fail safe**:

> **When an obligation cannot be recorded, the decision that would create it is
> not emitted.**

A press that cannot be tracked is **suppressed**, not forwarded untracked. An
action that cannot be recorded emits no `RunAction`. A dropped keystroke is
recoverable by pressing the key again; a key the OS believes is held forever, or
an action with no release, is not.

The decision buffer is the exception, and only because its capacity is *derived*
from the true worst case — `release_all` unwinding every held action and every
forwarded press at once — rather than guessed.

"A human has ten fingers" is not a safety argument. Malformed drivers, injected
events, device reconnects and lost releases are all in the threat model, so
overflow behaviour is specified and **MUST** be tested by deliberately forcing
it, not merely observed not to happen under ordinary traffic.

Capacity drops **MUST** be counted and exposed, so that a condition this
unreachable surfaces as a diagnostic if it ever occurs.

#### 6.3.2 Decisions describe the event they resolve

A `Suppress` decision **MUST** report the `KeyState` of the input event it
suppressed, not of some other event the engine was attempting to synthesise.
The one case where these differ is the grace-window tap replay, which forwards
a press while handling a release: if that press cannot be forwarded, the
resulting `Suppress` reports the **release**.

The rule exists so a `Suppress` is self-describing. Logging, tracing,
diagnostics and any future IPC consumer read the decision stream without access
to the event that produced it, and a decision that misreports its own state is
a trap for every one of them.

#### 6.3.3 Event ordering

Decisions **MUST** be emitted in a deterministic order, and that order **MUST**
be the order the keys were pressed in. This applies to every place the engine
resolves more than one key at once:

- buffered presses whose grace window lapses together;
- buffered presses promoted together when CapsLock arrives;
- held actions released when the layer is left;
- forwarded presses unwound by `release_all`.

Input order is observable. It decides how chords resolve, which of two
simultaneous movement keys starts first, and whether a bug reproduces. Leaving
it to a hash table's iteration order would make the engine's output depend on
nothing the user can see or control.

#### 6.3.4 Malformed and duplicate events

The engine **MUST** tolerate physical event streams that do not alternate
cleanly. A dropped event, a stuck driver, or a device re-plugged mid-keypress
can all produce them.

| Event | Treatment |
|-------|-----------|
| A press for a key already forwarded | Forwarded **as a repeat**, not as a second press. A second press would owe a second release, and only one physical release is coming — so the key would be left down forever. |
| A press for a key already running an action | Suppressed; the action is already held. |
| A press for a key already buffered | Kept buffered, retaining the **original** press time, so a repeated press cannot extend the grace window indefinitely. |
| A release for a key with no matching press | Suppressed (the mirror obligation, below). |
| An invalid key code | Suppressed in every direction. It never produces a press, so it can never owe a release. |

**CapsLock is not exempt.** It is dispatched before the general per-key logic,
so it **MUST** carry its own physical-state tracking or it bypasses this policy
entirely:

| Event | Treatment |
|-------|-----------|
| A duplicate CapsLock press | Suppressed, or forwarded as a repeat if the press was a real CapsLock. It **MUST NOT** re-run activation — in `toggle` mode that would flip the layer twice for one physical press. |
| A CapsLock release with no matching press | Suppressed, and **MUST NOT** change the mode, the latch, or anything else. |
| A CapsLock release after `release_all` | The same: `release_all` clears the physical-down flag, so the release that follows is an orphan by definition. |
| A `Shift+CapsLock` gesture whose forwarding could not be recorded | Press and release both suppressed. It remains an **escape gesture** throughout: the press never engaged the layer, so the release **MUST NOT** leave it. |

The escape-gesture classification **MUST** be tracked independently of whether
forwarding it to the OS succeeded. Deriving one from the other means a gesture
that failed on a capacity bound falls through into layer-release handling, where
it can leave a layer its press never engaged.

The recorded press time **MUST** be cleared on release. Left set, it remains
available to a later malformed event, which could latch the layer from a clock
reading that belongs to a keypress long finished.

**The mirror obligation.** A release **MUST NOT** be forwarded unless the
matching press was forwarded. If the layer deactivates while suppressed keys are
still physically held — an unbound key, or one that was running an action — their
releases **MUST** be suppressed too. Forwarding them would send the OS a key-up
for a key it never saw go down, which is the same class of corruption as a stuck
key, in the opposite direction.

Both directions are proved by property tests over randomised event sequences
(§13), not merely asserted. The generator's event space covers what this section
claims: presses and releases, autorepeat, CapsLock transitions in all three
activation modes, grace-window expiry, `release_all` mid-sequence with physical
releases continuing afterwards, configuration reloads while keys are held,
malformed or duplicate physical events — including malformed CapsLock
specifically, since it is dispatched before the general per-key logic — and
invalid key codes.

**Only bound keys are ever delayed.** Buffering adds up to `grace_ms` of latency,
and it applies **only** to keys bound to an action in the cursor layer, and
**only** while the layer is off. Three categories are never buffered and never
delayed:

- keys with no cursor-layer binding — nothing to disambiguate;
- keys bound to `key.passthrough` — they behave identically in both modes;
- every key, once the layer is already engaged — the ambiguity the window exists
  to resolve cannot arise.

This matters because the delay is otherwise invisible to the user: a keyboard
that felt sluggish on ordinary typing would be a worse product than one without
the layer at all.

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
  by CLI flag.
- The virtual device **MUST** advertise every `EV_KEY` code the real device has,
  plus `EV_REL` `REL_X`/`REL_Y`/`REL_WHEEL`/`REL_HWHEEL` and the mouse buttons.
- Requires membership in the `input` group and write access to `/dev/uinput`. A
  shipped udev rule grants this without running as root.
- Hot-plug: watch for device add/remove via udev and re-grab.

**Multiple keyboards.** All detected keyboard devices **MUST** be grabbed, and
events from every one of them map onto the single layout the user has selected,
by physical key code. A laptop's built-in keyboard and an external board both
drive the same overlay.

The core **MUST NOT** switch layouts based on which device an event came from.
That is a deliberate exclusion, not an oversight: the overlay's job is to be a
stable map, and a keyboard that redraws itself when the user's hands move between
two boards is worse than one that is occasionally slightly wrong about key
shapes. A key present on one board and absent from the drawn layout simply does
not light up; it still functions normally.

The architecture keeps the door open — `InputBackend` reports a device id
alongside each event, and nothing in the engine assumes a single source — but
per-device layout selection is out of scope for v1.

### 8.2 Windows input — low-level hook

`SetWindowsHookExW(WH_KEYBOARD_LL, …)` on a dedicated thread with its own message
pump. Returning non-zero from the hook proc suppresses the event.

**Hard constraints, to be surfaced in `hello.limitations` and in the UI:**

- The hook proc **MUST** return within the `LowLevelHooksTimeout` window
  (default 300 ms) or Windows silently unhooks it. Therefore the hook proc
  **MUST** do nothing but decide suppression and enqueue; all dispatch, IPC and
  logging happen on other threads.
- **The event path performs no dynamic allocation.** The layer engine runs
  inside the hook, so this is a hard requirement rather than an optimisation:
  an allocation can block on a heap lock held by any other thread in the
  process, and a hook that stalls is a hook that gets unregistered.

  Concretely, that means the engine holds all per-event state in fixed-size
  arrays indexed by key code rather than in node-based hash containers, resolves
  the key codes it compares against once at construction rather than by name per
  event, and writes into a fixed-capacity `DecisionBuffer` rather than a
  `std::vector` whose capacity the caller might not have reserved. Every one of
  those capacities has a defined overflow behaviour — documented in
  `layer_engine.hpp` — rather than undefined behaviour or a reallocation.

  Allocating convenience overloads exist for tests and for callers that are not
  on the hook path. They are not used by the hook.
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

### 8.5 Linux display environment

**X11 is the supported Linux display environment for v1.** This is a statement of
scope, not a temporary gap to be quietly worked around.

Wayland forbids, by design, exactly the things this overlay is built on: an
application cannot make its own window click-through, cannot reliably identify
the focused window, cannot warp the pointer, and cannot place itself above other
surfaces without a compositor-specific protocol. `wlr-layer-shell` covers
wlroots compositors but not GNOME; portals cover some of the window information
but not the input side.

Consequently:

- The overlay **MAY** run under Wayland, and **MUST** detect it and report which
  features are unavailable (`clickthrough.unavailable_reason()` already does this
  for the click-through toggle specifically).
- The core's input path is unaffected — evdev works regardless of display server,
  so the cursor layer itself can function; it is the *overlay* that is limited.
- No compositor-specific code ships. A feature that works on KDE and silently
  fails on GNOME is worse than one that is documented as unavailable.

Revisit when a single mechanism covers the major compositors. The
`WindowBackend` seam exists for that implementation; see §15.7.

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
- Each key: rounded outline, face + border from theme tokens (or the key's own
  `style` overrides), one centred legend, optional `sub` text in the top-right
  corner, optional LED dot for `toggle` role.
- Painted in one pass with clipping to the damaged region; a key press repaints
  only that key's bounding rect plus its glow margin.
- Hit-testing exists only for the non-click-through mode (used by the binding
  editor and the layout editor, where clicking a key selects it).

**Segmented keys.** A logical key is drawn as the **union** of its segments, not
as a pile of independent rectangles:

1. Build one path per key by uniting its segment rectangles.
2. Round only the **exterior** corners of that union. Interior corners — where
   two segments meet — stay square, because a real ISO Enter has a sharp inner
   corner and rounding it produces a visible pinch.
3. Fill and stroke the united path once. Stroking each segment separately would
   draw a seam across the middle of the key.
4. Draw the legend once, centred on the **largest** segment by area. The centroid
   of an L-shape's bounding box lands in the notch, outside the key.
5. Highlight, fade and hit-test against the whole union (§4.1.3).

A single-segment key is the degenerate case of exactly this path and **MUST**
render identically to a plain rounded rectangle — the common case must not pay
for the rare one, visually or in code.

Rounding an arbitrary rectilinear union is more work than it sounds. The
supported case is small and fixed — a key of two axis-aligned segments sharing a
full or partial edge — and the renderer **MAY** restrict itself to that,
falling back to per-segment rounded rectangles for any union it cannot trace,
with a `layout.complex_key` diagnostic. Falling back to something visibly
imperfect is acceptable; silently mis-drawing the key is not.

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

## 10. Editors

Two editors share one canvas, one selection model and one hit-testing
implementation: the **layout editor** shapes the keyboard, the **binding editor**
decides what its keys do. Both render through the same `KeyboardView` the overlay
uses, in an editing mode -- two renderers would drift, and a user editing against
a picture that does not match the overlay is being lied to.

### 10.1 Layout editor

Editing JSON by hand is a fine way to author a layout and a poor way to *fix*
one. The bundled laptop layouts are representative templates rather than
model-exact reproductions (§15.5), so correcting a board to match the machine in
front of you is an ordinary user task, not an advanced one. It needs a visual
tool.

**JSON stays the persistence and interchange format.** It does not stop being the
source of truth, and hand-editing remains fully supported. It simply stops being
the only way in.

#### 10.1.1 Scope

The editor is a separate window in the Python application, opened from the
control bar. It edits **layout documents only** — not bindings, themes or
profiles, which have their own editors (§14, M5).

#### 10.1.2 Getting a layout to edit

The bundled layouts are read-only. Every editing session starts by producing a
user-owned copy:

| Action | Result |
|--------|--------|
| **New from template** | Pick a bundled layout, name the copy. `metadata.source_template` records the original id. |
| **Duplicate** | Copy any layout, bundled or user, under a new id. |
| **Edit** | Open a user layout directly. |
| **Reset to template** | Re-copy from `metadata.source_template`, discarding every change. Requires confirmation, and is refused when no source template is recorded. |

Attempting to edit a bundled layout **MUST** offer to duplicate it rather than
either silently forking or flatly refusing. Saving a copy whose `id` equals a
bundled one is permitted and is how the shadowing rule (§3.2) is meant to be
used — the UI **MUST** say plainly that the copy will replace the bundled layout
everywhere.

#### 10.1.3 Canvas operations

| Operation | Behaviour |
|-----------|-----------|
| **Select** | Click a key. Shift-click or rubber-band to extend. Selection is by logical key, never by segment (§4.1.3). |
| **Move** | Drag, or nudge with arrow keys. All segments of the key translate together. Multi-selection moves as a rigid group. |
| **Resize** | Eight handles on the selection's bounding box. A single-segment key resizes directly. A multi-segment key scales its segments proportionally, unless segment-edit mode is active. |
| **Add** | From a key palette (§10.4), or duplicate the selection. New keys land offset from the original and are selected. |
| **Delete** | Removes the selected keys. |
| **Assign code** | Change a key's physical `code` from a searchable list of the §2.1 vocabulary, or by pressing the physical key. |
| **Relabel** | Edit `legend.base`, `legend.shift` and `legend.sub` inline. |
| **Style** | Set or clear the per-key `style` overrides of §4.1.4. |
| **Align** | Left, right, top, bottom, centre horizontally, centre vertically, across the selection. |
| **Distribute** | Even horizontal or vertical spacing across three or more keys. |
| **Snap** | Configurable grid, default `0.25u`, matching the granularity real keyboards actually use. Toggleable, and suspended while a modifier is held. |
| **Undo/redo** | Unlimited within a session, over every operation above. |

The canvas renders through the **same** `KeyboardView` the overlay uses, in an
editing mode. Two renderers would drift, and the user would end up editing
against a picture that does not match what the overlay draws.

#### 10.1.4 Key palette

A panel of ready-made keys to drag onto the canvas, grouped by role: alphanumeric,
modifiers, function row, navigation, numpad, system. Each carries a sensible
default `code`, `legend`, `role` and size — dragging a `ShiftLeft` from the
palette produces a `2.25u` key labelled "Shift", not a blank `1u` square.

#### 10.1.5 Segment-edit mode

Entered on a selected key. While active:

- each segment shows its own resize handles and can be moved independently;
- segments can be added to and removed from the key, subject to a minimum of one;
- the rest of the canvas dims and is not selectable, so it is unambiguous that
  edits apply within one key;
- overlap between this key's own segments is **not** flagged (§4.1.5).

This is the only place segments are individually addressable. Everywhere else,
including plain resize, they move and scale as one.

#### 10.1.6 Validation surface

The editor runs the §4.1.5 rules continuously and shows results in a problems
panel. Clicking a problem selects the key it concerns.

- **Errors** block saving: duplicate key `id`, non-positive segment dimensions, a
  key with no segments.
- **Warnings** do not block saving, but **MUST** be acknowledged before the first
  save that introduces them: `layout.duplicate_code`, `layout.unknown_code`,
  `layout.overlap`, `layout.out_of_bounds`.

Overlapping keys are drawn with a hatched marker on the overlapping region so the
problem is visible on the canvas and not only in a list. The editor **MUST NOT**
resolve an overlap by moving anything (§4.1.5).

#### 10.1.7 Persistence, import and export

- **Save** writes to the user layouts directory, bumps `metadata.revision`, and
  emits `config_changed` so a running overlay picks it up without a restart.
- **Export** writes the document to a file the user chooses — the same JSON, so
  an exported layout is directly droppable into anyone else's `layouts/`.
- **Import** reads such a file, validates it, and reports problems before
  accepting. An import whose `id` collides prompts to rename or replace.
- **Autosave** keeps an in-progress draft so a crash does not lose an hour of
  nudging keys. Drafts are separate from saved layouts and are offered for
  recovery on next launch.

#### 10.1.8 Compatibility requirement

The editor introduces **no schema of its own**. It reads and writes exactly the
documents of §4.1, the same ones the renderer consumes and the bundled layouts
use. Anything the editor can express, a hand-written file can express, and the
reverse. This is why segments, stable ids and metadata are specified now, in v1,
even though the editor itself ships later (§14) — so its arrival is a feature
addition and not a breaking format migration.

### 10.2 Binding editor

Decides what each key does while the cursor layer is engaged. It edits binding
documents (§4.2), and it renders on the same canvas as the layout editor: click a
key on the drawn keyboard, choose a command, done.

#### 10.2.1 A key holds one command; a command may sit on many keys

These are not symmetric, and the asymmetry is what makes reassignment simple.

- **A key holds at most one command.** There is nowhere to put a second.
- **A command may be bound to several keys.** This is normal and useful — the
  shipped defaults put left-click on both `KeyD` and `Space`, because the thumb
  and the index finger are both good at it.

#### 10.2.2 Reassignment is silent

Assigning command **C** to key **K**:

1. If **K** already held command **P**, **P** is removed from **K**. No prompt,
   no warning, no confirmation dialog.
2. If **P** is now bound to **no key at all**, it moves to the **unassigned
   commands** list. If **P** is still bound elsewhere, nothing further happens.
3. If **C** came from the unassigned list, it leaves that list.
4. **C** is bound to **K**.

**No warning is shown at any point.** A dialog asking "this key is already used,
are you sure?" is noise: the user just picked a key and a command, and the
meaning of that is unambiguous. The old command is not destroyed — it is one
click away in the unassigned list — so there is nothing to protect the user
against.

This is a deliberate departure from the `layout.duplicate_code` warning of
§4.1.5, and the two are not in tension. A duplicate *physical key code* in a
layout is a mistake with no sensible reading — two keys claiming to be the same
key. A reused *key assignment* has exactly one sensible reading, and the editor
simply performs it.

#### 10.2.3 The unassigned commands list

A panel beside the keyboard holding every command that is currently bound to no
key. It is a first-class part of the editor, not an undo buffer.

It has two parts, presented as one list:

| Source | Contents |
|--------|----------|
| **Displaced** | Commands the user had configured that reassignment pushed out. Persisted in the document's `unassigned` array (§4.2), so they survive a restart with their parameters and custom legends intact. |
| **Available** | Every command in the action catalog (§7) not currently bound and not already listed above. Derived, never stored. This is what makes the list useful on first launch instead of empty. |

Assigning from the list is drag-to-key or select-then-click-key. A displaced
command **MUST** retain its `params` and its custom `legend` while it sits in the
list — a user who wrote "Send to left screen" on a key does not want to retype it
because they moved it.

The list **MUST** distinguish the two sources visually, because they mean
different things: one is *your* configuration waiting to be re-homed, the other
is a menu of things you have not used yet.

Clearing a key without replacing it (an explicit "unassign" action) moves its
command to the displaced list by the same rules.

#### 10.2.4 Other operations

| Operation | Behaviour |
|-----------|-----------|
| **Edit parameters** | Change a bound command's `params` in place — which monitor `window.move_to_monitor` targets, which grid cell `warp.grid` jumps to |
| **Custom legend** | Override the catalog's default label for this binding |
| **Tuning** | Edit the binding set's `settings` block: pointer speed, ramp, precision factor, scroll speed |
| **Duplicate set** | Fork a binding document under a new id, the same way layouts are forked (§10.1.2) |
| **Reset** | Restore the whole set to its source template, with confirmation |
| **Undo/redo** | Unlimited within a session, covering reassignment and displacement together — one undo puts both commands back where they were |

---

## 11. Diagnostics and failure behaviour

Every diagnostic carries a stable machine-readable `code`.

| Code | Level | Meaning |
|------|-------|---------|
| `layout.invalid` | warn | Layout document failed validation; skipped entirely |
| `layout.duplicate_id` | warn | Two documents claim one id; later wins |
| `layout.key_invalid` | warn | One logical key was dropped; the layout still loaded |
| `layout.duplicate_code` | warn | Two logical keys claim the same physical `code` |
| `layout.unknown_code` | warn | A key's `code` is outside the §2.1 vocabulary; it can never highlight |
| `layout.overlap` | warn | Two different logical keys overlap geometrically |
| `layout.out_of_bounds` | warn | A segment extends past the declared `size` |
| `layout.complex_key` | info | A segment union the renderer could not trace; drawn per-segment |
| `layout.upgraded` | info | A schema 1 document was upgraded in memory |
| `binding.unknown_action` | warn | Binding skipped |
| `binding.unassigned_conflict` | warn | A command appears in both `bindings` and `unassigned`; the binding wins |
| `binding.unknown_key` | warn | Binding references a code absent from the active layout |
| `profile.invalid` | warn | Profile skipped |
| `config.clamped` | info | A setting was outside its range |
| `input.permission_denied` | error | Cannot open the input device / install the hook |
| `input.hook_lost` | warn | Windows unhooked us; re-installing |
| `input.elevated_window` | info | Interception inert; an elevated window has focus |
| `window.unsupported` | warn | A window operation is unavailable on this backend |
| `ipc.client_overflow` | warn | A client's queue overflowed; events dropped |
| `ipc.version_mismatch` | error | Client and core protocol majors differ |
| `ipc.endpoint_unsafe` | error | A component of the endpoint's path failed a type, ownership or permission check (§5.1.2); the core refused to bind |
| `ipc.endpoint_in_use` | error | Another core holds the startup lock, or the endpoint is live or not provably stale (§5.1.3); the core refused to start |

**The governing rule (P6):** one bad file never prevents the rest from loading,
and a missing capability is always reported and disabled — never emulated with
something that merely looks similar.

---

## 12. Security and permissions

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
   a nicety. It covers the *directory* the endpoint sits in as much as the
   endpoint itself: a directory another user owns is a directory in which the
   core's socket can be replaced by theirs, so the core verifies ownership and
   mode before it binds and refuses rather than repairs (§5.1.2). Only the core
   ever creates or removes an endpoint; the overlay and the launcher connect and
   nothing else (§5.1.3).
4. **Positional codes only on the wire** (§5.3). The core does not resolve
   keystrokes to characters, so the IPC stream cannot reconstruct typed text
   without independently knowing the user's keyboard layout.
5. **Least privilege.** The core takes exactly the privileges its backend needs —
   the `input` group on Linux via a shipped udev rule, and *no* elevation on
   Windows by default. Elevation is opt-in, and the UI explains what it buys
   (interception over elevated windows) and what it costs.

---

## 13. Testing strategy

| Layer | Approach |
|-------|----------|
| Key vocabulary | Round-trip every code through each backend's table; assert bijection and no unmapped entries |
| Layout registry | Validate every bundled layout against the schema; assert every bound key exists in every bundled layout; assert key and segment ids are unique and stable across a save/load round-trip |
| Layout geometry | Assert no two **different** logical keys overlap in any bundled layout, and that segments of the *same* key are exempt from that rule; assert the ISO Enter is two segments and the ANSI Enter is one; assert every segment lies within the declared `size` |
| Schema upgrade | Load a schema 1 document and assert it produces the same logical keys as its schema 2 equivalent, with synthesised ids |
| Binding editor | Table-driven reassignment: assigning over an occupied key displaces exactly one command, and it reaches `unassigned` only when its last key is taken. Assert a command bound to two keys survives one of them being reassigned. Assert `params` and custom `legend` survive a displace/restore round-trip |
| Update policy | Load a settings file missing new keys and assert defaults appear; load one carrying removed keys and assert they survive a rewrite; assert loading a prior-schema document never writes to disk |
| Layout editor | Property test: every canvas operation (move, resize, align, distribute, snap, add, delete, segment edit) leaves a document that still validates. Round-trip test: save → load → save is byte-identical. Undo/redo returns to the exact prior document |
| Layer engine | **Pure unit tests over a synthetic event trace.** The engine takes events and a clock and returns decisions — no OS involved. This is where the grace window and P7 are proven. |
| Grace window | Table-driven: for each of the three resolutions in §6.3, assert the exact output event sequence |
| P7 invariant | Property test over 200 randomised event sequences, across all three activation modes and including modifiers and a `key.passthrough` binding: assert every forwarded press has a matching forwarded release once the sequence is wound down |
| P7 mirror | The same sweep in the opposite direction: assert no release is ever forwarded without a matching forwarded press, so a suppressed key held across a mode change cannot produce an orphan key-up |
| Property-test coverage | Assert the generator is not degenerate: that it actually produced forwards, suppressions, actions, action releases, buffered presses and repeats in quantity. A generator that quietly stopped exercising a path would keep passing and prove nothing |
| Event ordering | Exact-sequence tests: buffered presses expiring together, buffered presses promoted by CapsLock, held actions released on leaving the layer, and `release_all` all emit in press order. Plus a replay test asserting identical decision sequences across repeated runs |
| Allocation contract | Drive the engine through many event cycles against one `DecisionBuffer` and assert it never overflows its fixed capacity; separately construct the true worst-case unwind and assert the derived capacity holds it |
| Forced capacity overflow | Deliberately exceed each bounded list — forwarded presses, held actions, buffered presses — with synthetic key codes, and assert the fail-safe: no forwarded press left unreleased, no action started without a release, no keystroke silently dropped where degrading would do. Observing that ordinary traffic does not overflow proves nothing about overflow |
| Key domain | Assert a code interned beyond the built-in vocabulary is tracked like any other, that an invalid code is suppressed in both directions, and that the largest valid id is tracked, bindable and safe to index |
| Malformed CapsLock | Duplicate press does not double-toggle; orphan release does not change the mode or the latch; a stale press time cannot latch the layer later; an escape gesture suppressed for want of capacity leaves the mode and latch untouched through both its press and its release, in all three activation modes |
| Reload | Assert a pending press survives a reload that keeps its binding, keeps its original press time, and resolves as a forwarded keystroke when the binding is removed |
| Motion | Assert diagonal speed equals cardinal speed; assert fractional accumulation never loses pixels; assert the ramp is monotonic |
| IPC | Golden-file tests of serialised messages; a fake client that stops reading, asserting the core does not block |
| Overlay | `pytest-qt` against the mock backend; render each bundled layout at several scales and assert no key overlaps and no clipped legends |
| Platform backends | Manual test matrix, documented in `docs/manual-tests.md` — these cannot be meaningfully automated without a real display and real hardware, and pretending otherwise produces tests that pass while the product is broken |

The layer engine being pure — a function of `(events, clock)` — is a deliberate
design constraint, not an accident. It is the only way the concurrency-sensitive
logic inherited from the prototype can be tested at all.

---

## 14. Milestones

| # | Milestone | Contents |
|---|-----------|----------|
| **M0** | Foundation | Docs, repo layout, schemas, bundled layouts, default bindings, themes, IPC protocol definition |
| **M1** | Overlay on the mock backend | Renders any layout **including segmented keys**, all four legend layers, feedback, themes, click-through, pin, opacity, persistence |
| **M2** | Core skeleton + IPC | Engine, motion integrator, action dispatcher, IPC server, unit tests. No OS backends — driven by a synthetic input backend |
| **M3** | Windows backend | Hook, `SendInput`, Win32 windows/monitors. End-to-end on Windows |
| **M4** | Linux/X11 backend | evdev, uinput, EWMH/XRandR, udev rule, all-keyboard grab. End-to-end on Linux |
| **M5** | Configuration UI | Settings dialog, binding editor with silent reassignment and the unassigned-commands list, profile editor |
| **M6** | **Visual layout editor** | Canvas, key palette, move/resize/align/distribute/snap, segment-edit mode, validation panel, import/export, reset-to-template |
| **M7** | Packaging | Windows installer, Linux packages, first release |

**M1 is revised, not extended.** Segmented keys land in the schema, the loader,
the validator, the renderer and the bundled layouts *now*. What is deferred to
M6 is only the editing UI.

That split is deliberate. The format is the thing that is expensive to change
later — every layout anyone has authored has to be migrated — while an editor is
additive and can arrive whenever. Shipping the format without the editor costs
nothing; shipping the editor without the format would mean a breaking migration
the first time someone drew an ISO Enter.

**The editor is placed after the platform backends** because a layout editor is
most valuable once the layer it configures actually drives the mouse, and because
it shares its canvas, selection model and hit-testing with the M5 binding editor.
Building the binding editor first means the layout editor inherits working
infrastructure rather than inventing it.

**The launcher scripts land between M4 and M5.** Their contract — option
surface, prerequisite and privilege rules, autostart mechanism and exit codes —
is specified in [LAUNCHING.md](LAUNCHING.md) and is settled *now*, before M2,
because M2 creates the `keygnosys-core` executable and the IPC server the
launcher will manage. The implementation waits for M4 so that it is designed
against two real platforms rather than against Windows plus a guess: written
after M3 it would be a Windows-shaped design with a Linux script retrofitted
into it, and the two platforms disagree on process detachment, endpoint
semantics, autostart mechanism and toolchain discovery. The launcher is not the
installer; packaging remains M7.

---

## 15. Resolved design decisions

These were open questions during design. They are settled. Each is recorded here
with its rationale and a pointer to the section that implements it, so a later
reader can tell a decision from an accident.

### 15.1 Non-rectangular keys — segmented rectangles

**Decided.** A logical key is drawn as one or more axis-aligned rectangular
segments sharing a single identity. ISO Enter is two segments. Arbitrary SVG
paths and polygons are **not** supported.

Approximating an ISO Enter as one rectangle was rejected: it is wrong on every
ISO board, and the format would have needed a breaking change to fix it later.
Arbitrary paths were rejected as disproportionate — they solve one key shape at
the cost of a node editor, path hit-testing, and path overlap detection.

Segments render, highlight, move and hit-test as one key (§4.1.3). They are
individually addressable only inside the editor's segment-edit mode (§10.5).

*Correction from an earlier draft: the ISO left Shift is **not** L-shaped. It is
a short rectangle with a separate `IntlBackslash` key beside it, and the two are
modelled as two independent logical keys.*

→ §4.1, §9.3, §10.5

### 15.2 Visual layout editor — specified, deferred to M6

**Decided.** Users are not required to hand-edit JSON. A visual editor with
drag-to-move, handle resize, a key palette, multi-select, snap, align,
distribute, segment editing, rename, reset-to-template, and import/export is a
specified product feature.

JSON remains the persistence and interchange format — hand-editing stays fully
supported — but it becomes an implementation detail for ordinary users rather
than the only way in.

The **schema and renderer support ship in M1**; the editing UI ships in **M6**.
The reasoning is in §14: formats are expensive to change after people have
authored against them, editors are not.

→ §10, §14

### 15.3 One schema for everything

**Decided.** Bundled layouts, user layouts, the renderer and the editor all use
the same document format. The editor introduces no schema of its own and can
express nothing a hand-written file cannot.

The format carries stable layout, key and segment ids; unit-based rather than
pixel-based geometry; physical key codes independent of labels and geometry;
optional labels and per-key style overrides; metadata naming the source template
and author; and forward-compatible `schema` versioning.

Validation is graded rather than all-or-nothing (§4.1.5). Overlapping *different*
logical keys produce a warning that the editor surfaces and the user must
acknowledge — never a silent failure and never an automatic correction. Segments
of the *same* key are exempt, since sharing an edge is how an L-shape is built.

→ §4.1.2, §4.1.5, §10.8

### 15.4 Layout naming — conventional key counts

**Decided.** `us-ansi-104` is the standard US ANSI full-size layout and the
default. `us-iso-105` is the ISO full-size variant. Both are bundled. "104" is
never used as a generic name covering both.

"105-key" conventionally denotes the ISO board while the standard US full-size
board is ANSI 104, so collapsing them under one name would be wrong for one set
of users whichever name was chosen.

→ §4.1, `data/layouts/`

### 15.5 Laptop layout fidelity — templates, not guarantees

**Decided.** The bundled ThinkPad and Asus layouts are **representative starting
templates** based on current mainstream models. They are not claimed to be
model-exact, and they should not be described as though they were.

Users duplicate the closest template and adjust it visually to match their own
machine (§10.2). Every difference between boards is expressed in **layout data** —
never in renderer-specific code, and never in a per-model branch. `metadata`
records the source template and any model information.

This is what makes the approximation acceptable rather than sloppy: being
slightly wrong is fine when correcting it is a two-minute drag-and-drop.

→ §4.1.2 `metadata`, §10.2

### 15.6 Multiple keyboards on Linux — all devices, one layout

**Decided.** All detected keyboard devices are grabbed. Events from every one of
them map onto the single layout the user has selected, matched by physical key
code. Layouts do **not** switch based on which device an event came from.

A map that redraws itself when the user's hands move between a laptop board and
an external one is worse than one that is occasionally slightly wrong about key
shapes — the overlay's value is in being stable enough to memorise.

The architecture does not preclude per-device layout selection later:
`InputBackend` reports a device id with each event and the engine assumes no
single source. Implementing it is out of scope for v1.

→ §8.1

### 15.7 Wayland — deferred, X11 is the supported Linux target

**Decided.** **X11 is the supported Linux display environment for v1.** Native
Wayland support is deferred, not abandoned.

Wayland forbids, by design, an application making its own window click-through,
identifying the focused window, or warping the pointer. There is no mechanism
that covers GNOME, KDE and wlroots uniformly, and the alternative is
compositor-specific implementations that multiply the Linux surface area and the
testing burden.

Revisit when `wlr-layer-shell`, a suitable desktop portal, or an equivalent can
provide the required overlay behaviour without per-compositor branches. The
`WindowBackend` seam exists for it. Until then the limitation is stated plainly
in the UI and the README rather than partially emulated.

→ §8.4, §9.2, README platform table

### 15.8 Reassigning a key is silent

**Decided.** Assigning a command to a key that already holds one displaces the
old command without a prompt, a warning, or a confirmation. The displaced command
moves to the unassigned list, where it keeps its parameters and its custom label
and can be re-homed in one click.

A confirmation dialog here would be noise. The user has just named a key and a
command; there is one possible meaning, nothing is destroyed, and the undo is
immediate and visible.

A key holds one command; a command may be bound to several keys. Only the key
side is exclusive, which is why displacement is well-defined and why binding
left-click to both `Space` and `KeyD` remains legal.

*This does not contradict the `layout.duplicate_code` warning of §4.1.5. Two
layout keys claiming the same physical code is a mistake with no sensible
reading. A reused key assignment has exactly one.*

→ §4.2, §10.2.2, §10.2.3

### 15.9 An update never costs the user their configuration

**Decided.** Updates add capability and nothing else. The installer never writes
to the user config root; user documents permanently shadow bundled ones; settings
gain new defaults without losing existing values, including values whose keys
have been retired; schema upgrades happen in memory and never rewrite a file the
user did not save; and new actions reach existing users because the editor offers
commands from the live catalog rather than a stored inventory.

The one honest cost is stated rather than hidden: a forked document stops
receiving upstream improvements to its original. That is inherent to guaranteeing
the fork is never touched, so the UI surfaces the divergence and offers a review
rather than pretending it does not exist.

→ §3.4

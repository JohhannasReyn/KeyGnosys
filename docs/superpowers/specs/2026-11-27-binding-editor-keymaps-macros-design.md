# Binding editor, per-program keymaps and macros — design

**Status:** draft, 2026-11-27. Brainstormed with the operator in this session.
**Supersedes for these features:** SPEC §10.2 (binding editor placement), the
M5 row of §14 (ordering), and §3.2 (fork UX). Each amendment is listed in §9.

## 1. Intent

The operator wants the cursor layer to become something people configure for
themselves. That means three things:

- **Redefine what each key does, from the overlay they already have on screen.**
- **Have the map follow them from program to program automatically.**
- **Record macros that drive both the keyboard and the mouse**, assign them
  to keys, and share them with other people who use the same program.

The operator sees "macros + mapping + mouse control" as what will make
KeyGnosys worth promoting. The layer already owns the mouse, and macros are
how that becomes leverage.

**Success looks like this:**
- A user enters define mode, clicks a key, and picks an action. The key does
  the new thing the next time they engage the layer.
- They tag Blender once. From then on the layer is their Blender map whenever
  Blender has focus, even while they flip back and forth with another program
  every few seconds.
- They record "export the selection" in a program, put it on a key, export
  it, and a friend imports it. It works on the friend's machine even though
  the friend's window is a different size.

## 2. Goals, non-goals, anti-goals

### Goals

The work is four projects, in this order. Each one gets its own plan and PR.

| # | Project | What it delivers |
|---|---------|------------------|
| 1 | **Binding editor** | Define mode on the overlay. A side picker, silent reassignment, the unassigned list, custom labels, parameter editing, layer-settings tuning, and undo/redo. The universal map only. |
| 2 | **Export / import** | A user's maps (and, once project 4 exists, their macros) to and from a single file. |
| 3 | **Per-program keymaps** | Tag a program. The core switches the effective map on focus change. The editor gains an "Editing: <program>" selector. |
| 4 | **Macro recorder** | Keyboard and mouse recording, three click-placement modes, a macro editor with prepend and append, `macro.run` with a safety stop, and universal or per-program scope. |

### Non-goals (for all four)

- No scripting language, conditionals or loops in macros. A macro is a flat
  list of recorded steps.
- No image or pixel matching to find click targets. It is too fragile and too
  heavy.
- No online sharing service. Sharing means a file the user sends however they
  like.
- No Linux implementation now (M4 is still ahead). The core-side pieces are
  specified backend-neutrally, so Linux can implement them later.

### Anti-goals (what would make this a failure even if it works)

- **A key left held down.** That covers a macro aborted mid-press, a program
  switch mid-drag, and a reload mid-edit. P7 applies to everything here.
- **Losing a user's work silently.** That covers a displaced command that
  vanishes, an edit to a shipped file that a future update overwrites, and
  an import that clobbers without asking.
- **Features that need the overlay running.** The overlay is a viewer (P4).
  Per-program maps and macros must keep working with the overlay closed.
- **Lag when switching programs.** If flipping between two programs makes the
  layer stutter or briefly use the wrong map, the feature has failed.
- **A recorder that records when the user didn't ask it to.** It records
  only while explicitly armed, and the arming is visible.

## 3. Constraints from the existing system

- **P4:** clients are viewers. Killing the overlay must not disturb typing,
  and now must not disturb maps or macros either.
- **P7:** every forwarded or synthesised press gets its release, on every exit
  path. Macros add a new source of synthesised presses.
- **The core ignores injected input** (`LLKHF_INJECTED`). Macro playback can
  therefore never trigger itself or the layer.
- **User documents shadow bundled ones by `id`** (SPEC §3.2). The core searches
  `%APPDATA%\KeyGnosys\bindings\<id>.json` before `data/bindings/<id>.json`.
- **The core has no config file watcher.** A client applies changes by sending
  `reload_config`. Today `reload()` calls `releaseEverything()` first, which is
  fine for an edit and **not** fine for a program switch (see §5.2).
- **The activation key is not a binding.** CapsLock always leaves the layer, so
  no edit can lock a user inside it. The editor never offers it as a target.
- **Already built, and the editor uses it:**
  - in `documents.BindingSet`: `assign()` (silent reassignment, SPEC
    §10.2.2), `unassign()`, `keys_for()` and `available_commands()`, with
    tests in `tests/test_assignment.py`;
  - `actions.CATALOG`, `COMMAND_PARAMS` and `catalog_commands()`;
  - `KeyboardView.keyClicked` (emitted, connected to nothing);
  - the `NoticeTray` docking pattern;
  - `focus` events from the core, already received by the overlay.
- **Missing:**
  - `BindingSet` has no serialiser;
  - nothing writes to the user config dir except `settings.py`.

## 4. Approaches (chosen: 1)

### Approach 1 — the core runs; the overlay edits and records (chosen)

- **The core:**
  - resolves the effective map from the focused window;
  - plays macros and aborts them;
  - captures recorded input.
- **The overlay:**
  - hosts define mode and the editors;
  - writes the documents;
  - tells the core to reload.
- **Why:**
  - It keeps everything working with the overlay closed (P4).
  - Program switches are instant, because the core already sees focus changes.
  - The safety stop sits next to the layer state it depends on.
  - Macro presses go through the journal that already guarantees P7.
- **Cost:** C++ work and SPEC additions, mostly in projects 3 and 4.
  Project 1 needs two small core changes (§6.4.1 and §6.4.2).

### Approach 2 — the overlay does everything (rejected)

The overlay would watch focus, send `set_bindings` on every switch, and play
macros with its own `SendInput`.

- **Its appeal:** it is quicker to build, in Python only.
- **Rejected because:**
  - Closing the overlay silently disables maps and macros.
  - Every switch costs a round trip, and `set_bindings` releases everything.
  - The abort-on-leaving-the-layer stop would race against the core.
  - It breaks P4 and P7 as validated in M3.

## 5. The design across all four projects

These formats are fixed now, so the early projects don't paint the later ones
into a corner. Field-level detail beyond what is written here is the
builder's, inside each project's plan.

### 5.1 Documents

**Universal map.** This is the existing `keygnosys/bindings/2` document,
unchanged. The active id is `behavior.bindings` in settings, `default` out of
the box.

**Program map.** This is a new kind, `keygnosys/appmap/1`, stored in a new
directory, `appmaps/`.

```jsonc
{
  "schema": "keygnosys/appmap/1",
  "id": "blender",
  "name": "Blender",
  "priority": 50,
  "match": { "process": ["blender.exe"], "wm_class": [], "title_regex": null },
  "bindings": { "KeyQ": { "action": "macro.run", "params": { "id": "export-fbx" }, "legend": "Export FBX" } },
  "unassigned": []
}
```

- **Matching is identical to app profiles** (SPEC §4.4: any field matches,
  case-insensitive, highest `priority` wins).
- **`bindings` holds only the keys this program overrides.** Every other key
  falls through to the universal map, so a later change to the universal map
  still reaches every program that didn't override that key. The operator
  accepted this proposal.
- **A key a program wants to do nothing** is an explicit
  `{ "action": "none" }` override. Project 3 settles whether "nothing" means
  suppressed or passed through.

**Macro.** This is a new kind, `keygnosys/macro/1`, stored in `macros/`.

```jsonc
{
  "schema": "keygnosys/macro/1",
  "id": "export-fbx",
  "name": "Export FBX",
  "scope": { "appmap": "blender" },          // or "universal"
  "placement": "anchored",                     // default for new steps
  "recorded": { "window": [1600, 900], "dpi": 144, "screen": [2560, 1440] },
  "speed": 1.0,
  "steps": [
    { "t": 0,   "kind": "key",    "code": "ControlLeft", "down": true },
    { "t": 40,  "kind": "button", "button": "left", "down": true,
      "at": { "proportional": [0.94, 0.03],
              "anchored": { "edge": "top-right", "dx": -96, "dy": 27 },
              "control": { "name": "Export", "type": "Button", "automation_id": "btnExport" } },
      "placement": "control" }
  ]
}
```

- **`t` is milliseconds since the previous step.** Playback honours it,
  scaled by `speed`.
- **Every pointer step records all three placements.** `placement` picks which
  one replays, per step, and defaults to the macro's `placement`. This is the
  operator's choice: all three are built in, chosen at record time, and
  changeable per step afterwards.
- **`scope`:** a program macro replays relative to the focused window. A
  universal macro replays relative to the screen. (Operator decision.)
- **`control` is optional.** It is absent when the program doesn't reveal its
  controls.

### 5.2 Core responsibilities (projects 3 and 4)

**The effective map.**
- `universal ⊕ best-matching appmap`, recomputed on every `focus` change.
- **A switch swaps the lookup table only. It releases nothing.**
  - A held action finishes under the map it started in.
  - The next press uses the new map.
  - This is the operator's "smooth, set-and-forget" requirement, and it is
    why program switching cannot reuse `set_bindings`/`reload()`, which
    release everything.

**`macro.run {id}`, a new action.**
- **Plays the steps through the same output path and journal as every other
  synthesised press**, so P7 covers it.
- **The safety stop (operator decision):** leaving the layer aborts the rest
  of the macro, and everything the macro holds is released.
  - Leaving covers the activation key, `layer.release` and every existing
    `releaseAll` exit path.
- **A second press of the same key while it runs:** project 4 decides between
  abort and ignore.

**Recording.**
- **New IPC commands:** `record_start {kinds:["keys","mouse"], placement}` and
  `record_stop`.
- **The core streams `record_step` events to the requesting client.** For a
  click, it also captures the control under the pointer at click time,
  off the hook thread.
- **Mouse recording is armed and disarmed by Space (operator decision).** The
  core swallows those two presses and doesn't record them.
- **The client assembles the steps and writes the macro document.**

### 5.3 Overlay responsibilities

- **Define mode, the side picker and the program selector:** projects 1 and 3.
- **The macro editor:**
  - It shows the step list, each step's placement mode, and the speed.
  - **Prepend and append** splice another macro's steps in as a **copy**.
    That is my call: a copy keeps every macro self-contained, so exporting
    one never needs its dependencies.
- **Writing documents:** atomic write (temp file, then replace) into the user
  config dir, then `reload_config`.
- **Export and import:** project 2.

### 5.4 Export / import (project 2)

- **The bundle is one JSON file** with `"schema": "keygnosys/bundle/1"`. It
  holds any number of bindings, appmap and macro documents, each embedded
  verbatim.
  - An exported bundle that holds one document is also accepted when the
    file is a bare document of a known kind, matching SPEC §10.1.7's
    "directly droppable" rule.
- **Import:**
  - validates every document;
  - reports every problem before accepting anything;
  - on an `id` collision, prompts for rename or replace, per document;
  - never writes a partial bundle.
- **Macros carry their `recorded` block**, so the importer can see the
  source's window size and DPI.

## 6. Project 1 in detail: the binding editor

### 6.1 What the user sees

- **A Define toggle on the control bar.**
- **Turning it on:**
  - **Turns click-through off for the duration** and restores the previous
    state on exit. In click-through mode the overlay can't receive the
    clicks it needs.
  - **Shows a `● DEFINE MODE` indicator on the top bar.** Undo, Redo and Done
    sit next to it. The "Editing: <program>" selector and Record macro are
    reserved for projects 3 and 4 and are absent in project 1.
- **Clicking a key selects it** (orange outline) and opens the **side picker**
  beside the keyboard (mockup option A, chosen by the operator).
  - **The picker docks to the keyboard window's right edge.** It follows
    drags and repositions like `NoticeTray`, but it accepts input.
  - **Picker contents, top to bottom:**
    1. **The header:** "Assign to D (now: Left click)".
    2. **A search field** that filters every list below.
    3. **Unassigned.** Commands bumped off a key, kept with their params and
       label. They are drawn distinctly, showing "was on A".
    4. **The catalog's available commands, grouped:** Clicks · Pointer ·
       Scroll · Warp · Windows · Layer & system.
    5. **For the key's current command:** its parameter editor, a label field
       and Clear key.
- **Choosing an item assigns it to the selected key immediately.** This is
  silent reassignment, via `BindingSet.assign`.
  - **Drag-to-key and select-then-click** also work in the other direction:
    pick an item, then click any key (SPEC §10.2.3).
- **Parameterised commands** ask for their parameter inline, from
  `COMMAND_PARAMS`, before assigning. Examples: `warp.grid` cell,
  `window.slot` index, `button.drag_lock` button.
- **A Layer settings section** (collapsible, at the bottom of the picker)
  edits the bindings document's `settings` block: pointer speeds, ramp and
  precision factor (SPEC §10.2.4).
- **Undo and Redo** cover every change made in define mode: assign, clear,
  label, params, settings. Clicking **Done**, or switching define mode off,
  leaves the mode.

### 6.2 Saving and applying

- **Every change saves immediately** (atomic write) and then sends
  `reload_config`, debounced so a burst of edits causes one reload.
  - Saving each change is my call. A separate Save step is one more way to
    lose work, and undo makes immediate saving safe.
- **The first edit of a document that exists only in the bundled `data/`
  directory forks it.** That means a copy to the user dir under the **same
  id** (so it shadows the shipped one), with `metadata.source_template` set
  to that id.
  - The fork is silent. It is what "edit" means, and SPEC §3.2's one-click
    fork is satisfied by it.
- **Reset to shipped** (in the picker's settings section) deletes the user
  copy after a confirmation. It is offered only when a user copy shadows a
  shipped one.
- **`BindingSet` gains `to_dict()`.** It round-trips every field it parses,
  and bumps `metadata.revision` on save.
- **If the core is absent** (mock backend), editing still saves. The notice
  strip says changes apply when the core is running.
- **If the core reports a reload failure**, the notice strip shows it, and the
  last good map stays in effect (§6.4.1).
- `reload()` releases everything. This is acceptable while editing, because
  in define mode the user is working with the mouse on the overlay, not
  holding layer keys.

### 6.3 Tests project 1 must have

- **Table-driven reassignment** (SPEC §10.2 tests). `tests/test_assignment.py`
  already covers the model. Extend it only where the editor adds paths:
  - assign from unassigned;
  - assign a parameterised command;
  - clear.
- **Params and a custom legend survive** displace → unassigned → restore,
  through save and reload (`to_dict` → `from_dict`).
- **Fork:**
  - the first save writes to the user dir under the same id with
    `source_template`;
  - the shipped file is untouched.
- **Undo/redo restores the exact document**, settings included.
- **Define mode restores the user's click-through state on exit.**
- **Losing the core does not end define mode** (§6.4.6).
- **The activation key is never offered as an assignment target.**
- **The core keeps the last good map when a user document is broken, and emits
  `binding.invalid`** (a `ctest`).
- **A parameter edit to a bound command** displaces the old identity, under the
  same rules as any assignment.
- **`was_on` round-trips** through save and reload, and is cleared when the
  command is assigned again.

### 6.4 Settled during the builder check

These are my calls, marked so the operator can check them.

1. **A broken map never silently becomes the shipped one.**
   - **Today:** `loadBindings` falls through the search path when a document
     fails to parse. A broken user copy is replaced by the shipped
     `default` without a word, and `reload()` replies success regardless.
   - **Project 1 changes the core** (small, C++):
     - **On reload or `set_bindings`,** if the user-dir document for the
       active id exists but fails to load, the core **keeps the map it is
       running** and emits a `binding.invalid` diagnostic naming the file and
       the reason.
     - **At startup** there is no running map. The core may fall back to the
       shipped document, with the same diagnostic.
   - **The overlay already shows diagnostics in the notice strip.** It also
     validates every document before writing, so the editor cannot produce
     a broken one. The core rule exists for hand edits.
   - SPEC §3.3 gets this rule.
2. **The editor edits the map the core is actually running.**
   - **Today:** the core takes its id from `--bindings` (default `default`) and
     never reads `behavior.bindings`.
   - **Change:** `hello` gains `bindings_id`. `set_bindings` and `reload_config`
     both broadcast `config_changed` with `bindings_id`, and `set_bindings`
     doesn't broadcast today.
   - **The editor edits that id.** With the mock backend, it uses the setting.
     The overlay doesn't send `set_bindings` on connect, because that
     releases everything and a reconnect must not disturb typing.
3. **A parameterised action is one picker entry, not one per value.**
   - Choosing `window.slot` asks for the index inline.
   - Values already on a key are marked "on 3" but stay choosable, because a
     command may sit on several keys (§10.2.1).
   - **Editing a bound command's parameter is an assignment of the new
     identity to the same key.** The old identity goes to `unassigned` if it
     is now bound nowhere, exactly as SPEC §10.2.2 says for any displacement.
   - The model needs no new method beyond what `assign` already does.
4. **"Was on A" survives restarts** through a new optional field, `was_on`, on
   `unassigned` entries.
   - It is additive within `bindings/2`. The core already ignores unknown
     fields there (`config.cpp`, the `unassigned` loop).
   - It is set on displacement and dropped on reassignment.
5. **Duplicate set and Reset to source template (§10.2.4) are deferred.**
   - With per-program maps (project 3), one universal map per user is the
     expected shape. A second universal set has no UI to switch to it yet.
   - Project 1 offers Reset to shipped for the same-id fork only. The SPEC
     amendment says so.
6. **Losing the core does not end define mode.**
   - Edits keep saving, and the notice strip says they apply when the core
     returns.
   - **On reconnect,** the overlay sends `reload_config` once if anything
     was saved while disconnected.
7. **Undoing past the first edit leaves a user copy identical to the shipped
   document.** It does not delete the file, so "Reset to shipped" still
   appears. Undo restores document content. It never creates or deletes
   files.

## 7. Projects 2–4: decided now, detailed in their own plans

Recorded here so each later plan starts from the operator's decisions.

**Project 3:**
- **"Tag a program"** offers the currently and recently focused programs by
  process name. There is no free-typing requirement.
- **Unchanged keys show dimmed** (inherited from universal), and overridden
  keys are tinted. This is the mockup shown on 2026-11-27.
- **Reset <program> to universal** deletes the overrides, with confirmation.

**Project 4:**
- **Mouse recording:** choose "Record mouse macro", pick a placement mode,
  press Space to start and Space to stop.
- **Prepend and append** recorded actions to other macros, to build series of
  events.
- **A macro is universal or belongs to one program.**
- **The recorder indicates live whether the program under the pointer reveals
  its controls**, so the user knows whether Control mode will work.
- **A step that falls back** (Control → Anchored on replay) is reported once
  per run in the notice strip, naming the step.
- **Recording keystrokes captures passwords too.** The recorder is visible
  while armed, and that warning goes in the user docs.

## 8. Left to the builder

- Widget structure, signal wiring and class names inside the overlay.
- How the picker sizes itself at different overlay scales.
- The undo stack's representation (snapshots are fine at this document size).
- The debounce interval.
- The exact catalog grouping labels.

## 9. SPEC amendments this work makes

These are made in project 1's PR unless noted.

1. **§10.2:** the binding editor lives on the overlay as define mode with a
   side picker, not in the settings dialog. Its behaviour (§10.2.1–§10.2.4) is
   unchanged.
2. **§14:** the binding editor moves ahead of M4. Per-program maps and macros
   are added as new work items; the milestone layout is updated then.
3. **§3.2:** editing a shipped document forks it automatically and silently.
   Reset to shipped replaces the separate one-click fork action. Duplicate set
   and Reset to source template (§10.2.4) are deferred (§6.4.5).
4. **§3.3:** a user document that fails to load never replaces the running
   map (§6.4.1). **§7 (IPC):** `hello` carries `bindings_id`, and so does
   `config_changed` (§6.4.2).
5. **§4.2:** `unassigned` entries may carry `was_on`.
6. **Project 3:** a new §4.x for `appmap/1`, plus the no-release switch rule.
7. **Project 4:** a new §4.x for `macro/1`, a new action `macro.run`, the
   `record_*` IPC, and the safety stop as a P7 exit path.

## 10. Open, to be settled in later projects

- **Project 3:** whether `{ "action": "none" }` suppresses the key or passes it
  through to the program.
- **Project 4:**
  - how a **keyboard** recording is stopped, given that Space can be part of
    what's being recorded;
  - what a second press of a running macro's key does.

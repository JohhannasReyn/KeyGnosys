# Export and import of configurations (project 2) — design

**Status:** draft for operator approval, 2026-11-27.

**Parent design:**
- `2026-11-27-binding-editor-keymaps-macros-design.md`, §5.4.
- This document fills in project 2 inside the frame that design fixed. Where
  the two disagree, this one is newer and wins for project 2.

**Operator answers:** the 14 questions put on 2026-11-27 were all accepted as
suggested, except Q14, which the operator later reversed: dropping a file on
the keyboard imports it (§6.1). They are recorded as decisions below, marked **(operator)**.
Everything marked **(my call)** is mine and open to correction.

## 1. Intent

**The aim is sharing a setup, and backing it up.**
- Someone who has shaped their cursor layer can save it to one file.
- They can send it to someone else, or keep it for a new machine.
- Loading it back takes one button and one look at what will change.

**This is the start of the "macros + mapping + mouse control" pitch.** A map
that can't leave the machine can't be shared, posted or downloaded.

**Today there is one kind of thing to share: the universal binding map.**
- Program maps (project 3) and macros (project 4) don't exist yet.
- The file format and the import flow must already make room for them, so
  those projects add a kind rather than redesign the feature.

## 2. Goals, non-goals, anti-goals

### Goals
- **Export** writes the running map to a file the user names.
- **Import** reads such a file, shows what it will do, and on confirmation
  makes it the running map.
- **The file is plain JSON** with a recognisable extension. It opens in a
  text editor and is safe to email.
- **Nothing is lost.**
  - An import is undoable.
  - The map it replaced is kept on disk.
  - Commands this build doesn't know survive the round trip.

### Non-goals (this project)
- Exporting or importing overlay settings, layouts or themes **(operator, Q3)**.
- Picking individual keys out of an imported map **(operator, Q9)**.
- Keeping several universal maps side by side and switching between them
  **(operator, Q5)**.
- An online gallery, links or any network use.

### Anti-goals (a failure even if it works)
- **An import that half-applies.** It is all or nothing.
- **An import that silently drops something.** That covers keys bound to
  commands this build doesn't know, and keys absent from the user's layout.
- **An import that overwrites a map with no way back.**
- **A shared file that leaks something the sender didn't mean to share.**
  This matters most once macros can hold typed text (§7).

## 3. Constraints from the existing system

- **P4: the overlay is a viewer.**
  - It writes the user copy and asks the core to `reload_config`.
  - It never sends `set_bindings`.
  - An import is one more edit through that path.
- **The single source of truth is the user copy,**
  `%APPDATA%\KeyGnosys\bindings\<id>.json`. It shadows the shipped map
  under the same id.
- **Define mode already provides the building blocks:**
  - a `BindingEditor` with snapshot undo and redo;
  - an atomic `binding_store.save`;
  - a broken-copy guard (`BrokenUserCopy`, Reset to shipped);
  - the core's rule that a user map failing to load never replaces the
    running one.
- **The registry skips moved-aside files** named `<id>.broken-<digits>.json`.
  Any new backup file must be skipped the same way, or it will shadow a real
  map. That bug was found and fixed in PR #15.
- **Python validation** is `BindingSet.from_dict` plus
  `binding_store._first_problem`. It accepts `keygnosys/bindings` majors 1
  and 2. Entries it can't model are kept in `extra_*`.

## 4. Approaches

### Approach 1: the overlay reads and writes the files; the core only reloads (chosen)
- **How it works:**
  - Export serialises the editor's map into a bundle file.
  - Import parses and validates the file in the overlay.
  - On confirmation, it replaces the editor's map in one undoable edit,
    which saves and reloads like any other edit.
- **Why:**
  - It reuses every safety property define mode already has.
  - It needs no new IPC.
  - It follows approach 1 of the parent design: the overlay edits, the core
    runs.

### Approach 2: new `export`/`import` commands in the core (rejected)
- The core would own file dialogs' output and validation.
- It would duplicate Python's validation in C++, and add an IPC write path.
  That weakens P4.
- It would bypass the editor's undo.
- Nothing is gained, because the core already re-reads the file.

### Approach 3: copy the file by hand (rejected)
- "Export" would just reveal the user copy in Explorer.
- There would be no validation, no preview and no backup, and it would
  break the moment program maps and macros live in other directories.

There is one sensible way, so no choice was put to the operator. The
operator's earlier "approach 1 is the correct build design" covers it.

## 5. The file

### 5.1 Name and extension
- The extension is **`.keygnosys.json`** **(operator, Q4)**.
- The save dialog suggests `<map name>.keygnosys.json` in the user's
  Documents folder.
- The open dialog filters on `*.keygnosys.json`, with an "All JSON (*.json)"
  alternative. This is so a bare map file and a backup can be imported too.

### 5.2 The bundle

```jsonc
{
  "schema": "keygnosys/bundle/1",
  "name": "Sam's cursor layer",        // defaults to the map's name
  "author": "",                         // empty unless the user fills it in (Q12)
  "exported": "2026-11-27T14:03:00Z",   // UTC, informational
  "generator": "KeyGnosys 0.4.0",       // informational
  "documents": [
    { "schema": "keygnosys/bindings/2", "id": "default", "name": "…", … }
  ]
}
```

- **`documents` holds whole documents, embedded verbatim** (parent §5.4).
  "Verbatim" means the exported map is exactly what the editor would save:
  `BindingSet.to_dict()`, unknown entries and fields included.
- **The author's name lives only in the bundle header** **(my call)**.
  - It is not written into the embedded map's `metadata.author`, so export
    never changes a document.
  - On import, a non-empty bundle `author` becomes the map's
    `metadata.author`, unless the map already names one.
- **Unknown top-level bundle fields** are ignored on import, and are not
  preserved, because a bundle is never saved back **(my call)**.
- **This project exports exactly one document:** the running universal map
  **(operator, Q1)**. It exports whatever is running, edited or not
  **(operator, Q2)**.
  - With no user copy, that is the shipped map.

### 5.3 What import accepts
- **A `keygnosys/bundle/1` file.**
- **A bare `keygnosys/bindings/1` or `/2` document** (parent §5.4). This is
  also how a backup (§6.4) is restored.
- **Anything else is refused** with a reason:
  - not JSON;
  - an unknown schema;
  - a newer bundle or bindings major ("made by a newer KeyGnosys");
  - a file over **5 MB** **(my call: a real map is a few KB; this stops a
    wrong file being parsed into memory)**.

## 6. Import, step by step

### 6.1 Where it starts
- **Export and Import buttons sit on the control bar while define mode is
  on**, next to Undo and Redo **(operator, Q13)**.
- **Import is disabled while the user copy is broken.** Its tooltip reads
  "Reset to shipped first" **(my call)**.
  - Reset is already highlighted yellow in that state.
  - One way out keeps the never-overwrite-a-broken-file rule simple.
- **Export is also disabled while broken**, because there is no map to
  export.

**Dropping a file on the keyboard imports it** **(operator, Q14 reversed)**.
- **It is the same import, started differently.** The dropped file goes
  through the same validation (§6.2) and the same preview (§6.3), and
  nothing changes until Import is pressed. A drop never imports silently.
- **While a file is dragged over the keyboard**, the keyboard shows a
  "Drop to import" highlight across the whole view, distinct from the key
  outline used when dragging a command from the picker.
- **What is accepted:** exactly one local file ending `.json`. More than
  one file, a folder, a non-local URL or another extension is refused at
  drag-over (no-drop cursor), so nothing is read.
- **The existing picker drag keeps working.** A command dragged from the
  picker still lands on a key. The two are told apart by their data: the
  picker's own MIME type, or file URLs.
- **Outside define mode** **(my call)**:
  - Click-through must be off for the window to receive a drop at all.
    With click-through on, the drop goes to whatever is underneath; that
    is the operating system's behaviour, not a choice here. The tooltip on
    the click-through control mentions it.
  - When a file is dropped outside define mode, define mode opens first,
    because the import is a define-mode edit with Undo. If the preview is
    cancelled, define mode closes again, so a cancelled drop leaves the
    overlay as it was.
- **While the map file is broken**, a drop is refused with the same
  "Reset to shipped first" notice as the greyed-out button.
- **Windows elevation:** if the overlay ever runs elevated, Windows blocks
  drops from a non-elevated Explorer. The overlay isn't elevated today;
  this is a live-check note, not a design change.

### 6.2 Validation (before anything is shown)

The whole file is checked before the preview opens. Findings fall in two
classes.

**Blocking: nothing can be imported.**
- The file is unreadable, not JSON, or over the size limit.
- The bundle schema is unknown or newer.
- **A document kind this build doesn't know** (for example an
  `appmap/1` from a later version) **(my call)**.
  - Never-partial (parent §5.4) rules out importing the rest.
  - The message names the kinds and says a newer KeyGnosys is needed.
- **The bundle holds no bindings document, or more than one** **(my call)**.
  - This build has one universal map.
  - Choosing among several waits for a version that can hold several
    (project 3 revisits this).
- **The bindings document is structurally broken**, so
  `BindingSet.from_dict` raises: a bad id, `bindings` not an object, or an
  unsupported bindings major.
  - A known command with parameters this build rejects is **not** blocking
    **(my call, builder check)**. `from_dict` already keeps such an entry
    unchanged in `extra_bindings`, on purpose, because it may be a newer
    version's value. Import treats it like an unknown command (below), the
    same way the user copy loads.

**Reported, not blocking.** The preview lists these.
- **Commands this build doesn't know, or known commands with parameters it
  rejects** (everything in `extra_bindings` and `extra_unassigned`).
  - They are kept untouched, so a newer build will run them **(operator, Q7)**.
  - They do nothing here. Each is listed by key, for example "F7: unknown
    command `window.snap`".
- **Keys the map binds that aren't on the current keyboard layout.**
  - They are kept, because they may exist on the sender's keyboard.
  - They are listed.
- **Settings outside this build's bounds.**
  - They are listed with the value that will be used.
  - Clamping is the core's existing behaviour, not changed here.

### 6.3 The preview

A modal dialog over the overlay:

```
Import "Sam's cursor layer"            by Sam    (exported 27 Nov 2026)
────────────────────────────────────────────────────────────────────
This replaces your current map. Your current map is kept as a backup,
and Undo brings it straight back.

Changes  (14 keys)
  D        Left click   →  Right click
  Q        —            →  Double click
  F        Scroll down  →  (nothing)
  …

Kept but not usable in this version (1)
  F7       unknown command "window.snap"

Not on your keyboard (2)
  NumpadAdd, NumpadSubtract
────────────────────────────────────────────────────────────────────
                                            [ Cancel ]  [ Import ]
```

- **"Changes"** is a per-key diff of old against new, in the overlay's key
  names (`Layout.display_name`) and the picker's command labels.
  - It is sorted in layout order.
  - Legend-only and parameter-only changes count as changes.
  - Layer-setting changes are listed by name, old → new.
- **When nothing differs**, the dialog says so and offers only Close.
- **Import is the default button.** Escape cancels.
- **Imported macros will add a plain-words preview of their steps here**,
  with an explicit confirm and an "imported" mark **(operator, Q10)**.
  - That arrives with project 4, which adds macros.
  - The dialog is built to take extra sections, so project 4 adds one rather
    than rebuilding it.

### 6.4 Applying
1. **Backup first.**
   - **If a user copy exists**, it is copied, byte for byte, to
     `<id>.backup-<unix_ts>.json` in the same directory.
     - If that name is taken, a `-2`, `-3`… suffix is added, so a fast
       second import never overwrites a backup **(my call; this also fixes
       PR #15's same-second deferred minor for backups)**.
   - **If no user copy exists**, no backup file is written, because the
     shipped map is the backup and Reset to shipped restores it.
   - If the backup can't be written, the import stops with a notice.
     Nothing is changed.
2. **Replace the map as one edit.**
   - It goes through a new `BindingEditor.replace_all(bset)`, so define-mode
     **Undo** reverts the whole import in one step **(operator, Q6)**.
   - The imported document keeps everything except two things
     **(my call)**:
     - **`id`** is rewritten to the running id. The file name and the core's
       `bindings_id` don't change, so define mode stays open, with no
       switched-map exit.
     - **`metadata`** **(my call, builder check)**:
       - `revision` continues from the current map's.
       - `source_template` is kept as the current map's, so Reset to
         shipped still knows its target.
       - `imported_from: {id, name, author, exported}` records the origin.
       - Everything else (`author`, `model`, unknown metadata fields) comes
         from the imported document. When its `author` is empty, the bundle
         header's `author` fills it.
   - **Undo of an import restores the previous metadata too**, except
     `revision`, which never goes backwards. This is a special case in the
     editor's history: ordinary edits keep the live metadata across undo
     (project 1), but an import's snapshot carries metadata, so undoing it
     leaves no `imported_from` behind.
3. **Save and reload** through the existing `_on_binding_edit`: an atomic
   save, then the 300 ms debounce, then `reload_config`.
   - **With the core offline, Import still works** **(my call, builder
     check)**, like every other define-mode edit: the offline notice shows,
     and the map applies when the core connects (project 1, §6.4.6).
   - **If the core rejects the file**, its keep-last-good rule holds the old
     map running and the existing `binding.invalid` notice shows. Define
     mode does **not** undo the import by itself; Undo is one click away.
     Python's validation is the same as the core's for everything it
     models, so this is a backstop, not an expected path.
4. **Notice:** "Imported '<name>'. Your previous map is saved as
   <backup file name>." When there is no backup, it reads "Undo or Reset to
   shipped brings back the previous map."

**The registry must skip `*.backup-<digits>[-n].json`** exactly as it skips
`*.broken-<digits>.json`.
- This is the generalisation of `_is_moved_aside`.
- The core needs nothing: it opens `<id>.json` by name
  (`bindingsSearchPaths`) and never scans the directory.

### 6.5 Collisions (later kinds)
- Today there is no id collision. The imported map always replaces the
  running one (§6.4), and the backup covers it.
- **When program maps and macros arrive**, each colliding document gets
  **Replace** (the old one is backed up), **Keep both** (the new one takes
  `<id>-2`, `-3`…) or **Skip** **(operator, Q8)**.
  - Projects 3 and 4 build that.
  - This project's import code keeps the document handling per kind, so
    they slot in.

## 7. Export, step by step
1. **Click Export.** The export preview opens:
   - the map's name;
   - the number of keys bound;
   - an optional **Your name (for credit)** field, empty by default
     **(operator, Q12)**;
   - a **Warnings** section.
     - Today it lists only commands this build doesn't know, which are
       exported unchanged.
     - Project 4 adds **typed text in macros**, shown so passwords can be
       removed first **(operator, Q11)**.
2. **Save…** opens the system save dialog (§5.1).
3. **The bundle is written atomically** (temp file, then replace) to the
   chosen path.
   - Overwriting an existing file is the save dialog's own confirmation.
4. **Notice:** "Exported to <file name>."

- **Uncommitted typing.** Export first commits a label still being typed in
  the picker, the same way leaving define mode does. The exported file then
  matches what the user sees.
- **Export doesn't change the map**, so it adds no undo entry, makes no save
  and sends no reload.

## 8. Where the code goes **(my call; the names are the builder's)**
- **`python/keygnosys/bundle.py`, new, no Qt:**
  - `export_bundle(bset, author) -> dict`;
  - `read_import(path) -> ImportPlan`, which raises `ImportRefused(reasons)`
    for blocking findings;
  - `ImportPlan` holds the parsed `BindingSet`, its header, and the reported
    findings;
  - `diff(old, new, display, label) -> list[Change]`.
  - It is pure and unit-testable without a window.
- **`binding_store`:**
  - `backup(bindings_id) -> Path | None`;
  - a shared `is_set_aside(path)` covering broken and backup names, used by
    the registry.
- **`BindingEditor.replace_all(bset)`:** one snapshot edit.
- **`ui/transfer_dialogs.py`, new:** the export and import previews.
- **`ui/define_mode.py`:** an `export()` and an `import_()` that wire the
  dialogs, store and editor together.
- **`ui/control_bar.py`:** the two buttons, enabled per state.
- **`ui/keyboard_view.py`:** accepts file drops alongside the picker's
  drags, draws the "Drop to import" highlight, and emits a
  `fileDropped(path)` signal that define mode handles.
- **The dialogs must be parented and raised** so they appear above the
  topmost, no-activate overlay and take keyboard focus.
  - That is a live-check item, because the same focus question is still
    open from project 1.

## 9. Tests this project must have
- **Bundle round trip:**
  - export, then import, gives an identical map;
  - unknown commands, unknown fields and `extra_*` all survive.
- **Accepts:** a bundle; a bare bindings/1 and /2 document; a backup file.
- **Refuses, with the right reason:**
  - not JSON;
  - unknown schema;
  - newer major;
  - unknown document kind;
  - zero or two bindings documents;
  - an oversize file.
  - In every refusal, nothing is written: no backup and no user copy.
- **Reports:** unknown commands, known commands with rejected parameters,
  and off-layout keys appear in the plan, and the import still proceeds.
- **Backup:**
  - it is written byte for byte before the replace;
  - no backup is written without a user copy;
  - a second backup in the same second gets a suffix;
  - a backup failure aborts the import;
  - the registry skips backup files, as a regression test like
    `_is_moved_aside`.
- **Replace:**
  - one Undo restores the previous map exactly, metadata included (no
    `imported_from` left), except `revision`; Redo reapplies;
  - `id` is rewritten;
  - `revision` continues;
  - `imported_from` is set;
  - `source_template` is kept.
- **Diff:** a binding change, a legend-only change, a parameter-only change,
  a cleared key, a new key and a setting change each appear once. An
  identical map gives no changes.
- **Define mode:**
  - Import is disabled while broken;
  - cancelling the preview changes nothing;
  - confirming saves and reloads once (debounced);
  - Export commits a pending label first and makes no save.
- **Dropping a file:**
  - one `.json` file is accepted and opens the preview, and nothing
    changes until Import;
  - two files, a folder, or a `.txt` file are refused at drag-over;
  - a picker command drag still assigns to a key;
  - a drop outside define mode opens define mode, and cancelling the
    preview closes it again;
  - a drop while broken shows the notice and reads nothing.
- **Manual rows 14.1–14.5** in `docs/manual-tests.md`:
  - export, then import on the same machine;
  - import a hand-edited file with an unknown command and see it listed;
  - Undo an import live;
  - refuse a broken file;
  - drag a real file from Explorer onto the keyboard, in and out of
    define mode.
  - The dialogs taking focus above the overlay is checked here.

## 10. SPEC amendments
- **A new §4.x, `keygnosys/bundle/1`:**
  - the header fields;
  - `documents`, verbatim;
  - what import accepts (§5.3);
  - unknown document kinds are refused whole.
- **§3.2:** backup files `<id>.backup-<ts>[-n].json` are written by import
  and never loaded, alongside the broken copies.
- **§10.2:** Export and Import in define mode; dropping a file on the
  keyboard; the preview; the backup; one Undo.
- **§10.1.7** (layout import/export, M6) is unchanged. A layout later gets
  its own kind in the same bundle.

## 11. Left to the builder
- Dialog layout and widget structure, within the sketch in §6.3.
- The diff's exact wording and how commands are labelled, reusing the
  picker's labels.
- How `generator` reads the app version.
- The atomic-write helper for the export path: reuse `_write_atomic` or a
  sibling.

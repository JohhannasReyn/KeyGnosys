# Export and Import Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the user export the running cursor-layer map to a `.keygnosys.json` file, and import one back. Import can start from a button or from dropping a file on the keyboard. Every import gets a preview, a backup and a one-step Undo.

**Architecture:**
- A pure-Python `bundle` module reads, validates and writes `keygnosys/bundle/1` files, and diffs two maps.
- `binding_store` gains a backup, and `BindingEditor` gains an undoable `replace_all`.
- Define mode wires these to two preview dialogs, two control-bar buttons and file drops on the keyboard view.
- The core is unchanged: an import is one more define-mode edit, saved to the user copy and applied with `reload_config` (P4).

**Tech Stack:** Python 3.12, PySide6, pytest and pytest-qt.

**Spec:** `docs/superpowers/specs/2026-11-27-export-import-design.md`, approved 2026-11-27. Its parent is `docs/superpowers/specs/2026-11-27-binding-editor-keymaps-macros-design.md` §5.4.

## Global Constraints

- **Branch:** `feat/export-import`, cut from `design/export-import`.
- **Commits:** never add AI attribution. No `Co-Authored-By` trailer, no "Generated with" footer and no session lines.
- **Tests:**
  - Run Python tests with `.venv/Scripts/python.exe -m pytest -q`. The suite must stay green; it has 248 passing before this plan.
  - The core is untouched. `ctest --test-dir build/default` must still show 23/23.
- **P4: the overlay never sends `set_bindings`.** An import saves the user copy through the existing `_on_binding_edit` path, then reloads.
- **File format:**
  - The extension is `.keygnosys.json`.
  - The bundle schema string is `keygnosys/bundle/1`.
  - The header fields are `schema`, `name`, `author`, `exported` (UTC ISO 8601 with a `Z` suffix), `generator` (`"KeyGnosys " + keygnosys.__version__`) and `documents`.
- **Import accepts** a bundle, or a bare `keygnosys/bindings/1` or `/2` document. It refuses files over **5 MB** (5 * 1024 * 1024 bytes).
- **Never partial:** any blocking finding means nothing is written, neither a backup nor the user copy.
- **The backup name** is `<id>.backup-<unix_ts>.json`. If that name is taken, it becomes `<id>.backup-<unix_ts>-2.json`, then `-3`, and so on. A backup is written only when a user copy exists.
- **The registry never loads** `<id>.broken-<digits>.json` or `<id>.backup-<digits>[-<n>].json`.
- **Metadata of an imported map** (design §6.4):
  - `id` is the running id.
  - `revision` continues from the current map's.
  - `source_template` is the current map's.
  - `imported_from = {id, name, author, exported}`.
  - Everything else comes from the imported document. An empty `author` is filled from the bundle header's `author`.
- **Undo of an import** restores the previous metadata too, except `revision`, which never goes backwards.
- **Import and Export are disabled** while the user copy is broken. Their tooltip and the drop refusal both read "Reset to shipped first".
- **Notice copy, exact:**
  - `"Imported '{name}'. Your previous map is saved as {backup}."`
  - `"Imported '{name}'. Undo or Reset to shipped brings back the previous map."`
  - `"Exported to {file}."`
  - `"Reset to shipped first."`
- **No dialog may open in tests.** Every dialog and file chooser is an injectable callable on `DefineMode`, like the existing `confirm`.

## Review Focus

1. **A dropped or chosen file that is huge, binary or not UTF-8** should be refused with a reason, without reading the whole file or crashing. The size check comes before reading (Task 1, `test_refuses_oversize_without_reading`). Undecodable bytes are refused (Task 1, `test_refuses_non_utf8`).
2. **Importing the file you just exported, unchanged**, should say nothing changes and offer only Close. It must not create a backup or an undo entry (Task 5, `test_identical_import_changes_nothing`).
3. **The core disconnects between confirming and reloading.** The import is saved, the offline notice shows once, and it reloads on connect (Task 5, `test_import_while_offline_reloads_on_connect`).
4. **A drop arrives while the picker is mid-drag, or with mixed data (a file URL plus text)**, as when dragging a file from a browser. File URLs win only when the data is exactly one local `.json` file (Task 6, `test_mixed_mime_with_one_local_json_is_accepted`, `test_remote_url_refused`).
5. **Import while a label is half-typed in the picker.** The pending label is committed first, so it is part of the map being replaced and comes back on Undo (Task 5, `test_import_commits_pending_label_first`).

---

### Task 1: The bundle module — read, validate, write

**Files:**
- Create: `python/keygnosys/bundle.py`
- Modify: `python/keygnosys/actions.py`. Move `KNOWN_SETTINGS`, `_SPEED` and `_RAMP` here from `ui/binding_picker.py`.
- Modify: `python/keygnosys/ui/binding_picker.py`. Import `KNOWN_SETTINGS` from `..actions` and delete the local copy.
- Test: `tests/test_bundle.py`

**Interfaces:**
- Produces:
  - `BUNDLE_SCHEMA = "keygnosys/bundle/1"`, `EXTENSION = ".keygnosys.json"`, `MAX_BYTES = 5 * 1024 * 1024`.
  - `class ImportRefused(Exception)`, with `.reasons: list[str]`, one human-readable line each.
  - `@dataclass(frozen=True) class Finding`, with `kind: str` (one of `"unusable"`, `"off_layout"`, `"setting"`) and `text: str`.
  - `@dataclass class ImportPlan`, with `bset: BindingSet`, `name: str`, `author: str`, `exported: str | None` and `findings: list[Finding]`.
  - `export_bundle(bset: BindingSet, author: str = "") -> dict`
  - `write_bundle(path: Path, bundle: dict) -> None`, an atomic write.
  - `read_import(path: Path, layout_codes: set[str] | None = None) -> ImportPlan`, which raises `ImportRefused`.

- [ ] **Step 1: Write the failing tests** in `tests/test_bundle.py`:
  - `test_export_then_read_round_trips`. Load `data/bindings/default.json`, add an `extra_bindings["F7"] = {"action": "window.snap"}` and a `metadata.extra["x"] = 1`, then export and write. `read_import(...).bset.to_dict()` must equal the original `to_dict()`.
  - `test_export_header`:
    - `schema == BUNDLE_SCHEMA`;
    - `name` equals the map name;
    - `author == ""` by default, and `"Sam"` when passed;
    - `exported` ends with `"Z"`;
    - `generator == f"KeyGnosys {keygnosys.__version__}"`;
    - `len(documents) == 1`;
    - the embedded `metadata.author` is unchanged by a passed author.
  - `test_reads_bare_bindings_v1_and_v2`. Both give a plan whose `name` is the document's name and whose `author` is `""`.
  - Refusals. Each test asserts `ImportRefused`, and that one reason contains the substring given:
    - `test_refuses_not_json` → `"not a KeyGnosys file"`
    - `test_refuses_non_utf8` (bytes `b"\xff\xfe\x00"`) → `"not a KeyGnosys file"`
    - `test_refuses_unknown_schema` (`{"schema": "foo/1"}`) → `"not a KeyGnosys file"`
    - `test_refuses_newer_bundle_major` (`keygnosys/bundle/2`) → `"newer KeyGnosys"`
    - `test_refuses_newer_bindings_major` (a bundle holding a `keygnosys/bindings/3` document) → `"newer KeyGnosys"`
    - `test_refuses_unknown_document_kind` (a bundle holding `keygnosys/appmap/1` plus a valid map) → `"appmap"` and `"newer KeyGnosys"`
    - `test_refuses_no_map` (`documents: []`) → `"no binding map"`
    - `test_refuses_two_maps` → `"more than one binding map"`
    - `test_refuses_broken_map` (`"bindings": []`) → `"'bindings' must be an object"`
    - `test_refuses_oversize_without_reading`. Write a file of `MAX_BYTES + 1` bytes and monkeypatch `Path.read_bytes` to raise if called. The reason contains `"too large"`.
  - `test_known_command_with_rejected_params_is_reported_not_refused`. `KeyD: {"action": "button.click", "params": {"button": "sideways"}}` gives a plan with a `Finding("unusable", ...)` whose text contains `"KeyD"`. The entry survives in `plan.bset.extra_bindings`.
  - `test_unknown_command_reported`. `F7: window.snap` gives an `unusable` finding naming `F7` and `window.snap`.
  - `test_off_layout_keys_reported`. With `layout_codes={"KeyD"}` and a map binding `KeyD` and `NumpadAdd`, there is one `off_layout` finding naming `NumpadAdd`. The key stays in `plan.bset.bindings`.
  - `test_setting_out_of_bounds_reported`. `precision_factor: 5.0` gives a `setting` finding naming `precision_factor` and the value `1.0` that will be used. The stored value is left as `5.0`.
  - `test_bundle_author_fills_empty_map_author`. With a header author of `"Sam"` and an empty `metadata.author`, `plan.author == "Sam"`. With a header of `"Sam"` and a map author of `"Lee"`, `plan.author == "Lee"`.
  - `test_write_bundle_is_atomic`. Monkeypatch `os.replace` to raise. The existing file at the path is unchanged, and no `*.tmp` sibling is left behind.
- [ ] **Step 2: Run** `.venv/Scripts/python.exe -m pytest tests/test_bundle.py -q`. Expected: it fails with `ModuleNotFoundError: keygnosys.bundle`.
- [ ] **Step 3: Implement `bundle.py`:**
  - **Size check:** use `path.stat().st_size` before reading. Decode as UTF-8 and parse with `json.loads`. Any `OSError`, `UnicodeDecodeError` or `JSONDecodeError` gives `"not a KeyGnosys file"`.
  - **Kind dispatch** goes on the `schema` string's middle segment (`bundle`, `bindings`, or other). A known kind with a higher major gives `"made by a newer KeyGnosys"`.
  - **Bindings documents** are parsed with `BindingSet.from_dict`, which raises `DocumentError`. Pass its message through.
  - **`unusable` findings** come from `bset.extra_bindings` (by code) and `bset.extra_unassigned`.
  - **Clamping** for the `setting` finding uses `actions.KNOWN_SETTINGS` `(kind, low, high, step)`.
  - **The atomic write** is a temp file beside the target (`<name>.tmp`), then `os.replace`, unlinking the temp file on failure.
  - `plan.name` is the header `name`, or the map name for a bare document.
- [ ] **Step 4: Run** the new tests and then the full suite. Expected: all pass, including `tests/test_binding_picker.py`, after the `KNOWN_SETTINGS` move.
- [ ] **Step 5: Commit** `bundle: read, validate and write keygnosys/bundle/1`.

### Task 2: Diffing two maps

**Files:**
- Modify: `python/keygnosys/bundle.py`
- Test: `tests/test_bundle.py`

**Interfaces:**
- Consumes: `BindingSet`, `Binding`, and `actions.default_legend(action, params)`.
- Produces:
  - `@dataclass(frozen=True) class Change`, with `kind: str` (`"key"` or `"setting"`), `name: str`, `before: str` and `after: str`.
  - `diff(old: BindingSet, new: BindingSet, display: Callable[[str], str] = str, order: Sequence[str] = ()) -> list[Change]`.

- [ ] **Step 1: Write the failing tests:**
  - `test_diff_identical_is_empty`.
  - `test_diff_each_kind_once`. Make one map from another with:
    - `KeyD` Left → Right click;
    - `KeyQ` legend-only change;
    - `KeyE` parameter-only change;
    - `KeyF` cleared;
    - `KeyZ` newly bound;
    - `precision_factor` 0.25 → 0.5.

    Assert six changes, one per name, with names given through `display` (pass `lambda c: c.removeprefix("Key")`).
    - The cleared key's `after == "(nothing)"`, and the new key's `before == "—"`.
    - The setting change reads `before="0.25"`, `after="0.5"`.
  - `test_diff_follows_layout_order`. Given `order=["KeyZ", "KeyD"]`, key changes come Z then D. Codes not in `order` follow, sorted. Setting changes come after all key changes.
  - `test_diff_counts_extra_bindings`. An unknown command added on `F7` is one change whose `after` contains `"window.snap"`.
- [ ] **Step 2: Run** them. Expected: they fail with `ImportError: cannot import name 'diff'`.
- [ ] **Step 3: Implement `diff`.**
  - **Equality per key** compares `Binding.to_dict()`, or the raw dict for `extra_bindings`.
  - **Description:** `legend` if set, else `default_legend(action, params)`. Append ` "<legend>"` when only the legend differs, so before and after read differently.
  - An unknown command is described by its raw action string.
- [ ] **Step 4: Run** the tests. Expected: PASS.
- [ ] **Step 5: Commit** `bundle: per-key diff of two maps`.

### Task 3: Backup, set-aside names, and `replace_all`

**Files:**
- Modify:
  - `python/keygnosys/binding_store.py`
  - `python/keygnosys/documents.py:943-982`
  - `python/keygnosys/binding_editor.py:145-170`
- Test: `tests/test_binding_store.py`, `tests/test_documents.py`, `tests/test_binding_editor.py`

**Interfaces:**
- Produces:
  - `binding_store.backup(bindings_id: str) -> Path | None`. It returns the backup path, or `None` when no user copy exists. It raises `StoreError` if it can't write.
  - `documents.is_set_aside(path: Path) -> bool`, which replaces `_is_moved_aside` (rename it; keep no alias).
  - `BindingEditor.replace_all(new: BindingSet, imported_from: dict) -> None`.

- [ ] **Step 1: Write the failing tests:**
  - `test_backup_copies_bytes_exactly`. The user file holds odd whitespace. The backup's bytes are identical, and its name matches `default.backup-<digits>.json`.
  - `test_backup_without_user_copy_is_none`.
  - `test_second_backup_same_second_gets_suffix`. Monkeypatch `time.time` to a constant. The names are `…-<ts>.json`, then `…-<ts>-2.json`, then `-3`.
  - `test_backup_failure_is_store_error`. Monkeypatch `shutil.copyfile` to raise `OSError`.
  - `test_registry_skips_backup_files`, in `test_documents.py`. Write `default.backup-1795842643.json` and `default.backup-1795842643-2.json`, each holding a map with id `default` and a distinct name. The registry's `default` is still the real one. Add the parametrised name checks:
    - `is_set_aside` is True for `a.broken-1.json`, `a.backup-1.json` and `a.backup-1-2.json`;
    - it is False for `a.json`, `a.backup-x.json`, `backup-1.json` and `a.backup-1-.json`.
  - `test_replace_all_sets_import_metadata`. Starting from a map with revision 4 and `source_template="default"`, import a map with author `"Sam"`, revision 9, no `source_template` and `metadata.extra={"y": 2}`. The result:
    - has the current `id`;
    - has `revision == 4`, which save then bumps;
    - has `source_template == "default"`;
    - has `author == "Sam"`;
    - has `extra["y"] == 2`;
    - has `extra["imported_from"]` equal to the passed dict;
    - fires `on_change` once.
  - `test_undo_of_import_restores_metadata_but_not_revision`:
    1. Replace, then simulate a save by bumping `revision` to 5.
    2. Undo. Content and metadata equal the pre-import map, with no `imported_from`, and `revision` stays at 5.
    3. Redo. The import comes back, with `imported_from` present.
  - `test_ordinary_undo_still_keeps_live_metadata`. This is the existing behaviour, as a regression test.
- [ ] **Step 2: Run** them. Expected: they fail (missing names).
- [ ] **Step 3: Implement.**
  - **`backup`:** use `shutil.copyfile`, picking the first free name in the order given in Global Constraints.
  - **`replace_all`:**
    - Snapshot as `_edit` does, but push the history entry as a marker, for example `{"doc": snapshot, "with_metadata": True}`.
    - `_travel` restores metadata from the snapshot only for marked entries, keeping the live `revision`.
    - Redoing a marked entry is marked too.
- [ ] **Step 4: Run** the full suite. Expected: PASS.
- [ ] **Step 5: Commit** `store: import backups; editor: undoable replace_all`.

### Task 4: The preview dialogs

**Files:**
- Create: `python/keygnosys/ui/transfer_dialogs.py`
- Test: `tests/test_transfer_dialogs.py`

**Interfaces:**
- Consumes: `ImportPlan`, `Finding` and `Change` from Task 1 and Task 2.
- Produces:
  - `class ImportPreview(QDialog)`:
    - `__init__(plan: ImportPlan, changes: list[Change], parent=None)`;
    - attributes `import_button: QPushButton | None` (None when `changes` is empty) and `close_button`;
    - `sections() -> dict[str, list[str]]`, the visible lines per section title, for tests.
  - `class ExportPreview(QDialog)`:
    - `__init__(name: str, key_count: int, warnings: list[str], parent=None)`;
    - `author_field: QLineEdit` and `save_button`;
    - `author() -> str`, stripped.
  - `ask_import(plan, changes, parent=None) -> bool` and `ask_export(name, key_count, warnings, parent=None) -> str | None`. These run the dialogs. `None` means cancelled; a string is the author, possibly empty.

- [ ] **Step 1: Write the failing tests:**
  - `test_import_preview_sections`. A plan with 2 changes, 1 `unusable` and 1 `off_layout` finding has the sections `"Changes (2 keys)"`, `"Kept but not usable in this version (1)"` and `"Not on your keyboard (1)"`.
    - The header line contains the name, `by Sam` and the export date as `27 Nov 2026`. `by` is omitted when the author is empty.
    - The intro reads: `This replaces your current map. Your current map is kept as a backup, and Undo brings it straight back.`
  - `test_import_preview_setting_findings_section`. This one has the title `"Settings adjusted (1)"`.
  - `test_import_preview_no_changes_offers_close_only`. `import_button is None`, and the text contains `"Nothing changes"`.
  - `test_import_is_default_and_escape_cancels`. `import_button.isDefault()`, and pressing Escape gives `result() == QDialog.Rejected`.
  - `test_export_preview_author_empty_by_default_and_stripped`. Enter `"  Sam "`, and `author()` returns `"Sam"`.
  - `test_export_preview_lists_warnings`.
- [ ] **Step 2: Run** them. Expected: they fail with `ModuleNotFoundError`.
- [ ] **Step 3: Implement.**
  - Sections are built from a list of `(title, lines)` pairs, so project 4 can append a macro section.
  - `ask_*` set `Qt.WindowStaysOnTopHint`, then call `raise_()`, `activateWindow()` and `exec()`.
- [ ] **Step 4: Run** the tests. Expected: PASS.
- [ ] **Step 5: Commit** `ui: import and export preview dialogs`.

### Task 5: Export and Import in define mode, plus the bar buttons

**Files:**
- Modify:
  - `python/keygnosys/ui/define_mode.py`
  - `python/keygnosys/ui/control_bar.py`
  - `python/keygnosys/ui/overlay.py:162-166`
  - `python/keygnosys/ui/binding_picker.py`, adding `commit_pending()`
- Test: `tests/test_define_mode.py`; the bar's tests go there too, since there is no `tests/test_control_bar.py`

**Interfaces:**
- Consumes:
  - from Task 1: `read_import`, `export_bundle` and `write_bundle`;
  - from Task 2: `diff`;
  - from Task 3: `backup` and `replace_all`;
  - from Task 4: `ask_import` and `ask_export`.
- Produces:
  - **On `DefineMode`, injectable callables** (tests replace them):
    - `choose_open_path: Callable[[], Path | None]`;
    - `choose_save_path: Callable[[str], Path | None]`, whose argument is the suggested file name;
    - `ask_import: Callable[[ImportPlan, list[Change]], bool]`;
    - `ask_export: Callable[[str, int, list[str]], str | None]`.
  - **On `DefineMode`, methods:**
    - `export() -> None`;
    - `import_(path: Path | None = None) -> bool`, which returns whether the map was replaced;
    - `can_transfer: bool`, a property that is False while there is no editor (the broken state).
  - **On `ControlBar`:**
    - signals `exportRequested` and `importRequested`;
    - `export_button` and `import_button`;
    - `set_define_mode(on, can_undo, can_redo, can_transfer=True)`.
  - `BindingPicker.commit_pending() -> None`, the public form of `_emit_legend()`.

- [ ] **Step 1: Write the failing tests** with `make_overlay`, stubbing the four callables:
  - `test_buttons_visible_only_in_define_mode`.
  - `test_buttons_disabled_while_broken_with_tooltip`. The tooltip contains `"Reset to shipped first"`.
  - `test_export_writes_bundle_and_makes_no_save`:
    - The file at the path from `choose_save_path` reads back through `read_import` to the running map.
    - `choose_save_path` received `"<map name>.keygnosys.json"`.
    - There are no reloads, the user copy is unchanged (or absent) and `can_undo` is False.
    - The notice reads `"Exported to <file name>."`.
  - `test_export_cancelled_writes_nothing`. This covers both `ask_export → None` and `choose_save_path → None`.
  - `test_export_commits_pending_label_first`.
  - `test_import_replaces_saves_and_reloads_once`. Export a modified map from one overlay, then import it in another:
    - The user copy holds the imported bindings, with the running id and `imported_from`.
    - After the debounce, there is exactly 1 reload (`qtbot.waitUntil`).
    - The notice matches one of the two import notices from Global Constraints.
  - `test_import_writes_backup_when_user_copy_exists`. The backup exists, and the notice names it.
  - `test_import_cancel_changes_nothing`. With `ask_import → False`, there's no backup, the user copy is unchanged and there are no reloads.
  - `test_refused_import_writes_nothing`. A bad file produces a notice containing the reason. There's no backup and `ask_import` is not called.
  - `test_identical_import_changes_nothing`. `ask_import` is called with `changes == []`, and returning False changes nothing, with no backup and no undo entry. (Review Focus 2.)
  - `test_undo_reverts_import`. One Undo restores the previous map in memory and on disk, and the core reloads again.
  - `test_import_while_offline_reloads_on_connect`. The client is disconnected, so the offline notice appears and there are 0 reloads. After `connect_core()`, there is 1 reload. (Review Focus 3.)
  - `test_import_commits_pending_label_first`. Type a label without committing, then import and Undo. The label is back. (Review Focus 5.)
  - `test_backup_failure_aborts_import`. Monkeypatch `binding_store.backup` to raise `StoreError`. A notice shows, and the map and user copy are unchanged.
- [ ] **Step 2: Run** them. Expected: they fail with `AttributeError` on the missing members.
- [ ] **Step 3: Implement.**
  - **Default callables:**
    - `choose_open_path` uses `QFileDialog.getOpenFileName` with the filter `"KeyGnosys files (*.keygnosys.json);;All JSON (*.json)"`.
    - `choose_save_path` uses `QFileDialog.getSaveFileName` in `QStandardPaths.DocumentsLocation`, with the suggested name.
    - Both are parented to the overlay window.
  - **`import_` order:**
    1. Commit the pending label.
    2. `read_import(path, layout.codes())`, giving a notice on `ImportRefused` (reasons joined with `"; "`).
    3. Diff against `editor.bset`, using the overlay's display names (`Layout.display_name`) and the order `[k.code for k in layout.keys]`. `Layout.codes()` is a set, so it can't give the order.
    4. Ask.
    5. Back up.
    6. `replace_all`.
    7. Show the notice.
    8. Return True.
  - **Header fields go into `imported_from`:** `{"id": original id, "name": plan.name, "author": plan.author, "exported": plan.exported}`.
  - **`export` order:**
    1. Commit the pending label.
    2. Warnings are one line per entry in `bset.extra_bindings` (`"<key name>: unknown command \"<action>\" (exported unchanged)"`) and in `extra_unassigned`.
    3. `ask_export(name, len(bset.bindings) + len(bset.extra_bindings), warnings)`.
    4. `choose_save_path(f"{name}{EXTENSION}")`, then `write_bundle`, then the notice. A `OSError` gives the notice `"Export failed — {exc}"`.
  - **`_refresh`** passes `can_transfer` to the bar.
  - **Overlay wiring:** `bar.exportRequested → define.export` and `bar.importRequested → define.import_`.
- [ ] **Step 4: Run** the full suite. Expected: PASS.
- [ ] **Step 5: Commit** `define mode: export and import with preview, backup and undo`.

### Task 6: Drop a file on the keyboard to import

**Files:**
- Modify:
  - `python/keygnosys/ui/keyboard_view.py:448-485`
  - `python/keygnosys/ui/define_mode.py`
  - `python/keygnosys/ui/overlay.py`
  - `python/keygnosys/ui/control_bar.py`, for the click-through tooltip
- Test: `tests/test_define_mode.py`, and a new `tests/test_keyboard_view.py` for `dropped_file` and the drag events

**Interfaces:**
- Consumes: `DefineMode.import_(path)` from Task 5.
- Produces:
  - `KeyboardView.fileDropped = Signal(object)`, which carries a `Path`;
  - `keyboard_view.dropped_file(mime: QMimeData) -> Path | None`;
  - `KeyboardView.file_drag_active: bool`, which drives the highlight;
  - `DefineMode.on_file_dropped(path: Path) -> None`.

- [ ] **Step 1: Write the failing tests:**
  - **`dropped_file` unit tests:**
    - one local `C:/x/a.json` URL → that path;
    - `a.keygnosys.json` → that path;
    - two URLs → None;
    - a `.txt` → None;
    - `https://example.com/a.json` → None (`test_remote_url_refused`);
    - a directory → None;
    - URL plus text, with one local `.json` → the path (`test_mixed_mime_with_one_local_json_is_accepted`).
  - `test_file_drag_accepted_outside_and_inside_define_mode`. Simulated drag-enter with file-URL mime data is accepted, and `file_drag_active` is True. Drag-leave clears it.
  - `test_picker_drag_still_assigns`. The existing `drop_at` with `BINDING_MIME` still emits `bindingDropped`, and does not emit `fileDropped`.
  - `test_drop_inside_define_mode_opens_preview`. `ask_import` is called once, and nothing changes until it returns True.
  - `test_drop_outside_define_mode_enters_then_exits_on_cancel`. Define mode is active while `ask_import` runs. After `False`, define mode is inactive again and click-through is restored.
  - `test_drop_outside_define_mode_stays_open_after_import`. After `True`, define mode stays active, so Undo is available.
  - `test_drop_while_broken_is_refused`. The notice is `"Reset to shipped first."`, `read_import` is never called (monkeypatch it to raise) and define mode is left as it was.
  - `test_click_through_tooltip_mentions_drops`. The bar's click-through control tooltip contains `"drop"`.
- [ ] **Step 2: Run** them. Expected: they fail.
- [ ] **Step 3: Implement.**
  - **Accepting drops:** `setAcceptDrops(True)` stays as it is. `dragEnterEvent` and `dragMoveEvent` accept when `dropped_file(...)` is not None, in any mode, else they fall through to the existing picker logic. `dropEvent` emits `fileDropped`.
  - **The highlight:** while `file_drag_active` is True, paint a translucent full-view overlay using the selection colour `#FFB020` at about 25% alpha, with the centred text `Drop to import`.
  - **`on_file_dropped`:**
    - If broken, show the notice and stop.
    - If define mode is not active, remember that, call `enter()`, then `import_(path)`.
    - If define mode was entered for this drop and `import_` returned False, call `exit()`.
  - **Overlay wiring:** `view.fileDropped → define.on_file_dropped`.
- [ ] **Step 4: Run** the full suite. Expected: PASS.
- [ ] **Step 5: Commit** `keyboard: drop a file to import`.

### Task 7: SPEC, manual tests

**Files:**
- Modify: `docs/SPEC.md` (§3.2, a new §4.6 "Bundle" after §4.5 Settings and before §5, §10.2), `docs/manual-tests.md`

- [ ] **Step 1: SPEC.**
  - **The new §4.6, `keygnosys/bundle/1`:**
    - the header fields, as in Global Constraints;
    - `documents`, embedded verbatim;
    - what import accepts;
    - unknown document kinds are refused whole;
    - the 5 MB limit.
  - **§3.2:** add the backup files beside the broken copies, written by import and never loaded.
  - **§10.2:** add a subsection "Export and import" covering:
    - the buttons in define mode;
    - the preview;
    - the backup;
    - one Undo;
    - dropping a file on the keyboard (click-through must be off; a drop outside define mode opens it and a cancelled one closes it);
    - disabled while broken.
- [ ] **Step 2: Manual tests.** Add a "## 14. Export and import" table:
  - **14.1:** Export, change a key, then import the exported file. The preview lists that one key, and after Import the key is back.
  - **14.2:** Hand-add `"F7": {"action": "window.snap"}` to an exported file, then import it. It's listed under "Kept but not usable", and the user copy still holds it afterwards.
  - **14.3:** Import a map, then Undo. The previous map is live again on the next layer press.
  - **14.4:** Import a truncated file. It's refused with a reason, and no backup file appears.
  - **14.5:** Drag a real file from Explorer onto the keyboard, inside define mode and then outside it with click-through off. The "Drop to import" highlight shows, the preview opens above the overlay and takes keyboard focus (Escape cancels), and cancelling outside define mode leaves define mode closed.
- [ ] **Step 3: Commit** `SPEC and manual tests: export and import`.

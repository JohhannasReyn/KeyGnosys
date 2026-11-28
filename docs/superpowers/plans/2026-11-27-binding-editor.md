# Binding Editor (Project 1) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** a define mode on the overlay. The user clicks a key, picks a command from a side picker, and the layer does the new thing. Edits save to the user's config and apply live.

**Architecture:**
- A pure-Python editing model (`BindingEditor`) wraps the existing `BindingSet` reassignment rules and adds undo/redo.
- A small store module writes user copies atomically, forks shipped documents, and refuses to overwrite a broken user file.
- Qt widgets (a picker docked beside the keyboard, plus a Define toggle on the bar) drive the model.
- The overlay saves through the store and sends `reload_config`.
- The core gains two small behaviours:
  - it keeps the running map when a user document is broken;
  - it reports which bindings id it is running.

**Tech Stack:**
- Python 3.12, PySide6, pytest + pytest-qt (`qtbot`);
- C++ core (MSYS2 UCRT64 toolchain, CMake, the in-repo `KGN_TEST` harness).

**Spec:** `docs/superpowers/specs/2026-11-27-binding-editor-keymaps-macros-design.md`. Project 1 is §6; the cross-project formats are §5. The SPEC (`docs/SPEC.md`) §10.2 still governs the editor's behaviour.

**Branch:** create `feat/binding-editor` from `design/binding-editor` (it holds the design doc).

## Global Constraints

- **No AI attribution anywhere.** No `Co-Authored-By: Claude`, no "Generated with Claude Code", and no `Claude-Session:` lines in commits, PRs or comments. This is the operator's rule and overrides any harness default.
- **Build the core:** `PATH=/c/msys64/ucrt64/bin:$PATH cmake --build build/default`.
- **Core tests:** `PATH=/c/msys64/ucrt64/bin:$PATH ctest --test-dir build/default --output-on-failure`.
  - The core crashes with 0xC0000005 unless `C:\msys64\ucrt64\bin` is first on PATH.
- **Python tests:** `.venv/Scripts/python.exe -m pytest -q`. The suite currently has 130 passing tests and must stay green.
- **Document schemas:**
  - `keygnosys/bindings/2` is unchanged, except for the additive optional `was_on` field on `unassigned` entries.
  - The user copy lives at `paths.user_root() / "bindings" / f"{id}.json"`.
  - The shipped copy lives at `paths.BUNDLED_ROOT / "bindings" / f"{id}.json"`.
- **Silent reassignment follows SPEC §10.2.2 exactly.** Use the existing `BindingSet.assign` / `unassign`; don't reimplement them.
- **The activation key `CapsLock` and the eight modifier codes are never assignment targets.** Ruling: modifiers are always forwarded in the layer (SPEC §6 table), so a binding on one would draw a legend the layer never honours. The modifier codes are `ShiftLeft`, `ShiftRight`, `ControlLeft`, `ControlRight`, `AltLeft`, `AltRight`, `MetaLeft`, `MetaRight`.
- **Diagnostic code for a user document that fails to load:** `binding.invalid`.
- **IPC field name:** `bindings_id`, in the `hello` payload and the `config_changed` payload.
- **Every write to the user config dir is atomic** (write `<name>.tmp`, then `os.replace`), mirroring `settings.py:110`.
- **P4: the overlay is a viewer.** The overlay must never send `set_bindings` on connect or reconnect.
- **The reload debounce is 300 ms.**

## Review Focus

1. **A user file that exists but is broken** (a hand edit, or a file from a newer version). The editor must not "fork" over it and destroy it. Expected: define mode reports the problem and offers only Reset. Reset moves the broken file aside to `<id>.broken-<unix_ts>.json`; it never deletes it. Test in Task 3.
2. **A binding whose action this build doesn't know** (written by a newer release). Expected: it survives load → edit → save, byte-for-byte in meaning. Test in Task 2.
3. **The process dies mid-save.** Expected: the previous file is intact, and no partial JSON is ever at the real path. Test in Task 3.
4. **A burst of edits** (dragging through several assignments, or typing a label). Expected: exactly one `reload_config` per 300 ms of quiet. Test in Task 6.
5. **The core switches to another bindings id while define mode is open** (`config_changed` with a different `bindings_id`). Expected: define mode exits, with a notice, rather than saving edits into a map the core no longer runs. Test in Task 6.

---

### Task 1: Core — keep the running map on a broken user document; report `bindings_id`

**Files:**
- Modify: `core/src/core.cpp`:
  - `loadBindings` (~280);
  - `reload()` (~970);
  - the `set_bindings` handler (~1040);
  - the hello setup (~1149).
- Modify: `core/include/kgn/ipc.hpp` (`HelloInfo`), `core/src/ipc.cpp` (`HelloInfo::toJson`).
- Modify: `core/include/kgn/core.hpp`, for one test seam.
- Modify: `python/keygnosys/coreclient/base.py`, `python/keygnosys/coreclient/ipc.py`.
- Modify: `docs/SPEC.md`, §3.3 and the IPC event table (§7, the `hello` and `config_changed` rows).
- Test: `core/tests/test_core.cpp`, `tests/test_ipc_dispatch.py`.

**Interfaces:**
- Produces, core:
  - `HelloInfo::bindingsId` (`std::string`), serialised as `"bindings_id"`;
  - `config_changed` payloads gain `"bindings_id"`, broadcast by both `reload_config` and a successful `set_bindings`;
  - test seam `[[nodiscard]] std::string Core::bindingsSourceForTests() const`, the path the running map was loaded from.
- Produces, Python:
  - `CoreClient.configChanged = Signal(dict)`;
  - `CoreClient.bindings_id` property, `str | None`: from `hello`, updated by `config_changed`; always `None` on `MockClient`.

- [ ] **Step 1: Write the failing core tests.**
  - Use a fresh temp directory per test, made with `std::filesystem::temp_directory_path() / uniqueEndpoint(...)`-style unique names. Write `bindings/default.json` into it as both `configDir` and `dataDir`, in separate subdirs.

  ```cpp
  KGN_TEST(a_broken_user_map_on_reload_keeps_the_running_map) {
      // configDir/bindings/default.json valid (binds KeyQ), dataDir/bindings/default.json valid (binds KeyW)
      // start → bindingsSourceForTests() == user path
      // overwrite user file with "{ not json"
      // send reload_config
      KGN_CHECK_EQ(fixture.core.bindingsSourceForTests(), userPath);      // not the shipped path
      // a diagnostic with code "binding.invalid" whose file == userPath was emitted
  }
  KGN_TEST(a_broken_user_map_at_startup_falls_back_and_says_so) {
      // user file broken before start → source == shipped path, diagnostics contain "binding.invalid" with file == userPath
  }
  KGN_TEST(hello_and_config_changed_carry_the_running_bindings_id) {
      // hello["d"]["bindings_id"] == "default"; after reload_config, the config_changed event's d.bindings_id == "default"
  }
  KGN_TEST(set_bindings_broadcasts_config_changed_with_the_new_id) {
      // second valid doc "alt" in dataDir; set_bindings {id:"alt"} → config_changed with bindings_id "alt"
  }
  ```

- [ ] **Step 2: Build and run.** The new tests fail: the first falls back to the shipped path, and the other three find no `bindings_id`.

- [ ] **Step 3: Implement.**
  - **`loadBindings`:** when a path's file **exists but fails to parse** and a map is already running, emit `binding.invalid` (message: the reason; file: the path) and return `false` *without trying later paths*.
    - With no running map (startup), emit the same diagnostic and continue down the search path.
    - Track "a map is running" with a `bool haveBindings` set on the first success.
  - **`reload()`:** load first.
    - Only on success: `releaseEverything()`, then `applyBindings()`.
    - On failure: leave the running map and its held actions alone, and reply `Reply::failure("binding.invalid", ...)`.
    - Keep the existing `loaded` counts on success.
  - **Broadcasts:** both `reload` and `set_bindings` broadcast `config_changed` with `kinds:["bindings"]` and `bindings_id: options.bindingsId`.
  - **Hello:** set `hello.bindingsId` at start and whenever the id changes.
  - **Existing test:** `reload_reports_what_it_actually_loaded` now expects a failure reply when nothing loads. Update its assertion to `!ok` with code `binding.invalid`, and keep its intent ("says what it loaded, never a made-up number").

- [ ] **Step 4: Build and run `ctest`.** Expected: all pass.

- [ ] **Step 5: Write the failing Python test** in `tests/test_ipc_dispatch.py`.
  - The test feeds `IpcClient._dispatch` a `hello` with `bindings_id: "default"`, then a `config_changed` event with `bindings_id: "alt"`.
  - It asserts that `client.bindings_id` goes `"default"` → `"alt"` and that `configChanged` fired once with the payload.
  - Follow the file's existing pattern for constructing a client without a socket.

- [ ] **Step 6: Implement.** Add the `configChanged` signal and the `bindings_id` property in `base.py` (default `None`). In `ipc.py`, record the id from `_on_hello` and dispatch `config_changed`.

- [ ] **Step 7: Run the tests.** `.venv/Scripts/python.exe -m pytest -q`: all pass.

- [ ] **Step 8: Amend the SPEC.**
  - **§3.3:** "A user document that exists but fails to load never replaces the running map. The core keeps what it is running and emits `binding.invalid`. Only at startup, with nothing running, may it fall back to the next document in the search path, with the same diagnostic."
  - **IPC event table:** `hello` gains `bindings_id`; `config_changed` gains `bindings_id` and is also sent by `set_bindings`.

- [ ] **Step 9: Commit.** Message: `Core keeps the running map when a user document is broken; report bindings_id`.

---

### Task 2: Model — serialise `BindingSet`, preserve unknown bindings, remember `was_on`

**Files:**
- Modify: `python/keygnosys/documents.py`: `Binding`, `Metadata`, `BindingSet`, `_binding_from_dict`.
- Modify: `docs/SPEC.md` §4.2: document `was_on`.
- Test: `tests/test_assignment.py`, `tests/test_documents.py`.

**Interfaces:**
- Produces:
  - `Binding.was_on: str | None = None`. It is **not** part of `identity()`.
  - `Binding.to_dict() -> dict`: `action`, `params`, plus `legend` and `was_on` only when set.
  - `Metadata.to_dict() -> dict`.
  - `BindingSet.extra_bindings: dict[str, dict]`: raw entries for keys whose action this build doesn't know, written back verbatim.
  - `BindingSet.extra_fields: dict[str, Any]`: top-level document fields this class doesn't model.
  - `BindingSet.to_dict() -> dict`: a full `keygnosys/bindings/2` document. It merges `extra_bindings` under `bindings`, where a modelled binding on the same key wins.
- Behaviour:
  - `assign(code, cmd)` sets `displaced.was_on = code` when the displaced command lands in `unassigned`.
  - `unassign(code)` does the same.
  - A command leaving `unassigned` has its `was_on` cleared.

- [ ] **Step 1: Write the failing tests.**

  ```python
  def test_to_dict_round_trips_the_shipped_default():
      doc = json.loads((BUNDLED_ROOT / "bindings" / "default.json").read_text(encoding="utf-8"))
      again = BindingSet.from_dict(BindingSet.from_dict(doc).to_dict()).to_dict()
      assert again == BindingSet.from_dict(doc).to_dict()
      assert again["schema"] == "keygnosys/bindings/2"

  def test_params_and_legend_survive_displace_save_restore():
      bset = BindingSet.from_dict(minimal_doc({"KeyD": {"action": "window.slot", "params": {"index": 3}, "legend": "Mail"}}))
      bset.assign("KeyD", Binding("button.click", {"button": "left"}))
      reloaded = BindingSet.from_dict(bset.to_dict())
      [u] = reloaded.unassigned
      assert (u.action, u.params, u.legend, u.was_on) == ("window.slot", {"index": 3}, "Mail", "KeyD")
      reloaded.assign("KeyF", u)
      assert reloaded.bindings["KeyF"].legend == "Mail" and reloaded.unassigned == []
      assert "was_on" not in reloaded.to_dict()["bindings"]["KeyF"]

  def test_an_unknown_action_survives_load_and_save():
      doc = minimal_doc({"KeyZ": {"action": "future.thing", "params": {"x": 1}}})
      out = BindingSet.from_dict(doc).to_dict()
      assert out["bindings"]["KeyZ"] == {"action": "future.thing", "params": {"x": 1}}

  def test_unmodelled_top_level_fields_survive():
      doc = minimal_doc({}); doc["x-note"] = "keep me"
      assert BindingSet.from_dict(doc).to_dict()["x-note"] == "keep me"
  ```

  `minimal_doc(bindings)` is a local test helper that returns a valid `bindings/2` dict.

- [ ] **Step 2: Run them.** `.venv/Scripts/python.exe -m pytest -q tests/test_assignment.py tests/test_documents.py`. Expected: FAIL, because `to_dict` doesn't exist.

- [ ] **Step 3: Implement** the Interfaces above.
  - `_binding_from_dict` keeps its warning for an unknown action. `BindingSet.from_dict` then stores that raw entry in `extra_bindings` instead of discarding it.
  - `to_dict` emits `schema`, `id`, `name`, `description` (when non-empty), `metadata`, `settings`, `bindings`, `unassigned`, plus `extra_fields`.

- [ ] **Step 4: Run the full Python suite.** All pass.

- [ ] **Step 5: Amend the SPEC.** In §4.2, under `unassigned`: "Entries may carry `was_on`, the key the command was last bound to. It is informational, set when a command is displaced, and dropped when it is assigned again."

- [ ] **Step 6: Commit.** Message: `BindingSet serialises losslessly and remembers where an unassigned command came from`.

---

### Task 3: Store — load, fork-on-save, atomic write, broken-file guard, reset

**Files:**
- Create: `python/keygnosys/binding_store.py`.
- Test: `tests/test_binding_store.py`.

**Interfaces:**
- Consumes: `BindingSet.from_dict`, `BindingSet.to_dict` (Task 2); `actions.validate_binding`.
- Produces:
  - `class StoreError(Exception)`.
  - `class BrokenUserCopy(StoreError)`, with attributes `path: Path` and `reason: str`.
  - `user_path(bindings_id: str) -> Path`, `shipped_path(bindings_id: str) -> Path`.
  - `load(bindings_id: str) -> BindingSet`:
    - user copy first, else shipped;
    - raises `BrokenUserCopy` if the user file exists but doesn't parse or validate;
    - raises `StoreError` if neither exists.
  - `shadows_shipped(bindings_id: str) -> bool`: true when a user copy exists **and** a shipped copy exists.
  - `save(bset: BindingSet) -> Path`:
    - refuses (raises `StoreError`) if any binding fails `validate_binding`;
    - refuses (raises `BrokenUserCopy`) if the existing user file is broken;
    - on the first save with no user copy and a shipped copy, sets `metadata.source_template = bset.id`;
    - increments `metadata.revision`;
    - creates the directory if needed and writes atomically.
  - `reset_to_shipped(bindings_id: str) -> BindingSet`:
    - moves the user file to `<id>.broken-<int(time.time())>.json` if it is broken, else deletes it;
    - returns the shipped set.

- [ ] **Step 1: Write the failing tests.**
  - All tests monkeypatch `paths.user_root` → `tmp_path / "user"` and `paths.BUNDLED_ROOT` → `tmp_path / "data"`, seeded with a shipped `default.json`.
  - The required test names and assertions:
    - `test_first_save_forks_under_the_same_id_and_leaves_shipped_untouched`: user file written; `source_template == "default"`; shipped bytes unchanged.
    - `test_save_bumps_revision`: two saves → `revision` is the original + 2.
    - `test_load_prefers_the_user_copy`.
    - `test_a_broken_user_copy_is_never_overwritten`:
      - user file `"{ nope"`;
      - `load` raises `BrokenUserCopy`;
      - `save(shipped_set)` raises `BrokenUserCopy`;
      - the file bytes are still `"{ nope"`.
    - `test_reset_moves_a_broken_copy_aside`: after reset, `user_path` is gone, exactly one `default.broken-*.json` exists with the original bytes, and the returned set equals shipped.
    - `test_reset_deletes_a_healthy_copy`.
    - `test_an_invalid_binding_is_refused_before_writing`: a `pointer.move` with `dir: "sideways"` raises `StoreError`, and no file is written.
    - `test_a_failed_write_leaves_the_previous_file_intact`:
      - save once;
      - monkeypatch `json.dumps` (as used by the store) to raise mid-save;
      - `save` raises;
      - the user file still parses to the first version;
      - no `.json.tmp` is left behind.

- [ ] **Step 2: Run them.** `.venv/Scripts/python.exe -m pytest -q tests/test_binding_store.py`. Expected: FAIL, because the module doesn't exist.

- [ ] **Step 3: Implement** `binding_store.py` per the Interfaces. A failed write must remove its temp file before re-raising.

- [ ] **Step 4: Run the full Python suite.** All pass.

- [ ] **Step 5: Commit.** Message: `Binding store: fork on first save, atomic writes, never overwrite a broken user copy`.

---

### Task 4: Editor model — targets, picker entries, edits, undo/redo

**Files:**
- Create: `python/keygnosys/binding_editor.py`.
- Test: `tests/test_binding_editor.py`.

**Interfaces:**
- Consumes: `BindingSet`, `Binding` (Task 2); `actions.CATALOG`, `COMMAND_PARAMS`, `default_legend`.
- Produces:
  - `NOT_TARGETS: frozenset[str]`: `CapsLock` plus the eight modifier codes from Global Constraints.
  - `GROUPS: dict[str, str]`, mapping action prefix to group label:
    - `button` → "Clicks";
    - `pointer` → "Pointer";
    - `scroll` → "Scroll";
    - `warp` → "Warp";
    - `window` → "Windows";
    - `layer`, `overlay`, `system` and `key` → "Layer & system".
  - `@dataclass PickerEntry`:
    - `group: str`;
    - `label: str`;
    - `action: str`;
    - `choices: list[dict]` (empty for parameterless actions);
    - `bound_on: dict[int, list[str]]`, mapping a choice index to the keys holding that choice;
    - `stored: Binding | None`, the unassigned command for entries in the "Unassigned" group.
  - `class BindingEditor`:
    - `__init__(self, bset: BindingSet, on_change: Callable[[BindingSet], None])`;
    - `bset: BindingSet` (live);
    - `is_target(code: str) -> bool`;
    - `entries(query: str = "") -> list[PickerEntry]`:
      - the "Unassigned" group first, one entry per `bset.unassigned` item, labelled with its legend or `default_legend`, plus "was on <code>" when set;
      - then one entry per catalog action, in `GROUPS` order;
      - `key.passthrough` is one entry whose single choice is filled in by the caller (`{"code": <selected key>}`);
      - `query` filters case-insensitively on label and action name;
    - `assign(code: str, binding: Binding) -> None`: raises `ValueError` if `not is_target(code)`;
    - `clear(code: str) -> None`;
    - `set_legend(code: str, legend: str | None) -> None`: empty string → `None`;
    - `set_params(code: str, params: dict) -> None`: assigns `Binding(current.action, params, current.legend)` through `assign`, so the old identity is displaced per §10.2.2;
    - `set_setting(name: str, value: float | int) -> None`: only keys present in `bset.settings`, else `KeyError`;
    - `undo() -> bool`, `redo() -> bool`;
    - `can_undo: bool`, `can_redo: bool`.
  - Every successful mutation:
    1. pushes a `to_dict()` snapshot taken *before* the change;
    2. clears redo;
    3. calls `on_change(bset)` once.
  - Undo and redo restore by `BindingSet.from_dict(snapshot)`, then call `on_change`.

- [ ] **Step 1: Write the failing tests.**

  ```python
  def test_activation_and_modifier_keys_are_not_targets(): ...   # every NOT_TARGETS code → is_target False; assign raises ValueError
  def test_a_parameterised_action_is_one_entry_with_choices():
      e = [x for x in editor.entries() if x.action == "window.slot"]
      assert len(e) == 1 and len(e[0].choices) == 9
      assert e[0].bound_on[0] == ["Digit1"]                       # on the shipped default
  def test_unassigned_entries_come_first_and_show_where_they_were(): ...
  def test_editing_params_displaces_the_old_identity():
      editor.set_params("Digit1", {"index": 5})                     # Digit5 also holds index 5 → allowed, two keys
      assert editor.bset.bindings["Digit1"].params == {"index": 5}
      assert any(u.params == {"index": 1} for u in editor.bset.unassigned)
  def test_undo_restores_the_exact_document_including_settings():
      before = editor.bset.to_dict()
      editor.assign("KeyD", Binding("button.click", {"button": "right"}))
      editor.set_legend("KeyD", "Menu"); editor.set_setting("pointer_max_speed", 40)
      while editor.undo(): pass
      assert editor.bset.to_dict() == before and editor.can_redo
  def test_each_mutation_notifies_once_and_redo_is_cleared_by_a_new_edit(): ...
  def test_search_filters_on_label_and_action_name(): ...
  ```

  Build `editor` with a fixture: the shipped default set and a recording `on_change`.

  Table-driven, as the SPEC's binding-editor test requirement asks: add
  `@pytest.mark.parametrize` cases for each of these through `BindingEditor`:
  - assign from Unassigned;
  - assign a parameterised command;
  - clear;
  - assign onto an empty key.

  Each case asserts the bindings, the unassigned list and `was_on`.

- [ ] **Step 2: Run them.** `.venv/Scripts/python.exe -m pytest -q tests/test_binding_editor.py`. Expected: FAIL.

- [ ] **Step 3: Implement** `binding_editor.py`.

- [ ] **Step 4: Run the full Python suite.** All pass.

- [ ] **Step 5: Commit.** Message: `Binding editor model: targets, grouped picker entries, undoable edits`.

---

### Task 5: The side picker widget

**Files:**
- Create: `python/keygnosys/ui/binding_picker.py`.
- Test: `tests/test_binding_picker.py`.

**Interfaces:**
- Consumes: `BindingEditor`, `PickerEntry` (Task 4); `ResolvedTheme`, for colours, following `ControlBar.apply_theme`.
- Produces `class BindingPicker(QWidget)`. It is a frameless `Qt.Tool` window that **accepts input**, docked beside the keyboard.
- Methods:
  - `set_editor(editor: BindingEditor | None)`;
  - `set_selected_key(code: str | None)`;
  - `set_can_reset(can: bool)`;
  - `dock_beside(anchor: QRect)`: left edge at `anchor.right() + 4`, top aligned with `anchor.top()`;
  - `refresh()`.
- Signals:
  - `assignRequested = Signal(str, object)` (code, `Binding`);
  - `armed = Signal(object)` (a `Binding`, or `None`): emitted when an entry is chosen with no key selected;
  - `clearRequested = Signal(str)`;
  - `legendEdited = Signal(str, str)`;
  - `settingEdited = Signal(str, object)`;
  - `resetRequested = Signal()`.
- Constructor arg: `confirm: Callable[[str], bool] = _ask`. `_ask` shows a `QMessageBox.question`. Tests pass a stub, so no modal dialog ever opens under test.
- Layout, top to bottom:
  1. the header "Assign to <key name> (now: <legend>)", or "Pick a key" with no key selected;
  2. a search `QLineEdit`;
  3. one list grouped by `PickerEntry.group`, with Unassigned entries visually distinct (an orange left border);
  4. a choice `QComboBox`, shown for an entry with more than one choice, which marks choices already bound as "(on <key>)";
  5. "Assign";
  6. the current key's label `QLineEdit`, emitting on `editingFinished`;
  7. "Clear key";
  8. a collapsible "Layer settings" section: one spin box per `bset.settings` key, `QDoubleSpinBox` for floats and `QSpinBox` for ints;
  9. "Reset to shipped", visible only when `set_can_reset(True)`, and emitted only if `confirm("Discard your edits to this map and return to the shipped one?")` returns True.
- List items carry a drag payload: MIME type `application/x-keygnosys-binding`, holding the JSON of `Binding.to_dict()`, so items can be dragged onto a key (Task 6).

- [ ] **Step 1: Write the failing `qtbot` tests.**
  - `test_choosing_an_entry_with_a_key_selected_requests_assignment`: select `KeyD`, pick "Right click" → `assignRequested("KeyD", Binding("button.click", {"button": "right"}))`.
  - `test_choosing_with_no_key_selected_arms_instead`.
  - `test_a_parameterised_entry_asks_for_its_choice_first`: `window.slot` with choice 4 → the Binding has params `{"index": 4}`.
  - `test_passthrough_targets_the_selected_key`: the Binding is `key.passthrough` with `{"code": "KeyD"}`.
  - `test_reset_needs_confirmation`: `confirm` returns False → no signal; returns True → one `resetRequested`.
  - `test_search_narrows_the_list`.
  - `test_it_docks_to_the_right_of_the_keyboard`.

- [ ] **Step 2: Run them.** `.venv/Scripts/python.exe -m pytest -q tests/test_binding_picker.py`. Expected: FAIL.

- [ ] **Step 3: Implement.** Expose the list, search box, choice box and buttons as attributes for tests, following the existing widgets' style.

- [ ] **Step 4: Run the full Python suite.** All pass.

- [ ] **Step 5: Commit.** Message: `Side picker for define mode`.

---

### Task 6: Overlay integration — Define toggle, selection, save, reload, lifecycle

**Files:**
- Modify: `python/keygnosys/ui/control_bar.py`:
  - a Define toggle;
  - Undo and Redo buttons;
  - the `● DEFINE MODE` indicator.
- Modify: `python/keygnosys/ui/keyboard_view.py`:
  - selection outline;
  - define-mode drops.
- Modify: `python/keygnosys/ui/overlay.py`.
- Modify: `python/keygnosys/state.py`: a notice for `binding.invalid`.
- Modify: `docs/SPEC.md`:
  - §10.2, placement on the overlay;
  - §3.2, implicit fork plus Reset to shipped; Duplicate set and Reset to source template deferred;
  - §14, an M5 note: the binding editor moved ahead of M4, and per-program maps and macros follow as projects 3–4.
- Modify: `docs/manual-tests.md`, adding rows for define mode.
- Test: `tests/test_define_mode.py`, `tests/test_state.py`.

**Interfaces:**
- Consumes: Tasks 1–5.
- `ControlBar` gains:
  - `defineToggled = Signal(bool)`, `undoRequested = Signal()`, `redoRequested = Signal()`;
  - `set_define_mode(on: bool, can_undo: bool, can_redo: bool)`.
  - Undo and Redo are hidden outside define mode. The indicator label is shown only in define mode.
  - There is no separate Done button. While define mode is on, the Define toggle reads "Done" and turning it off exits.
- `KeyboardView` gains:
  - `set_selected(code: str | None)`, which draws a 2 px orange (`#FFB020`) outline;
  - `set_define_mode(on: bool)`, which enables `setAcceptDrops`;
  - `bindingDropped = Signal(str, object)` (code, `Binding`), emitted on a drop of the Task 5 MIME type onto a key.
  - In define mode, non-target keys (`NOT_TARGETS`) are drawn dimmed, and clicks on them are ignored.
- `AppState.note_config_problem(diagnostic: dict, now: float) -> bool`:
  - for code `binding.invalid`, sets `notice` to `"Bindings not applied — {message}"`, with the same `NOTICE_S` deadline as `note_blocked`, and returns True;
  - otherwise returns False.
  - `Overlay._on_diagnostic` tries `note_blocked`, then `note_config_problem`, and shows the tray if either fires.
- `Overlay` define-mode behaviour:
  - **Entering:**
    1. Remember the current click-through state, then call `clickthrough.set_click_through(window, False)`. Don't change the stored setting.
    2. Choose the id: `client.bindings_id or settings["behavior.bindings"]`.
    3. Run `binding_store.load(id)`. On `BrokenUserCopy`, show the tray notice `"Your bindings file has an error ({reason}). Fix it, or use Reset to shipped."`, and offer only Reset in the picker, with no editor.
    4. Build `BindingEditor(bset, on_change=self._on_binding_edit)`.
    5. Set `state.binding_set = editor.bset`.
    6. Show the picker, docked with `dock_beside(window.frameGeometry())`.
    7. Call `bar.set_define_mode(True, ...)`.
  - **Clicking a key:** `keyClicked(code)` → if it is a target, `view.set_selected(code)` and `picker.set_selected_key(code)`. If the picker is armed, assign the armed binding to that key and disarm.
  - **Picker signals** map to `editor.assign` / `clear` / `set_legend` / `set_setting`. `bindingDropped` maps to `editor.assign`. `resetRequested` maps to `binding_store.reset_to_shipped(id)`, followed by a fresh editor and an immediate reload.
  - **`_on_binding_edit(bset)`:**
    1. `binding_store.save(bset)`. On `StoreError`, show the tray notice and don't reload.
    2. `view.update()`; refresh the picker and bar.
    3. Restart a 300 ms single-shot reload timer.
  - **When the timer fires:**
    - if `state.connected`, call `client.reload_config()`;
    - otherwise set `_reload_pending = True`. When `client.name == "mock"`, show the notice `"Saved — changes apply when the core is running."`.
  - **`_on_connected`:** if `_reload_pending`, call `reload_config()` once and clear the flag.
  - **`_on_disconnected`** doesn't touch define mode.
  - **`configChanged(payload)`:** if define mode is on and `payload["bindings_id"]` differs from the editor's id, exit define mode with the notice `"The core switched to bindings '{id}'; define mode closed."`. If define mode is off, reload `state.binding_set` from `binding_store.load(id)`, falling back to the registry on error.
  - **Exiting** (toggle off, or Done): restore the remembered click-through state; hide the picker; clear the selection; call `bar.set_define_mode(False, ...)`. If the reload timer is still active, fire it now.
  - **The picker follows the overlay.** `_on_dragged` and `_reposition` also call `picker.dock_beside(...)`.

- [ ] **Step 1: Write the failing tests.**
  - `tests/test_define_mode.py` builds an `Overlay` with `MockClient` (follow `tests/test_app_backend.py` for construction). It also uses a `FakeClient(CoreClient)` that records `reload_config` calls and lets tests emit `connected`, `disconnected` and `configChanged`. `binding_store` paths are monkeypatched to `tmp_path`, as in Task 3.
  - The required tests:
    - `test_entering_define_mode_turns_click_through_off_and_exiting_restores_it`: monkeypatch `clickthrough.set_click_through` to record calls.
    - `test_clicking_a_key_then_choosing_assigns_saves_and_redraws`: the user file has the new binding; `state.binding_set.bindings["KeyD"]` changed.
    - `test_a_burst_of_edits_reloads_once`: three edits within 100 ms → after `qtbot.wait(400)`, exactly one `reload_config`.
    - `test_edits_while_disconnected_reload_once_on_reconnect`.
    - `test_losing_the_core_keeps_define_mode_open`.
    - `test_a_core_switch_to_another_id_closes_define_mode`: Review Focus 5.
    - `test_a_broken_user_file_offers_only_reset`: Review Focus 1, at the UI level. The file is unchanged after entering and leaving define mode.
    - `test_the_activation_key_cannot_be_selected`.
    - `test_undo_from_the_bar_reverts_and_saves`.
  - Add to `tests/test_state.py`: `test_binding_invalid_becomes_a_notice`.

- [ ] **Step 2: Run them.** `.venv/Scripts/python.exe -m pytest -q tests/test_define_mode.py tests/test_state.py`. Expected: FAIL.

- [ ] **Step 3: Implement** the Interfaces above.

- [ ] **Step 4: Run the full Python suite.** All pass.

- [ ] **Step 5: Amend the SPEC and add manual-test rows.**
  - The SPEC sections are listed under Files.
  - The new manual-test rows live in a new "Define mode" group in `docs/manual-tests.md`, with the same columns as the existing rows:
    1. Enter define mode, reassign D to Right click, engage the layer, and press D: the right click happens.
    2. The displaced Left click appears under Unassigned as "was on D".
    3. Undo returns D to Left click, live.
    4. With the core stopped, make an edit, then start the core: the edit applies.
    5. Hand-break the user file: entering define mode says so, and Reset moves it aside.

- [ ] **Step 6: Commit.** Message: `Define mode on the overlay: pick, assign, save, reload, undo`.

---

### Task 7: Live check and wrap-up

**Files:**
- Modify: `docs/manual-test-logs/2026-08-30-m3-windows.md`, or start `docs/manual-test-logs/2026-11-27-binding-editor.md` if a new log is cleaner. The executor decides.

- [ ] **Step 1: Run the whole suite.**
  - Build the core.
  - Run `ctest`: all pass, and the count is the previous 23 plus the new tests.
  - Run `.venv/Scripts/python.exe -m pytest -q`: all pass.
- [ ] **Step 2: Give the operator a guided script for the five manual rows.**
  - Like the T9 guided rounds, the agent launches the core and overlay. The operator only clicks and presses keys.
  - Launch both with `C:\msys64\ucrt64\bin` first on PATH, and `python -m keygnosys --backend auto`.
  - Record PASS or FAIL per row in the log.
- [ ] **Step 3: Commit the log.** Message: `Binding editor: live check log`.

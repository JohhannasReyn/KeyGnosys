# O-10 Elevated-Window Inertness Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The core reports when the focused window is beyond its reach (it runs at a
higher integrity level than the core), and the overlay says so plainly.

**Architecture:**
- **Windows backend.** The backend already opens each window's process to read
  its name. It also compares that process's integrity level with the core's,
  and marks the `WindowInfo` `elevated`.
- **Core.** The core derives `inert` from the focused window and publishes it
  on the existing `mode` event and in `get_state`. Each change from not inert
  to inert also emits the `input.elevated_window` diagnostic SPEC §11 already
  defines.
- **Overlay.** The overlay keeps `inert` in `AppState`, dims the cursor-layer
  legends, and changes its status text.

**Tech Stack:** C++17 core (MinGW UCRT64, CMake/Ninja, `kgn_test.hpp`), Python 3.12
overlay (PySide6, pytest).

**Spec:** `docs/superpowers/specs/2026-11-16-o10-elevated-inert-design.md`. It
implements SPEC §8.2 ("the UI states plainly when the layer is inert for this
reason") and §11 (`input.elevated_window`, level `info`, "Interception inert;
an elevated window has focus").

## Global Constraints

- **Build:** `PATH=/c/msys64/ucrt64/bin:$PATH cmake --build build/default`.
  Warnings are errors (`-Werror -Wconversion -Wsign-conversion`).
- **Core suite:** `ctest` in `build/default`. 21 of 21 pass today.
- **Python suite:** `.venv/Scripts/python.exe -m pytest`.
- **Relinking:** the running core locks `keygnosys-core.exe`, so stop it first:
  `kgn.py send release_all`, then stop the process.
- **Wire format:** the `mode` payload becomes
  `{mode, latched, activation, inert: bool}`. `inert` is always present.
- **Status copy, exact:**
  - status text `layer inactive: admin window has focus`;
  - its tooltip `Windows does not let a non-elevated program read keys typed into an elevated (administrator) window. The layer resumes when an ordinary window has focus.`
- **Unreadable integrity:** if the target's integrity cannot be read, it is
  **not elevated**. Never claim inertness on a guess.
- **Latch:** unchanged when inertness begins or ends.

## Review Focus

1. **Integrity unreadable** (`OpenProcess` refused, e.g. a protected process).
   Expect `elevated == false`. Covered by Task 1's fallback rule; it is not
   reachable in a test.
2. **Focus moves between two elevated windows.** Expect no second diagnostic,
   and no `mode` re-publish, because `inert` did not change. Task 2 tests it.
3. **No foreground window** (desktop, lock screen). Expect `inert == false`.
   Task 2 tests it with `focusedId = 0`.
4. **The core itself runs elevated.** The same-level comparison then yields
   false everywhere, which is correct. Task 1's own-process test covers the
   equal-level branch.
5. **Overlay connects while already inert.** Expect `get_state` / the first
   `mode` event to carry `inert: true`. Task 2 asserts that `get_state` carries
   it.

---

### Task 1: Windows backend marks windows above the core's integrity

**Files:**
- Modify: `core/include/kgn/backends.hpp:49-55` (`WindowInfo`)
- Modify: `core/src/platform/windows/win32_window.hpp`,
  `core/src/platform/windows/win32_window.cpp` (in `enumerate`, where the
  process is already opened)
- Test: `core/tests/test_window_move.cpp` (an existing Windows platform test;
  add the tests there)

**Interfaces:**
- Produces: `bool elevated = false;` on `kgn::WindowInfo`, documented as "its
  process runs above this process's integrity level; Windows withholds its
  input from our hook".
- Produces: `bool kgn::win::runsAboveOurIntegrity(HANDLE process);`
  (declared in `win32_window.hpp`). Returns false on any failure to read
  either token.

- [ ] **Step 1: Write the failing tests**

```cpp
KGN_TEST(our_own_process_is_not_above_our_integrity) {
    KGN_CHECK(!kgn::win::runsAboveOurIntegrity(::GetCurrentProcess()));
}
KGN_TEST(an_unreadable_process_is_not_called_elevated) {
    KGN_CHECK(!kgn::win::runsAboveOurIntegrity(nullptr));
}
```

- [ ] **Step 2: Run them and see them fail to compile** (the function is
  undeclared). Then add a stub returning `true`, and see
  `our_own_process_is_not_above_our_integrity` fail.

Run: `cmake --build build/default --target test_window_move && ./build/default/core/test_window_move.exe`

- [ ] **Step 3: Implement `runsAboveOurIntegrity`.**
  - Read `TokenIntegrityLevel` (the last sub-authority of the label SID) for
    the target and for `GetCurrentProcess()`.
  - Cache our own level in a function-local static.
  - Return `target > ours`.
  - In `enumerate`, set `info.elevated = runsAboveOurIntegrity(process)` while
    the handle is open.
  - Leave `elevated` false when `OpenProcess` fails.

- [ ] **Step 4: Run the tests.** Expected: all `test_window_move` tests pass,
  and `ctest` passes 21 of 21.

- [ ] **Step 5: Commit**: "Mark windows whose process runs above the core's
  integrity level".

### Task 2: Core publishes `inert` and the diagnostic

**Files:**
- Modify: `core/src/core.cpp`:
  - `Impl` gains `bool inert = false;`
  - `publishWindowState` (focus-change block, ~line 731)
  - `publishState` (~line 815)
  - `stateSnapshot` (~line 848)
- Modify: `docs/SPEC.md`: §5.3 `mode` row (line 983); §8.2 elevated bullet
  (~line 1582)
- Test: `core/tests/test_core.cpp`

**Interfaces:**
- Consumes: `WindowInfo::elevated` (Task 1); `RecordingWindow` in
  `test_core.cpp:293`, whose `focused()` returns the entry for `focusedId`.
- Produces:
  - `mode` payload key `inert` (bool), and `get_state` key `inert` (bool);
  - the diagnostic `{level:"info", code:"input.elevated_window", message:
    "Interception inert: an elevated window has focus, and this process is not elevated."}`.

- [ ] **Step 1: Write the failing tests** in `test_core.cpp`. Use
  `RecordingWindow` with two windows, 11 (ordinary) and 22 (`elevated =
  true`), a `RawClient`, and `fixture.core.step(...)` between reads.

```cpp
KGN_TEST(focusing_an_elevated_window_makes_the_layer_inert_and_says_so_once)
// focusedId 11 -> step: get_state d["inert"] == false.
// focusedId 22 -> step: a `mode` event with d["inert"] == true, and exactly one
//   `diagnostic` with d["code"] == "input.elevated_window", d["level"] == "info".
// Another elevated window (33, elevated) focused -> step: no further diagnostic,
//   no further `mode` event.
// focusedId 11 -> step: a `mode` event with d["inert"] == false, no diagnostic.
// get_state now reports d["inert"] == false, d["latched"] unchanged.

KGN_TEST(no_focused_window_is_not_inert)
// focusedId 0 -> step: get_state d["inert"] == false.
```

- [ ] **Step 2: Run them and see them fail.** The `inert` key is absent.

Run: `cmake --build build/default --target test_core && ./build/default/core/test_core.exe`

- [ ] **Step 3: Implement.**
  - **In `publishWindowState`, on a focus change:** compute
    `nowInert = focused && focused->elevated`.
  - **If it changed:** store it, call `publishState()`, and on a false→true
    change call `note(DiagLevel::Info, "input.elevated_window", <message
    above>)`.
  - **Publishing:** `publishState` adds `inert` to `mode`, and
    `stateSnapshot` adds `inert`.
- [ ] **Step 4: Update SPEC.**
  - **§5.3 `mode` row:** `{mode, latched, activation, inert:bool}`, triggered
    by "Layer engaged/released, or `inert` changed".
  - **§8.2 elevated bullet:** "The core reports this as `mode.inert` and emits
    `input.elevated_window` when it begins."
- [ ] **Step 5: Run the suites.** Expected: the new tests pass and `ctest`
  passes 21 of 21.
- [ ] **Step 6: Commit**: "Publish inert on mode when an elevated window has
  focus (O-10)".

### Task 3: Overlay shows inertness

**Files:**
- Modify: `python/keygnosys/coreclient/base.py` (new signal),
  `python/keygnosys/coreclient/ipc.py:109-111` (`mode` handling)
- Modify: `python/keygnosys/state.py`:
  - `AppState` gains `inert: bool = False`;
  - `render_key`: in the cursor layer, while inert, `dim=True` for every key,
    text unchanged;
  - new `status_text()`.
- Modify: `python/keygnosys/ui/overlay.py`:
  - connect the signal;
  - `_update_status` uses `AppState.status_text()` when connected and inert.
- Test: `tests/test_state.py`

**Interfaces:**
- Consumes: the `mode` payload `inert` (Task 2).
- Produces:
  - `CoreClient.inertChanged = Signal(bool)`, emitted by `IpcClient` on every
    `mode` event with `bool(data.get("inert"))`;
  - `AppState.inert: bool`;
  - `AppState.status_text() -> tuple[str, str] | None`, which returns the exact
    status and tooltip from Global Constraints when `inert`, else `None`.

- [ ] **Step 1: Write the failing tests** in `tests/test_state.py`, using the
  existing `state` fixture.

```python
def test_inert_cursor_layer_dims_every_key_but_keeps_its_text(state):
    state.cursor_layer = True
    key = state.layout.key_by_code("KeyH")  # use the fixture's lookup idiom
    before = state.render_key(key)
    state.inert = True
    after = state.render_key(key)
    assert after.dim is True and after.text == before.text

def test_inert_status_names_the_reason(state):
    state.inert = True
    text, tip = state.status_text()
    assert text == "layer inactive: admin window has focus"
    assert "elevated (administrator) window" in tip

def test_no_status_override_when_not_inert(state):
    assert state.status_text() is None
```

  Look up the key the way the existing tests in that file do.

- [ ] **Step 2: Run them and see them fail.**

Run: `.venv/Scripts/python.exe -m pytest tests/test_state.py -q`

- [ ] **Step 3: Implement** the `AppState` changes, the signal, `IpcClient`'s
  emit, and the overlay wiring. In the overlay, on `inertChanged` set
  `state.inert`, call `_update_status()`, and `view.update()`.
- [ ] **Step 4: Run** `.venv/Scripts/python.exe -m pytest -q`. Expected: all pass.
- [ ] **Step 5: Commit**: "Overlay says when the layer is inert over an
  elevated window (O-10)".

### Task 4: Live re-run of rows 8.1 and 8.2

- [ ] **Step 1: Rebuild and restart** the core. Restart the overlay with
  `--backend ipc`, and arm `record.py`.
- [ ] **Step 2: Operator steps.**
  - Latch the layer and focus an admin Command Prompt. Expected: the overlay's
    status shows the exact text, and the legends are dimmed.
  - Return to Sublime. Expected: the status and legends return to normal, and
    the layer still works.
- [ ] **Step 3: Check the recording.** Expected: one `input.elevated_window`
  diagnostic, and `mode` with `inert: true`, then `false`. Sanitize to an F12
  boundary, as in earlier rows.
- [ ] **Step 4: Log and commit.** Append the rows 8.1 and 8.2 re-run to
  `docs/manual-test-logs/2026-08-30-m3-windows.md`, and close O-10.

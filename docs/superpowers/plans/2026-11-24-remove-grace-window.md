# Remove the Grace Window Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** CapsLock pressed first is the only way into the cursor layer. A key
bound in the layer, pressed while the layer is off, reaches the OS at once,
natively, exactly like an unbound key.

**Architecture:**

- The layer engine loses its pending buffer. A bound key pressed while the
  layer is off takes the existing unbound-key path.
- With nothing left to expire, the engine has no timed work. Removed with it:
  - `tick`;
  - `nextDeadline`;
  - `Decision::Kind::Buffer`;
  - the Windows `GraceTimer` / `GraceTimerPlan` deadline machinery.
- The `behavior.grace_ms` setting disappears from the core and the overlay.
- The SPEC and the manual matrix record the removal.

**Tech Stack:** C++20 core (CMake, MSYS2 UCRT64 GCC, `-Werror`, the in-repo
`KGN_TEST` harness), Python 3.12 overlay (pytest).

**Spec:** `docs/superpowers/specs/2026-11-22-remove-grace-window-design.md`

## Global Constraints

- **Build:** `PATH=/c/msys64/ucrt64/bin:$PATH cmake --build build/default`. It
  must build warning-free (`-Werror`).
- **Core tests:** `ctest --test-dir build/default`. Every remaining suite must
  pass.
- **Overlay tests:** `.venv/Scripts/python.exe -m pytest -q`.
- **Running core.** A running `keygnosys-core.exe` locks the binary.
  1. Before linking, run `.venv/Scripts/python.exe tools/manual/kgn.py send
     release_all`.
  2. Then stop the process.
- **P7 (the forwarded-release invariant) is untouched.**
  - A key whose press reached the OS always has its release forwarded.
  - Every existing P7, release-all, reload and delivery-channel test that does
    not depend on buffering passes unchanged.
- **No dormant path.**
  - No flag, no `grace_ms: 0` mode, and no commented-out code.
  - Before deleting a helper, confirm with grep that nothing besides the grace
    window uses it.
- **Unchanged behaviour:**
  - `hybrid_tap_ms`;
  - CapsLock tap, hold and latch in every activation mode;
  - `real_capslock_gesture`;
  - everything that happens while the layer is engaged.
- **Commit messages** carry no AI attribution lines (no `Co-Authored-By`, no
  "Generated with").

## Review Focus

1. **A bound key held down while CapsLock goes down, then released.** The press
   went to the OS natively, so P7 owes its release. The release must be
   forwarded, not swallowed by the layer. Add the test in Task 1.
2. **A bound key pressed with the layer off, then CapsLock tapped to latch,
   then the key released.** This is the same obligation across a latch, and the
   same test family. Add it in Task 1.
3. **`setBindings` (reload) while a bound key is held with the layer off.** It
   was forwarded natively, so the reload must not release or re-send it. An
   existing reload test may cover this. Otherwise add it in Task 1.
4. **An old `settings.json` that still contains `behavior.grace_ms`.** The
   overlay must still load and must not crash. The key is ignored. Add the test
   in Task 3.
5. **A client that still sends `set_setting behavior.grace_ms`.** It gets
   `ok: false`, `config.invalid`, and the core keeps running. Add the test in
   Task 3.

---

### Task 1: Engine — bound keys pass straight through while the layer is off

**Files:**
- Modify: `core/include/kgn/layer_engine.hpp`
- Modify: `core/src/layer_engine.cpp`
- Test: `core/tests/test_layer_engine.cpp`, `core/tests/test_hookchannel.cpp`,
  `core/tests/test_delivery_channel.cpp`

**Interfaces:**
- **Produces.** `LayerEngine::onKey` never emits `Decision::Kind::Buffer`.
  - With the layer off, a bound, non-passthrough key's Down yields exactly one
    `Forward` of that code and state. That is the native-passthrough condition
    in SPEC §6.3 / `hookchannel.cpp`.
  - `tick(now, out)` and `nextDeadline()` still exist after this task (Task 2
    deletes them). `tick` emits nothing, and `nextDeadline()` always returns
    `std::nullopt`, so the hook pump compiles and never arms its timer.
- **Removed from the engine:**
  - the pending array and `pendingCount_`;
  - `Slot::kPending`;
  - `addPending`, `bufferPress`, `takePending`, `resolvePendingBefore`,
    `resolveOldestPending` and `promoteBuffered`;
  - grace expiry;
  - `EngineConfig::grace`.
  - **Also:** `Decision::Kind::Buffer`, along with its `case` in
    `hookchannel.cpp`.
  - **Capacity:** `capacityDrops_` stays only if a non-grace path still
    increments it. Otherwise it goes, along with its accessor and any use in
    `core.cpp`.

- [ ] **Step 1: Write the failing tests** in `test_layer_engine.cpp`, using the
  file's existing fixture and binding idiom (`H` bound to an action, CapsLock
  through the fixture's helpers):
  - `a_bound_key_is_forwarded_at_once_when_the_layer_is_off`. `H` Down at t=0
    yields exactly `[Forward H Down]`, and `H` Up at t=10 yields exactly
    `[Forward H Up]`.
  - `a_bound_key_just_before_capslock_types_then_the_layer_engages`. `H` Down
    at 0 yields `[Forward H Down]`. CapsLock Down at 5 makes `mode()` `CURSOR`.
    `H` Up at 20 yields `[Forward H Up]` (P7), and `H`'s action never starts.
  - `a_bound_key_held_with_the_layer_off_repeats_natively`. `H` Down, then three
    `Repeat`s, each yield a single `Forward` of that state.
  - `a_bound_key_released_after_a_latch_is_still_forwarded` (Review Focus 2).
    `H` Down, then a CapsLock tap that latches, then `H` Up yields
    `[Forward H Up]`.
  - `a_reload_leaves_a_natively_held_bound_key_alone` (Review Focus 3).
    `H` Down, then `setBindings` with the same map, emits nothing for `H`, and
    `H` Up yields `[Forward H Up]`. Skip this test if an existing reload test
    already asserts exactly this.
- [ ] **Step 2: Run, and confirm they fail.** Build, then run
  `ctest --test-dir build/default -R test_layer_engine --output-on-failure`.
  The new tests should fail, with `Buffer` emitted instead of `Forward`.
- [ ] **Step 3: Implement.** In `onKey`'s Down branch for `NORMAL`, route every
  key that is not a modifier or CapsLock down the path unbound keys take today:
  `forwardPress(code, state, out)`, with no pending resolution before it. Then
  delete everything listed under "Removed" above.
- [ ] **Step 4: Delete obsolete tests.** Delete every test whose subject is
  buffering, expiry, promotion of buffered presses, or ordering inside the
  buffer. That covers O-2 and O-12, and every `f.tick(...)` that expects
  output. Delete these too:
  - `test_hookchannel.cpp`: `the_grace_replay_synthesises_both_halves_in_order`,
    and the `Buffer` half of `suppress_and_buffer_decisions_cost_no_capacity`.
    Keep its `Suppress` half, renamed `suppress_decisions_cost_no_capacity`.
  - `test_delivery_channel.cpp`:
    - delete `a_grace_pending_press_commits_to_neither_channel`;
    - **keep** tests that build `Forward` decisions by hand, such as the
      grace-replay scenario at lines 51-80, which still describe synthetic
      replays used by `release_all`. Reword their comments away from "grace".
  - **Rule:** a test that asserts P7 or release-all behaviour is never deleted;
    rewrite its setup to avoid buffering instead.
- [ ] **Step 5: Run the full suite.** `cmake --build build/default && ctest
  --test-dir build/default`. Expect all suites to pass.
- [ ] **Step 6: Commit.** `git commit -m "Forward bound keys at once while the
  layer is off: remove the grace buffer"`

### Task 2: Remove the deadline machinery

**Files:**
- Delete: `core/src/platform/windows/grace_timer.hpp`,
  `core/src/platform/windows/grace_timer.cpp`,
  `core/tests/test_grace_timer.cpp`
- Modify: `core/CMakeLists.txt` (lines 106 and 196),
  `core/include/kgn/hookpump.hpp` (`GraceTimerPlan`),
  `core/src/platform/windows/hook_input.{hpp,cpp}`, `core/src/core.cpp:1205`
  (comment), `core/include/kgn/layer_engine.hpp`,
  `core/src/layer_engine.cpp`
- Test: `core/tests/test_hookpump.cpp`, `core/tests/test_layer_engine.cpp`

**Interfaces:**
- **Consumes.** After Task 1, `LayerEngine::nextDeadline()` always returns
  nullopt, and `tick` emits nothing.
- **Produces.** `LayerEngine` has no `tick` or `nextDeadline`.
  - `HookInput` has no `graceTimer_`, `graceTimerArmed_` or `graceGeneration_`,
    no `syncGraceTimer` or `killGraceTimer`, and no `kMsgGraceExpired`.
  - The pump's `MsgWaitForMultipleObjectsEx` waits with `INFINITE`, or whatever
    timeout it used before grace, instead of a grace-derived one.

- [ ] **Step 1: Delete.**
  - Remove `GraceTimerPlan` and its tests in `test_hookpump.cpp` (the
    `// GraceTimerPlan` section from line 151, plus the `using Action` alias at
    line 25 if nothing else uses it).
  - Remove the deadline arming and expiry handling in `hook_input.cpp`: the
    blocks at 249-255, 308, 351, 366, 408-445 and 510-520.
  - Remove the timer's start and stop calls.
  - Remove `tick`, `nextDeadline` and their allocating overloads from
    `LayerEngine`, and every test call to them.
  - Rewrite the `core.cpp:1205` comment to say the engine has no timed work.
- [ ] **Step 2: Build and run.** `cmake --build build/default && ctest
  --test-dir build/default`. Expect it to build warning-free, with all suites
  passing. There is one suite fewer, because `test_grace_timer` is gone.
- [ ] **Step 3: Confirm.** `grep -rn -i "grace" core/` matches only the comments
  that record the removal, if any.
- [ ] **Step 4: Commit.** `git commit -m "Remove the grace deadline timer: the
  engine has no timed work"`

### Task 3: Retire the `grace_ms` setting

**Files:**
- Modify: `core/src/core.cpp:1027-1046`, `python/keygnosys/settings.py:32,50`
- Test: `core/tests/test_core.cpp:574-587`, `core/tests/test_ipc.cpp:296-300`,
  the overlay settings tests under `tests/` (use the existing settings test
  file's idiom)

**Interfaces:**
- **Produces.** `set_setting` with path `behavior.grace_ms` replies
  `ok: false`, `e.code == "config.invalid"`. The existing fallthrough
  ("no setting at '…'") gives exactly this.
  - `behavior.hybrid_tap_ms` behaves exactly as before.

- [ ] **Step 1: Write the failing tests.**
  - `test_core.cpp`: rename `a_known_setting_is_applied` to use
    `behavior.hybrid_tap_ms` with value 200, and keep its `ok` assertion.
  - `test_core.cpp`: add `the_retired_grace_setting_is_refused`. Send
    `set_setting {"path":"behavior.grace_ms","value":40}`, then assert
    `reply["ok"]` is false and `reply["e"]["code"] == "config.invalid"`.
  - `test_ipc.cpp:296-300`: change the path to `behavior.hybrid_tap_ms`. That
    test checks that the server passes data through, not grace.
  - Overlay: `test_an_old_settings_file_with_grace_ms_still_loads`. Write a
    settings JSON containing `"behavior": {"grace_ms": 50, "hybrid_tap_ms":
    200}`, load it, and assert that loading succeeds and
    `settings.get("behavior.hybrid_tap_ms") == 200`.
- [ ] **Step 2: Run, and confirm the new core test fails.** `ctest --test-dir
  build/default -R test_core --output-on-failure`. Expect a failure, because
  the reply is `ok: true`.
- [ ] **Step 3: Implement.**
  - `core.cpp`: drop `|| path == "behavior.grace_ms"` and the `grace` branch.
  - `settings.py`: remove the `"grace_ms": 50` default and the
    `("behavior.grace_ms", 0, 400)` clamp entry.
  - If the settings loader rejects unknown keys, make it ignore them instead.
    Check its behaviour first.
- [ ] **Step 4: Run both suites.** `cmake --build build/default && ctest
  --test-dir build/default` and `.venv/Scripts/python.exe -m pytest -q`. Expect
  everything to pass.
- [ ] **Step 5: Commit.** `git commit -m "Retire the grace_ms setting in the core
  and the overlay"`

### Task 4: SPEC and manual matrix

**Files:**
- Modify: `docs/SPEC.md`, `docs/manual-tests.md`

- [ ] **Step 1: Update the SPEC.**
  - **§6.3:**
    - Replace "The grace window (inherited from the prototype)" and its table
      with this rule: "CapsLock first is the only way in. A key bound in the
      cursor layer, pressed while the layer is off, is ordinary typing: it
      passes through natively, exactly like an unbound key." Follow it with one
      sentence on why: removed on 2026-11-22; the latency on everyday letters
      and the reordering bugs O-2/O-12 outweighed catching a key pressed a few
      ms before CapsLock.
    - Remove "Only bound keys are ever delayed" and its list.
    - Rewrite line 1182 so that no key is delayed.
    - In the native-passthrough bullets (around 1636), drop the grace-replay
      bullet.
  - **§6.3.3:** remove the buffered-ordering cases (around 1284) and the
    "already buffered" row (around 1313).
  - **§6.3.1:** drop the "grace window" sub-state (line 1138).
  - **Line 1428 (deadline wakeups):** state that the engine has no timed work.
  - **Remove** the `grace_ms` line from the config example (line 646).
  - **§15.10:** replace the whole section, calibration plan included, with a
    short "Removed: the grace window" note that gives the date, the reason and
    the design doc path.
  - **Delete** the grace rows in the test-strategy table (around 2159-2160).
  - **Finally,** `grep -n -i grace docs/SPEC.md` should match only the removal
    notes.
- [ ] **Step 2: Update the manual matrix (`docs/manual-tests.md`).**
  - Replace section 3 ("The grace window") with "## 3. Layer entry", holding one
    row:
    - **3.1, procedure:** "With the layer off, type `h`, `j`, `k`, `l` quickly,
      and hold `J` for a second. Then press `J` and, while it is down, press
      `CapsLock`."
    - **3.1, expected:** "Letters appear instantly and autorepeat normally; no
      key waits on CapsLock. In the last step `j` is typed, then the layer
      engages, and `J`'s release reaches the OS (no stuck key)."
  - Remove row 11.7.
  - Fix any cross-reference to rows 3.2–3.6 or 11.7 elsewhere in the file.
- [ ] **Step 3: Commit.** `git commit -m "SPEC and matrix: CapsLock first is the
  only way into the layer"`

### Task 5: Live check and M3 log (controller, with the operator)

**Files:**
- Modify: `docs/manual-test-logs/2026-08-30-m3-windows.md` (append only)

- [ ] **Step 1: Restart the core** on the built binary (stop it with
  `release_all` first).
- [ ] **Step 2: The operator runs new row 3.1.** The run is recorded by
  `record.py`, F12-gated, and cut at the F12 marker as an exact byte prefix,
  with hashes logged and no backup kept.
  - **What may be listed:** `mode` events, plus counts of `key` events with
    `suppressed: false` versus `true` while the layer was off. Every key the
    operator presses with the layer off should be `suppressed: false`.
  - **Not inspected:** typed text.
- [ ] **Step 3: Append a dated log entry.**
  - The decision and the design doc path.
  - **Retired:** rows 3.1–3.6 (3.5 had been unverified) and 11.7.
  - **Closed:** O-2, as no longer applicable, since nothing is buffered.
  - New row 3.1's result, with the capture hashes.
  - The operator's verdict on typing feel.
- [ ] **Step 4: Commit.** `git commit -m "Log the grace-window removal and new row
  3.1"`

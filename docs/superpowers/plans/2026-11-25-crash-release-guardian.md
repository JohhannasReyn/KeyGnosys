# Crash-Release Guardian Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A synthesized mouse button held by a forcibly terminated core is
released within 1 s by an external guardian. The core reports its protection
state honestly, and while it is unprotected it refuses to engage drag lock.

**Architecture:**

- **Write-ahead journal.** The core records each button transition in an
  unnamed shared-memory section, writing intent before `SendInput` and the
  result after it.
- **Guardian process.** `keygnosys-guardian.exe` is spawned per core instance
  via a relay. It waits on an inherited handle to the core process. On death it
  lifts only journaled buttons that Windows still reports down.
- **Protection state.** The state is platform-free in the core
  (`not_applicable` / `protected` / `unprotected{reason}`). It gates
  `button.drag_lock` and flows to `hello`, the `mode` and `get_state` payloads,
  diagnostics, and the overlay banner.

**Tech Stack:**

- C++20 (MSYS2 UCRT64, CMake, the in-repo `kgn_test.hpp` harness) and Win32:
  `CreateFileMapping`, `PROC_THREAD_ATTRIBUTE_HANDLE_LIST`, job objects and
  `GetAsyncKeyState`.
- Python 3 / PySide6 for the overlay (pytest).

**Spec:** `docs/superpowers/specs/2026-09-15-crash-release-guardian-design.md`.
The "Decisions of 2026-11-25" section overrides the older draft below it.

## Global Constraints

**Build and test commands.**

- Build: `PATH=/c/msys64/ucrt64/bin:$PATH cmake --build build/default`.
- Core tests: `PATH=/c/msys64/ucrt64/bin:$PATH ctest --test-dir build/default`
  (the PATH is needed for the DLLs).
- Overlay tests: `.venv/Scripts/python.exe -m pytest -q`.
- A running `keygnosys-core.exe` locks the binary. Before rebuilding, stop it
  with `.venv/Scripts/python.exe tools/manual/kgn.py send release_all` and then
  stop the process.

**Values the spec pins.**

- **Protection states:**
  - `not_applicable`, `protected`, `unprotected`.
  - Reasons: `spawn_failed`, `breakaway_denied`, `attach_timeout`,
    `guardian_identity_mismatch`, `guardian_lost`, `restart_exhausted`.
- **Diagnostic codes:**
  - `output.unprotected`, with the reason in the message;
  - `output.protected`;
  - `button.drag_lock_refused`.
- **Slot values:** `UP`=0, `DOWN_INTENT`=1, `DOWN`=2, `UP_INTENT`=3, one aligned
  32-bit word per button (Left, Right, Middle), each with a 32-bit
  `generation`.
- **Header fields:** `magic`, `layout_version`, `core_pid`,
  `core_creation_time`, `instance_nonce` (128-bit), `guardian_pid`, and `flags`
  (`ATTACHED`, `CLEAN_EXIT`). The header and the slots are on separate pages.
  The guardian maps the slot page read-only.
- **Timings:**
  - attach timeout 2 s;
  - restart backoff 1 s, 5 s, 30 s, at most 3 attempts per 10 minutes, then
    `restart_exhausted`;
  - recovery retry window ≤ 1 s from death detection.
- **The guardian sends mouse-button Up only, for L/R/M.**
  - No code path in it can send a Down, a key, a move or a scroll.
  - No hook, IPC, window, registry or network.
  - Its only dependencies are kernel32, user32 and advapi32 (the last for
    `RtlGenRandom`/`SystemFunction036`, if needed).
- **Recovery rule:**
  - `UP` → nothing.
  - Windows reports the button up → nothing (never a stray Up).
  - Otherwise send Up, re-read, and retry until the window expires.
- **Spawn ladder:**
  1. `CREATE_BREAKAWAY_FROM_JOB`.
  2. If that is denied and the job lacks `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`,
     spawn inside the job.
  3. Otherwise **Unprotected `breakaway_denied`**.
- **The guardian is spawned and attached before the input hook is installed.**
- **Option B gate:**
  - While the state is `unprotected` (including pending gaps), a drag-lock
    toggle that would *engage* produces no effect and one
    `button.drag_lock_refused` diagnostic.
  - A drag lock already engaged can still be toggled off.
  - `not_applicable` never gates.
- **Overlay:**
  - While `unprotected`, show the banner "Unprotected — drag lock off".
  - Flash the same text briefly on `button.drag_lock_refused`.
  - The inert banner takes precedence.
  - `not_applicable` shows nothing.
- **Process rules.**
  - **No AI attribution** in commits: no `Co-Authored-By: Claude` and no
    "Generated with" lines.
  - **Never inspect captured keystroke content.**
  - **Operator-run steps** are launched by the operator, never by the agent
    (human-stimulus rule).
- **O-4 is out of scope.** Do not change client-disconnect behaviour.

## Review Focus

1. **Clean exit sends nothing.** On every clean close path the guardian must
   inject nothing and exit 0, because `Core::stop` already released everything.
   Covered by a Task 5 test.
2. **The physical button is up but the journal says `DOWN`,** for example after
   a death between a successful Up and the `UP` record. No Up may be sent.
   Covered by Task 4 `recovery_skips_a_journaled_button_windows_reports_up`.
3. **A guardian restart while a drag lock is held.** The new guardian must
   attach to the *same* journal and protect the already-held button. Covered by
   Task 5 `a_restarted_guardian_reattaches_to_the_same_journal`.
4. **An old guardian must never lift a new core's press.** Its recovery runs
   once, bounded to 1 s, then it exits, and each core has a new nonce and
   section. Covered by Task 4 `recovery_gives_up_after_the_window`.
5. **The core must start and work** when the guardian binary is missing. The
   state is `unprotected{spawn_failed}`, and every other function still works.
   Covered by Task 5 `a_missing_guardian_binary_reports_spawn_failed`.

---

### Task 1: Measure job breakaway and swapped buttons (M1, M2)

The controller runs this task with the operator. It produces evidence and
rulings, not product code.

**Files:**
- Create: `tools/manual/probe_job.ps1`. It prints, for the shell it runs in:
  - `IsProcessInJob`;
  - the job's `LimitFlags` (from `QueryInformationJobObject`
    `JobObjectExtendedLimitInformation`), and whether `BREAKAWAY_OK`,
    `SILENT_BREAKAWAY_OK` and `KILL_ON_JOB_CLOSE` are set;
  - whether `CreateProcess(... CREATE_BREAKAWAY_FROM_JOB)` of `cmd /c exit`
    succeeds.

  Inline C# goes through `Add-Type`.
- Create: `tools/manual/probe_swap.ps1`. It:
  1. records `GetSystemMetrics(SM_SWAPBUTTON)`;
  2. calls `SwapMouseButton(TRUE)`;
  3. over a script-owned topmost window, injects `MOUSEEVENTF_LEFTDOWN`;
  4. reads `GetAsyncKeyState(VK_LBUTTON)` and `(VK_RBUTTON)`;
  5. injects `MOUSEEVENTF_LEFTUP`;
  6. restores the original swap state in a `finally` block.

  It prints which VK reported down.
- Modify: `docs/manual-test-logs/2026-08-30-m3-windows.md`, appending a dated
  entry.

- [ ] **Step 1:** Write both probes. `probe_swap.ps1` must restore the swap
  state even if it throws, and must inject only over its own window.
- [ ] **Step 2:** The operator runs `probe_job.ps1` from:
  - Windows Terminal (PowerShell);
  - a plain conhost window (`conhost.exe powershell`);
  - VS Code's integrated terminal;
  - a detached start (`Start-Process -WindowStyle Hidden powershell -ArgumentList '-File',...,'-OutFile'`).
- [ ] **Step 3:** The operator runs `probe_swap.ps1` once.
- [ ] **Step 4:** Log the results. Record two rulings in the plan ledger:
  - **Spawn ladder per host.** Expected Protected or Unprotected for each of the
    four hosts. This fixes Task 9's expectations.
  - **Swap mapping.** If `GetAsyncKeyState` reports the *physical* VK under a
    swap, Task 4's `physicalVk(MouseButton, bool swapped)` maps
    Left↔Right when swapped. Otherwise it is the identity.
- [ ] **Step 5:** Commit: `git commit -m "Measure job breakaway per host and GetAsyncKeyState under swapped buttons"`.

### Task 2: Protection state in the core and the drag-lock gate (platform-free)

**Files:**
- Modify: `core/include/kgn/backends.hpp` (`OutputBackend`).
- Modify: `core/include/kgn/actions.hpp` and `core/src/actions.cpp`
  (`Dispatcher`).
- Modify: `core/src/core.cpp`:
  - `modeJson`;
  - the `get_state` snapshot (around line 872);
  - `Core::start` for the hello limitation;
  - `Core::step`.
- Test: `core/tests/test_actions.cpp` and `core/tests/test_core.cpp`.

**Interfaces:**
- Produces, in `backends.hpp`:

  ```cpp
  enum class Protection : std::uint8_t { NotApplicable, Protected, Unprotected };
  struct ProtectionStatus { Protection state = Protection::NotApplicable; std::string reason; };
  // OutputBackend:
  [[nodiscard]] virtual ProtectionStatus protection() { return {}; }  // polled once per core tick
  ```

- Produces, on `Dispatcher`:
  - `void setDragLockAllowed(bool allowed)`, default `true`;
  - `[[nodiscard]] std::uint32_t takeDragLockRefusals()`, which returns the
    refusals since the last call and resets the count.

- [ ] **Step 1: Write the failing tests** in `test_actions.cpp`:
  - `drag_lock_refuses_to_engage_when_not_allowed`:
    - call `setDragLockAllowed(false)`;
    - fire a `button.drag_lock` Left press;
    - assert that no `DragLock` and no `Button` effect was emitted, and that
      `takeDragLockRefusals() == 1`.
  - `an_engaged_drag_lock_can_still_be_released_when_not_allowed`:
    - engage while allowed, then call `setDragLockAllowed(false)` and toggle
      again;
    - assert a `DragLock{down=false}` then `Button{Left, down=false}`, and 0
      refusals.
  - `disallowing_drag_lock_leaves_an_engaged_lock_held`:
    - engage, then call `setDragLockAllowed(false)`;
    - assert that no effect was emitted, so the button is still held.

  In `test_core.cpp`, use a fake output backend whose `protection()` returns a
  settable status:
  - `a_build_without_a_guardian_reports_not_applicable`:
    - the `get_state` reply has `protection == "not_applicable"`;
    - `hello.limitations` contains a line mentioning forced termination;
    - drag lock still engages.
  - `an_unprotected_backend_gates_drag_lock_and_says_so`:
    - set `Unprotected{"guardian_lost"}` and step once;
    - one `output.unprotected` diagnostic is broadcast, with `guardian_lost` in
      its message;
    - the `mode` payload carries `"protection": "unprotected"` and
      `"protection_reason": "guardian_lost"`;
    - a drag-lock binding produces a `button.drag_lock_refused` diagnostic and
      no `drag_lock` event.
  - `returning_to_protected_lifts_the_gate`:
    - go from Unprotected to Protected;
    - one `output.protected` diagnostic is broadcast, and drag lock engages
      again.
- [ ] **Step 2:** Run `ctest -R "test_actions|test_core"`. Expected: the new
  tests FAIL (they don't compile until the API exists, so add stubs first, then
  watch them fail on their assertions).
- [ ] **Step 3: Implement.**
  - In `Core::step`, call `backends.output->protection()` after
    `drainBackendDiagnostics()`. On a change of state or reason:
    - set `dispatcher.setDragLockAllowed(state != Unprotected)`;
    - `note()` the transition diagnostic;
    - `publishState()`.
  - Turn the refusals into `note(DiagLevel::Warn, "button.drag_lock_refused", ...)`
    after each effect batch.
  - Add the `protection` and `protection_reason` fields to `modeJson` and to
    the `get_state` snapshot.
  - Add the hello limitation for `NotApplicable`: "Forced termination of the
    core (crash, taskkill /F) is not covered on this build: a mouse button it
    is holding stays down until clicked." For `Unprotected` at start, add the
    same sentence prefixed "Crash-release guardian unavailable (<reason>):".
- [ ] **Step 4:** Run the full `ctest`. Expected: all PASS.
- [ ] **Step 5:** Commit: `git commit -m "Report protection state and refuse to engage drag lock while unprotected"`.

### Task 3: The obligation journal and write-ahead in `SendInputOutput::button`

**Files:**
- Create: `core/src/platform/windows/journal_layout.hpp`. It is header-only and
  shared with the guardian. It holds:
  - the `JournalHeader` and `JournalSlots` structs, `static_assert`ed to fit
    one page each;
  - `kJournalMagic` = `0x4B474A31` ("KGJ1") and `kJournalLayoutVersion = 1`;
  - the `SlotState` enum, with the values above;
  - the flag bits `kFlagAttached = 1` and `kFlagCleanExit = 2`.
- Create: `core/src/platform/windows/obligation_journal.hpp` and `.cpp`.
- Modify: `core/src/platform/windows/sendinput_output.hpp` and `.cpp`.
- Modify: `core/CMakeLists.txt` (add the sources to `KGN_PLATFORM_SOURCES`, and
  register the test).
- Test: `core/tests/test_obligation_journal.cpp`, via
  `kgn_add_test_with(test_obligation_journal kgn_platform)`.

**Interfaces:**
- Produces `class ObligationJournal`:
  - `static std::unique_ptr<ObligationJournal> create()` creates an unnamed,
    pagefile-backed, inheritable 2-page section. It returns null on failure.
  - `HANDLE sectionHandle() const`.
  - `void writeHeader(DWORD corePid, FILETIME created, const std::array<std::uint8_t,16>& nonce)`.
  - `void setSlot(MouseButton, SlotState)` is an atomic release-store that also
    bumps `generation`.
  - `SlotState slot(MouseButton) const`.
  - `bool attached() const`, `DWORD guardianPid() const`, `void markCleanExit()`.
- Produces `void SendInputOutput::setJournal(ObligationJournal* journal)`. It
  is non-owning, and null means no journaling.

- [ ] **Step 1: Write the failing tests**, using a `SendInputOutput` subclass
  whose `dispatch` override reads the journal slot at call time and returns a
  scripted result:
  - `a_press_writes_intent_before_dispatch_and_down_after`:
    - the slot seen inside `dispatch` is `DOWN_INTENT`;
    - afterwards it is `DOWN`.
  - `a_refused_press_leaves_the_slot_up`.
  - `a_release_writes_intent_before_dispatch_and_up_after`: the slot seen
    inside is `UP_INTENT`, and afterwards `UP`.
  - `a_refused_release_leaves_the_slot_down`.
  - `each_transition_bumps_the_generation`.
  - `without_a_journal_buttons_still_work`.
  - `the_header_round_trips_and_the_slots_sit_on_their_own_page`:
    `offsetof`, or the section base plus page size.
- [ ] **Step 2:** Run `ctest -R test_obligation_journal`. Expected: FAIL.
- [ ] **Step 3: Implement.**
  - `button()` order: `setSlot(intent)`, then `dispatch`, then
    `setSlot(ok ? result : prior)`.
  - Keep the existing `heldButtons_` and the failure-counter rules unchanged.
- [ ] **Step 4:** Run the full `ctest`. Expected: PASS.
- [ ] **Step 5:** Commit: `git commit -m "Journal every mouse-button transition write-ahead in shared memory"`.

### Task 4: The guardian executable

**Files:**
- Create: `guardian/guardian_logic.hpp` and `.cpp`. This is pure logic behind
  ports, and it includes `../core/src/platform/windows/journal_layout.hpp`.
- Create: `guardian/main.cpp`. This is Win32 glue only.
- Modify: `core/CMakeLists.txt`. On `WIN32`:
  - `add_library(kgn_guardian_logic guardian/...)`;
  - `add_executable(keygnosys-guardian guardian/main.cpp)`, linking only
    kernel32, user32 and advapi32, with the output in the same directory as
    `keygnosys-core`;
  - `kgn_add_test_with(test_guardian kgn_guardian_logic)`.

  If the repo layout makes a top-level `guardian/` awkward, use
  `core/src/guardian/`, and record which in the report.
- Test: `core/tests/test_guardian.cpp`.

**Interfaces:**
- Produces:
  - `enum class GuardianExit : int { Clean = 0, BadArgs = 2, IdentityMismatch = 3, ReleaseUnconfirmed = 4, Relayed = 5 }`.
  - `struct GuardianArgs { std::uintptr_t section, coreProcess; std::array<std::uint8_t,16> nonce; bool relay; }`.
  - `std::optional<GuardianArgs> parseArgs(int argc, char** argv)`. The command
    line is `keygnosys-guardian [--relay] --section <hex> --core <hex> --nonce <32 hex>`.
  - `bool validateHeader(const JournalHeader&, const std::array<std::uint8_t,16>& nonce, DWORD actualPid, FILETIME actualCreated)`.
  - `struct RecoveryPorts { std::function<bool(MouseButton)> windowsReportsDown; std::function<void(MouseButton)> sendUp; std::function<std::chrono::steady_clock::time_point()> now; std::function<void(std::chrono::milliseconds)> sleep; };`
  - `GuardianExit recover(const JournalSlots&, RecoveryPorts&, std::chrono::milliseconds window = std::chrono::milliseconds(1000))`.
  - `int physicalVk(MouseButton, bool swapped)`, per the Task 1 ruling.

- [ ] **Step 1: Write the failing tests.**
  - **One test per §7 table row,** each driving `recover()` with fake ports and
    asserting which buttons got `sendUp`:
    - `recovery_sends_nothing_when_every_slot_is_up`.
    - `recovery_skips_a_journaled_button_windows_reports_up`: slot 1 or 3, with
      Windows up.
    - `recovery_lifts_a_down_intent_windows_reports_down`.
    - `recovery_lifts_a_held_button`.
    - `recovery_lifts_an_up_intent_windows_still_reports_down`.
    - `recovery_lifts_each_held_button_exactly_once`.
    - `recovery_retries_until_windows_reports_up`: the fake stays down for two
      reads.
    - `recovery_gives_up_after_the_window`: it returns `ReleaseUnconfirmed`, and
      no `sendUp` happens after `now()` passes 1000 ms.
  - **Header checks:** `header_validation_rejects_bad_magic`, then
    `..._bad_version`, `..._wrong_nonce`, `..._pid_mismatch` and
    `..._creation_time_mismatch`. Plus `header_validation_accepts_a_match`.
  - **Arguments:** `parse_args_rejects_missing_or_malformed_values`,
    `parse_args_reads_relay_mode`.
  - **Swap mapping:** `physical_vk_maps_per_swap_ruling`.
- [ ] **Step 2:** Run `ctest -R test_guardian`. Expected: FAIL.
- [ ] **Step 3: Implement `guardian_logic`, then `main.cpp`.**
  - **`--relay` mode:** spawn the same exe without `--relay`, inheriting
    exactly the two handles (`PROC_THREAD_ATTRIBUTE_HANDLE_LIST`), with
    `DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP`. Then exit `Relayed` at once,
    so the guardian's parent is dead.
  - **Normal mode:**
    1. Map the header page read-write and the slot page read-only.
    2. Validate the header against `GetProcessId` and `GetProcessTimes` on the
       core handle.
    3. Write `guardian_pid`, then set `ATTACHED`.
    4. `WaitForSingleObject(core, INFINITE)`.
    5. Call `recover()` with real ports:
       - `GetAsyncKeyState(physicalVk(b, GetSystemMetrics(SM_SWAPBUTTON)))`;
       - `SendInput` with `MOUSEEVENTF_*UP` only.
  - **On mismatch or unconfirmed release,** append one line to
    `%LOCALAPPDATA%\KeyGnosys\logs\guardian.log`, then exit with the code.
- [ ] **Step 4:** Run the full `ctest` and build `keygnosys-guardian`.
  Expected: PASS. Check that `objdump -p build/default/keygnosys-guardian.exe | grep "DLL Name"`
  lists only KERNEL32, USER32, ADVAPI32 and the C/C++ runtime DLLs.
- [ ] **Step 5:** Commit: `git commit -m "Add keygnosys-guardian: lift journaled buttons after its core dies, and nothing else"`.

### Task 5: Spawn, attach and supervise the guardian from the core

**Files:**
- Create: `core/src/platform/windows/guardian_supervisor.hpp` and `.cpp`.
- Create: `core/src/platform/windows/restart_policy.hpp`. It is header-only
  pure policy.
- Modify: `core/src/platform/windows/sendinput_output.{hpp,cpp}`. It owns the
  supervisor and implements `protection()`.
- Modify: `core/src/platform/windows/platform_windows.cpp`. In
  `createBackends`, create the journal and supervisor, then `start()` it
  *before* constructing `HookInput`.
- Modify: the `Core::stop` path, so that `markCleanExit()` runs after
  `releaseAll()`. Reach it through the output backend: add
  `virtual void markCleanExit() {}` to `OutputBackend`.
- Test: `core/tests/test_guardian_supervisor.cpp` (registered with ctest).

**Interfaces:**
- Produces `class RestartPolicy`:
  - `explicit RestartPolicy(steady_clock::time_point start)`.
  - `std::optional<steady_clock::time_point> nextAttempt(steady_clock::time_point lostAt)`
    follows the 1 s / 5 s / 30 s schedule. It returns nullopt once 3 attempts
    have been made within 10 minutes.
  - `void attemptMade(steady_clock::time_point)`.
- Produces `class GuardianSupervisor`:
  - `GuardianSupervisor(ObligationJournal&, std::wstring guardianPath)`.
  - `ProtectionStatus start()` runs the spawn ladder and waits up to 2 s for
    `ATTACHED`.
  - `ProtectionStatus poll(steady_clock::time_point now)` does a non-blocking
    liveness check and drives restarts.
  - `void markCleanExit()`.
- Consumes:
  - from Task 2: `OutputBackend::protection()`;
  - from Task 3: `ObligationJournal`;
  - from Task 4: the guardian's command line and exit codes.

- [ ] **Step 1: Write the failing tests.**
  - `restart_policy_follows_1_5_30_then_exhausts`.
  - `restart_policy_window_slides_after_10_minutes`.
  - **Integration tests.** These spawn the real `keygnosys-guardian.exe`
    against a throwaway child process (e.g. `cmd /c ping -n 2 127.0.0.1`)
    standing in for the core. They use a journal whose slots the test sets.
    They inject nothing, because Windows reports no button down:
    - `a_guardian_attaches_within_the_timeout`: `start()` returns `Protected`,
      and the header has `ATTACHED` and the guardian pid.
    - `a_guardian_exits_clean_when_nothing_is_held`: the child exits, the
      guardian exits 0, and no `sendUp` path ran.
    - `a_guardian_skips_a_journaled_button_windows_reports_up`: the slot is
      `DOWN` while Windows reports up, and the guardian exits 0.
    - `a_wrong_nonce_is_refused`: the guardian exits 3, and `start()` reports
      `Unprotected{attach_timeout}` or `guardian_identity_mismatch`, whichever
      the supervisor can tell.
    - `a_missing_guardian_binary_reports_spawn_failed`.
    - `a_killed_guardian_reports_guardian_lost`, then
      `a_restarted_guardian_reattaches_to_the_same_journal`: `poll()` after the
      backoff returns `Protected`, and the header has the new pid.
- [ ] **Step 2:** Run `ctest -R test_guardian_supervisor`. Expected: FAIL.
- [ ] **Step 3: Implement.**
  - **Spawn.** Start `keygnosys-guardian.exe --relay ...` from the core's own
    directory (`GetModuleFileNameW`). Use
    `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` with the section and a duplicated
    `SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION` core handle, both
    inheritable. Flags are
    `DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP | CREATE_BREAKAWAY_FROM_JOB`.
  - **On `ERROR_ACCESS_DENIED`:** query the job with
    `QueryInformationJobObject(nullptr, JobObjectExtendedLimitInformation)`.
    Retry without breakaway iff `KILL_ON_JOB_CLOSE` is clear. Otherwise the
    state is `breakaway_denied`.
  - **Nonce:** `BCryptGenRandom` or `RtlGenRandom`.
  - **Liveness:** the guardian's PID comes from the header. Open it with
    `SYNCHRONIZE` and check with `WaitForSingleObject(h, 0)`.
- [ ] **Step 4:** Run the full `ctest`. Expected: PASS. Then start the real core
  by hand. Its `get_state` shows `protection: protected`, and `tasklist` shows
  one `keygnosys-guardian.exe` whose parent is not the core.
- [ ] **Step 5:** Commit: `git commit -m "Spawn and supervise the guardian; report protection from the output backend"`.

### Task 6: A fault-injection binary for death at each transition

**Files:**
- Create: `core/tests/manual_guardian_fault.cpp`. It is built as
  `kgn_guardian_fault` and is **not** registered with ctest, like
  `kgn_hook_smoke`.

**Interfaces:**
- Consumes `ObligationJournal`, `GuardianSupervisor` and a `SendInputOutput`
  subclass whose `dispatch` can call `TerminateProcess(GetCurrentProcess(), 9)`
  at a chosen point.
- The command line is
  `kgn_guardian_fault --button left|right|middle --die before-down|after-down|held|before-up|after-up`.
  It opens its own topmost window, moves the cursor over it, presses, and dies
  where told.

- [ ] **Step 1:** Write it.
  - **`before-down`:** die after `DOWN_INTENT`, before `SendInput`.
  - **`after-down`:** die after `SendInput(down)`, before `DOWN` is recorded.
  - **`held`:** die with the slot at `DOWN`.
  - **`before-up`:** die after `UP_INTENT`, before `SendInput(up)`.
  - **`after-up`:** die after `SendInput(up)`, before `UP` is recorded.
- [ ] **Step 2:** Build it. Check that it builds and that `--help` prints its
  usage. It is not run here; Task 9 runs it.
- [ ] **Step 3:** Commit: `git commit -m "Add a fault-injection binary that dies at each journal transition"`.

### Task 7: The overlay shows the protection state

**Files:**
- Modify: `python/keygnosys/coreclient/base.py` and
  `python/keygnosys/coreclient/ipc.py`. Add a
  `protectionChanged = Signal(str, str)` (state, reason), emitted from `mode`
  events the way `inertChanged` is.
- Modify: `python/keygnosys/state.py`. Add the `protection` and
  `protection_reason` fields and a `flash_until` timestamp. Extend
  `banner_text()` and `status_text()`.
- Modify: the overlay window wiring in `python/keygnosys/app.py` or `ui/`,
  wherever `inertChanged` and the banner are wired today. Connect the signal,
  and flash on a `diagnostic` whose `code == "button.drag_lock_refused"`.
- Test: `tests/test_state.py` and `tests/test_ipc_dispatch.py`.

- [ ] **Step 1: Write the failing tests.**
  - `test_banner_text_while_unprotected`: the banner reads "Unprotected — drag
    lock off".
  - `test_inert_banner_takes_precedence_over_unprotected`.
  - `test_not_applicable_shows_no_banner`.
  - `test_refused_drag_lock_flashes_the_banner_briefly`: the banner shows the
    text before `flash_until` and not after. The flash lasts 2 s, which is
    *my call* and is set in `state.py` as `PROTECTION_FLASH_S = 2.0`.
  - `test_unprotected_status_names_the_reason`: the tooltip contains the reason.
  - `test_mode_event_drives_protection_changed`, with
    `test_mode_event_without_protection_reads_as_not_applicable` covering older
    cores.
- [ ] **Step 2:** Run `.venv/Scripts/python.exe -m pytest -q`. Expected: the new
  tests FAIL.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** Run `pytest -q`. Expected: all PASS.
- [ ] **Step 5:** Commit: `git commit -m "Overlay: show when the core is unprotected and when drag lock is refused"`.

### Task 8: SPEC, matrix and launcher contract

**Files:**
- Modify: `docs/SPEC.md`:
  - a new guardian component section, next to §8.3 Output;
  - §5: the `hello.limitations` line, plus the `protection` and
    `protection_reason` fields on `mode` and `get_state`;
  - §7.2, `button.drag_lock`: refused while unprotected;
  - §11: the three diagnostic codes.

  Leave the P7 text unchanged.
- Modify: `docs/manual-tests.md` §10. Keep row 10.2's expectation. Add rows
  for the design's §12.3 table, using the Task 1 host expectations.
- Modify: `docs/LAUNCHING.md`. Make the design's §11 amendments.
- Modify: `docs/superpowers/specs/2026-09-15-crash-release-guardian-design.md`.
  Change its status line to "implemented", pointing at this plan.

- [ ] **Step 1:** Make the edits. Every value must match the Global
  Constraints exactly.
- [ ] **Step 2:** Run `grep -n "drag_lock_refused\|output.unprotected\|not_applicable" docs/SPEC.md`.
  Expected: each appears.
- [ ] **Step 3:** Commit: `git commit -m "SPEC, matrix and launcher contract: the crash-release guardian"`.

### Task 9: Live validation and the M3 log

The controller runs this task with the operator. The operator launches every
human-stimulus step.

- [ ] **Step 1:** Rebuild, then have the operator start the core the way they
  normally start it.
- [ ] **Step 2: Row 10.2.** Drag lock Left, then `taskkill /F` the core. Pass:
  Windows reports the button up within 1 s, with no rescue click. Judge this
  with probe-validated `GetAsyncKeyState` sampling (the section-10 harness
  `tools/manual/delayed.ps1`), not observer accounting alone.
- [ ] **Step 3: The remaining §12.3 rows:**
  - clean exits;
  - `taskkill /T /F`;
  - Task Manager End task, on both a detached and a console core;
  - guardian killed first: the overlay banner appears, and drag lock is
    refused with a flash;
  - restart and reattach;
  - two buttons held;
  - `kgn_guardian_fault` at each `--die` point;
  - partial unwind, then kill;
  - a missing binary;
  - each host from Task 1;
  - swapped buttons.
- [ ] **Step 4:** Append a dated M3 log entry with the per-row results. Record
  each capture's hashes and boundary as before. Read only the events the row
  needs.
- [ ] **Step 5:** Commit: `git commit -m "M3 log: crash-release guardian validated live; row 10.2 passes"`.

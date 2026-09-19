# Crash-release guardian — production design (draft for review)

**Status:** design only. No guardian code exists outside the throwaway E6 spike.
No SPEC, P7 wording, launcher contract or production behaviour is changed by
this document. Implementation waits for review **and** for finding O-4 (§4).

**Problem.** A synthesized mouse button held by the core stays down in Windows
after the core is forcibly terminated — `taskkill /F`, a crash, Task Manager
**End task** on a detached core. Row 10.2 observed ≈ 25.5 s of a held left
button until a physical click. No code in the terminated process can run, so
this cannot be repaired in-process.

**What is already settled** (see
[`2026-09-14-p7-termination-options.md`](2026-09-14-p7-termination-options.md)
and the M3 log):

| Termination | Status |
|---|---|
| Ctrl+C / Ctrl+Break; console-window close; Windows Terminal close | Handled in-process (O-3 repair, `fix/m3-console-close-release`, live-validated) |
| Task Manager End task on a **console-attached** core | Same `CTRL_CLOSE_EVENT` path (E3 characterisation); not re-run with held input on the repair |
| Logoff / shutdown | **Open** — not delivered to a `user32` process; hidden window not built |
| `TerminateProcess`, crash, End task on a **detached** core | **Not coverable in-process** — this document |

E6 demonstrated the direction: an external process detected the target's death
and released a stranded button ≈ 6 ms later, covered the inject-before-record
race, stayed quiet on clean exit, and refused stale or wrong identities.

---

## 1. Scope

**In scope: persistent synthetic mouse-button obligations** — Left, Right,
Middle (`MouseButton`, `keycode.hpp`; there are no others).

**Out of scope, deliberately:**

- **Synthesized keys.** Every synthesized key-down the core produces mirrors a
  key the user is physically holding (grace replays, re-synthesised forwards),
  and after the core dies the physical key-up reaches Windows directly and
  discharges it — observed in `EXCLUDED-attempt1-10.2`. Revisit only if a key
  obligation without a physical counterpart is introduced.
- Natively forwarded presses (already crash-safe, threading design §3).
- Logoff/shutdown (separate open gap; the session teardown is the likely effect,
  unverified).
- Linux/M4 parity (XTest/uinput semantics on client death are unverified).
- Any recovery of *core* state. The guardian restores nothing; it only lifts.

**The guardian's trigger is loss of the guarded core instance.** Nothing else.

---

## 2. Concepts that must stay distinct

| Lifetime | Begins | Ends | Owns |
|---|---|---|---|
| **IPC client** | connect | disconnect / crash | nothing global (pending O-4) |
| **Core instance** | process creation | process termination (any way) | the dispatcher, the output backend, every synthetic obligation |
| **Guardian** | spawned for one core instance | after that instance is gone and recovery is done | a read-only view of one instance's obligation journal; the right to lift a journaled button |
| **Synthetic obligation** | a successful injected button-down | the matching successful up | — owned by the core; *shadowed* by the journal |

The guardian never owns an obligation while the core lives. It inherits
responsibility for exactly the obligations the journal shows, at exactly the
moment the core instance ceases to exist.

---

## 3. Process and lifecycle

### 3.1 Who launches the guardian — the core

**Proposed: the core spawns its own guardian at startup.**

The launcher is a developer convenience whose implementation waits for M4
(LAUNCHING §1.1–1.2), and today cores are started by hand, by the launcher, and
eventually by autostart. If the launcher owned the guardian, every other start
path would run unprotected. The core is the one component present on every path,
so protection travels with it.

**Spawn sequence** (core main thread, after the output backend exists, before
the hook is installed and before any input is accepted):

1. Create the **obligation journal** (§5) as an *unnamed* pagefile-backed
   section, handle marked inheritable.
2. Duplicate an inheritable handle to the core process itself with
   `SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION`.
3. Generate a 128-bit random instance nonce; write the journal header (§4).
4. Start `keygnosys-guardian.exe` (same directory as the core) via a short-lived
   **intermediate** (§3.3), inheriting exactly those two handles
   (`PROC_THREAD_ATTRIBUTE_HANDLE_LIST`), with `DETACHED_PROCESS |
   CREATE_NEW_PROCESS_GROUP | CREATE_BREAKAWAY_FROM_JOB` and the handle values
   plus nonce on its command line.
5. Wait, bounded (proposed 2 s), for the guardian to write its PID and an
   `ATTACHED` marker into the header.
6. Record the protection state: **Protected**, or **Unprotected** with a reason
   (§8).

The core never waits on the guardian again except to watch for its death.

### 3.2 Guardian lifetime

- **Exactly one guardian per core instance.** It serves no other instance, ever.
- It blocks on the inherited core-process handle.
- When the handle is signalled it runs recovery **once** (§6), with a short,
  bounded retry window, then exits. It never lingers, polls later, or survives
  into a future core's lifetime.
- **Clean core shutdown:** `Core::stop()` releases everything, so every slot is
  `UP`; the core also sets `CLEAN_EXIT` in the header just before `main` returns.
  The guardian finds nothing held, injects nothing, exits 0. `CLEAN_EXIT` is
  diagnostic only — recovery decisions come from the slots and Windows state,
  never from the flag (a core can die after setting it).

### 3.3 Staying out of the core's termination domain

| Threat | Mitigation | To validate |
|---|---|---|
| `CTRL_CLOSE_EVENT` sent to every process on the core's console | `DETACHED_PROCESS` — the guardian has no console | E6 did not test a console core; production test |
| `taskkill /T` walks descendants by parent PID | The intermediate spawns the guardian and exits at once, so the guardian's parent is a dead process, not the core | E6 T2 proved survival only when *not* in the tree; production test with the intermediate |
| Job object with `KILL_ON_JOB_CLOSE` around the core (IDEs, some terminals, service wrappers) | `CREATE_BREAKAWAY_FROM_JOB`; if breakaway is not permitted, spawn fails → **Unprotected**, reported | Which launch hosts deny breakaway (Windows Terminal, IDE runners) — unknown |
| Ctrl+C to the core's process group | `CREATE_NEW_PROCESS_GROUP` + no console | production test |
| Logoff / shutdown | Both processes end with the session; the input desktop goes with it | out of scope (§1) |

### 3.4 Guardian dies first

- The core watches the guardian's process handle (non-blocking check once per
  core tick; `WaitForSingleObject(h, 0)` is a syscall on a 60 Hz loop, well
  within budget, and never on the hook thread).
- On guardian death: state → **Unprotected** (`guardian_lost`), P6 diagnostic
  broadcast, `get_state` updated (§8).
- **Restart policy (proposed):** respawn against the *same* journal and nonce,
  with backoff (1 s, 5 s, 30 s), at most 3 attempts per 10 minutes; then remain
  Unprotected until the core restarts. Obligations already journaled are covered
  again the moment a new guardian attaches. Between the death and the reattach
  the core is unprotected, and says so.

### 3.5 Launcher exit and core restart

- A foreground launcher stops the processes *it* started (LAUNCHING §4.3). It
  stops the **core**; the guardian is the core's, not the launcher's, and exits
  on its own after the core. The launcher must not kill the guardian, and must
  not treat it as an instance signal (LAUNCHING §5 already restricts instance
  detection to the IPC endpoint).
- **Restart race.** A dying core's guardian runs recovery within milliseconds and
  then exits; a new core takes on the order of a second to start. The retry
  window is bounded (proposed ≤ 1 s from death detection) so an old guardian can
  never lift a button a *new* core has just pressed. The new core has its own
  journal and guardian.

---

## 4. Core-instance identity

Identity is purely about the core *process*; IPC clients play no part in it
(see §10 for where O-4 does matter).

The guardian must act for exactly one core instance and no other. PID alone is
insufficient: PIDs are reused.

**Primary binding — handle, not number.** The guardian receives an inherited
handle to the core process (§3.1). A process handle refers to one process object
for as long as it is open, regardless of PID reuse. Waiting on it cannot be
confused with any later process.

**Header identity** (written by the core before spawning; checked by the
guardian before `ATTACHED`):

| Field | Purpose |
|---|---|
| `magic`, `layout_version` | reject a foreign or incompatible section |
| `core_pid` | human diagnostics; cross-check `GetProcessId(core_handle)` |
| `core_creation_time` (FILETIME) | cross-check `GetProcessTimes(core_handle)` |
| `instance_nonce` (128-bit) | must equal the nonce on the guardian's command line |
| `guardian_pid` | written by the guardian at attach |
| `flags` | `ATTACHED`, `CLEAN_EXIT` |

Any mismatch → the guardian writes nothing, injects nothing, exits with a
distinct code; the core stays **Unprotected** (`guardian_identity_mismatch`).

**Stale state.** The section is unnamed: it cannot be opened by name, only
through inherited or duplicated handles, and it is destroyed when its last
handle closes. A restarted core creates a new section with a new nonce; nothing
from a previous instance is reachable. (E6 T5 proved the named-section
equivalent.)

---

## 5. Obligation journal and write-ahead protocol

### 5.1 Where it hooks in

The only mouse-button injection point on Windows is `SendInputOutput::button`,
and its `heldButtons_` already records **successful OS-visible transitions
only** (its header, and SPEC §8.3). The journal shadows exactly that state —
nothing upstream.

Because the dispatcher emits a `Button` effect only on the 0↔1 edge of *click
refcount OR drag lock* (`Dispatcher::syncButton`), the journal never needs to
model refcounts, drag locks, or which key holds a button. Two keys on one button,
a click plus a drag lock, or a drag lock alone all reduce to one OS-visible
obligation per button, which is what the guardian must lift.

**Interface (proposed):** an `ObligationJournal` passed to the output backend at
construction; a null journal when Unprotected. No IPC, no locks, no allocation on
the injection path.

### 5.2 Slot states (one 32-bit word per button)

| Value | State | Written |
|---|---|---|
| 0 | `UP` | initial; after a successful Up; after a failed Down |
| 1 | `DOWN_INTENT` | immediately **before** `SendInput(down)` |
| 2 | `DOWN` | immediately **after** a successful Down; after a failed Up |
| 3 | `UP_INTENT` | immediately **before** `SendInput(up)` |

Each slot also carries a monotonically increasing 32-bit `generation`,
incremented on every transition (diagnostics and fault-injection tests only).

```
button(b, down=true):
    slot[b] = DOWN_INTENT
    ok = SendInput(down)
    slot[b] = ok ? DOWN : UP          # heldButtons_[b] = ok (unchanged rule)

button(b, down=false):
    slot[b] = UP_INTENT
    ok = SendInput(up)
    slot[b] = ok ? UP : DOWN          # a failed Up does not clear (unchanged rule)
```

The rule is the one E6 proved: **intent is visible before the injection;
the result is recorded after it.**

### 5.3 Atomicity and memory ordering

- **Single writer.** Only the core thread writes slots (it is the only caller of
  `SendInputOutput::button`). No CAS is required.
- **Word-sized stores.** Each transition is one store of a 4-byte aligned word
  (`InterlockedExchange` or `std::atomic<std::uint32_t>` placed in the section).
  A reader can observe the old or the new value, never a mix.
- **No concurrent reader in the recovery path.** The guardian reads slots only
  **after** the core-process handle is signalled, i.e. after every core thread
  has stopped. Recovery never races the writer.
- **Durability of the last store.** The section is shared physical memory; a
  store retired by the CPU is visible to the other process without flushing.
  `TerminateProcess` stops threads at instruction boundaries, so a retired store
  cannot be lost.
- **No dead-store elimination.** The intent store is followed by a call into
  `SendInput`, an external function, so a compiler cannot prove it unobserved;
  using the atomic/`Interlocked` store makes that explicit rather than relying
  on the optimiser's reasoning.
- **Header** is written once, before the guardian is spawned; afterwards only
  `guardian_pid`/`ATTACHED` (guardian, once) and `CLEAN_EXIT` (core, once) change.

---

## 6. Conservative recovery

### 6.1 Rule

After the core-process handle is signalled, for each button `b`:

```
if slot[b] == UP:                   do nothing
elif GetAsyncKeyState(vk(b)) is up: do nothing        # E5: never send a stray Up
else:                               SendInput(up(b)); verify; retry within window
```

"Possibly held" (`DOWN_INTENT`, `DOWN`, `UP_INTENT`) is necessary but **not
sufficient**; Windows must also report the button down.

**Why check Windows first (E5).** A stray or duplicate left-up was ignored by
standard controls — no click, no selection change — but the application **did
receive** `WM_LBUTTONUP`. Custom mouse-up handlers (drawing tools, games,
drag-end logic, click-on-release controls) could react. E5 is not proof that
stray Ups are harmless; it is evidence that avoiding them is worth a check. The
check eliminates the stray Up in the two cases where the journal is ahead of
Windows (death after `DOWN_INTENT` before injecting; death after a successful Up
before `UP` is recorded).

### 6.2 The unavoidable tradeoff — stated explicitly

`GetAsyncKeyState` reports the combined state of the button; it cannot tell a
synthesized hold from a **physical** one. If the user happens to be physically
holding the same button at the instant the core dies and the journal says
possibly-held, the guardian releases it. The user sees their physical press end
early.

**Chosen:** accept it. P7 outranks fidelity to the gesture (the same trade SPEC
§7.2 makes for drag lock on layer exit). A physical hold interrupted at the moment
of a crash is recoverable by pressing again; a synthesized hold left down is the
failure P7 exists to prevent. The window is narrow: the user must be pressing that
exact button during the milliseconds of recovery.

### 6.3 Verification and bounded retry

`SendInput` does not report UIPI blocking, and the input desktop may be switched
(secure desktop, UAC). So after each Up the guardian re-reads
`GetAsyncKeyState`. If still down, retry with short backoff until the window
(proposed 1 s, §3.5) expires; then log the failure (§8) and exit non-zero.

**To validate:** the `GetAsyncKeyState` documentation says it reads *physical*
mouse buttons, not logical ones under `SwapMouseButton`; the core injects with
`MOUSEEVENTF_LEFTDOWN` etc. The vk↔flag mapping must be confirmed with swapped
buttons before relying on it.

---

## 7. Death at every transition

| Core dies… | Slot | Windows | Guardian | Result |
|---|---|---|---|---|
| with nothing held | `UP` | up | nothing | correct |
| after `DOWN_INTENT`, before `SendInput(down)` | 1 | up | skip (Windows up) | correct, no stray Up (E6 T3b) |
| after a successful Down, before `DOWN` recorded | 1 | down | Up | released (E6 T3a) |
| holding (`DOWN`) — click, drag lock, or both | 2 | down | Up | released (E6 T1/T2) |
| after `UP_INTENT`, before `SendInput(up)` | 3 | down | Up | released |
| after a successful Up, before `UP` recorded | 3 | up | skip | correct, no stray Up |
| after a **failed** Down, before `UP` recorded | 1 | up | skip | correct |
| after a **failed** Up (slot back to `DOWN`) | 2 | down | Up (retried, verified) | released, or logged failure if Windows blocks it too |
| with several buttons held | each slot independently | per button | per button | each released exactly once |
| mid double-click (between the two pairs) | `UP` | up | nothing | the second pair simply never happens — incomplete gesture, no obligation |
| mid double-click (inside a pair) | per the rows above | | | per the rows above |
| after a partial clean unwind (some Ups done) | remaining slots only | | lift the remainder | only what is still held |
| while a physical press of the same button is in progress | 1–3 | down | Up | physical hold released (§6.2 tradeoff) |

---

## 8. Failure visibility (P6)

The core must never claim crash-safe release it does not have.

**Protection state** (new, dynamic): `protected` | `unprotected{reason}` with
reasons `spawn_failed`, `breakaway_denied`, `attach_timeout`,
`guardian_identity_mismatch`, `guardian_lost`, `restart_exhausted`.

- **Startup:** reported in `hello.limitations` when Unprotected ("synthesized
  mouse buttons held when this core is forcibly terminated will stay held until
  a physical click").
- **Runtime:** a diagnostic event on every transition, and a `get_state` field,
  because `hello` is a startup snapshot and the guardian can die later.
- **Guardian-side failures after core death** (identity mismatch, Up not
  confirmed): the core is gone, so the guardian appends one line to the log root
  (SPEC §3.1) and exits with a distinct code. Nothing else — no dialogs, no
  telemetry.

**PRODUCT DECISION REQUIRED — behaviour while Unprotected.**

| Option | Behaviour | Cost |
|---|---|---|
| A. Warn only | Everything works; the limitation is visible | A crash can strand a button, exactly as today |
| B. Gate the unbounded hold | `button.drag_lock` refuses to engage (diagnostic) while Unprotected; momentary clicks still work | A user loses drag lock when protection is lost; held click keys still strand on crash, but only while a key is held |
| C. Gate all synthesized holds | Click keys emit down+up immediately (no hold); drag lock refused | Breaks click-and-hold dragging whenever protection is lost |

Leaning **B**: drag lock is the only obligation that persists with no key held,
so it is where a crash strands a button for longest. But this changes feature
behaviour on a failure path and is a product call, not an engineering one.

---

## 9. Privilege and security

- **Binary:** `keygnosys-guardian.exe`, small, no dependencies beyond
  `kernel32`/`user32`.
- **Capabilities:** read one inherited section; wait on one inherited process
  handle; write `guardian_pid`/`ATTACHED` once; call `GetAsyncKeyState`; send
  **mouse-button Up only**, for L/R/M. It cannot send a Down, a key, a move or a
  scroll — none of that code exists in it.
- **No** keyboard or mouse hook, configuration parsing, IPC server, network,
  window, registry writes, autostart, or elevation. Same user, same integrity
  level as the core (a guardian at higher integrity would also be UIPI-inconsistent
  with the core's own injections).
- **Least access to the journal itself:** the header and the slots occupy
  separate pages. The guardian maps the header page writable (for
  `guardian_pid`/`ATTACHED`) and the slot page **read-only**, so it cannot
  create or alter an obligation record.
- **Journal access:** unnamed section, reachable only via inherited or duplicated
  handles. A same-user process able to duplicate the core's handles could already
  call `SendInput` itself, so the journal grants nothing new. If an unnamed section
  proves impractical, the fallback is a `Local\` name containing the nonce with a
  DACL granting only the user's SID.
- **Inputs it trusts:** its command line (handle values, nonce) and the header.
  Everything is validated (§4); on doubt it does nothing.

---

## 10. OPEN DEPENDENCY: O-4 / client disconnect semantics

SPEC.md:1179-1181 says the P7 invariant applies to "client disconnects" and that
every such exit path MUST run `releaseAll()`. The implementation does not
(`Server::dropClosed`), and row 10.8 observed held obligations surviving a
disconnect. **O-4 is unresolved, and this design takes no position on it.**

Specifically, this design does **not** decide that an ordinary IPC client
disconnect:

- releases global synthetic obligations,
- terminates the core,
- terminates, detaches or re-parents the guardian, or
- changes who owns an obligation.

**Provisional rule:** the guardian's only trigger is loss of the guarded core
instance (§1), because that is what E6 proved. Nothing in §3–§9 depends on client
lifetime. Before lifecycle semantics are finalised, O-4 must answer:

1. What "client" meant when SPEC §6.3 was written (overlay? any IPC peer? the
   launcher?).
2. Whether any IPC peer should have ownership over global synthetic input.
3. If a disconnect *should* release, whether that is a core action (core alive,
   `releaseAll()`), which needs no guardian involvement at all.

If O-4 concludes that some client is an *owner* whose death must release input,
that is a core-side rule. The guardian's scope stays core death.

---

## 11. Launcher-contract impact (identified, not made)

The launcher is unimplemented (M4). Required amendments to LAUNCHING.md when the
guardian lands:

| Section | Change |
|---|---|
| §1.3 | The contract stays launcher-only; note that the core may start a helper process of its own. |
| §4.1 order of operations | Unchanged: the core spawns its guardian inside "start the core". Readiness (§4.2) still means `hello`; `hello` then also reports protection state. |
| §4.3 foreground | The launcher stops the core it started; it **MUST NOT** stop the guardian, which exits after the core. |
| §4.3 `--background` | Unchanged; the guardian is detached independently. |
| §5 instance detection | Unchanged, and reinforced: the guardian is never an instance signal. |
| §8.2 diagnostics | Report protection state from `hello` / `get_state`. |
| §9 autostart | Unchanged: the registered command starts the core, which starts its guardian. |
| §12 not specified | Add: guardian executable discovery beyond "same directory as the core". |

**SPEC changes required at implementation time (not made now):** P7 text only if
the review so decides; §8.3 output backend (journal); §5 `hello`/`get_state`
protection fields and diagnostic codes; §11 diagnostics; a new component section
for the guardian.

---

## 12. Production validation strategy

Evidence must include **Windows button state** (probe-validated
`GetAsyncKeyState` samples), not observer accounting alone. Observers are
validated against physical known-positives before any negative is believed.

### 12.1 Automated (ctest, all platforms where applicable)

- Journal state machine: every `button()` path writes intent-before and
  result-after; failed Down → `UP`; failed Up → `DOWN`.
- Recovery decision as a pure function `(slots, windowsState) → releases`,
  table-tested against every row of §7.
- Header validation: magic, version, nonce, pid/creation-time mismatch each
  rejected.
- Protection-state transitions and backoff schedule.
- Each test confirmed to fail against a deliberately broken implementation
  (the O-3 practice).

### 12.2 Windows integration (manual smoke, not ctest — needs a desktop)

A test core binary with **fault injection**: terminate itself (`TerminateProcess`
on self) at a chosen transition, so "death at every write-ahead transition" is
deterministic rather than timed. Always over a script-owned window, as E5/E6.

### 12.3 Live matrix (operator + validated observers)

| Test | Pass |
|---|---|
| clean exit (every close path: Ctrl+C, console close, WT close) | no guardian Up; Windows up; guardian exits 0 |
| `taskkill /F` during drag lock | Windows up within the retry window, no rescue input |
| `taskkill /T /F` (via the intermediate) | guardian survives, releases |
| Task Manager End task, **detached** core | released |
| Task Manager End task, console core | O-3 path releases; guardian finds nothing |
| guardian killed first | core reports Unprotected (diagnostic + `get_state`); behaviour per §8 decision |
| guardian restart | reattaches to the same journal; Protected again |
| stale identity (restarted core) / wrong identity (bad nonce, bad pid) | guardian refuses; nothing injected |
| multiple buttons held (left drag lock + right click held) | each released exactly once |
| death at each write-ahead transition (§12.2 binary) | per §7 |
| partial clean unwind, then kill | only the remainder released |
| guardian unavailable at startup (breakaway denied / binary missing) | Unprotected reported in `hello`; behaviour per §8 decision |
| core launched from Windows Terminal, an IDE, and detached | protection state as expected in each |
| swapped mouse buttons | the correct button is checked and lifted |

---

## 13. Open questions for review

1. **Product decision (§8):** behaviour while Unprotected — A, B or C.
2. **O-4 (§10):** must be resolved before §3 lifecycle semantics are final.
3. Is core-spawned the right owner, versus launcher-spawned? (§3.1 argues core.)
4. Retry window length and restart backoff numbers (§3.4, §3.5, §6.3).
5. Whether the intermediate process is acceptable, or a different
   out-of-tree spawn mechanism is preferred (§3.3).
6. Job-object breakaway behaviour under the hosts users actually run — needs a
   measurement before implementation.
7. Swapped-button mapping (§6.3) — needs a measurement before implementation.

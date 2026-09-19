# P7 under termination — design options for row 10.2 and finding O-3

**Status:** options report for decision. No code, SPEC or P7 wording has been
changed. Scope is deliberately limited to row 10.2 (`taskkill /F`) and finding
O-3 (console close / logoff / shutdown). O-1, O-2 and O-4 are out of scope.

**Evidence labels used throughout**

- **[OBSERVED]** — measured live on this machine (Windows 11 26200, `c1e6794`),
  recorded in `docs/manual-test-logs/2026-08-30-m3-windows.md`.
- **[CODE]** — established by reading the current source.
- **[DOC]** — stated in Microsoft documentation (links at the end).
- **[INFERRED]** — follows from the above but has not been live-tested.

---

## 0. The obligation being protected

P7 (OUTLINE): *"Any suppressed or synthesized key press has a guaranteed
matching release, on every exit path — including crash, layer change, and
config reload."*

Not every synthetic obligation is equally exposed. The current code produces
three kinds [CODE]:

| Kind | Source | What discharges it if the core dies |
|---|---|---|
| Natively forwarded press | `translateDecisions` passthrough (`hookchannel.cpp:76-81`) | The user's physical release, which reaches Windows once the hook is gone (threading design §3) |
| Synthesized **key** down | `WorkItem::SendKey` → `SendInputOutput::sendKey` (`core.cpp:352-353`) — grace-window replays and re-synthesised forwards | Also the user's physical release of the same key: Windows accepts a physical up against an injected down [OBSERVED: `EXCLUDED-attempt1-10.2` — injected `D`/`J` downs, core killed, physical ups, Windows state clean] |
| Synthesized **mouse button** down | `button.click` (4 keys), `button.drag_lock`, `button.double_click` pairs (`core.cpp:394, 629-645`) | **Nothing.** No physical key maps to it. It stays down until a physical mouse click [OBSERVED: 10.2, 10.1b] |

So the exposed class is **synthesized mouse buttons**, and the worst member is
`button.drag_lock`: it holds the button with no key held at all. Double-click
pairs are sent back-to-back, so their exposure window is the gap between two
`SendInput` calls.

---

## 1. Current guarantee boundary

### 1.1 Clean Ctrl+C / Ctrl+Break shutdown

- **[CODE]** `onConsoleEvent` (`main.cpp:35-47`) sets `stopRequested` and
  returns `TRUE`. `Core::run` (`core.cpp:1191-1204`) notices within one tick
  (`kTickInterval` = 16.667 ms, `motion.hpp:34`) and calls `Core::stop`
  (`core.cpp:1208-1254`): owner `ReleaseAll` (bounded by `controlTimeout` =
  250 ms), input backend stopped, owner `Stop` (bounded, 250 ms), work drained,
  dispatcher `releaseAll`, output backend `releaseAll`, server shutdown.
  `main` then returns 0.
- **[DOC]** For `CTRL_C_EVENT` / `CTRL_BREAK_EVENT` returning `TRUE` ends
  processing of the signal; there is **no timeout**.
- **[OBSERVED]** Row 10.1: button released 10–25 ms after `CTRL_C_EVENT`;
  `shutdown` event broadcast; exit code 0.

**Guaranteed today**, provided the core is the only process on its console —
[OBSERVED] a PowerShell wrapper on the same console terminated the core
mid-unwind (exit −1).

### 1.2 Disable / `release_all` / reload / `set_bindings`

- **[CODE]** Each runs `releaseEverything()` (`core.cpp:868-888`) on the core
  thread while the process is alive.
- **[OBSERVED]** Rows 10.3, 10.5, 10.6, 10.7: button up within 19–44 ms of the
  command, drift stopped, nothing held.

**Guaranteed today.**

### 1.3 `taskkill /F` / `TerminateProcess` / crash

- **[DOC]** `TerminateProcess` runs no user-mode code in the target.
- **[OBSERVED]** Row 10.2: no injected up; Windows held the left button down
  after every physical key was released, for ≈ 25.5 s until a physical click.
- **[INFERRED]** An unhandled crash (access violation, `std::terminate`,
  stack overflow) behaves the same: no unwind runs.
- **[INFERRED]** Task Manager **End task** on a *windowless* core — the
  `--background` launch of LAUNCHING §4.3 — terminates the process, so it is
  this case too. Not live-tested.

**Cannot be guaranteed by any code inside the core.** This is an architectural
boundary, not a bug in the unwind.

### 1.4 Console close — `CTRL_CLOSE_EVENT`

- **[DOC]** Sent to every process attached to a console when the user closes
  the console window *or* uses Task Manager **End task** on it. "Return TRUE. In
  this case, no other handler functions are called and the system terminates
  the process." Timeout: `SPI_GETHUNGAPPTIMEOUT`, default 5000 ms. The handler
  runs "on a new thread in the process".
- **[CODE]** The handler returns `TRUE` immediately after setting a flag. The
  core thread is usually inside `sleep_until`; Windows terminates the process
  before the next tick reaches `Core::stop`.
- **[OBSERVED]** Row 10.1b: exit code `0xC000013A`, **no `shutdown` event**, no
  injected up, button held ≈ 10.8 s until a physical click. The same stop
  request under `CTRL_C_EVENT` completes in ~15–25 ms.

**Not guaranteed today — but for a reason that is inside the core's control.**
The unwind exists and is fast; the handler simply returns before it has run.
See §4.

### 1.5 Logoff / shutdown

- **[DOC]** "If a console application loads the gdi32.dll or user32.dll
  library, the HandlerRoutine … does not get called for the CTRL_LOGOFF_EVENT
  and CTRL_SHUTDOWN_EVENT events." Such processes must create a **hidden
  top-level window** and handle `WM_QUERYENDSESSION` / `WM_ENDSESSION`. The
  HandlerRoutine page adds that interactive applications are terminated at
  logoff before the console signal is even sent.
- **[CODE]** The core loads `user32` (hooks, `SendInput`) and creates **no
  window** — `CreateWindow` / `WM_ENDSESSION` do not occur in `core/`. Its
  `CTRL_LOGOFF_EVENT` / `CTRL_SHUTDOWN_EVENT` cases in `main.cpp` are therefore
  dead code.
- **[INFERRED]** At logoff/shutdown the core receives no notification and is
  terminated by the session teardown. Because the session's input desktop is
  destroyed with it, a stranded button has no lasting effect *after* logoff;
  the exposure is the interval between the core's termination and the session
  ending, and the case where a logoff is cancelled after the core was already
  ended. Not live-tested.

**Not guaranteed today; low practical impact; fixable in-process** (§4.4).

### 1.6 Summary

| Exit path | Release today | Evidence | Fixable in-process? |
|---|---|---|---|
| Ctrl+C / Ctrl+Break | yes | OBSERVED | — |
| disable, `release_all`, reload, `set_bindings` | yes | OBSERVED | — |
| console close / Task Manager End task on a console core | **no** | OBSERVED | **yes** — §4 |
| logoff / shutdown | **no** (no notification received) | DOC + CODE; effect INFERRED | **yes** — hidden window, §4.4 |
| `TerminateProcess`, `taskkill /F`, crash, End task on a windowless core | **no** | OBSERVED (taskkill); crash / windowless INFERRED | **no** — needs something outside the process |

A consequence worth stating plainly: the matrix's primary emergency exit
(`Ctrl+Alt+Del` → Task Manager → **End task**) lands in the stranding rows —
`CTRL_CLOSE_EVENT` for a console core [OBSERVED equivalent: 10.1b], termination
for a windowless one [INFERRED]. `panic.ps1` is safe only because it releases
buttons *after* killing the core.

---

## 2. Design options

### A. Narrow P7

**Wording that would be needed** (illustrative, not proposed text):

> P7 guarantees a matching release on every exit path on which the core is able
> to execute its release: layer changes, disable, reload, `set_bindings`,
> `release_all`, console control events (Ctrl+C, Ctrl+Break, console close),
> and session end. A process ended without notice — `TerminateProcess`, a
> crash, a forced kill — cannot run code, and synthesized mouse buttons held at
> that moment are not released. This is reported in `hello.limitations`.

**Classification of the observed cases.** Console close, logoff and shutdown
are *announced* terminations: Windows gives the process a callback and a time
budget (5 s for close). They belong on the guaranteed side. Only
`TerminateProcess` and crashes are genuinely forced. Narrowing P7 to cover
10.2 does **not** excuse O-3.

**User-facing consequence.** A held left button after the core dies is not a
cosmetic defect. The next mouse movement drags whatever is under the pointer —
text selection, a window, a file into another folder — until the user happens
to click. It presents as "Windows is broken", not as a KeyGnosys fault, which is
exactly the failure class section 10 exists to catch. Drag lock makes it likely
rather than rare: the button is held with no key down, for as long as the user
likes.

**Acceptability.** Narrowing alone turns a principle into a description of the
current implementation. It is compatible with P6 (it fails visibly, via
`hello.limitations`) but it quietly abandons OUTLINE's explicit "including
crash". Acceptable only as a *documented, tracked limitation*, not as a
redefinition of what P7 means.

### B. External guardian

A small separate process whose only job is: *if the core dies, lift what it
left down.*

**Who owns the authoritative state.** The core stays authoritative. The
guardian holds a *write-ahead mirror*: a small shared-memory section with one
slot per mouse button (and, optionally, per synthesized key), each slot a state
word. No IPC round-trip on the hot path.

**How it learns every Down and Up — the ordering that matters.**

1. Before `SendInput(button down)`: set slot = `DOWN_INTENDED`.
2. After `SendInput` returns success: set slot = `DOWN`.
3. Before `SendInput(button up)`: set slot = `UP_INTENDED`.
4. After success: set slot = `UP`.

The guardian treats any slot not equal to `UP` as a possible obligation.
Writing intent *before* injecting closes the dangerous race — core dies after
the OS accepted the Down but before recording it — at the cost of a possible
spurious Up (dies after writing intent, before injecting). A mouse-up with no
matching down, or a duplicate up, is harmless to Windows [INFERRED — to be
confirmed, E6]. Slot writes are single-word stores on the core thread, which
already owns `SendInput`; the hook thread is untouched.

**How it learns the core died.** It holds a process handle and waits on it
(`WaitForSingleObject`). That fires for `TerminateProcess`, crashes and
Task Manager, without any cooperation from the core.

**Release policy on death.** For each slot not `UP`: send the up. Optionally
consult `GetAsyncKeyState` first and skip buttons that already read up. That
check cannot distinguish a user's *physical* hold from the stranded synthetic
one; sending the up anyway releases a physical hold the user is making at that
instant — the same "P7 outranks fidelity" trade SPEC §7.2 already makes for drag
lock.

**Duplicate releases.** A clean shutdown sets every slot to `UP` before exit, so
the guardian sends nothing. A guardian that fires after a partial unwind may
duplicate an up; see above.

**Guardian crash.** The core must also watch the guardian's handle. On its
death the core reports a P6 diagnostic ("crash-release protection unavailable")
and may respawn it. Nothing is stranded by the guardian's death alone; only a
double fault (both processes lost) strands.

**Lifecycle.**
- It must *not* share the core's console, or `CTRL_CLOSE_EVENT` ends both
  (`DETACHED_PROCESS` / `CREATE_NO_WINDOW`).
- It must survive `taskkill /T` on the core's process tree, so it should not be
  a plain child: start it from the launcher, or through an intermediate that
  exits, and break away from any job.
- It exits after releasing, or when the core exits cleanly and says so.
- It must bind to one core instance, keyed by PID plus start time, so a restarted
  core does not inherit a stale mirror.

**Keeping it from becoming a second privileged monolith.**
- It installs no hooks, runs no IPC server and reads no configuration.
- Its only capability is "send the up for a slot currently marked held" — it
  cannot be driven to inject arbitrary input.
- It runs at the core's integrity level; it must not run elevated.
- The shared-memory section is ACL'd to the user's SID.
- Size target: a few hundred lines.

**Does it cover the observed cases?**
- `TerminateProcess`, crash, End task on either launch mode: **yes** [INFERRED — E6].
- Console close: yes, *if* detached from the console. But §4 fixes this
  in-process more simply.
- Logoff/shutdown: the guardian is ended too, but the session teardown discards
  input state anyway [INFERRED].

**Cost.** A second executable; a launcher-contract change (LAUNCHING §4 —
ownership, `--background`); tests that kill the core mid-drag; a Linux (M4)
parity question [unknown: whether X11 releases an XTest client's held buttons
on disconnect]; and one more thing that can go wrong at startup.

### C. Move synthetic output out of the core (output broker)

A dedicated process owns `SendInput` and every synthetic obligation; the core
becomes a client that asks it to press and release.

**Is the invariant cleaner?** Conceptually yes: *the process that injects is the
process that releases, and it is not the process most likely to crash.* The
broker is simpler than the core, but it is still a process: `taskkill /F` on the
broker strands exactly as 10.2 did. C **moves** the single point of failure; it
does not remove it.

**Core dies, broker survives.** The broker sees the connection drop and releases
everything it holds. Strong.

**Broker dies.** The core still holds its logical state but can no longer lift
anything. Strands, unless the core also keeps a fallback `SendInput` path — at
which point there are two injectors to reconcile.

**Sequencing.** Ownership must follow *successful OS-visible transitions*.
Every press needs an acknowledgement carrying the `SendInput` result, and
`release_all`'s `ok:true` (SPEC §5.4 — "applied") must wait for the broker's
ack. Grace-window replay order, the double-click interval (≈ 90 ms, D-2) and
drag timing all gain a cross-process hop with its own latency and failure
modes.

**Impact on existing invariants.** The threading design's work ring,
the release-capacity proof (`kMaxReleaseWork`), `SendInputOutput`'s held-state
tracking and the shutdown fallback in `Core::stop` all assume in-process
injection. C re-opens most of the M3 threading design.

**Scope and risk.** Highest of the four, for protection B provides at far lower
cost. Not a candidate for M3.

### D. Windows-native release primitive

**None was found.**
- **[OBSERVED]** 10.2 shows that Windows does not tie the async button state to
  the injecting process: the button stayed down with the process gone.
- **[DOC]** `SendInput` is documented not to reset or own keyboard state.
- **[DOC]** `CreateSyntheticPointerDevice` accepts only `PT_TOUCH` and `PT_PEN`,
  not mouse, and documents no release-on-exit behaviour.
- **[DOC]** `Windows.UI.Input.Preview.Injection.InputInjector` requires the
  restricted `inputInjectionBrokered` capability (a packaged app). The only
  documented teardown call, `UninitializeTouchInjection`, is a touch API;
  nothing documents release of injected mouse buttons on exit.
- Job objects, process handles and low-level hooks are all released on
  termination, but none of them owns input state.

This is "not found in the documentation", not proof that no mechanism exists.
The practical conclusion is the same: nothing documented can be relied on.

---

## 3. Recommendation

**Split the two problems, because they differ in kind.**

**O-3 — architectural repair, in-process, small.** Console close is an
announced termination with a 5-second budget. The core already has a correct
unwind that completes in ~25 ms; it loses only because the handler returns
first. Making the handler wait for the unwind (§4) restores P7 on that path
without new processes, new privilege or threading churn. Adding a hidden window
for `WM_ENDSESSION` extends it to logoff and shutdown. This is fixing a defect,
not changing the architecture. It should wait only on experiment E1.

**10.2 — defer, as an explicit known limitation, with B as the intended
remedy. Do not narrow P7.**

- **P7 unchanged.** It stays as the principle, and the failure is recorded as a
  violation: `hello.limitations` gains an entry saying a forcibly terminated
  core can leave a synthesized mouse button held until the next physical click;
  the matrix and README recovery guidance say so; row 10.2 stays FAIL. This is
  P6 — fail visibly — without pretending the guarantee holds.
- **Why not A.** Rewording P7 to exclude crashes would make the principle
  describe the implementation, and the consequence (an invisible held drag) is
  severe enough that OUTLINE called out crashes by name.
- **Why not C.** It moves the failure point, reopens the threading design, and
  adds latency to every injected event, for protection B provides more cheaply.
- **Why B, and why not now.** It is the only option that covers
  `TerminateProcess`, at minimal privilege and without touching the hot path.
  But it adds a process and a launcher-contract change, and one of its core
  claims — that a spurious or duplicate up is harmless — is still unmeasured.
  Measure first (E6), then scope it as its own milestone item.

**On M3 promotion.** With O-3 repaired and 10.2 recorded as a documented,
tracked violation, M3 could be promoted *with that limitation stated*. Whether a
known P7 violation should block promotion is a product decision for the
operator; this report does not presume it.

---

## 4. O-3 specifics

### 4.1 Is the current flow "cleanup too late"?

Yes.
- **[CODE]** The handler sets an atomic and returns `TRUE` at once
  (`main.cpp:42-43`).
- **[DOC]** On `CTRL_CLOSE_EVENT`, returning `TRUE` hands termination to the
  system.
- **[OBSERVED]** 10.1b produced no `shutdown` event — that broadcast is the last
  step of `Core::stop` — while 10.1 under `CTRL_C_EVENT` did.

The unwind is not failing; it never starts.

### 4.2 Can the handler release synchronously before returning?

Two designs.

**(i) Handler waits for the core thread's unwind — preferred.** On
`CTRL_CLOSE_EVENT` (and, harmlessly, `CTRL_C` / `CTRL_BREAK`), the handler sets
`stopRequested`, then blocks on an "unwind complete" event that `Core::stop`
signals after the output backend's `releaseAll()`. The wait is bounded below
the close timeout, e.g. `min(SPI_GETHUNGAPPTIMEOUT − margin, ~3 s)`. All release
work stays on the core thread, which owns `SendInputOutput` and the dispatcher,
so there is no new cross-thread access.
- Budget [CODE]: at most one tick (16.7 ms), plus two bounded control waits
  (≤ 500 ms), plus a handful of `SendInput` calls. [OBSERVED]: the whole unwind
  took ~15–25 ms under `CTRL_C_EVENT`.
- The worst case sits an order of magnitude inside the 5 s default.

**(ii) The handler performs the release itself.** It would touch
`SendInputOutput`'s held-state arrays from the system-created thread while the
core thread may be mid-`SendInput`. That is a data race; it needs a lock-free
mirror of held buttons, or a lock the core thread takes around every injection.
It is only worth doing as a *fallback* if (i) times out, and the bounded wait
makes that very unlikely.

### 4.3 Safe and reliable within the time limit?

**[INFERRED]** Yes, with two conditions:
- **The wait must be bounded.** The handler must return before
  `SPI_GETHUNGAPPTIMEOUT`, which is a user-adjustable parameter; read it at
  runtime and keep a margin, instead of hard-coding 5 s.
- **Nothing in the unwind may call console functions** ([DOC] they "may not work
  reliably" during these signals). `Core::stop` writes nothing to the console;
  `main.cpp` prints only at startup [CODE].

**[DOC]** caveat: the handler "will be terminated by another thread" if `main`
returns and the process exits first. That is the desired outcome here — it
means the unwind finished.

### 4.4 Does this fix close, logoff and shutdown — and not `TerminateProcess`?

- **Console close:** fixed by 4.2(i) [INFERRED; confirm with E1].
- **Logoff / shutdown:** **not** fixed by the handler alone. Because the core
  loads `user32`, those signals never reach it [DOC]. It needs a hidden
  top-level window — not `HWND_MESSAGE`, since message-only windows do not
  receive session broadcasts — whose `WM_ENDSESSION` handler runs the same
  synchronous unwind [DOC; INFERRED effectiveness].
- **`TerminateProcess`:** **not** fixed, and not fixable in-process (§1.3). It
  stays separate: option B, or the documented limitation.

---

## 5. Evidence needed before deciding

Narrow experiments, in priority order. None changes committed M3 behaviour;
code experiments belong on a throwaway branch.

| # | Question | Experiment | Distinguishes |
|---|---|---|---|
| **E1** | Does waiting in the handler fix console close? | Throwaway build where `onConsoleEvent` waits (bounded) for an "unwind complete" event set at the end of `Core::stop`. Re-run 10.1b exactly: `D`+`F`+`H` hold, `delayed.ps1 -Do close-core-console`. Expect: `shutdown` event, injected up, Windows button up, exit code still `0xC000013A`, and the unwind's duration measured. | O-3 in-process repair vs. needing B |
| **E2** | Does Windows Terminal behave like conhost on tab/window close? | With nothing held: core alone in a WT tab, close the tab; record the exit code and whether a `shutdown` event appears (before and after the E1 build). | Whether the E1 fix covers the terminal users actually run |
| **E3** | What does Task Manager **End task** actually do to each launch mode? | With nothing held: (a) console core → End task; (b) windowless/detached core → End task. Record exit codes (`0xC000013A` vs `1`/other) and the `shutdown` event. | Which stranding row the documented emergency exit lands in |
| **E4** | Does `Ctrl+Alt+Del` (the secure-desktop switch) clear a stranded synthetic button? | Reproduce the 10.2 strand, then `Ctrl+Alt+Del` → Cancel, without clicking; read `GetAsyncKeyState`. | Whether the emergency exit also recovers, or only escapes |
| **E5** | Are spurious and duplicate mouse-ups harmless? | With no core: inject (a) a left-up with no preceding down, (b) two ups after one down, over a text field, Explorer and a game-like app; observe any side effect. | A core assumption of B's write-ahead ordering |
| **E6** | Can a guardian actually release after `TerminateProcess`? | Throwaway spike outside the repo: a process holding the core's handle plus a shared-memory slot word; `taskkill /F`, `taskkill /T`, and Task Manager End task during a drag lock; measure time to release. | Whether B delivers what it promises, before scoping it |
| E7 | (optional, disruptive) Logoff/shutdown | Only if E1 succeeds and a hidden-window prototype exists: sign out during a drag lock in a disposable session; check the release is logged. | Whether the WM_ENDSESSION path is worth building |

E1 and E3 decide the O-3 repair. E5 and E6 decide whether B is real enough to
schedule. E4 affects recovery guidance under either outcome.

---

## 6. Experiment results — 2026-09-14

Approved: E1, E2, E3, E5, E6. E4 and E7 deferred. Captures are in the
git-ignored `.superpowers/sdd/2026-09-04-m3-completion/captures/` (`e1`–`e6`).
No SPEC, P7 wording or production behaviour was changed. The E1 build lives only
on branch `experiment/e1-close-wait` (`a377617`, `main.cpp` only), in a separate
worktree.

### E1 — bounded wait in the close handler: **O-3 repaired in-process**

The E1 build's handler sets the existing stop request, then on
`CTRL_CLOSE_EVENT` waits — bounded to `min(SPI_GETHUNGAPPTIMEOUT − 1 s, 4 s)` —
for an event `main` signals after `Core::run()` returns, i.e. after `Core::stop`
has released everything. The handler releases nothing itself. Its `ctest
--preset default` run passed 16/16.

The held test re-ran the 10.1b scenario (`D`+`F`+`H`, observers validated,
`delayed.ps1 -Do close-core-console`), timed from the helper's FIRE on one QPC
clock:

| Step | Time |
|---|---|
| `WM_CLOSE` posted (the helper's child process startup) | +332.5 ms |
| handler receives `CTRL_CLOSE_EVENT`, starts waiting | +340.0 ms |
| `Core::run()` returns — `Core::stop` complete | +345.1 ms |
| handler wakes (**waited 4.4 ms**) and returns | +345.3 ms |
| first Windows sample with the button **up** | +356.5 ms |
| mouse observer's injected up | ≈ +360.6 ms |

The `shutdown` event appeared, the core exited with **0**, and no recovery click
was needed — the operator reported the drift stopping and no button held. On
the production build the same scenario gave `0xC000013A`, no `shutdown` event,
and a button held for ≈ 10.8 s. An idle run (nothing held) waited 13.2 ms.

The exit code changes from `0xC000013A` to 0 because `main` returns — and the
process exits normally — before the handler returns control to Windows.

### E2 — Windows Terminal tab close (nothing held)

Core launched directly as a tab's process (`wt -w new new-tab`, no shell); the
operator closed only that tab.

| Build | Handler | `shutdown` event | Exit code |
|---|---|---|---|
| production | returns immediately | no | `0xC000013A` |
| E1 | received type 2 (`CTRL_CLOSE_EVENT`), wait SIGNALED after 11.2 ms | yes | 0 |

A WT tab close is the same announced close, and E1 covers it. The held-key
variant was not run in WT (the helper's close action requires a classic console
window); the release itself was shown in conhost.

### E3 — Task Manager **End task** (nothing held, production build)

| Launch mode | Exit code | `shutdown` event | Category |
|---|---|---|---|
| console-attached (alone in conhost) | `0xC000013A` | no | announced close — O-3; E1 applies |
| detached (`DETACHED_PROCESS`: no console, no window, parent exited) | **1** | no | `TerminateProcess` — the 10.2 class |

The documented emergency exit is therefore repairable for a console core and a
hard kill for a background core.

### E5 — stray and duplicate left-ups (script-owned window only)

| Injection | Received by the window | Effect |
|---|---|---|
| stray up over a button | `MouseUp` | no `Click` |
| stray up over a text box with a selection | `MouseUp` | selection unchanged |
| stray up over a blank panel | `MouseUp` | no `Click` |
| down, up, **duplicate** up over a button | two `MouseUp`s | exactly **one** `Click` |
| duplicate up after a click in the text box | `MouseUp` | selection unchanged by the duplicate |
| normal click afterwards | — | one `Click`; state not corrupted |

Windows' button state stayed up throughout, and the mouse observer saw every
injection. **Limitation:** the application *does* receive the stray
`WM_LBUTTONUP`. Standard controls ignore it because they act on capture and
click, but custom mouse-up logic — drawing tools, games, drag-end handlers,
controls that act on release — could react. The result supports prototyping the
"possible obligation → send Up" policy, with the refinement of skipping the Up
when `GetAsyncKeyState` already reads the button up.

### E6 — guardian spike: **forced termination is recoverable from outside the process**

Throwaway PowerShell/C# under `captures/e6/`; a fake target stands in for the
core. The write-ahead map is named by the target's pid plus start FILETIME; the
guardian waits on the target's handle, is launched by the controller rather than
by the target, and has no hooks, IPC server or configuration.

| Case | Result |
|---|---|
| T0 target exits cleanly (released, slot `UP`) | guardian: "nothing to release"; no extra injected event |
| T1 `taskkill /F` while holding | released **+5.8 ms** after death detected; Windows button up +7.7 ms after the target's exit was seen |
| T2 `taskkill /T /F` | guardian **survived** (not in the target's tree); released +6.1 ms; up +13.6 ms |
| T3a death after `SendInput(down)`, before `DOWN` was recorded | slot still `DOWN_INTENDED` → released +6.0 ms; up +30.2 ms |
| T3b death after the intent, before injecting | Windows read the button up → **skipped**, no Up sent |
| T4 guardian killed | target reported "GUARDIAN LOST" 15.2 ms later |
| T5a guardian pointed at a dead pid | refused (exit 3) |
| T5b stale state | a dead instance's map disappears once its holders exit; a restarted target gets a new identity; a guardian pointed at a live process without a map refuses (exit 4); the new guardian attaches only to the new instance |

Not run: Task Manager End task against the target. E3b already places End task
on a windowless process in the `TerminateProcess` category that T1 exercises.

### What the results settle

- **O-3 is fixable in-process** with the bounded handler wait, for console
  close, Windows Terminal tab close and Task Manager End task on a console core.
  Logoff/shutdown still need the hidden-window path, which was not built.
- **Forced termination cannot be fixed in-process, but an external guardian
  demonstrably recovers it**, within ≈ 6 ms of death detection, including the
  race between injection and recording, without killing itself alongside the
  target, and without releasing on a clean exit or attaching to stale state.
- **Still open:** a guardian adds a process, a lifecycle and a launcher-contract
  change; E5's harmlessness result covers standard controls only; the spike
  guards one button and uses PowerShell, so production latency and
  multi-obligation behaviour are unmeasured.

---

## Sources

- [HandlerRoutine callback — Windows Console](https://learn.microsoft.com/en-us/windows/console/handlerroutine) — close/logoff/shutdown semantics, new-thread execution, timeouts table
- [SetConsoleCtrlHandler — Windows Console](https://learn.microsoft.com/en-us/windows/console/setconsolectrlhandler) — user32/gdi32 processes do not receive logoff/shutdown; hidden window with `WM_QUERYENDSESSION`/`WM_ENDSESSION`
- [SendInput function](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-sendinput) — does not reset keyboard state
- [CreateSyntheticPointerDevice](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-createsyntheticpointerdevice) — `PT_TOUCH`/`PT_PEN` only
- [Simulate user input through input injection](https://learn.microsoft.com/en-us/windows/apps/design/input/input-injection) and [InputInjector.InjectMouseInput](https://learn.microsoft.com/en-us/uwp/api/windows.ui.input.preview.injection.inputinjector.injectmouseinput?view=winrt-26100) — restricted `inputInjectionBrokered` capability

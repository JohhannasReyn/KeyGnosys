# M3 — Windows backend threading and ownership design

**Status:** approved (design), pending implementation
**Milestone:** M3
**Base:** `main` @ `ee26cec`
**Supersedes nothing.** Extends `docs/SPEC.md` §6.2, §8.2, §8.3, §8.4; requires the SPEC amendments in §12.

---

## 1. Thread ownership

| Thread | Owns |
|---|---|
| **Hook thread** | `LayerEngine`; the `enabled` flag gating interception; per-key *physical* down-state used to derive `KeyState::Repeat`; the grace-window deadline |
| **Core thread** (60 Hz loop) | IPC server and sessions; `Dispatcher`; both `Integrator`s; `OutputBackend`; `WindowBackend`; the window-slot registry; all publication; all configuration parsing; every diagnostic not raised synchronously inside the hook |

No other thread calls a mutating `LayerEngine` method. There is no mutex on the engine.

**Backend-side `Repeat` derivation is load-bearing.** `KBDLLHOOKSTRUCT` carries no repeat
count. Without a per-key physical down-state bitmap owned by the input backend, every
autorepeat arrives as `Down`, the engine converts a duplicate `Down` on a forwarded key
into `Forward(c, Repeat)` (`layer_engine.cpp:257`), and the native-passthrough rule in §3
would then suppress and re-inject every autorepeat of every ordinary key.

---

## 2. Two streams, not one

An engine *decision* is not a physical *key event*. The mapping is many-to-many:

- one physical event can produce several decisions (grace replay, CapsLock promotion);
- grace expiry, `release_all` and config changes produce decisions with **no** physical event;
- a decision satisfied by native passthrough needs publication but **no** synthesis.

So two separate streams cross from the hook thread to the core thread.

### Stream A — work items (ordered, must never be lost)

```cpp
struct WorkItem {                      // 4 bytes
    enum class Kind : std::uint8_t { SendKey, RunAction, ReleaseAction };
    Kind kind;
    bool down;                         // SendKey only
    std::uint16_t code;
};
```

Only decisions that require the core to *act*. `Suppress` and `Buffer` produce none.

### Stream B — physical resolution records (coalescable, droppable)

```cpp
struct PhysicalRecord {                // 8 bytes
    std::uint16_t code;
    KeyState state;                    // the PHYSICAL state, as the OS delivered it
    bool suppressed;                   // true iff the hook returned 1
    std::uint32_t version;             // published-state version after this callback
};
```

Exactly one per hook callback. **Zero** per timer or control operation.

### The two invariants

> **INV-PUB.** `key` IPC events are produced from `PhysicalRecord`s — one per physical
> hook callback — never by iterating a `DecisionBuffer`.

> **INV-SYN.** A decision satisfied by native passthrough is never turned into a
> `SendKey` work item.

INV-PUB corrects a latent M2 defect: `core.cpp:243-251` broadcasts one `key` event per
*decision*. That was harmless with no input backend. Under M3 it would emit a `key` event
for the synthetic `Down` of a grace replay and for every key released by `release_all`.

---

## 3. The suppression verdict

The hook calls `engine.onKey()` synchronously and reads the resulting `DecisionBuffer`
before returning. Bounded and allocation-free: worst case is `promoteBuffered` over
≤ `kMaxPending` (64) or `leaveCursorMode` over ≤ `kMaxHeld` (256), all flat-array work.

> **Native passthrough rule.** The current physical event passes through natively
> (`return 0`) **iff** the engine emitted exactly one decision and it is
> `Forward(currentCode, currentState)`. Otherwise the hook returns 1 and every emitted
> decision becomes a work item.

`Decision::Kind::Forward` means "the OS should see this key event", not "pass this
through". When code and state match the event in hand, native passthrough is the cheapest
way to make that true.

Two consequences:

- **The grace tap replay cannot use native passthrough.** The physical event is the `Up`;
  the engine emits `Forward(c,Down)` then `Forward(c,Up)` (`layer_engine.cpp:331-341`). A
  synthetic `Down` must precede the release, so the hook suppresses the `Up` and both are
  synthesized in order on the core thread.
- **Native passthrough is P7-safe across a process crash.** A natively-forwarded press is
  physically down in the OS; if the process dies, the hook dies with it and the user's own
  physical release reaches the OS. A re-injected press would have no such guarantee.

### Translation (hook side)

```
native = buffer.size() == 1
      && buffer[0].kind  == Forward
      && buffer[0].code  == currentCode
      && buffer[0].state == currentState;

if (!native)
    for each decision d in buffer:
        Forward       -> WorkItem{SendKey, d.state != Up, d.code}
        RunAction     -> WorkItem{RunAction, _, d.code}
        ReleaseAction -> WorkItem{ReleaseAction, _, d.code}
        Suppress      -> nothing
        Buffer        -> nothing

publish PhysicalRecord{currentCode, currentState, !native, version};
return native ? 0 : 1;
```

Timer and control paths run the same translation with `native == false` and emit no
`PhysicalRecord`.

---

## 4. Capacity proof — overflow on a release path is structurally impossible

### 4.1 Every `Up` path, traced

Non-CapsLock, from `layer_engine.cpp:322-355`:

| # | Condition | Decisions | Native | Work items | State consumed |
|---|---|---|---|---|---|
| U1 | invalid code | `Suppress(c,Up)` | no | 0 | — |
| U2 | `kForwarded` | `Forward(c,Up)` | **yes** | **0** | F−1 |
| U3 | pending, `forwardPress` succeeds | `Forward(c,Down)`, `Forward(c,Up)` | no | 2 | P−1 |
| U4 | pending, `forwardPress` refused | `Suppress(c,Up)` | no | 0 | P−1 |
| U5 | `kHeldAction` | `ReleaseAction(c)` | no | 1 | H−1 |
| U6 | none of the above | `Suppress(c,Up)` | no | 0 | — |

CapsLock, from `layer_engine.cpp:439-489`:

| # | Condition | Decisions | Native | Work items | State consumed |
|---|---|---|---|---|---|
| C1 | orphan (`!capsPhysicallyDown_`) | `Suppress` | no | 0 | — |
| C2 | escape gesture, forwarded | `Forward(CapsLock,Up)` | **yes** | **0** | F−1 |
| C3 | escape gesture, not forwarded | `Suppress` | no | 0 | — |
| C4 | Toggle | `Suppress` | no | 0 | — |
| C5 | **Hold** | `Suppress` + H×`ReleaseAction` | no | **H** | H→0 |
| C6 | Hybrid, tap latches on | `Suppress` | no | 0 | — |
| C7 | **Hybrid, hold or tap-off** | `Suppress` + H×`ReleaseAction` | no | **H** | H→0 |

**A single `Up` can emit up to `1 + kMaxHeld` = 257 decisions and `kMaxHeld` = 256 work
items** (C5, C7). The earlier "an Up emits at most 2 decisions" claim was true only of the
ordinary-key cases and is withdrawn.

### 4.2 The residual charge

With `H = heldCount_`, `P = pendingCount_`, `F = forwardedCount_`, define

```
U := H + 2·P
```

`U` bounds the total work items all future `Up` events can produce, given that no
obligation-creating operation is admitted meanwhile. Checking every row above, the work
items emitted are `≤ −ΔU`:

| Rows | items | ΔU |
|---|---|---|
| U2, C2 | 0 | 0 (consumes F, which carries no charge) |
| U3 | 2 | −2 |
| U4 | 0 | −2 |
| U5 | 1 | −1 |
| C5, C7 | H | −H |
| U1, U6, C1, C3, C4, C6 | 0 | 0 |

**The rows that emit no work items also consume no ring capacity.** That is precisely what
the §2 separation buys. Under a single combined stream, row U6 — an `Up` for a key with no
obligation at all — would consume a slot while consuming no state, and no bound would
exist. Publication traffic in the obligation ring destroys the proof; this is why
`ModeChanged` and `ModifiersChanged` are *not* ring entries (§5).

Since `H ≤ kMaxHeld` and `P ≤ kMaxPending` by the engine's own fixed capacities:

```
W_max := kMaxHeld + 2·kMaxPending = 256 + 128 = 384
U ≤ W_max, always and unconditionally
```

### 4.3 Admission rule

Work-ring capacity `N`. Admission gate

```
G := kDecisionCapacity + W_max = 584 + 384 = 968
```

- **Obligation-creating operations** — `Down`, `Repeat`, `tick()`, and every control
  operation — are admitted only when `free ≥ G`. Otherwise the engine is **not called**:
  the physical event is suppressed, the deadline re-armed, the control message left in its
  ring. No state changes, so no obligation is created.
- **`Up` events are never refused.**

### 4.4 Theorem

> **Invariant I:** `free ≥ U` at all times.

*Initially.* `H = P = 0`, so `U = 0` and `free = N`. ∎

*`Up` event.* Emits `w` items; by §4.2, `U` drops by at least `w`. So
`free' − U' = (free − w) − U' ≥ free − U ≥ 0`. I is preserved, and since `U' ≥ 0` we have
`free' ≥ 0` — **the ring cannot overflow on a release path.** ∎

*Admitted obligation-creating operation.* At admission `free ≥ G`. The operation emits
`w ≤ kDecisionCapacity` work items, because its `DecisionBuffer` is capped there and work
items never exceed decisions. So `free' ≥ G − kDecisionCapacity = W_max`. And
`U' ≤ W_max` unconditionally (§4.2). Hence `free' ≥ W_max ≥ U'`: I is restored. ∎

*Refused operation.* No state change, no items. I trivially preserved. ∎

*Core drains the ring.* `free` increases, `U` unchanged. I preserved. ∎

**Corollary.** The work ring never overflows, so no release-path decision is ever dropped,
and P7 cannot be violated by queue pressure. ∎

Minimum `N = G = 968`. **Chosen: `N = 2048`** (power of two for cheap masking, 8 KB),
leaving 1080 slots of slack so the admission gate is unreachable under any realistic
core-loop delay. The gate deliberately uses the conservative `kDecisionCapacity` rather
than a tighter per-operation bound, so it needs no revisiting when the engine changes.

### 4.5 Behaviour at the gate

Reaching the gate requires the core loop to miss roughly 130 consecutive ticks (>2 s),
which is a core bug rather than a load condition. Response: new diagnostic
`input.queue_overflow` (warn), and the core issues `release_all`. While the gate is
closed, new key presses are suppressed — a missing keystroke, never a stranded one. No
watchdog uninstalls the hook; see §13.

---

## 5. Published state

One 64-bit atomic replaces ordered notification entries:

```cpp
// high 32 bits: monotonically increasing version
// low  32 bits: mode | latched | activation | shift | control | alt | meta
std::atomic<std::uint64_t> published_;
```

The hook stores (release) after any engine mutation that changes the low half, bumping the
version. The core loads (acquire).

**Ordering without ordered entries.** Each `PhysicalRecord` carries the version *after* its
callback. Draining stream B, the core emits the `key` event, then — if the record's version
differs from the last it published — emits `mode` and/or `modifiers`. The key that engaged
the layer is therefore reported before the mode change it caused. Versions arising from
control operations (no `PhysicalRecord`) are picked up by one comparison per tick.

SPEC §5.3 specifies emission *conditions*, not cross-event ordering, so coalescing is
compliant. Two mode flips inside one 16.7 ms tick produce one `mode` event carrying the
final state; §12 adds a SPEC sentence permitting this explicitly.

**Stream B overflow:** drop the oldest, count it, emit `input.publication_dropped` (info).
The consequence is an overlay key that fails to light — never a correctness issue. Sized
1024 records (8 KB).

---

## 6. Core → hook control

Second SPSC ring plus one auto-reset event.

```cpp
struct Control {
    enum class Kind : std::uint8_t { SetConfig, SetBindings, ReleaseAll, SetEnabled, Stop };
    Kind kind;
    bool flag;                 // SetEnabled
    std::uint32_t seq;
    const void* payload;       // EngineConfig* or BindingMap*, owned by the core
};
```

> **Corrected 2026-09-01 after live validation.** What follows replaces the
> original wake mechanism, which was wrong. The original text is summarised
> under "What was wrong and how it was found" below rather than quietly
> deleted — the reasoning that produced it looked sound on paper and would be
> re-derived by the next person otherwise.

**Wake mechanism:** `GetMessageW` is the blocking primitive. The loop is woken by
posted thread messages and by a thread timer:

```
GetMessageW(&msg, nullptr, 0, 0)          // blocks; dispatches hook callbacks
  ├─ WM_APP+1  doorbell → disarm, then drain the control ring
  ├─ WM_APP+2  grace-sync → re-evaluate the timer
  ├─ WM_TIMER  → kill the timer, then run the grace expiry
  └─ otherwise → Translate/Dispatch
then: re-arm or kill the grace timer from LayerEngine::nextDeadline()
```

**Why `GetMessageW` and not a wait.** `WH_KEYBOARD_LL` callbacks are delivered to
the installing thread and are dispatched **only while that thread is inside a
message-retrieval call**. `MsgWaitForMultipleObjectsEx` is a *wait*, not a
retrieval call. `GetMessageW` blocks with zero wakeups when idle *and* retrieves,
which is the property the design needs and the previous primitive did not have.

**Control wakeups are doorbells, not payload.** The SPSC control ring above stays
authoritative for payload and ordering; `PostThreadMessageW(tid, WM_APP+1, 0, 0)`
carries nothing. This answers the original objection to `PostThreadMessageW`,
which was about payload width and queue capacity, not about waking: a doorbell has
no payload, and it is coalesced through a single atomic flag (`kgn::Doorbell`), so
a burst of a thousand controls costs one thread message rather than a thousand.

**The doorbell's ordering rule.** The consumer calls `disarm()` **before** draining,
never after. Draining first and clearing second loses any control pushed between
"ring looks empty" and "flag cleared": the producer sees the flag still set, posts
nothing, and the item waits forever. `test_hookpump` drives both orderings and
asserts the broken one strands work.

**Startup.** `PeekMessageW` on an empty range forces the queue into existence; only
then is the thread id published, then the hook installed, then `ready_` set. A
producer that reads a non-zero thread id therefore knows a queue is behind it. A
producer that reads zero has not been stranded either: the id is stored *before*
the thread's startup drain, so that drain observes anything already pushed.

**Grace deadlines use a one-shot thread timer.** `SetTimer` arms it from
`nextDeadline()`; the loop kills it when it fires, *before* consulting the engine,
then re-arms only if a deadline remains. Killing first is what makes a stale or
coalesced `WM_TIMER` harmless — the expiry re-reads the clock and does nothing
unless something is genuinely due — and it is what stops a periodic timer from
degenerating into a poll. The arm/kill decision is `kgn::GraceTimerPlan`, which is
platform-free and tested.

Because `pending_` is in press order and the grace window is uniform, the head
deadline only ever moves *later*, so an armed timer never needs replacing with an
earlier one; it merely fires early once, which the not-yet-due path absorbs.

**The hook callback posts at most one message, and only on a transition.** The loop
is blocked inside `GetMessageW` while the callback runs, so it cannot notice that a
buffered press just created a deadline. The callback therefore posts `WM_APP+2` —
but only when `!graceTimerArmed_ && nextDeadline().has_value()`, so ordinary typing
adds no syscall at all.

**Zero idle wakeups is preserved.** With no keyboard activity, no control traffic and
no pending grace deadline, nothing is armed and `GetMessageW` blocks indefinitely.
Measured after the repair: the hook thread consumed **0.000 s of CPU over 10 idle
seconds**, while the core's own 60 Hz motion loop consumed 0.234 s.

### What was wrong, and how it was found

The original design chose an auto-reset `Event` plus
`MsgWaitForMultipleObjectsEx(1, &wake, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE)`
specifically because the wait's `timeout` parameter could double as the
grace-window deadline timer — "one primitive, both jobs". It rejected
`PostThreadMessageW` (payload width, the system queue's 10 000 cap, a startup
race) and a message-only `HWND_MESSAGE` window (class and handle lifetime for no
gain).

**That premise was empirically falsified.** `QS_ALLINPUT` does not cause a blocked
`MsgWaitForMultipleObjectsEx` to dispatch a low-level hook callback. With no
control traffic and no pending deadline the timeout was `INFINITE`, so the thread
parked forever and the `PeekMessageW` on the following line — the call that
actually dispatches the hook — was never reached.

The failure mode was quiet in the worst way: the hook installed, returned a valid
`HHOOK`, never registered a loss, reported itself healthy, and intercepted
nothing. Ordinary typing was unaffected, so the first matrix row *passed*.

It was caught by live validation on 2026-09-01 (`docs/manual-test-logs/`), by
running an independent `WH_KEYBOARD_LL` hook in a separate process over the same
keystrokes: **529 callbacks to the independent hook, 0 to ours**. Capping the idle
wait at 50 ms — changing nothing else — produced 1816 callbacks, which isolated
the pump as the sole variable.

The automated suite was green throughout, because nothing in it depended on the
hook thread retrieving messages. That gap is now covered from both sides:
`test_hookpump` pins the doorbell and timer logic, and `kgn_hook_smoke` — built,
deliberately not in ctest — installs the real hook and reports PASS only after a
genuine keystroke reaches the publication ring.

**The hook thread never waits on the core thread.** The event wait is a wait on a kernel
object with a timeout, never a wait for the core to release anything, and the hook callback
is never inside it.

**Control handlers must be bounded.** They run on the hook thread between message-retrieval
calls; a thread that stops pumping stops servicing input. `engine.setBindings()` over ~100
entries is flat-array flag writes — microseconds.

**Payload lifetime.** The core allocates, publishes a pointer, and frees only after
acknowledgement. `LayerEngine::setBindings` reads the map and retains nothing
(`layer_engine.cpp:68`), so ack-then-free is sufficient and no ownership crosses.

---

## 7. Timed engine work

`LayerEngine::tick()` does exactly one thing: expire grace-window pending presses and
forward them (`layer_engine.cpp:522-537`). Hybrid-tap resolution is *not* timed — it is
computed on the CapsLock release by comparing `capsPressedAt_` (`layer_engine.cpp:468`). The
engine therefore has no 60 Hz requirement; it has a single deadline.

- New pure method: `std::optional<TimePoint> LayerEngine::nextDeadline() const`, returning
  `pending_[0].pressedAt + config_.grace` when `pendingCount_ > 0`, else `nullopt`.
  `pending_` is in press order and `grace` is uniform, so the head entry is the earliest.
- The pump's wait timeout is `clamp(deadline − now, 0, INFINITE)`.
- **`Core::step()` stops calling `engine.tick()`.**
- The motion integrator and `Dispatcher::tick()` stay on the core's 60 Hz loop. SPEC §6.4
  specifies 60 Hz for *motion*; it says nothing about engine cadence.

Strictly better than today: the grace window resolves at its deadline rather than up to
16.7 ms late, and an idle keyboard produces zero hook-thread wakeups instead of 60/second.

---

## 8. Completion for engine-mutating IPC commands

Each control message carries `seq`. After draining control, the hook stores
`lastAppliedSeq` (release) and signals a completion event. The core, for commands that must
mean *applied*, waits with a 250 ms timeout, checks `lastAppliedSeq >= mine`, then replies.

The core waiting is safe: it stalls the core loop, never the hook callback, and is bounded
because the hook drains control immediately on wake and never blocks.

Commands requiring completion: `release_all`, `set_enabled`, `set_bindings`,
`reload_config`, `set_activation_mode`, and `set_setting` for the three `behavior.*` paths
reaching `EngineConfig`.

On timeout the core replies `{ok:false}` with `input.queue_overflow` rather than a false
success (P6).

Reads (`get_state`, `mode`/`modifiers` events) do **not** round-trip; they use the published
atomic of §5.

---

## 9. Shutdown, and the fallback when the hook never acknowledges

### 9.1 Coverage argument

At any instant, OS-visible obligations partition into exactly three disjoint sets:

1. **Natively-forwarded physical presses** — every key with `kForwarded` delivered by
   native passthrough. Each corresponds to a key still physically held: the flag is cleared
   only by the physical `Up` (row U2/C2) or by `releaseAll`. Discharged by *uninstalling the
   hook*, after which the user's own release reaches the OS directly.
2. **Dispatcher state** — buttons down, drag locks, held directions, precision. Core-owned.
   Discharged by `Dispatcher::releaseAll()`.
3. **Backend-synthesized keys and buttons** — anything `OutputBackend` emitted as a press
   with no matching release. Discharged by `OutputBackend::releaseAll()`, which
   `backends.hpp:96` already mandates.

The engine's remaining bookkeeping maps to no obligation outside those sets: `kPending`
never reached the OS, and `kHeldAction` manifests only as set 2. Sets 1 and 3 may overlap
for a synthetic press that is also physically held; a duplicate key-up is harmless.

### 9.2 Formal fallback

Runs on acknowledgement timeout, hook-thread death, or any abnormal exit:

```
1. InputBackend::stop()           — UnhookWindowsHookEx; hook thread exits or is abandoned
2. Drain the work ring; execute every item still in it
3. Dispatcher::releaseAll()       — set 2
4. OutputBackend::releaseAll()    — set 3
5. Server::shutdown(reason); EndpointOwner::release()
```

Step 1 must precede step 2 so no new items arrive mid-drain.

**This does not depend on the hook acknowledging anything.** The acknowledged path is an
optimisation producing a tidier unwind; the fallback is independently sufficient. Shutdown
correctness therefore does not rest on the hook thread being alive.

**It is complete only if `SendInputOutput::releaseAll()` tracks its own synthesized
presses.** That is an explicit, tested M3 requirement (plan Task W3).

### 9.3 Clean shutdown

```
1. Core: send ReleaseAll control, wait for ack (250 ms)
2. Core: drain the work ring completely — sendKey(k,false), dispatcher releases
3. Core: send Stop control; hook thread UnhookWindowsHookEx, exits pump; join
4. Core: dispatcher.releaseAll() + OutputBackend::releaseAll()   (belt and braces)
5. Core: InputBackend::stop(), Server::shutdown(reason), EndpointOwner::release()
```

The signal handler keeps its current shape — set a flag, nothing else — with the unwind on
the loop's own thread.

---

## 10. Enable / disable

`set_enabled(false)` → control message. The hook runs `engine.releaseAll()`, enqueues the
result, then sets `enabled_ = false` and thereafter returns 0 without touching the engine.
Re-enable sets it true; the engine is already fully released, so it restarts clean with no
divergence. The hook stays installed — simpler than uninstall/reinstall, and SPEC §5.4's
requirement is behavioural.

---

## 11. Backend composition root

- `Core(CoreOptions options, Backends backends = {})`. Default-constructed `Backends{}` is
  today's null-member behaviour, so no test needs a fake unless it wants one.
- `createBackends()` leaves `backends.hpp` for a new `kgn/platform.hpp` supplied by
  `kgn_platform`. `kgn_ipc` then has no undefined reference and `test_core` keeps linking
  `kgn_ipc` alone.
- `kgn_platform` **always exists**; with no real sources it compiles `platform_none.cpp`.
  `src/backends_none.cpp` is deleted — "no backends" is now just `Backends{}`.
- Only `keygnosys-core` links `kgn_platform`. That is SPEC §6.2's "one line in the factory",
  now genuinely at a boundary.
- Tests can inject fakes to cover the warp/window/effect paths that are currently
  untestable. A fake in a test is not a fake shipped to a user; P6 is unaffected.

**Capability ownership fix.** `core.cpp:544-556` currently reads `canWarpAbsolute` and
`canMoveWindows` off the *input* backend. Split `capabilities()` per interface:
`InputBackend` keeps `canSuppress`, `OutputBackend` gains `canWarpAbsolute`, `WindowBackend`
gains `canMoveWindows`, each contributing its own `limitations`.

---

## 12. SPEC amendments required

1. **§6.2** — per-interface `capabilities()`; `createBackends()` moves to `kgn/platform.hpp`.
2. **§8.2** — state that the only synchronous product of the hook is the suppression
   verdict, that decisions are delivered asynchronously, and that a grace tap replay is
   re-synthesized rather than passed through.
3. **§5.3** — add the `overlay_toggle` row; note that `mode` and `modifiers` may be
   coalesced within one core tick.
4. **§11** — add `ipc.bad_message`, `input.queue_overflow`, `input.publication_dropped`.
5. **§5.4** — say which replies mean *applied* rather than *accepted*.
6. **§6.3/§6.4** — record that engine timing is deadline-driven off the grace window and
   that only motion is 60 Hz.
7. **§7.2** — `button.double_click` is scheduled on the core loop without blocking; if the
   layer is released mid-sequence the pending second click is cancelled and the button is
   guaranteed up (P7 over fidelity).
8. **§5.3 line 990** → §11 (not §10); **line 997** → §12 (not §11).

---

## 13. Alternatives rejected

1. **Mutex around `LayerEngine`** — contended by construction (reload, `release_all`,
   shutdown all mutate from the core), so the hook can be made to wait past
   `LowLevelHooksTimeout`.
2. **Engine on the core thread with a precomputed suppression table** — the verdict depends
   on mode, latch, grace, pending and forwarded state that changes on nearly every event;
   any snapshot is stale by construction and destroys P7 tracking.
3. **Seqlock / RCU double-buffered engine state** — clever machinery in place of clear
   ownership.
4. **A single combined ring** — destroys the §4 capacity proof; row U6 consumes capacity
   while discharging nothing.
5. **Dropping decisions on overflow** — a dropped `Forward(k,Up)` from `releaseAll` leaves
   the key down forever and the engine's record already cleared. Unrecoverable.
6. **One uniform headroom rule for all events** — tidier, but refusing an `Up` in the
   tap-replay case leaves the key pending, `tick()` later synthesizes a `Down`, and the
   physical release is gone. Stranded key.
7. **`SendInput` from inside the hook** to order the tap replay — unbounded, potentially
   blocking work on the one path that must not have it.
8. **Hook blocking on a core round-trip for the verdict** — the direct route to
   `LowLevelHooksTimeout`.
9. **Routing all input through suppress + re-inject** — uniform, but marks ordinary typing
   `LLKHF_INJECTED`, adds latency to every keystroke, risks IME breakage, and forfeits the
   crash-safety property of §3.
10. **Watchdog uninstalling the hook on sustained overflow** — trades P7 for usability on a
    path that already requires a core bug to reach. Noted as the option if
    `input.queue_overflow` ever actually fires.

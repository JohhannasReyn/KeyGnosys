# O-5 — cross-channel delivery ordering: transport audit and repair design

Status: **design only. Not implemented.** No SPEC change proposed yet.

O-5 is separate from O-1 (routing provenance, repaired) and O-2 (ordering among
buffered presses inside the engine, open).

The invariant O-5 violates:

> Once an earlier decision for key K has been committed to asynchronous synthetic
> delivery, a later decision for the same physical press must not bypass that
> pending obligation through the native path if doing so can reverse their
> Windows-visible order.

---

## 1. When is an asynchronous obligation ordered?

Producer: the hook thread. Consumer: the core loop thread. One of each, which is
what `SpscRing` assumes rather than enforces (`hookchannel.hpp:124-131`).

| step | code | what it establishes |
|---|---|---|
| decision translation | `hookchannel.cpp:76-81` | chooses native vs ring, per decision batch |
| ring publication | `hookchannel.cpp:86`, `SpscRing::push` | `items_[head]` written, then `head_.store(release)` |
| ring consumption | `core.cpp:349`, `SpscRing::pop` | `head_.load(acquire)` pairs with the release; **`tail_.store(release)` advances here** |
| `SendInput` | `core.cpp:353` → `sendinput_output.cpp:123` | **runs after `tail_` has already advanced** |
| native `Forward` | `hook_input.cpp:257` | `CallNextHookEx` — event continues down the chain |
| hook callback return | — | the physical event is delivered only once the *whole* chain completes |

The acquire/release pairing is correct: an item's contents are visible to the
consumer before it is dequeued. That is not the property at issue.

**The property at issue is that `tail_` advances at dequeue, before `SendInput`.**
Consequences, both load-bearing:

1. **`ring.empty()` is not a safety condition and must not be used as one.** It
   reports true while a dequeued `SendKey` is still on its way to `SendInput`.
   This is the exact window O-5 lives in, so the obvious repair would leave the
   defect reachable while appearing to fix it.
2. `free()`, `size()` and `empty()` all describe *queue occupancy*, never
   delivery. Nothing in the transport records delivery.

**Identifying the point after which a native event may safely bypass an earlier
synthetic one: there isn't an observable one.** After `SendInput` returns, the
event is in the system input queue. But the physical Up we would forward is *not*
sequenced after it in any documented way — it entered the input stream earlier
(at the hardware) and is mid-traversal of the hook chain. Windows documents no
ordering between an injected event and a physical event still being processed by
low-level hooks. Our own data cannot settle it either: six of seven native-path
cases raced favourably and one did not, which is evidence of a race, not of a
rule.

**Conclusion that drives the design: any repair whose correctness depends on
observing the asynchronous channel's progress is unprovable. Do not build one.**
That rules out `ring.empty()`, a dequeue counter, and a "nothing pending for this
key" check — all are check-then-act against a channel whose completion we cannot
see.

---

## 2. Is per-key tracking sufficient?

**Yes, and it is the narrowest scope that preserves the semantics.**

P7 is a per-key property: Windows holds key state per key, so a reversal can only
strand the key it concerns. Relative order *between different keys* is a text
correctness property — that is O-2's territory, and O-5 must not be widened to
cover it or the two findings become impossible to validate separately.

| case | behaviour under a per-key rule | verdict |
|---|---|---|
| same-key Down → Up | the Up uses the press's channel; FIFO orders them | the defect, fixed |
| repeats | `layer_engine.cpp:371-375` emits `Forward(K, Repeat)`, currently native. Under the rule it follows the press's channel | consistent; cannot reverse against its own press |
| different keys | unaffected; no serialisation introduced | no regression, no O-2 overlap |
| modifiers | ordinary keys to this machinery | covered |
| CapsLock | dispatched separately (`capsForwarded_`) but through the same `translateDecisions` | covered, needs a test |
| grace replay | the origin case: press synthetic ⇒ release synthetic | fixed |
| `release_all` / cleanup | called with `KeyCode{}`; `code.valid()` is false so the native path is already unreachable — everything is ordered through the ring already | already correct |
| disable / re-enable | while disabled the hook returns `CallNextHookEx` before translation; `SetEnabled` first runs `engine_.releaseAll()` through the ring, discharging obligations | already correct; the channel record must be cleared with them |
| config reload | same as `release_all` | already correct |

No unrelated key is serialised, because the rule consults only the key in hand.

---

## 3. Can native forwarding join the same ordered channel?

**Yes, and this is the proposed design.** It is the option the existing comment
at `hookchannel.cpp:59-64` already describes — "the physical event must be
suppressed and re-synthesised so the order is ours to control" — applied to the
case that comment misses: work queued by an *earlier* batch.

### The rule

> **A press and its release must travel the same channel.** If the Windows-visible
> press for K was delivered synthetically, every later event of that press —
> repeat and release — is delivered synthetically too. If the press was delivered
> natively, the native fast path stays available.

### Why this is provable where a barrier is not

Same channel ⇒ one FIFO ring ⇒ one consumer thread ⇒ `SendInput` calls issued in
push order. Ordering is established **by construction**, not by observation.
There is no moment to sample, nothing to wait for, and no check-then-act.

The state required is one value per key: which channel delivered this press.
Crucially, **it is written and read only on the hook thread**: on Windows every
`translateDecisions` call that can select the native path runs there — the hook
callback (`hook_input.cpp:244`), `drainControl` (480/490/495) and `expireGrace`
(518) are all hook-thread code, and `HookInput::Owner::submit` only pushes a
control for that thread to drain. `LocalEngineOwner` (`core.cpp:170-180`) calls
`translateDecisions` on the core thread, but always with `KeyCode{}`, so the
native path is unreachable there and the record is never consulted.

So the new state introduces **no cross-thread access at all** — no atomics, no
memory-ordering argument beyond the ring's existing one.

### Shape

It mirrors `DeliveryProvenance` (the O-1 repair) deliberately, because it is the
same kind of fact about a different axis:

| | question | values |
|---|---|---|
| `DeliveryProvenance` (O-1) | did this press reach the **engine**, or go past it to Windows? | None / Engine / Native |
| proposed (O-5) | which **channel** delivered this press to Windows? | None / Native / Synthetic |

They are independent and must not be merged: a press can be engine-routed and
natively delivered (ordinary bound key passed through), or engine-routed and
synthetically delivered (grace replay).

### Alternative considered and rejected

An explicit per-key pending-delivery barrier (count published, decrement after
`SendInput`, native only at zero). Rejected because its correctness still rests
on the unprovable step in §1: a zero count means "submitted", not "delivered
ahead of an in-flight physical event". It adds cross-thread state and a
completion definition, and buys a slightly faster path in a case that is already
rare. One authoritative mechanism beats two.

---

## 4. Completion

**The design introduces no pending state and therefore needs no completion
definition.** That is its main advantage over the barrier: the only defensible
completion point ("`SendInput` returned") is exactly the one §1 shows to be
insufficient.

The per-key channel record is not an obligation; it is a fact about the current
press. Lifecycle, all on the hook thread:

- set to `Native` when a Down/Repeat for K takes the native path;
- set to `Synthetic` when a Down/Repeat for K is pushed as a `SendKey`;
- cleared when the release for K is emitted on either channel;
- cleared for all keys wherever `DeliveryProvenance::forgetAll()` is already
  called — `HookInput::run()` teardown (`hook_input.cpp:379`), strictly after
  `uninstall()`, for the same reason given there;
- cleared for a key whose obligations are discharged by `releaseAll` (disable,
  reload, `release_all`).

The existing `SendInputOutput::heldKeys_` remains the authority on what Windows
actually holds. The channel record never duplicates it and never second-guesses
it.

---

## 5. Failure behaviour

If the synthetic Down's `SendInput` fails (`sendinput_output.cpp:123-131`):

- `heldKeys_[K]` stays false — Windows never received the press;
- `failedPresses_` increments and is reported as `output.send_failed` (P6);
- the later synthetic Up is then an orphan key-up. **K-E5 measured exactly this
  case**: an injected key-up with no matching down left Windows' key state and a
  standard text box unchanged, with the recorded limitation about applications
  that handle raw key-ups.

The repair **never waits** for an obligation, so a failed or refused `SendInput`
cannot wedge it — the failure mode is a harmless extra key-up, not a hang. No
second ownership system is introduced: `heldKeys_`, `failedPresses_`,
`failedReleases_` and `releaseAll()` retry semantics are untouched.

---

## 6. Repair boundary

1. A per-key channel record owned by the Windows transport (new small class,
   shaped like `DeliveryProvenance`).
2. One added condition on the native fast path in `translateDecisions`
   (`hookchannel.cpp:76-81`): native additionally requires that this key's press
   was delivered natively. `translateDecisions` gains a parameter for the record;
   the `LocalEngineOwner` call sites pass none.
3. Record updates at the two points where the channel is chosen.

No `LayerEngine` change. No change to the decisions the engine emits. No change
to grace semantics — which is what keeps O-5 separate from O-2.

**Capacity check.** Making a repeat of a synthetically-pressed key synthetic
costs one ring item where it previously cost none. Repeats are already
admission-gated (`hook_input.cpp:236` gates every `state != KeyState::Up`) and
the gate reserves `kDecisionCapacity` for what the operation emits, so the
overflow proof in `hookchannel.hpp:66-98` still holds. To be restated in the
implementation rather than assumed.

**SPEC.** This looks like an implementation violation of an ordering rule SPEC
§6.3.3 already intends, not a gap in the normative text. No SPEC change proposed;
to be confirmed against §6.3.3's exact wording before implementing.

---

## 7. Tests, before implementation

`translateDecisions` is platform-free and takes a `WorkRing`, so O-5 reproduces
deterministically with no threads, no timing and no Windows.

### The reproduction

1. Grace expiry: translate `[Forward(K, Down)]` with `KeyCode{}` → a
   `SendKey(K, down)` is published.
2. **Do not drain the ring** — this models "published, delivery not complete".
3. Physical Up arrives: translate `[Forward(K, Up)]` with `code = K`,
   `state = Up`.
4. Assert it is **not** native and that `SendKey(K, up)` is published.
5. Drain: the order must be Down, then Up.

Against current code, step 4 fails: `hookchannel.cpp:76-81` returns native, the
Up bypasses the queued Down, and the drained ring contains only the Down — the
release never entered the ordered channel. That is the actual O-5 mechanism, not
a proxy for it.

### Coverage

- no pending obligation for K → native fast path still taken (guards against
  over-serialising);
- `takePending` fast tap → still two ordered decisions, unchanged;
- a *different* key with pending work → native still allowed (per-key scope);
- repeat of a synthetically-pressed key → ordered;
- repeat of a natively-pressed key → still native;
- Shift / modifier; CapsLock (separate dispatch path);
- disable → re-enable boundary: record cleared with the `releaseAll`;
- cleanup / `release_all` path;
- injection failure: a failed Down still yields an ordered Up, no wait, no wedge.

### Mutation tests on the guard

- **premature clear** — clear the channel record at the press instead of the
  release: the reproduction must fail;
- invert the guard (native only when synthetic): must fail;
- make the record global rather than per-key: the different-key test must fail;
- drop the repeat case: the repeat test must fail.

---

## 8. Repair validation (live, later)

Temporary diagnostics on one QPC clock, to prove the repaired build is correct
even when the earlier Down is deliberately delayed past the point where the old
build stranded the key:

physical-Up hook entry → decision translation → synthetic Down publication →
`SendInput` begin / end / result → how the physical Up was handled (native vs
ordered synthetic) → final Windows key state.

This demonstrates the repair under a forced delay, which is worth more than
further accidental samples. The 7.9-hour capture tail is preserved unmined.

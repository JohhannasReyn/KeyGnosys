# Remove the grace window — design

**Status:** approved approach A on 2026-11-22. This short design needs the
operator's approval before a plan is written.

## Intent

CapsLock pressed *first* is the only way into the cursor layer. A key bound in
the layer, pressed while the layer is off, is ordinary typing and reaches the
OS at once, natively, exactly like an unbound key.

**Why.** The grace window holds every bound key (`H`, `J`, `K`, …) for up to
`grace_ms` during ordinary typing. It does this to catch the rare chord where
the bound key lands a few milliseconds before CapsLock. The operator judged the
cost not worth it:

- latency on everyday letters;
- the reordering bug class O-2 and O-12, still felt as "teh".

They chose to give up that chord.

## Goals

- With the layer off, no key is ever delayed or re-synthesised by the layer
  engine. Bound keys pass through natively.
- Layer entry is unchanged otherwise:
  - CapsLock tap, hold and latch behave as now, in every activation mode;
  - `hybrid_tap_ms`, which decides tap versus hold on CapsLock itself, stays;
  - keys pressed *after* CapsLock behave as now.
- P7 (the forwarded-release invariant) and every other release obligation are
  unchanged.

## Non-goals and anti-goals

- **Non-goal:** any replacement chord-detection scheme.
- **Anti-goal:** a dormant code path. No hidden flag and no `grace_ms: 0` mode;
  the mechanism is deleted (approach A).
- **Anti-goal:** changing behaviour once the layer is engaged.

## Approaches considered

- **A. Remove outright. Chosen.** Delete the mechanism, its timer, its setting
  and its tests, and record the removal in the SPEC.
- **B. Keep, default off.** `grace_ms` defaults to 0, and 0 means "no
  buffering". Rejected: it keeps the machinery and its bug surface for a
  feature the operator does not want.

## What changes

- **Layer engine** (`layer_engine.{hpp,cpp}`).
  - **Removed:**
    - the pending buffer;
    - `bufferPress`, `takePending`, `resolvePendingBefore` and
      `resolveOldestPending`;
    - grace expiry in `tick`;
    - promoting buffered presses to layer presses on CapsLock;
    - `nextDeadline`, if nothing else uses it.
  - **The new rule:** a bound key pressed while the layer is off takes the same
    path an unbound key takes today.
  - **`tick()`:** `tick()` and `capacityDrops_` stay only if something other
    than grace still needs them. The builder checks.
- **Windows hook pump.**
  - **Removed:**
    - `GraceTimer` (`grace_timer.{hpp,cpp}`);
    - the `kMsgGraceExpired` message;
    - the deadline arming in `hook_input.cpp`;
    - the deadline timeout in the pump's wait;
    - `test_grace_timer.cpp`.
  - **The pump:** goes back to waiting only on messages and its own events.
- **Core** (`core.cpp`).
  - `behavior.grace_ms` is no longer a setting, and `set_setting` rejects it.
  - Grace-deadline wakeups in the core loop are removed.
  - **Builder's call:** whether a config file that still contains `grace_ms` is
    silently ignored or produces a one-time `config` diagnostic. Old
    `settings.json` files must still load.
- **Overlay** (`python/keygnosys/settings.py`): remove the `grace_ms` default
  and its clamp entry.
- **Tests.**
  - Delete the tests of buffering, expiry, promotion and ordering inside the
    buffer.
  - **New, failing before the change:**
    - a bound key pressed with the layer off is forwarded immediately, as a
      single native decision;
    - a bound key pressed just before CapsLock is typed, and the layer then
      engages;
    - holding a bound key with the layer off autorepeats natively;
    - `set_setting behavior.grace_ms` is refused.
  - Every P7 test still passes unchanged.
- **SPEC.**
  - §6.3 drops the grace window and states the new rule plainly: "CapsLock
    first is the only way in; a bound key pressed while the layer is off is
    ordinary typing."
  - §6.3.3's buffered-ordering cases go.
  - §15.10 and the M5 calibration plan are replaced by a short note that the
    window was removed, when, and why.
  - The config schema loses `grace_ms`.
- **Manual matrix** (`docs/manual-tests.md`).
  - Section 3 becomes one row: "With the layer off, a bound key types
    instantly; pressing it just before CapsLock types the letter, then the
    layer engages."
  - Row 11.7 (one event per release during a grace replay) is removed.
- **M3 log.** A dated entry records:
  - the decision;
  - O-2 closed as no longer applicable;
  - rows 3.1–3.6 and 11.7 retired, with 3.5 (unverified) among them;
  - the new row run live.

## Left to the builder

- Exact deletions where a helper turns out to serve something besides grace
  (check with grep before deleting).
- The wording of the config-key diagnostic, if one is emitted.
- Whether `LayerEngine::tick` survives.

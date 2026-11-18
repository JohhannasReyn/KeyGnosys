# O-10: say when the layer is inert over an elevated window — design

**Intent.** When an elevated (admin) window has focus, the core cannot intercept
keys, so the cursor layer does nothing. Today nothing says so, and the overlay
keeps showing an engaged layer. SPEC §8.2 requires that "the UI states plainly
when the layer is inert for this reason". Found in M3 row 8.1, Finding O-10.

**Goals**
- The core knows when the focused window is beyond its reach, and says so in
  its state.
- The overlay shows it plainly, and goes back to normal as soon as focus
  returns to an ordinary window.

**Non-goals**
- Making interception work over elevated windows. That needs an elevated core,
  which is opt-in and out of scope.
- Changing the latch. Per the operator, a latch held across an elevated window
  resumes on return, as now.

**Anti-goal.** A state that can drift. The overlay must not have to infer the
condition's end from other events.

## Approaches

- **A. `inert` field on the `mode` event, plus the SPEC diagnostic on entering.**
  *Chosen by the operator.* The core owns the rule, and the overlay reads the
  state.
- **B. `elevated` on the `focus` event, plus the diagnostic.** Rejected: the
  overlay would derive "inert" itself, spreading the rule across the core and
  the overlay.
- **C. Diagnostic only.** Rejected: a one-off message cannot tell the overlay
  when the condition ends.

## Design

**Core**
- **Detecting it.** On each focus change, the Windows window backend checks
  whether the focused window's process runs at a higher **integrity level**
  than the core's own process. That is the condition under which Windows
  (UIPI) withholds its input from the core's hook. If the integrity cannot be
  read, it is treated as *not* inert, so the overlay never claims inertness on
  a guess.
- **`mode` gains `inert: bool`.** The core publishes it, and re-publishes
  `mode` whenever `inert` changes. `get_state` carries it too.
- **The diagnostic.** On each change from not inert to inert, the core emits
  `input.elevated_window` (level `info`), as already defined in SPEC §11.
- **Other platforms.** Backends that cannot tell report `false`.

**Overlay**
- While `inert` is true:
  - the status text reads **"layer inactive: admin window has focus"**, with
    the reason in its tooltip;
  - the layer's key legends are drawn dimmed.
- When `inert` is false, both return to their normal state.

**SPEC**
- §5.3: add `inert` to the `mode` row, and add "inert changes" to its trigger.
- §8.2: point to `inert` as the way the UI learns it.

**Tests**
- **Core:** a fake window backend reporting elevated focus gives `inert: true`
  and one diagnostic. Leaving gives `inert: false` and no diagnostic.
- **Overlay:** a unit test checks that the status and dimming follow `inert`.
- **Live:** re-run M3 rows 8.1 and 8.2 with an admin Command Prompt.

**Left to the builder**
- Where the integrity check sits in the backend interface.
- How "dimmed" is drawn, reusing the overlay's existing dim styling if there
  is one.

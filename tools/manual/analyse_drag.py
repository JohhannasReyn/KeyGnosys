"""Judge drag-lock release TIMING, not merely balance (rows 5.4 and 5.5).

    analyse_drag.py CORE.jsonl MOUSE.csv

CORE.jsonl comes from record.py (the core's own stream); MOUSE.csv from
observe_mouse.ps1 (an independent WH_MOUSE_LL hook). Exit status: 0 PASS,
1 FAIL, 2 INCONCLUSIVE.

Why this exists. The planned check for these rows was `net down count == 0`,
and it PASSED on the row 5.5 failure: the button did come up, 9.215 s after the
layer was left, on an unrelated later keypress. Balance says nothing about when.
This script judges each drag-lock episode against the layer transition instead.

Two questions, one per instrument:

1. Core stream, one clock, judged by ORDER. An episode whose lock was still
   active when the layer went cursor -> normal must be released as part of that
   exit: no physical key press may lie between the transition and the release.
   The 5.5 failure had F12, CapsLock and G presses there. No time tolerance is
   needed or used on this side; the stream order is the evidence.

2. Independent hook, a second clock, judged by ALIGNMENT. The core cannot see
   its own injected button events, so the observer must confirm that each core
   `drag_lock` event is a real injected button edge. Events pair one-to-one in
   order. Every pair's clock offset (observer - core) is computed; the pairs not
   under test (every Down, and every in-layer toggle release) calibrate the
   offset, and their own spread is the tolerance -- floored at record.py's 1 ms
   timestamp resolution, and nothing wider. Each exit release must fall inside
   it. Fewer than two calibration pairs cannot establish an alignment, so the
   result is INCONCLUSIVE rather than a guess: run a lock/unlock cycle first.

Net balance is still reported, as a secondary sanity check. It is never
sufficient for PASS.
"""
import json
import statistics
import sys

RESOLUTION_S = 0.001   # record.py rounds its timestamps to 1 ms


def load_core(path):
    """Events in stream order: (t, kind, detail). kind is drag, mode or press."""
    events = []
    for line in open(path, encoding="utf-8"):
        line = line.strip()
        if not line:
            continue
        rec = json.loads(line)
        if "marker" in rec:        # an F12 release; its press is already a key event
            continue
        name, d = rec.get("n"), rec.get("d") or {}
        if name == "drag_lock":
            events.append((rec["t"], "drag", bool(d.get("active"))))
        elif name == "mode":
            events.append((rec["t"], "mode", d.get("mode")))
        elif name == "key" and d.get("state") == "down":
            events.append((rec["t"], "press", d.get("code")))
    return events


def load_mouse(path):
    """Injected left-button edges only: (seconds, 'D'|'U')."""
    out = []
    for line in open(path, encoding="utf-8"):
        parts = line.strip().split(",")
        if len(parts) == 3 and parts[2] == "1":
            out.append((float(parts[0]) / 1000.0, parts[1]))
    return out


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2
    core = load_core(argv[1])
    mouse = load_mouse(argv[2])
    failures, inconclusive = [], []

    # --- 1. episodes from the core stream --------------------------------
    transitions, mode = [], None
    for i, (_, kind, detail) in enumerate(core):
        if kind == "mode":
            if mode == "cursor" and detail == "normal":
                transitions.append(i)
            mode = detail

    drags = [i for i, e in enumerate(core) if e[1] == "drag"]
    episodes, open_at = [], None
    for i in drags:
        if core[i][2]:
            if open_at is not None:
                failures.append(f"t={core[i][0]:.3f}: a second lock began while one was open")
            open_at = i
        elif open_at is None:
            failures.append(f"t={core[i][0]:.3f}: a release with no lock open")
        else:
            episodes.append((open_at, i))
            open_at = None
    if open_at is not None:
        failures.append(f"t={core[open_at][0]:.3f}: lock NEVER released -- stranded button")

    def presses_between(a, b):
        lo, hi = min(a, b), max(a, b)
        return [core[k] for k in range(lo + 1, hi) if core[k][1] == "press"]

    exit_releases = set()
    print("drag-lock episodes (core stream):")
    for n, (down, up) in enumerate(episodes, 1):
        inside = [t for t in transitions if down < t < up]
        after = [t for t in transitions if t > up and not presses_between(up, t)]
        if inside:
            t = inside[0]
            between = presses_between(t, up)
            exit_releases.add(up)
            dt = (core[up][0] - core[t][0]) * 1000
            if between:
                codes = ", ".join(str(p[2]) for p in between)
                failures.append(f"episode {n}: released {dt:.0f} ms after the layer was "
                                f"left, AFTER unrelated presses ({codes})")
                verdict = "FAIL: not released at layer exit"
            else:
                verdict = "exit release, no press between"
            print(f"  {n}. lock {core[down][0]:.3f}  layer left {core[t][0]:.3f}  "
                  f"release {core[up][0]:.3f}  ({dt:+.0f} ms)  {verdict}")
        elif after:
            t = after[0]
            exit_releases.add(up)
            dt = (core[up][0] - core[t][0]) * 1000
            print(f"  {n}. lock {core[down][0]:.3f}  release {core[up][0]:.3f}  "
                  f"layer left {core[t][0]:.3f}  ({dt:+.0f} ms)  exit release, no press between")
        else:
            print(f"  {n}. lock {core[down][0]:.3f}  release {core[up][0]:.3f}  "
                  f"in-layer toggle (calibration)")

    if not exit_releases:
        inconclusive.append("no episode spanned a layer exit -- nothing to judge for row 5.5")

    # --- 2. alignment against the independent hook -----------------------
    core_edges = [(core[i][0], "D" if core[i][2] else "U", i) for i in drags]
    print(f"\ncore drag_lock events: {len(core_edges)}   observer injected edges: {len(mouse)}")
    if [e[1] for e in core_edges] != [m[1] for m in mouse]:
        failures.append("instruments disagree: the core's drag_lock sequence "
                        f"{''.join(e[1] for e in core_edges)} does not match the "
                        f"observer's injected sequence {''.join(m[1] for m in mouse)}")
    else:
        pairs = [(m[0] - c[0], c) for c, m in zip(core_edges, mouse)]
        calib = [off for off, c in pairs if c[2] not in exit_releases]
        if len(calib) < 2:
            inconclusive.append(f"only {len(calib)} calibration pair(s); need 2 -- "
                                "run a lock/unlock cycle inside the layer first")
        else:
            centre = statistics.median(calib)
            tol = max(max(calib) - min(calib), RESOLUTION_S)
            print(f"clock offset (observer - core): median {centre:.3f} s, calibration "
                  f"spread {(max(calib) - min(calib)) * 1000:.1f} ms, tolerance "
                  f"{tol * 1000:.1f} ms")
            for off, c in pairs:
                resid = (off - centre) * 1000
                tag = "EXIT " if c[2] in exit_releases else "calib"
                ok = abs(off - centre) <= tol
                print(f"  {tag} {c[1]} core {c[0]:.3f}  offset {off:.3f}  "
                      f"residual {resid:+.1f} ms  {'ok' if ok else 'OUTSIDE'}")
                if c[2] in exit_releases and not ok:
                    failures.append(f"exit release at core t={c[0]:.3f} is not confirmed by "
                                    f"the observer: residual {resid:+.1f} ms, tolerance "
                                    f"{tol * 1000:.1f} ms")

    balance = sum(1 if m[1] == "D" else -1 for m in mouse)
    print(f"\nnet injected down count (secondary, NOT a pass criterion): {balance}")
    if balance != 0:
        failures.append(f"net injected down count is {balance}: a button is still down")

    if failures:
        print("\nFAIL")
        for f in failures:
            print("  - " + f)
        return 1
    if inconclusive:
        print("\nINCONCLUSIVE")
        for f in inconclusive:
            print("  - " + f)
        return 2
    print("\nPASS: every lock active at a layer exit was released as part of that exit, "
          "and the independent hook confirms each release edge")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

# M3 Completion — Windows Manual Validation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Finish the Windows manual matrix on the repaired M3 implementation, fix any defect it exposes, and promote the M3 status only once the matrix genuinely passes.

**Architecture:** Most remaining work is **operator-in-the-loop validation**, not code. Each task arms an instrument, has the operator perform physical actions, verifies the result from captured evidence rather than from how it felt, records the row, and commits. Code changes appear only where a row fails; those follow ordinary TDD. The branch already carries the two real defects this milestone found (hook never dispatched, grace deadline carried ~21 ms of timer artifact) plus one it correctly declined to build (hook-loss recovery, which would not reproduce).

**Tech Stack:** C++17, CMake 3.20+, Ninja, UCRT64 MinGW; Win32 (`user32`, `dwmapi`, `shcore`); Python 3.12 + PySide6 6.11.2 for the overlay; the in-repo `kgn_test.hpp` harness; PowerShell 5.1/7 for the low-level hook observers.

**Spec:** `docs/SPEC.md` (§6.3 engine, §6.4 motion, §7 actions, §8.2 Windows input, §11 diagnostics, §15.10 `grace_ms` semantics) · `docs/manual-tests.md` (the canonical procedure) · `docs/superpowers/specs/2026-08-28-m3-threading-design.md` (corrected §6) · `docs/manual-test-logs/2026-08-30-m3-windows.md` (everything observed so far)

---

## Global Constraints

- **Branch:** originally `fix/m3-hook-message-pump`, which merged into `main` as PR #7 (`c1e6794`, 2026-09-05); its no-merge constraint is discharged. Remaining work runs on a narrow branch from updated `main` (`fix/m3-manual-validation`). **Do not start M4. Do not rewrite existing commits.**
- **P6 — degrade loudly.** An unavailable capability is reported and disabled, never emulated.
- **P7 — never strand a key.** Every suppressed or synthesized press has a guaranteed matching release on every exit path.
- **A row is PASS only when its expected result was actually observed.** Never infer a pass from an adjacent row, from automated tests, or from plausibility. `NOT RUN` with a reason is a valid, honest outcome; a speculative PASS is not.
- **`grace_ms` stays at 50.** It is the human ambiguity window (SPEC §15.10) and must not be tuned to compensate for implementation latency. Changing it needs the M5 calibration data, not a hunch.
- **Build/run commands must carry the UCRT64 prefix under MSYS2/Git Bash:**
  ```sh
  PATH=/c/msys64/ucrt64/bin:$PATH cmake --build --preset debug
  PATH=/c/msys64/ucrt64/bin:$PATH ./build/default/core/keygnosys-core.exe
  ```
  Without it a UCRT64-linked binary loads MINGW64's `libstdc++-6` and segfaults before printing its banner. The optimized build dies reliably; `debug` survives, which makes it look like a product defect.
- **C++17.** Warnings are errors under the `debug` preset: `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Werror`.
- **No network access, ever.** No telemetry, no update check.
- Verification commands, run before every commit that touches code:
  ```sh
  PATH=/c/msys64/ucrt64/bin:$PATH cmake --build --preset debug   && PATH=/c/msys64/ucrt64/bin:$PATH ctest --preset debug
  PATH=/c/msys64/ucrt64/bin:$PATH cmake --build --preset default && PATH=/c/msys64/ucrt64/bin:$PATH ctest --preset default
  wsl -d Ubuntu-24.04 -- bash -lc 'cd /mnt/c/Projects/KeyGnosys && cmake -S . -B /tmp/kgnlnx -DCMAKE_BUILD_TYPE=Debug -DKGN_BUILD_TESTS=ON -DKGN_WERROR=ON && cmake --build /tmp/kgnlnx -j4 && cd /tmp/kgnlnx && ctest'
  .venv/Scripts/python.exe -m pytest -q
  git diff --check
  ```
  Baseline at `d7adc38`: Windows **16/16** both presets, Linux **12/12**, pytest **90**, tree clean.

---

## Method Rules — learned the expensive way, do not relearn them

These cost roughly a day of this milestone. They are constraints, not advice.

1. **Validate an instrument against a known-positive before believing a negative from it.** A capture client received its first line, went deaf, and stayed alive — producing five convincing false negatives and two wrong conclusions that had to be retracted. Trigger a signal you know should appear; confirm the tool reports it; only then trust a zero.
2. **`kgn_hook_smoke` is the mandatory pre-check.** `PATH=/c/msys64/ucrt64/bin:$PATH ./build/default/core/kgn_hook_smoke.exe` — installs the real hook and reports PASS on the first genuine keystroke, in twelve seconds. Run it before any session of matrix rows. It exists because a dead hook was once discovered forty rows in. **The operator launches it locally after the brief; an agent never launches it and relays "press a key now" through chat** — relay latency lands inside the twelve-second window, and on 2026-09-13 exactly that produced a no-stimulus `FAIL` that was not counted. A timeout with no key pressed is inconclusive: re-run, conclude nothing.
3. **Injected events are invisible to the core's own `key` stream, by design** (`hook_input.cpp` skips `LLKHF_INJECTED`). Anything the core synthesizes — grace replays, `release_all` output — can only be seen by an *independent* hook observer. Never conclude "the core did nothing" from the absence of a synthetic event in its own stream.
4. **The operator types their replies on the same keyboard.** Every capture is polluted with prose. Filter by keycode and pair one-to-one; never assume a capture contains only the requested gesture. A ±100 ms pairing window once let one `CapsLock` press partner several `J` presses and invented 21 attempts out of 10.
5. **Never use a fixed short capture window.** The operator is a human with a life. Arm instruments for hours, flush incrementally, and expose a heartbeat so liveness is checkable *during* the run. A window that expires before the operator is ready writes an empty file, which reads exactly like a real negative.
6. **The emergency exit must never require typing.** While the layer is latched every unbound non-modifier key is swallowed, so `taskkill` cannot be typed and `Ctrl+C` never reaches a console. Use `Ctrl+Alt+Del` → Task Manager (OS-guaranteed, mouse-only) or the panic shortcut from Task 1.

---

## File Structure

| File | Responsibility |
|---|---|
| `tools/manual/README.md` | How to run the manual matrix: prerequisites, the safety exits, which tool answers which question |
| `tools/manual/kgn.py` | IPC client — send one command and print the reply, or stream events to a file |
| `tools/manual/record.py` | Event recorder: timestamps every core event, `F12` delimits segments, writes a heartbeat |
| `tools/manual/observe_keys.ps1` | Independent `WH_KEYBOARD_LL` observer; logs timestamp, vk, injected flag, direction |
| `tools/manual/observe_mouse.ps1` | Independent `WH_MOUSE_LL` observer; the only way to see a button left physically down |
| `tools/manual/panic.ps1` | Emergency exit: kill the core, then force-release every modifier and mouse button |
| `tools/manual/analyse_chords.py` | Strict one-to-one chord pairing and leak detection |
| `docs/manual-test-logs/2026-08-30-m3-windows.md` | The run log — appended per task, never rewritten to look tidier |

Existing product files are modified only if a row fails.

---

# Task 1: Preserve the validation harness

The instruments that made this milestone tractable live in a **session-scoped scratchpad** and will be lost when that session ends. `panic.ps1` in particular is the emergency exit `docs/manual-tests.md` now mandates, and the matrix currently describes it without shipping it.

Deliberately **not** preserved: `watch.ps1` (the broken client — it must not be reachable), `chord.py`, `hookprobe.ps1`, `hookobs.ps1`, `poke.ps1` (all superseded or one-off).

**Files:**
- Create: `tools/manual/README.md`, `tools/manual/kgn.py`, `tools/manual/record.py`, `tools/manual/observe_keys.ps1`, `tools/manual/observe_mouse.ps1`, `tools/manual/panic.ps1`, `tools/manual/analyse_chords.py`
- Modify: `docs/manual-tests.md` — point the Setup section at `tools/manual/`

**Interfaces:**
- Consumes: nothing.
- Produces: `kgn.py send <command> [json]`, `kgn.py watch <seconds> <outfile>`, `record.py --out FILE --timeout S`, `observe_keys.ps1 -Seconds N -Out FILE` (CSV `elapsed_ms,vk,injected,D|U`), `observe_mouse.ps1 -Seconds N -Out FILE` (CSV `elapsed_ms,D|U,injected`), `analyse_chords.py <keylog.csv> [window_ms]`.

- [ ] **Step 1: Recover the harnesses from the previous session's scratchpad**

The previous session's scratchpad is:
`C:/Users/Johha/AppData/Local/Temp/claude/C--Projects-KeyGnosys/050eadc1-1831-49e4-a6d5-28cdd58c1142/scratchpad`

If it still exists, copy `kgn.py`, `record.py`, `grace.ps1` (→ `observe_keys.ps1`), `mouse.ps1` (→ `observe_mouse.ps1`), `panic.ps1`, `chord2.py` (→ `analyse_chords.py`). If it has been cleaned, rewrite them from the contracts in the Interfaces block above — the run log documents what each measured and why.

- [ ] **Step 2: Verify each tool against a known-positive before committing it**

Per Method Rule 1, a tool that has not been shown to report a signal it should see is not yet evidence.

```sh
PATH=/c/msys64/ucrt64/bin:$PATH ./build/default/core/keygnosys-core.exe &
sleep 3
.venv/Scripts/python.exe tools/manual/kgn.py watch 8 /tmp/probe.jsonl &
sleep 2
.venv/Scripts/python.exe tools/manual/kgn.py send set_activation_mode '{"mode":"toggle"}'
.venv/Scripts/python.exe tools/manual/kgn.py send set_activation_mode '{"mode":"hybrid"}'
sleep 7
grep -c '"n": *"mode"' /tmp/probe.jsonl
```

Expected: at least 2 `mode` events. A result of 0 means the client is deaf — the exact failure that cost this milestone five false negatives.

- [ ] **Step 3: Verify `panic.ps1` runs clean**

```sh
powershell -NoProfile -ExecutionPolicy Bypass -File tools/manual/panic.ps1 < /dev/null
```

Expected: reports whether the core was running, then "released modifiers and mouse buttons". It must not throw.

- [ ] **Step 4: Create the desktop shortcut and confirm it exists**

```sh
powershell -NoProfile -Command "
\$d=[Environment]::GetFolderPath('Desktop'); \$w=New-Object -ComObject WScript.Shell
\$s=\$w.CreateShortcut((Join-Path \$d 'KEYGNOSYS PANIC.lnk'))
\$s.TargetPath='powershell.exe'
\$s.Arguments='-NoProfile -ExecutionPolicy Bypass -File C:\Projects\KeyGnosys\tools\manual\panic.ps1'
\$s.IconLocation='shell32.dll,131'; \$s.Save()
Test-Path (Join-Path \$d 'KEYGNOSYS PANIC.lnk')"
```

Expected: `True`.

- [ ] **Step 5: Write `tools/manual/README.md`**

It must state: the UCRT64 PATH requirement; the three emergency exits in order (`Ctrl+Alt+Del` → Task Manager, the panic shortcut, `release_all` over IPC); the six Method Rules above; and a table mapping question → tool ("did the core see the key?" → `record.py`; "did the core synthesize output?" → `observe_keys.ps1`; "is a mouse button still down?" → `observe_mouse.ps1`).

- [ ] **Step 6: Point the matrix at the committed tools**

In `docs/manual-tests.md`, replace the description of a "pre-made desktop shortcut" with a reference to `tools/manual/panic.ps1` and the Step 4 command.

- [ ] **Step 7: Commit**

```bash
git add tools/manual docs/manual-tests.md
git commit -m "Ship the manual-validation harness instead of describing it"
```

---

# Task 2: Rows 5.4 and 5.5 — drag lock (⚠ P7)

The last two button rows, and the first that can leave a **mouse button physically held down**. `observe_mouse.ps1` is the instrument: the core's own event stream cannot show a stranded button, because the button was synthesized and synthetic events are invisible to it (Method Rule 3).

`KeyG` is `button.drag_lock` (left) in `data/bindings/default.json`.

**Files:**
- Modify: `docs/manual-test-logs/2026-08-30-m3-windows.md`
- Product files only if a row fails.

**Interfaces:**
- Consumes: `tools/manual/observe_mouse.ps1`, `tools/manual/panic.ps1` from Task 1.
- Produces: recorded results for rows 5.4 and 5.5.

- [ ] **Step 1: Pre-check the hook**

```sh
PATH=/c/msys64/ucrt64/bin:$PATH ./build/default/core/kgn_hook_smoke.exe
```

The **operator** runs this from their own terminal on the machine under test, then presses a letter key (Method Rule 2). The agent briefs, waits, and reads back the output the operator reports.

Expected: `PASS: N physical record(s) observed.` Retry if the operator was not pressing keys; three consecutive failures with the operator actively typing means stop and investigate the pump before running any row.

> **Corrected 2026-09-13 (finding F-1).** Steps 4 and 6 originally judged these rows by `net down count == 0`. That check **passed on the real 5.5 failure** — the button came up 9.215 s late, on an unrelated keypress. Both rows are now judged on timing by `tools/manual/analyse_drag.py`; balance survives only as a secondary check.

- [ ] **Step 2: Arm the core, the recorder and the mouse observer**

Captures go to an **absolute Windows path** (`CAP` below), never `/tmp`: the observer runs under PowerShell, where `/tmp` is `C:\tmp`, while Git Bash reads MSYS2's. Launched by the agent — nothing here has a short window.

```sh
CAP='C:/Projects/KeyGnosys/.superpowers/sdd/2026-09-04-m3-completion/captures'   # git-ignored
mkdir -p "$CAP"
PATH=/c/msys64/ucrt64/bin:$PATH ./build/default/core/keygnosys-core.exe > "$CAP/core.log" 2>&1 &
sleep 3
powershell -NoProfile -ExecutionPolicy Bypass -File tools/manual/observe_mouse.ps1 -Seconds 28800 -Out "$CAP/drag.csv" > "$CAP/observer.out" 2>&1 &
.venv/Scripts/python.exe tools/manual/record.py --out "$CAP/drag.jsonl" --timeout 28800 > "$CAP/record.out" 2>&1 &
```

Then validate **both** instruments before believing anything (Method Rule 1):
- `observer.out` reads `mouse probe: ok 0x…`; `record.out` reads `recording to …`, and `drag.jsonl.hb` advances.
- Recorder known-positive: `kgn.py send set_activation_mode '{"mode":"hybrid"}'` must add a `mode` line to `drag.jsonl`.
- Observer known-positive: the operator makes one **physical** left click; `drag.csv` must gain a `D`/`U` pair with `injected=0`.

- [ ] **Step 3: Brief the operator on row 5.4 and the calibration cycle, and on recovery**

> Park the pointer over empty desktop. Engage the layer with `CapsLock`. Press `G` — the left button locks **down**. Move the pointer with `H`/`J`/`K`/`L` (across a file icon, for 5.4). Press `G` again — the button releases.
>
> If the button ever stays down: click once with the real mouse; if that does not clear it, double-click **KEYGNOSYS PANIC**. Killing the core alone does **not** release a `SendInput` button-down.

This lock/unlock cycle is also the **clock calibration** for 5.5: it gives `analyse_drag.py` the aligned pairs it needs. Run it in the same session as 5.5, before it.

- [ ] **Step 4: Verify 5.4 from the capture**

```sh
.venv/Scripts/python.exe tools/manual/analyse_drag.py "$CAP/drag.jsonl" "$CAP/drag.csv"
```

Expected for 5.4: the episode is reported as an in-layer toggle, released on the second `G`, with the core and observer sequences matching. The operator confirms the file moved. Any `FAIL` is a P7 failure — stop, record it, do not continue to 5.5.

- [ ] **Step 5: Brief the operator on row 5.5 — the auto-release row**

> Still in the layer, press `G` to lock the button down. Move a little with `H`/`J`/`K`/`L`. **Do not press `G` again.** Tap `CapsLock` to leave the layer. Then **stop** — press nothing else until told.

- [ ] **Step 6: Verify 5.5 — timing, not balance**

Re-run the Step 4 command. **PASS requires exit status 0**, meaning all of:
- in the core stream, **no key press** lies between the `mode` cursor→normal transition and `drag_lock {active:false}` — the release belongs to the exit, not to a later key;
- the observer's injected `U` pairs one-to-one with that release, and its clock offset lies within the tolerance measured from the session's own calibration pairs (floored at the recorder's 1 ms resolution, nothing wider);
- no lock is left open, and the sequences from both instruments agree.

The net injected down count is printed and must be 0, but it is **not** sufficient: it was 0 on the real failure. Exit status 2 (INCONCLUSIVE) means fewer than two calibration pairs — re-run with the Step 3 cycle, do not guess.

Also confirm the software state is clean — `mode: normal`, `latched: false`, every modifier false:

```sh
.venv/Scripts/python.exe tools/manual/kgn.py send get_state | grep REPLY | grep -o '"mode": "[a-z]*", "latched": [a-z]*\|"modifiers": {[^}]*}'
```

`get_state` carries **no** `drag_lock` field; an earlier version of this step grepped for one and printed nothing in every state. A held button is judged by the independent hook, never by this reply.

- [ ] **Step 7: Record both rows in the log with the evidence**

Append a block giving the full `analyse_drag.py` output for each row: the episode timeline, the transition-to-release delta, the calibration spread and each residual, and the net count as a secondary figure. If either failed, write the failure and its evidence — do not soften it. Then clean up: no core, observer or recorder left running (list command lines; a `-match 'observe_'` query matches itself), and `GetAsyncKeyState` clear on `0x01/0x02/0x04/0x10/0x11/0x12/0x5B`.

- [ ] **Step 8: Commit**

```bash
git add docs/manual-test-logs
git commit -m "Record drag lock rows 5.4 and 5.5"
```

---

# Task 3: Section 6 — warp across a multi-monitor desktop

The strongest section on this hardware. The rig is three monitors with `\\.\DISPLAY3` at **x = −3082**, so the virtual desktop spans negative coordinates — the case SPEC §6 calls out as the one primary-only normalisation cannot express at all.

Bindings: `Numpad1`–`Numpad9` are `warp.grid` cells 7,8,9 / 4,5,6 / 1,2,3; `Quote` is `warp.corner` centre.

**Files:**
- Modify: `docs/manual-test-logs/2026-08-30-m3-windows.md`
- Product files only if a row fails.

**Interfaces:**
- Consumes: `tools/manual/kgn.py`, `tools/manual/record.py`.
- Produces: recorded results for rows 6.1–6.7.

- [ ] **Step 1: Capture the monitor topology as the baseline**

```sh
.venv/Scripts/python.exe tools/manual/kgn.py send get_state | python -c "
import sys,json
d=json.loads([l for l in sys.stdin if l.startswith('REPLY')][0][6:])['d']
for m in d['monitors']: print(m)"
```

Record the output in the log. Row 6.3 is only meaningful if a monitor has a negative origin; confirm it does before claiming the row exercised anything.

- [ ] **Step 2: Arm the recorder**

```sh
.venv/Scripts/python.exe tools/manual/record.py --out /tmp/warp.jsonl --timeout 28800 > /dev/null 2>&1 &
```

`pointer` events carry `{x, y, monitor}` while the layer is engaged, so warp destinations are checkable from the capture rather than by eye.

- [ ] **Step 3: Rows 6.1–6.5, one segment each**

Brief the operator: press `F12` between rows, engage the layer, then

- **6.1** pointer on the primary, warp to each of the nine `Numpad` cells
- **6.2** move the pointer to `DISPLAY2`, repeat
- **6.3** move the pointer to **`DISPLAY3`** (the negative-origin one), repeat
- **6.4** `Quote` (centre) plus the four corner cells on each monitor
- **6.5** repeat `warp.monitor` next until it has visited every monitor and wrapped

- [ ] **Step 4: Verify each warp landed on the monitor the pointer was on**

```sh
.venv/Scripts/python.exe -c "
import json,collections
segs=collections.defaultdict(list)
for l in open('/tmp/warp.jsonl'):
    r=json.loads(l)
    if r.get('n')=='pointer': segs[r['seg']].append((r['d']['x'],r['d']['y'],r['d'].get('monitor')))
for s in sorted(segs):
    pts=segs[s]
    print(f'seg {s}: {len(pts)} points, monitors={sorted(set(p[2] for p in pts))}')
    print(f'   x range {min(p[0] for p in pts)} .. {max(p[0] for p in pts)}')"
```

Expected: the segment for 6.3 shows **negative x values** and stays on `DISPLAY3`'s monitor index. A 6.3 segment whose points land on the primary is the classic bug and a **FAIL**.

- [ ] **Step 5: Row 6.6 — live topology change**

Have the operator change the display arrangement in Windows Settings while the core runs, then warp again. Confirm a `monitors` event was emitted:

```sh
grep -c '"n": *"monitors"' /tmp/warp.jsonl
```

Expected: at least one after the change, and warps use the new topology. Have the operator restore the original arrangement afterwards.

- [ ] **Step 6: Row 6.7 — mixed DPI**

Have the operator set two monitors to different scaling (e.g. 100% and 150%), warp between them, then restore. Expected: the pointer lands where expected on both. Record the scaling values used.

- [ ] **Step 7: Record rows 6.1–6.7 and commit**

```bash
git add docs/manual-test-logs
git commit -m "Record section 6: warp across a negative-origin multi-monitor desktop"
```

---

# Task 4: Section 7 — window management and slots

Row 7.4 was already observed incidentally (10 windows, the tenth carrying `index: null`) and is recorded. The rest of the section is unrun.

**Files:**
- Modify: `docs/manual-test-logs/2026-08-30-m3-windows.md`
- Product files only if a row fails.

**Interfaces:**
- Consumes: `tools/manual/kgn.py`.
- Produces: recorded results for rows 7.1–7.3, 7.5–7.11.

- [ ] **Step 1: Snapshot the slot list**

```sh
.venv/Scripts/python.exe tools/manual/kgn.py send get_state | python -c "
import sys,json
d=json.loads([l for l in sys.stdin if l.startswith('REPLY')][0][6:])['d']
for w in d['windows']: print(f\"  {str(w['index']):5} {w['process'][:24]:24} mon={w['monitor']} {w['title'][:40]}\")"
```

- [ ] **Step 2: Rows 7.1–7.4 — slot stability**

7.1 every real window has a stable index. Then have the operator **close a middle window** and re-snapshot: 7.2 requires the remaining windows to **keep their indices** and the freed slot to be empty. Then **open a new window**: 7.3 requires it to take the **lowest free** index. 7.4 is already recorded; re-confirm it still holds.

Slot stability is the whole point of `slots.cpp` — a renumbering here means muscle memory breaks every time a window closes.

- [ ] **Step 3: Rows 7.6–7.8 — focus**

Engage the layer. `Digit1`–`Digit9` focus slots; `KeyN`/`KeyM` cycle prev/next; `Comma`/`Period` focus prev/next monitor.

- **7.6** press the slot key for a window on **another monitor** → that window is focused
- **7.7** cycle forward and back → focus follows **slot order, not recency**
- **7.8** focus a monitor → its topmost window is focused and the pointer follows

Verify focus changes from the event stream:

```sh
grep '"n": *"focus"' /tmp/win.jsonl | tail -20
```

- [ ] **Step 4: Rows 7.9–7.11 — moving windows**

`KeyB`/`Slash` are `window.move_to_monitor` prev/next.

- **7.9** move a normal window to another monitor → arrives at the same relative position, correctly scaled
- **7.10** **maximise** a window, then move it → arrives **still maximised**, not restored and not half off-screen
- **7.11** repeat both between monitors of **different DPI** → the window keeps its apparent size

7.10 and 7.11 are where Win32 window placement usually goes wrong; treat a "looks about right" as NOT RUN unless the operator can state it landed correctly.

- [ ] **Step 5: Row 7.5 — cloaked windows**

Open a UWP app (Settings or Calculator), then close it to the tray or switch away. Re-snapshot the slot list. Expected: **no invisible ghost window** appears. This is what the cloaked-window check exists for.

- [ ] **Step 6: Record and commit**

```bash
git add docs/manual-test-logs
git commit -m "Record section 7: window slots, focus and cross-monitor moves"
```

---

# Task 5: Section 8 — documented limitations

These rows assert that known limits **behave as specified rather than being "fixed"**. Row 8.5 is already recorded; §8.2's third limitation (silent hook removal, added in `046faa0`) must now appear there too.

**Files:**
- Modify: `docs/manual-test-logs/2026-08-30-m3-windows.md`

- [ ] **Step 1: Row 8.5 — re-verify the limitation list, now three entries**

```sh
.venv/Scripts/python.exe tools/manual/kgn.py send ping 2>&1 | head -3
```

Expected three limitations: elevated-window inertness, `Ctrl+Alt+Del` never interceptable, and silent hook removal being undetectable. The third was added when SPEC §8.2's unimplementable `MUST` was withdrawn.

- [ ] **Step 2: Rows 8.1–8.2 — elevated window**

Have the operator open an **administrator** Command Prompt (accepting the UAC prompt), focus it, and engage the layer. Expected: interception is **inert** while it has focus, nothing crashes, nothing is stranded. Then focus an ordinary window (8.2): the layer works again **immediately**.

- [ ] **Step 3: Row 8.3 — the Secure Attention Sequence**

Have the operator press `Ctrl+Alt+Del` and then cancel. Expected: the secure screen appears normally; the core does not intercept it. **This is correct behaviour, not a defect** — and it is also the emergency exit the matrix depends on, so a failure here would be serious in both directions.

- [ ] **Step 4: Row 8.4 — the `Fn` key**

Have the operator press `Fn` on the laptop keyboard. Expected: **nothing is reported** — `Fn` is handled in keyboard firmware and produces no scancode the OS can see, which is why `scancode_keymap.cpp` deliberately omits it. Confirm from a capture that no `key` event appears.

- [ ] **Step 5: Record and commit**

```bash
git add docs/manual-test-logs
git commit -m "Record section 8: the limitations behave as specified"
```

---

# Task 6: Section 9 — hook overrun behaviour

This section was **rewritten** in `046faa0` after a controlled experiment failed to reproduce silent hook removal at overruns of 2500 ms and 6500 ms. It no longer demands an unobservable automatic-reinstall branch.

**Files:**
- Modify: `docs/manual-test-logs/2026-08-30-m3-windows.md`

- [ ] **Step 1: Row 9.1 — survival under load**

Have the operator start a heavy build or export, then type with the layer engaged for a minute. Expected: the process stays alive and the desktop stays usable. **If interception continues**, record PASS for survival on this build. **If it stops**, record the observed limitation and stop — do **not** record a failure of automatic recovery, because none is claimed.

- [ ] **Step 2: Row 9.2 — record the limitation**

Confirm `hello.limitations` carries the silent-removal entry (already verified in Task 5, Step 1) and that it reads as a platform constraint rather than a defect. This row is recordable, not executable.

- [ ] **Step 3: Row 9.3 — stranded-key check**

Only meaningful if interception stopped during 9.1. If it did, confirm immediately that no key or button is stuck. A hook that stops being called cannot strand anything by itself — the OS never saw a suppressed press — but this is the row that would catch it if the reasoning is wrong.

- [ ] **Step 4: Record and commit**

```bash
git add docs/manual-test-logs
git commit -m "Record section 9: hook survives overrun on this build"
```

---

# Task 7: Section 10 — shutdown, disable and reload safety (⚠ every row)

**Every row here is a P7 test, and a failure leaves a key or a button held.** Read the emergency-exit rules before starting.

> **Revised 2026-09-13.** The original steps could not be executed as written: `Ctrl+C` cannot reach a console while the layer is engaged, the operator has no free hand to type or aim a mouse while holding the keys, commands "sent from a second terminal" put the agent's timing inside the hold, and the captures went to `/tmp`. Each row now fires through `tools/manual/delayed.ps1`, which the **operator** launches locally before taking up the hold (Method Rule 2 / README rule 7).

**Files:**
- Modify: `docs/manual-test-logs/2026-08-30-m3-windows.md`
- Product files only if a row fails — and a P7 failure is a stop-everything defect.

**Instruments, and what each one proves:**

| Question | Instrument |
|---|---|
| Is the button / a modifier held **in the OS**, and when did it change relative to the fire? | `<log>.samples.csv` from `delayed.ps1` — `GetAsyncKeyState` and pointer position every ~10 ms, on the same QPC clock as the fire |
| Was a change **injected** by the core, or physical? | `observe_mouse.ps1` (left button) and `observe_keys.ps1` |
| Did the core exit cleanly or get terminated? | the core exit code `delayed.ps1` logs (0 = unwound; `0xC000013A` = ended by the console subsystem; 1 = `taskkill /F`) |
| Did the command apply? | the reply `delayed.ps1` logs — `ok:true` means **applied** (SPEC §5.4) |
| What did the core see? | `record.py`, one per core instance |

**Stranded means positive.** For `observe_keys.ps1`, count injected `D` minus injected `U` per key. A **positive** count is a stranded synthetic key. A **negative** count is expected wherever the core unwinds a natively-forwarded press (e.g. a held `Shift` under `release_all`): it injects an up for a down the OS saw physically. The OS state in `samples.csv` and a final `GetAsyncKeyState` read are the ground truth.

**The standard hold** (unless a row says otherwise): pointer parked on empty desktop, near the top. **Tap** `CapsLock` (layer latched), then hold `D` (left click — the button goes down) with the left hand and `J` (pointer moves down) with the right. Keep holding through `FIRE` and for about two seconds after, then release everything.

- [ ] **Step 1: Prepare the capture directory and the 10.6 fixture**

```sh
CAP='C:/Projects/KeyGnosys/.superpowers/sdd/2026-09-04-m3-completion/captures/2026-09-13-s10'   # git-ignored
mkdir -p "$CAP/config/bindings"
.venv/Scripts/python.exe -c "
import json, sys
d = json.load(open('data/bindings/default.json', encoding='utf-8'))
d['id'] = 'm3-s10-unbound'; d['name'] = 'M3 section 10: default without KeyD'
del d['bindings']['KeyD']
json.dump(d, open(sys.argv[1], 'w', encoding='utf-8'), indent=2)" "$CAP/config/bindings/m3-s10-unbound.json"
```

- [ ] **Step 2: Arm both observers — before any core, and validate them**

```sh
powershell -NoProfile -ExecutionPolicy Bypass -File tools/manual/observe_keys.ps1  -Seconds 28800 -Out "$CAP/keys.csv"  > "$CAP/keys.out"  2>&1 &
powershell -NoProfile -ExecutionPolicy Bypass -File tools/manual/observe_mouse.ps1 -Seconds 28800 -Out "$CAP/mouse.csv" > "$CAP/mouse.out" 2>&1 &
```

Both `.out` files must report the probe `ok`. Known-positives, with **no core running** so nothing is suppressed: the operator presses one letter and clicks the real left button once. `keys.csv` must show that letter `D`/`U` with `injected=0`; `mouse.csv` a `D`/`U` with `injected=0`. Only then is a zero from either believed.

Note the hook order: an observer installed *before* a core is called *after* it, so while a core runs the key observer sees only what the core passes on — native passthrough and everything injected. That is what the stranded-key question needs, and it is why the physical known-positive is taken with no core running.

- [ ] **Step 3: Launch each core alone in a classic console — agent-launched, no keystroke**

```powershell
$env:PATH = 'C:\msys64\ucrt64\bin;' + $env:PATH
Start-Process conhost.exe -ArgumentList @('C:\Projects\KeyGnosys\build\default\core\keygnosys-core.exe', '--config-dir', '<CAP>\config')
```

**Nothing else may share that console.** A PowerShell wrapper (`& core | Tee-Object`) receives the same `CTRL_C_EVENT` and terminates the core mid-unwind: exit code `-1` in the 2026-09-13 self-test, against `0` for the core alone. Then start `record.py --out "$CAP/core-<n>.jsonl"` and take its known-positive (`set_activation_mode hybrid` must add a `mode` line) **before** telling the operator the row is ready — that command drops an engaged layer.

Rows 10.1 and 10.2 end the core; relaunch (and re-arm the recorder) before the next row.

- [ ] **Step 4: Each row — the operator fires it**

The operator runs, in **their own** terminal (not the core's console):

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File C:\Projects\KeyGnosys\tools\manual\delayed.ps1 -Do <action> -Seconds 8 -After 6 -Log <CAP>\10.<n>.log
```

then takes up the hold during the countdown. Cancel = close that window with the mouse. The agent reads `10.<n>.log`, `10.<n>.log.samples.csv`, the observers and the recorder.

| Row | `-Do` | Hold | Expected, as evidence |
|---|---|---|---|
| 10.1 | `ctrl-c-core` | standard | Core exit code **0**. `lbutton` 1 → 0 at the unwind, pointer stops, injected mouse `U`, no positive key count. **Substitution:** keyboard `Ctrl+C` is not executable with the layer engaged (`C` is swallowed); `CTRL_C_EVENT` is delivered by `GenerateConsoleCtrlEvent`, the same event the console raises for the keystroke. |
| 10.2 | `kill-core` | standard | taskkill exit 0; core exit code 1. Record the OS button state after the operator releases. **Predicted risk:** only natively-forwarded presses are crash-safe (threading design §3). The left button is a *synthetic* press, and no process can release it after `TerminateProcess` — expect it to stay down until a physical click. Record what is observed; do not pre-judge the row. |
| 10.3 | `set-enabled-false` | standard | `ok:true`; `lbutton` 1 → 0 and pointer motion stops within the reply latency; nothing held after release. |
| 10.4 | (agent sends `set_enabled true`; no hold) | — | `ok:true`; then the operator uses the layer normally (tap `CapsLock`, move, click, leave): mode events, balanced injected pairs, no leftover state. |
| 10.5 | `reload-config` | standard | `ok:true`; button up and motion stopped at the fire; nothing re-pressed while the keys are still held; nothing held after release. |
| 10.6 | `set-bindings-unbound` | standard | `ok:true`; the `D` click (unbound in the new document) is released at the fire, not left held by a binding that no longer exists. Afterwards the agent restores `set_bindings {id: default}`. |
| 10.7 | `release-all` | standard **plus left `Shift`** | `ok:true`; button up, motion stopped, `Shift` up in the OS at the fire (an injected `Shift U` with no injected `D` is expected). |
| 10.8 | `drop-client` | standard | The client process is terminated at the fire; `delayed.ps1` then pings the core `ok`. Record whether the hold survives the disconnect; nothing held after release. |
| 10.9 | — | — | After every row: `GetAsyncKeyState` clear on `0x01 0x02 0x04 0x10 0x11 0x12 0x5B`, no positive injected key count, and once at the end the on-screen keyboard (`osk.exe`) shows no modifier latched. |

**Do not use the console close button as an equivalent of 10.1.** `CTRL_CLOSE_EVENT` enters the same handler as `CTRL_C_EVENT`, but Windows ends the process as soon as that handler returns: exit code `0xC000013A` in 3 of 3 self-test runs, against `0` for `CTRL_C_EVENT`. See finding O-3 in the log. `delayed.ps1 -Do close-core-console` exists to test that path deliberately, not to stand in for 10.1.

- [ ] **Step 5: Clean up after the section**

Stop every recorder, both observers and any core. List command lines to confirm (a `-match 'observe_'` query matches itself), and read `GetAsyncKeyState` clear.

- [ ] **Step 6: Record and commit**

Per row: the `delayed.ps1` log lines (fire, reply or exit code), the `samples.csv` transition times relative to the fire, the observer counts, and the final OS state. Record substitutions as substitutions.

```bash
git add docs/manual-test-logs
git commit -m "Record section 10: shutdown, disable and reload safety"
```

---

# Task 8: Section 11 — IPC and clients

Row 11.1 is already recorded. Rows 11.6–11.9 are the ones that matter most, because they assert the properties the overlay depends on and the privacy property SPEC §12.4 requires.

**Files:**
- Modify: `docs/manual-test-logs/2026-08-30-m3-windows.md`

- [ ] **Step 1: Row 11.3 — second core refuses**

```sh
PATH=/c/msys64/ucrt64/bin:$PATH ./build/default/core/keygnosys-core.exe; echo "exit=$?"
```

with a core already running. Expected: **exit code 3**, a named condition on stderr, and the first core undisturbed.

- [ ] **Step 2: Row 11.2 — two concurrent clients**

Run two `record.py` instances at once and confirm both receive events and neither starves.

- [ ] **Step 3: Rows 11.6–11.7 — exactly one event per physical event**

Have the operator type a known short string, then a grace-replay gesture (tap an action-bound key such as `J`).

```sh
.venv/Scripts/python.exe -c "
import json,collections
keys=[json.loads(l)['d'] for l in open('/tmp/ipc.jsonl') if json.loads(l).get('n')=='key']
print('key events:',len(keys))
print('by state:',dict(collections.Counter(k['state'] for k in keys)))
bal=collections.Counter()
for k in keys: bal[k['code']] += 1 if k['state']=='down' else (-1 if k['state']=='up' else 0)
print('unbalanced:',{c:n for c,n in bal.items() if n})"
```

Expected: **one event per physical key event, none for keys the core synthesized** (11.6), and **one** `key` event for the physical release during a grace replay, not two (11.7).

- [ ] **Step 4: Row 11.8 — ordering**

Engage the layer with `Shift` held. Expected: `mode` and `modifiers` reflect reality, and the `key` event for `CapsLock` **precedes** the `mode` change it caused.

Note: the run log records that `mode` is currently emitted on **every modifier change**, not only on layer transitions. SPEC §5.3 specifies `mode` for "layer engaged/released". If that still holds, record it as a **minor divergence** with evidence — it is extra traffic rather than wrong data, but it should not pass silently.

- [ ] **Step 5: Row 11.9 — no character content, ever**

```sh
.venv/Scripts/python.exe -c "
import json
bad=[l for l in open('/tmp/ipc.jsonl') if json.loads(l).get('n')=='key'
     and set(json.loads(l)['d']) - {'code','state','suppressed'}]
print('key payloads with unexpected fields:', len(bad))"
```

Expected: **0**. Only positional codes cross the wire (SPEC §12.4). This is a privacy guarantee, not a formatting preference.

- [ ] **Step 6: Row 11.5 — a client that stops reading**

Write a throwaway client that connects, sends commands continuously, and never reads. Expected: the core does not block, no keystroke is delayed, and the client is eventually **disconnected** rather than growing the core's memory (SPEC §5.1, the four-times-bound ceiling).

- [ ] **Step 7: Row 11.4 — shutdown reaches clients**

Stop the core with a client attached. Expected: the client receives `shutdown` with a reason, and reconnects cleanly when the core returns.

- [ ] **Step 8: Record and commit**

```bash
git add docs/manual-test-logs
git commit -m "Record section 11: IPC event contract and client bounds"
```

---

# Task 9: Section 12 — overlay integration

The first task needing the Python overlay rather than the core alone. PySide6 6.11.2 is installed in `.venv`.

**Files:**
- Modify: `docs/manual-test-logs/2026-08-30-m3-windows.md`
- `python/keygnosys/coreclient/ipc.py` if `overlay_toggle` is unhandled.

- [ ] **Step 1: Run the overlay against the live core**

```sh
PATH=/c/msys64/ucrt64/bin:$PATH ./build/default/core/keygnosys-core.exe > /dev/null 2>&1 &
sleep 3
.venv/Scripts/python.exe -m keygnosys
```

- [ ] **Step 2: Row 12.1 — feedback includes suppressed keys**

Type with the layer off, then engage it and press bound keys. Expected: keys light up as pressed **including suppressed ones** — that is what makes the map usable in the cursor layer, where no character reaches the OS.

- [ ] **Step 3: Row 12.2 — legends change**

Engage the layer. Expected: legends switch to the cursor-layer bindings.

- [ ] **Step 4: Row 12.3 — `overlay.toggle`**

`Backquote` is bound to `overlay.toggle`. Expected: the overlay shows or hides.

The run log's earlier finding #2 noted SPEC §5.3's event table was missing the `overlay_toggle` row and that `coreclient/ipc.py` may lack a handler. Check both; if the handler is missing, that is a genuine defect — fix it with a test in `tests/` and commit separately.

- [ ] **Step 5: Row 12.4 — debounced slot updates**

Move a window between monitors and watch the overlay. Expected: the slot list updates within about 250 ms, and **not more often than that** (SPEC §5.3 requires `windows` to be debounced ≥250 ms).

```sh
.venv/Scripts/python.exe -c "
import json
ts=[json.loads(l)['t'] for l in open('/tmp/overlay.jsonl') if json.loads(l).get('n')=='windows']
gaps=[b-a for a,b in zip(ts,ts[1:])]
print('windows events:',len(ts))
print('min gap (MUST be >= 0.25s):', min(gaps) if gaps else 'n/a')"
```

- [ ] **Step 6: Record and commit**

```bash
git add docs/manual-test-logs
git commit -m "Record section 12: overlay integration against the live core"
```

---

# Task 10: Promote the M3 status and finish the branch

**Only reachable when every prior task is done and no row is left failing.** If any row failed and was not fixed, this task does not run — the status text stays as it is, which is the whole reason it was written that way.

**Files:**
- Modify: `README.md`, `core/README.md`, `docs/manual-test-logs/2026-08-30-m3-windows.md`

**Interfaces:**
- Consumes: a complete run log with no outstanding failures.
- Produces: an M3 status that a reader can trust.

- [ ] **Step 1: Audit the log before writing anything**

List every row and its result. Count PASS, NOT RUN and FAIL. **Any FAIL blocks this task.** Each NOT RUN must carry a reason a reader can evaluate (`1.4` no IME, `1.6` no injected-sensitive app, `3.5` not reachable with this operator's timing).

- [ ] **Step 2: Run the full verification set one last time**

All five commands from Global Constraints, plus `kgn_hook_smoke` (operator-launched, Method Rule 2). Record the numbers in the log.

- [ ] **Step 3: Replace the temporary status in `core/README.md`**

Current text says "implementation complete; live Windows validation in progress". Replace with wording that states what was verified, on what hardware, and what was not run and why. Do **not** write "M3 complete" without the caveats the log earned.

- [ ] **Step 4: Update the top-level `README.md`**

Promote the backends row from 🚧 only for Windows; Linux stays 🚧 until M4. The milestone line should name the Windows manual matrix as the thing that was passed.

- [ ] **Step 5: Commit**

```bash
git add README.md core/README.md docs/manual-test-logs
git commit -m "Promote M3 to manually verified on Windows"
```

- [ ] **Step 6: Request review before merging**

REQUIRED SUB-SKILL: use `superpowers:requesting-code-review` on the full branch diff (`git diff main...HEAD`), then `superpowers:finishing-a-development-branch` to decide integration. **Do not merge without the operator's explicit approval** — that constraint has held for the entire branch and does not lapse at the end.

---

## Self-Review

**Spec coverage.** Every unrun row in `docs/manual-tests.md` maps to a task: §5.4–5.5 → Task 2; §6 → Task 3; §7 → Task 4; §8 → Task 5; §9 → Task 6; §10 → Task 7; §11 → Task 8; §12 → Task 9. Rows 1.4, 1.6 and 3.5 are recorded NOT RUN with reasons and are deliberately not re-attempted — the first two need software this machine lacks, the third needs a chord ordering this operator's hands do not produce.

**Placeholders.** Every step carries the actual command or the actual briefing text. No "add appropriate handling", no "similar to Task N".

**Type consistency.** Tool invocations are identical across tasks: `kgn.py send <command> [json]`, `record.py --out FILE --timeout S`, `observe_keys.ps1 -Seconds N -Out FILE`, `observe_mouse.ps1 -Seconds N -Out FILE`. The CSV column orders are stated once in Task 1's Interfaces block and relied on unchanged in Tasks 2, 7 and 8.

**Known gap.** Task 8 Step 4 may surface the `mode`-on-every-modifier-change divergence. It is written as "record it, do not pass it silently" rather than as a fix, because deciding whether to change the event contract or the spec sentence is a judgement the operator should make — the same situation as the two spec defects this milestone already found.

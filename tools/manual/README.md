# Manual validation harness

Instruments for running [`docs/manual-tests.md`](../../docs/manual-tests.md)
against a real desktop. They exist because the M3 Windows validation could not
be done by eye: the defects it found were invisible without measurement, and two
of them were originally *missed* because an instrument was trusted before it was
checked.

Nothing here ships to a user. It is test equipment.

---

## Before anything else

**The `PATH` prefix is not optional under MSYS2/Git Bash:**

```sh
PATH=/c/msys64/ucrt64/bin:$PATH ./build/default/core/keygnosys-core.exe
```

`keygnosys-core` is UCRT64-linked. If another MinGW `bin` precedes the UCRT64 one
it loads that toolchain's `libstdc++-6` and segfaults before printing its banner.
The optimized build dies reliably; `debug` survives the mismatch, which makes the
failure look like a product defect rather than a DLL-resolution one.

**Run the boundary check first, every session:**

```sh
PATH=/c/msys64/ucrt64/bin:$PATH ./build/default/core/kgn_hook_smoke.exe
```

It installs the real hook and reports PASS on the first genuine keystroke, in
twelve seconds. It exists because a completely dead hook was once discovered
forty rows into a matrix run.

---

## Emergency exits, strongest first

The cursor layer swallows every unbound non-modifier key, so **any recovery that
requires typing is unusable in exactly the state that needs it** — `taskkill`
cannot be typed and `Ctrl+C` never reaches a console.

1. **`Ctrl+Alt+Del` → Task Manager → End task.** Windows guarantees the Secure
   Attention Sequence cannot be intercepted. Mouse-only, and it depends on
   nothing in this codebase.
2. **`panic.ps1`** — kills the core, *then* force-releases every modifier (both
   sides) and all three mouse buttons. The second half matters: killing the
   process removes the hook, but does **not** undo a `SendInput` key-down that
   never received its up.
3. **`release_all` over IPC** — `kgn.py send release_all`. Per SPEC §5.4 the
   reply means *applied*, not merely accepted.

Make the panic script clickable before running any suppression row:

```powershell
$d=[Environment]::GetFolderPath('Desktop'); $w=New-Object -ComObject WScript.Shell
$s=$w.CreateShortcut((Join-Path $d 'KEYGNOSYS PANIC.lnk'))
$s.TargetPath='powershell.exe'
$s.Arguments='-NoProfile -ExecutionPolicy Bypass -File C:\Projects\KeyGnosys\tools\manual\panic.ps1'
$s.IconLocation='shell32.dll,131'; $s.Save()
```

---

## Which tool answers which question

| Question | Tool |
|---|---|
| Did the core *see* the key? | `record.py` — the core's own event stream |
| Did the core *synthesize* output? | `observe_keys.ps1` — an independent hook; the core cannot see its own injected events |
| Is a mouse button still held down? | `observe_mouse.ps1` — same reason, for buttons |
| Did a chord resolve as a layer action or leak a letter? | `observe_keys.ps1` + `analyse_chords.py` |
| What does the core think its state is? | `kgn.py send get_state` |
| Is the hook alive at all? | `kgn_hook_smoke` (built by CMake, not here) |

### Usage

```sh
# one command, print the reply
.venv/Scripts/python.exe tools/manual/kgn.py send set_activation_mode '{"mode":"toggle"}'
.venv/Scripts/python.exe tools/manual/kgn.py send get_state

# stream core events; F12 delimits segments; writes FILE.hb as a heartbeat
.venv/Scripts/python.exe tools/manual/record.py --out /tmp/run.jsonl --timeout 28800 &

# independent observers (CSV; flush every second so the file fills as you watch)
powershell -NoProfile -ExecutionPolicy Bypass -File tools/manual/observe_keys.ps1  -Seconds 28800 -Out /tmp/keys.csv &
powershell -NoProfile -ExecutionPolicy Bypass -File tools/manual/observe_mouse.ps1 -Seconds 28800 -Out /tmp/mouse.csv &

# chord pairing, one-to-one, +/-80 ms window
.venv/Scripts/python.exe tools/manual/analyse_chords.py /tmp/keys.csv 80
```

`observe_keys.ps1` writes `elapsed_ms,vk,injected,D|U`.
`observe_mouse.ps1` writes `elapsed_ms,D|U,injected`.

---

## Method rules

These cost about a day of the M3 milestone. They are constraints, not advice.

**1. Validate an instrument against a known-positive before believing a negative
from it.** A capture client received its `hello` line, went deaf, and stayed
alive — producing five convincing false negatives and two conclusions that had to
be publicly retracted. Trigger something you know should appear (toggling
`set_activation_mode` emits `mode` events), confirm the tool reports it, and only
then trust a zero.

**2. Injected events are invisible to the core's own stream, by design.** The
hook skips `LLKHF_INJECTED` so its own `SendInput` output cannot feed back. So
grace replays and `release_all` output can only be seen by an *independent* hook.
Never read "no synthetic event in the core's stream" as "the core did nothing".

**3. The operator types their replies on the same keyboard.** Every capture is
polluted with prose. Filter by keycode and pair one-to-one. A ±100 ms pairing
window once let a single `CapsLock` press partner several `J` presses and
invented 21 attempts out of 10.

**4. Never use a short fixed capture window.** The operator is a human with a
life. Arm for hours, flush incrementally, expose a heartbeat. A window that
expires before the operator is ready writes an empty file, which reads exactly
like a real negative.

**5. Measure the thing, not the harness.** A timing test once polled with
`Sleep(1)` and reported the timer as ~15 ms worse than it was, because `Sleep(1)`
is itself ~15.6 ms. If a measurement lands suspiciously close to a known
scheduling quantum, suspect the instrument first.

**6. A row is PASS only when its expected result was actually observed.**
`NOT RUN`, with a reason, is honest. A pass inferred from an adjacent row, from
green unit tests, or from plausibility is not.

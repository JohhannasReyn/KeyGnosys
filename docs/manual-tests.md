# Manual test matrix

Some things cannot be meaningfully automated without a real display and real
hardware, and pretending otherwise produces tests that pass while the product is
broken (SPEC [§13](SPEC.md#13-testing-strategy)). This is the matrix for those.

**Scope: the Windows backend (M3).** The Linux/X11 rows arrive with M4.

## How to use this

Every row is a procedure and an expected result. A row is **PASS** only when the
expected result was observed on real hardware; anything else is FAIL or
NOT RUN. There is no "probably fine".

Run the whole matrix before a release, and the rows touching the area you
changed before a merge.

Record results in a copy, not in this file — this file is the matrix, not the
log.

### Setup

```sh
cmake --preset default
cmake --build --preset default
./build/default/core/keygnosys-core.exe
```

Run **unelevated** unless a row says otherwise. Keep a second terminal open with
a way to kill the process (`taskkill /IM keygnosys-core.exe /F`), because a row
that goes wrong may leave a modifier held.

> **Safety.** Rows marked ⚠ can leave a key or a mouse button held down if the
> code under test is broken — that is what they exist to detect. Have the kill
> command ready, and know that logging out clears any stuck modifier.

---

## 1. Ordinary input is unaffected

| # | Procedure | Expected |
|---|---|---|
| 1.1 | With the core running and the layer **off**, type a paragraph of ordinary prose in a text editor. | Every character appears, in order, at normal typing latency. No dropped or doubled characters. |
| 1.2 | Hold `a` for three seconds. | The character autorepeats at the system repeat rate, exactly as it does with the core stopped. |
| 1.3 | Hold `Shift` and type letters. | Capitals. The shift is not swallowed or duplicated. |
| 1.4 | Type in an application using an IME (e.g. Microsoft Pinyin). | Composition works normally. The candidate window appears and commits. |
| 1.5 | Use `Ctrl+C` / `Ctrl+V` in Explorer, and `Alt+Tab`. | Normal behaviour. |
| 1.6 | In a game or application that distinguishes injected input, type normally. | Input is accepted as physical. (This is the property native passthrough exists to preserve; re-injecting everything would break it.) |
| 1.7 | Press a key the layout does not name — a vendor macro key, a media key. | It works normally. An unrecognised key is passed through, never swallowed. |

## 2. CapsLock and the activation modes

| # | Procedure | Expected |
|---|---|---|
| 2.1 | Default (`hybrid`). Tap `CapsLock`. | The layer latches ON. The overlay shows cursor mode. No CapsLock LED change, no capitals. |
| 2.2 | Tap `CapsLock` again. | The layer latches OFF. |
| 2.3 | Hold `CapsLock` for a second, release. | The layer is engaged only while held. |
| 2.4 | `set_activation_mode` to `toggle`. Tap, then hold-and-release `CapsLock`. | Tap toggles. Release does nothing. |
| 2.5 | `set_activation_mode` to `hold`. Tap `CapsLock`. | The layer engages on press and drops on release; a tap leaves it off. |
| 2.6 | Hold `Shift`, press `CapsLock`, release both. | **Real CapsLock** — the LED changes and subsequent letters are capitals. The layer is not engaged. |
| 2.7 | ⚠ Engage the layer, hold a movement key, and press `CapsLock` to leave. | The pointer stops. No key or button remains held. |
| 2.8 | Press `CapsLock` twice very rapidly (under 30 ms apart). | The layer toggles once, not twice. |

## 3. The grace window

| # | Procedure | Expected |
|---|---|---|
| 3.1 | With the layer off, press an action-bound key (e.g. `J`) and release it quickly. | The character `j` is typed. |
| 3.2 | Press `J` and, within the grace window (default 50 ms), press `CapsLock`. | No `j` is typed; the layer engages and `J`'s action runs. |
| 3.3 | Press `J`, wait 200 ms, then press `CapsLock`. | `j` is typed, then the layer engages. |
| 3.4 | Press `J` and hold it for a second without touching `CapsLock`. | `j` autorepeats normally after the grace window lapses. |
| 3.5 | Press three action-bound keys in quick succession, then `CapsLock`. | Their actions start in press order, not in some other order. |

## 4. Pointer motion

| # | Procedure | Expected |
|---|---|---|
| 4.1 | Engage the layer, tap a direction key once. | The pointer moves a small, precise amount — a nudge, not a jump. |
| 4.2 | Hold the direction key. | The pointer accelerates smoothly to its maximum, then holds that speed. |
| 4.3 | Hold two perpendicular directions. | The pointer travels diagonally at the **same speed** as a cardinal direction, not √2 times faster. |
| 4.4 | While moving right, add left without releasing right. | Motion stops. Release right: motion resumes leftward, without a ramp restart. |
| 4.5 | Hold the precision modifier while moving. | Motion slows to the precision factor and stays controllable. |
| 4.6 | Tap a direction key twenty times slowly. | The pointer moves each time. It never sticks — fractional accumulation must survive between taps. |
| 4.7 | Cross all monitors left to right on one continuous hold. | Smooth, no stutter, no jump at monitor boundaries. |

## 5. Buttons and scrolling

| # | Procedure | Expected |
|---|---|---|
| 5.1 | Click, right-click and middle-click over a target. | The expected menus and selections. |
| 5.2 | Hold the click key over a text selection and move the pointer. | Text selects — click-and-hold works as a drag. |
| 5.3 | Double-click over a word. | The word is selected. Two pairs land within the OS double-click interval. |
| 5.4 | ⚠ Engage drag lock, move a file across a folder, engage again. | The file drags and drops. The button is down between the two presses and up after. |
| 5.5 | ⚠ Engage drag lock, then leave the layer without disengaging it. | The button is released automatically (SPEC §7.2). Nothing stays held. |
| 5.6 | With left-click bound to two keys, hold one and tap the other. | The button does not double-press, and does not lift while the first key is still held. |
| 5.7 | Scroll up and down, then left and right. | Content scrolls the expected way, with its own ramp. |
| 5.8 | Press the page-scroll key. | About a screenful per press. |

## 6. Warp across a multi-monitor virtual desktop

Needs at least two monitors. **A monitor positioned left of or above the primary
is the case that matters** — it has negative virtual-desktop coordinates, which
primary-only normalisation cannot express at all.

| # | Procedure | Expected |
|---|---|---|
| 6.1 | Warp to each of the nine grid cells with the pointer on the **primary** monitor. | The pointer lands in the right ninth of that monitor. |
| 6.2 | Move the pointer to a **secondary** monitor and repeat. | The grid is over the monitor the pointer is on, not the primary. |
| 6.3 | Repeat with the pointer on a monitor whose x or y is **negative**. | Correct placement. This is the row that catches the classic bug. |
| 6.4 | Warp to each of the five corners on each monitor. | The pointer lands on that monitor's corner, not one pixel onto the neighbour. |
| 6.5 | Warp `monitor next` repeatedly. | The pointer visits every monitor in index order and wraps. |
| 6.6 | Change the display arrangement in Windows Settings while the core runs, then warp. | Warp uses the new topology. A `monitors` event was emitted. |
| 6.7 | Set two monitors to different scaling (e.g. 100% and 150%) and warp between them. | The pointer lands where expected on both. |

## 7. Window management

| # | Procedure | Expected |
|---|---|---|
| 7.1 | Open several windows and read the slot list from `get_state`. | Each real window has a stable index. |
| 7.2 | Close a middle window and re-read. | The remaining windows **keep their indices**. The freed slot is empty. |
| 7.3 | Open a new window. | It takes the lowest free index. |
| 7.4 | Open more than nine windows. | The tenth and beyond appear with `index: null`. No existing slot moves. |
| 7.5 | Open a UWP application (Settings, Calculator), then close it to the tray or switch away. | No invisible ghost window appears in the slot list. This is what the cloaked check is for. |
| 7.6 | Press the slot key for a window on another monitor. | That window is focused. |
| 7.7 | Cycle focus forward and back. | Focus follows **slot order**, not recency. |
| 7.8 | Focus a monitor. | The topmost window there is focused and the pointer follows. |
| 7.9 | Move a normal window to another monitor. | It arrives at the same relative position, correctly scaled. |
| 7.10 | Maximise a window, then move it to another monitor. | It arrives maximised on the target, not restored and not half off-screen. |
| 7.11 | Repeat 7.9 and 7.10 between monitors of **different DPI**. | The window keeps its apparent size. |

## 8. Documented limitations — these must behave as specified, not be "fixed"

| # | Procedure | Expected |
|---|---|---|
| 8.1 | Focus an elevated window (an admin Command Prompt) and engage the layer. | Interception is **inert** while that window has focus. The UI says why. Nothing crashes, nothing is stranded. |
| 8.2 | Move focus back to an ordinary window. | The layer works again immediately. |
| 8.3 | Press `Ctrl+Alt+Del`. | The secure screen appears normally. The core does not intercept it. **This is correct**, not a defect. |
| 8.4 | Press `Fn` on a laptop keyboard. | Nothing is reported and nothing is bindable. The overlay draws it but never highlights it. |
| 8.5 | Read `hello.limitations` from a connected client. | Both the elevated-window and the Ctrl+Alt+Del limitations are listed verbatim. |

## 9. Hook loss and recovery

| # | Procedure | Expected |
|---|---|---|
| 9.1 | Load the machine heavily (a full build, a large export) and keep typing with the layer engaged. | Either the hook survives, or it is removed and **re-installed automatically** with an `input.hook_lost` diagnostic. The layer works again without a restart. |
| 9.2 | Lower `LowLevelHooksTimeout` in the registry to a small value, restart Windows, and repeat 9.1. | Same. Restore the original value afterwards. |
| 9.3 | ⚠ During a hook loss, check that no key is stuck. | No stranded key or button. |

## 10. Shutdown, disable and reload safety ⚠

Every row here is a P7 test. A failure leaves a key or a button held.

| # | Procedure | Expected |
|---|---|---|
| 10.1 | Engage the layer, hold a movement key and a click key, then `Ctrl+C` the core. | The core exits cleanly. Pointer stops. Button is up. No modifier stuck. |
| 10.2 | Same, but kill the process with `taskkill /F`. | The physical keys are still physically held by you; releasing them behaves normally. Nothing is stuck after release. |
| 10.3 | Hold a movement key and send `set_enabled(false)`. | Motion stops immediately and everything held is released. |
| 10.4 | Re-enable and use the layer. | It works, with no leftover state from before. |
| 10.5 | Hold an action key and send `reload_config`. | The action is released. No stuck button. |
| 10.6 | Hold an action key and send `set_bindings` for a document that does not bind it. | The action is released rather than left held by a binding that no longer exists. |
| 10.7 | Hold several keys and send `release_all`. | Everything lifts. The reply is `ok`. |
| 10.8 | Engage the layer, hold a key, and disconnect the overlay client abruptly. | The core keeps working. Nothing is stranded. |
| 10.9 | After every row above, check the modifier state (open an on-screen keyboard). | No modifier is stuck down. |

## 11. IPC and clients

| # | Procedure | Expected |
|---|---|---|
| 11.1 | Start the core, then start the overlay. | The overlay connects and reports the real backend names, not "mock". |
| 11.2 | Connect a second client while the first is connected. | Both receive events. Neither starves. |
| 11.3 | Start a second `keygnosys-core`. | It refuses with exit code 3 and names the condition. The first is undisturbed. |
| 11.4 | Stop the core with a client attached. | The client receives `shutdown` with a reason and reconnects cleanly when the core returns. |
| 11.5 | Connect a client that stops reading and keeps sending commands. | The core does not block. No keystroke is delayed. The client is eventually disconnected rather than growing the core's memory. |
| 11.6 | Watch the `key` event stream while typing. | Exactly one event per physical key event. **No duplicates**, and none for keys the core synthesised. |
| 11.7 | Watch the stream during a grace replay (3.2). | One `key` event for the physical release. Not two. |
| 11.8 | Watch `mode` and `modifiers` while engaging the layer with Shift held. | Both reflect reality. The `key` event for `CapsLock` precedes the `mode` change it caused. |
| 11.9 | Confirm no `key` event ever contains a character. | Only positional codes (SPEC §12.4). |

## 12. Overlay integration

| # | Procedure | Expected |
|---|---|---|
| 12.1 | Run the overlay against the live core and type. | Keys light up as pressed, including suppressed ones. |
| 12.2 | Engage the layer. | Legends change to the cursor layer. |
| 12.3 | Trigger the `overlay.toggle` action. | The overlay shows or hides. |
| 12.4 | Move a window between monitors and watch the overlay. | The slot list updates within about 250 ms, and not more often than that. |

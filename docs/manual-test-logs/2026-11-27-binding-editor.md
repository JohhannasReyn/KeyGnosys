# Binding editor (project 1): live check, 2026-11-27, Windows

**Build:** branch `feat/binding-editor` at `a8bc0e5`. The core is `build/default/core/keygnosys-core.exe`, and the overlay is run as `pythonw -m keygnosys` (auto backend).

**Method:**
- The operator was at the keyboard and did every click and keypress.
- The agent started and stopped the core, corrupted the user file for row 13.5, and read the user config directory to confirm the results.
- No keystroke capture was taken.

**Starting state:** `%APPDATA%\KeyGnosys\bindings\` was empty, so there was no user map yet.

| Row | Result | Observed |
|-----|--------|----------|
| 13.1 | **PASS** | D was reassigned to Right click in define mode. Engaging the layer and pressing D opened a right-click menu. |
| 13.2 | **PASS** | Space was cleared first, then D reassigned. Unassigned showed "Left click (was on D)". The key name is shown, not `KeyD`. |
| 13.3 | **PASS** | Undo on the bar put D back to Left click, live. |
| 13.4 | **PASS** | With the core stopped, Q was assigned Double click. The notice strip read "Saved — changes apply when the core is running." The user file held `KeyQ: button.double_click {button: left}`. After the core restarted, Q double-clicked in the layer. |
| 13.5 | **PASS** | The user file was replaced with truncated JSON. Entering define mode showed the error notice and offered only Reset to shipped. After Reset, `default.json` was gone and `default.broken-1795842643.json` (73 bytes) held the broken content unchanged. |

**End state:**
- The user bindings directory holds only the moved-aside broken file.
- The core runs the shipped `default` map.
- The first edit created a user copy (the fork), confirmed by `default.json` appearing in the user directory.

**Not exercised live:** drag-and-drop from the picker onto a key, and typing into the search and label fields. Tests cover these, but a real OS drag and keyboard focus beside the no-activate keyboard window were not checked in this session. The final review listed both as live-only.

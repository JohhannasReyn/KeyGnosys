# Export and import (project 2): live check, 2026-11-28, Windows

**Build:** branch `feat/export-import`. The core is `build/default/core/keygnosys-core.exe`, and the overlay is run as `pythonw -m keygnosys` (auto backend).

**Method:**
- The operator was at the keyboard and did every click and keypress.
- The agent started and restarted the core and overlay, made the hand-edited and truncated test files, and read the user config directory and the exported file to confirm the results.
- No keystroke capture was taken.

**Starting state:**
- There was no user map. `%APPDATA%\KeyGnosys\bindings\` held only the moved-aside file from the binding-editor check.
- The overlay first opened off-screen, because its saved position was on a monitor that had been turned off. The agent moved the saved position onto the primary screen. This is a separate bug, to be fixed on its own branch.

| Row | Result | Observed |
|-----|--------|----------|
| 14.1 | **PASS** | Exported to `Downloads\Default cursor layer.keygnosys.json`: a `keygnosys/bundle/1` file holding 1 map, with D on Left click and author "test" in the header. D was then changed to Right click and the file imported. The preview listed Changes (1 key) with D. After Import, D was back to Left click and the notice named the backup file. `default.backup-1795853714.json` held the pre-import map, with D on Right click. |
| 14.3 | **PASS** | Undo restored the pre-import map live, and D right-clicked in the layer. The user file no longer carried `imported_from`. |
| 14.2 | **PASS** | A copy of the export with `F7: window.snap` added was imported. Afterwards the user file still held `F7: {"action": "window.snap"}`, unchanged. A second backup, `default.backup-1795853818.json`, was written. |
| 14.4 | Not run | |
| 14.5 | Not run | |
| 14.6 | Not run | |

**Operator feedback acted on:** notices are now coloured by outcome: green for success, yellow for warnings, red for errors (commit `a4b4cf4`).

**Open from this session:**
- The operator reported that the Import button "looks like it's letting clicks pass through beneath it". That still needs clarifying and investigating.
- Rows 14.4–14.6 are still to run.

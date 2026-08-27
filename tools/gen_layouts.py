"""Generate the bundled keyboard layout documents.

The JSON files in ``data/layouts/`` are the source of truth at runtime -- this
script exists only to make *authoring* them tolerable, since a full-size board is
104 hand-placed rectangles. Editing the generated JSON directly is entirely
supported and is how users are expected to customise layouts.

Geometry is in layout units (1u = one standard alphanumeric key), measured from
the top-left corner. See docs/SPEC.md section 4.1.

Usage:  python tools/gen_layouts.py
"""

from __future__ import annotations

import json
from pathlib import Path

OUT_DIR = Path(__file__).resolve().parent.parent / "data" / "layouts"

# --------------------------------------------------------------------------
# Shared key runs
# --------------------------------------------------------------------------

# (code, base legend, shift legend or None)
DIGIT_ROW = [
    ("Backquote", "`", "~"), ("Digit1", "1", "!"), ("Digit2", "2", "@"),
    ("Digit3", "3", "#"), ("Digit4", "4", "$"), ("Digit5", "5", "%"),
    ("Digit6", "6", "^"), ("Digit7", "7", "&"), ("Digit8", "8", "*"),
    ("Digit9", "9", "("), ("Digit0", "0", ")"), ("Minus", "-", "_"),
    ("Equal", "=", "+"),
]

TOP_ROW = [
    ("KeyQ", "Q"), ("KeyW", "W"), ("KeyE", "E"), ("KeyR", "R"), ("KeyT", "T"),
    ("KeyY", "Y"), ("KeyU", "U"), ("KeyI", "I"), ("KeyO", "O"), ("KeyP", "P"),
]

HOME_ROW = [
    ("KeyA", "A"), ("KeyS", "S"), ("KeyD", "D"), ("KeyF", "F"), ("KeyG", "G"),
    ("KeyH", "H"), ("KeyJ", "J"), ("KeyK", "K"), ("KeyL", "L"),
]

BOTTOM_ROW = [
    ("KeyZ", "Z"), ("KeyX", "X"), ("KeyC", "C"), ("KeyV", "V"), ("KeyB", "B"),
    ("KeyN", "N"), ("KeyM", "M"),
    ("Comma", ",", "<"), ("Period", ".", ">"), ("Slash", "/", "?"),
]

FN_KEYS = [(f"F{i}", f"F{i}") for i in range(1, 13)]


def key(code, x, y, w=1.0, h=1.0, base=None, shift=None, sub=None, role="normal"):
    """Build one key document, omitting fields that carry their default."""
    legend = {"base": base if base is not None else code}
    if shift is not None:
        legend["shift"] = shift
    if sub is not None:
        legend["sub"] = sub
    k = {"code": code, "x": round(x, 4), "y": round(y, 4)}
    if w != 1.0:
        k["w"] = round(w, 4)
    if h != 1.0:
        k["h"] = round(h, 4)
    k["legend"] = legend
    if role != "normal":
        k["role"] = role
    return k


def run(keys, x, y, w=1.0, role="normal"):
    """Place a horizontal run of uniform-width keys.

    Positions are stepped by the *rounded* width rather than the exact one, so
    that a fractional key width (a laptop function row divided across the board)
    tiles exactly instead of leaving sub-thousandth overlaps between neighbours.
    """
    w = round(w, 4)
    out = []
    for i, entry in enumerate(keys):
        code, base = entry[0], entry[1]
        shift = entry[2] if len(entry) > 2 else None
        out.append(key(code, x + i * w, y, w=w, base=base, shift=shift, role=role))
    return out


def alpha_block(keys, *, iso_enter: bool):
    """Rows 1-3 of a standard 15u-wide alphanumeric block.

    These three rows are identical across every layout we ship, apart from the
    ANSI/ISO difference in how Enter and Backslash are arranged.
    """
    keys += run(DIGIT_ROW, 0, 1.25)
    keys.append(key("Backspace", 13, 1.25, w=2, base="Backspace", role="system"))

    keys.append(key("Tab", 0, 2.25, w=1.5, base="Tab", role="system"))
    keys += run(TOP_ROW, 1.5, 2.25)
    keys.append(key("BracketLeft", 11.5, 2.25, base="[", shift="{"))
    keys.append(key("BracketRight", 12.5, 2.25, base="]", shift="}"))

    keys.append(key("CapsLock", 0, 3.25, w=1.75, base="Caps", role="toggle"))
    keys += run(HOME_ROW, 1.75, 3.25)
    keys.append(key("Semicolon", 10.75, 3.25, base=";", shift=":"))
    keys.append(key("Quote", 11.75, 3.25, base="'", shift='"'))

    if iso_enter:
        # ISO: Backslash drops to the home row and Enter becomes the tall
        # L-shape. See SPEC.md section 4.1 for the segment model.
        keys.append(key("Backslash", 12.75, 3.25, base="\\", shift="|"))
        keys.append(key("Enter", 13.75, 2.25, w=1.25, h=2, base="Enter", role="system"))
    else:
        keys.append(key("Backslash", 13.5, 2.25, w=1.5, base="\\", shift="|"))
        keys.append(key("Enter", 12.75, 3.25, w=2.25, base="Enter", role="system"))
    return keys


def shift_row(keys, *, iso: bool, right_shift_w: float):
    if iso:
        keys.append(key("ShiftLeft", 0, 4.25, w=1.25, base="Shift", role="modifier"))
        keys.append(key("IntlBackslash", 1.25, 4.25, base="\\", shift="|"))
    else:
        keys.append(key("ShiftLeft", 0, 4.25, w=2.25, base="Shift", role="modifier"))
    keys += run(BOTTOM_ROW, 2.25, 4.25)
    keys.append(key("ShiftRight", 12.25, 4.25, w=right_shift_w,
                   base="Shift", role="modifier"))
    return keys


# --------------------------------------------------------------------------
# Full-size desktop boards
# --------------------------------------------------------------------------

def full_size(iso: bool):
    keys: list[dict] = []

    # Function row
    keys.append(key("Escape", 0, 0, base="Esc", role="system"))
    keys += run(FN_KEYS[0:4], 2, 0, role="system")
    keys += run(FN_KEYS[4:8], 6.5, 0, role="system")
    keys += run(FN_KEYS[8:12], 11, 0, role="system")
    keys.append(key("PrintScreen", 15.25, 0, base="PrtSc", role="system"))
    keys.append(key("ScrollLock", 16.25, 0, base="Scroll", role="toggle"))
    keys.append(key("Pause", 17.25, 0, base="Pause", role="system"))

    keys = alpha_block(keys, iso_enter=iso)
    keys = shift_row(keys, iso=iso, right_shift_w=2.75)

    # Modifier row
    keys.append(key("ControlLeft", 0, 5.25, w=1.25, base="Ctrl", role="modifier"))
    keys.append(key("MetaLeft", 1.25, 5.25, w=1.25, base="Win", role="modifier"))
    keys.append(key("AltLeft", 2.5, 5.25, w=1.25, base="Alt", role="modifier"))
    keys.append(key("Space", 3.75, 5.25, w=6.25, base="", role="normal"))
    keys.append(key("AltRight", 10, 5.25, w=1.25, base="Alt", role="modifier"))
    keys.append(key("MetaRight", 11.25, 5.25, w=1.25, base="Win", role="modifier"))
    keys.append(key("ContextMenu", 12.5, 5.25, w=1.25, base="Menu", role="system"))
    keys.append(key("ControlRight", 13.75, 5.25, w=1.25, base="Ctrl", role="modifier"))

    # Navigation cluster
    for i, (code, label) in enumerate(
        [("Insert", "Ins"), ("Home", "Home"), ("PageUp", "PgUp")]
    ):
        keys.append(key(code, 15.25 + i, 1.25, base=label, role="nav"))
    for i, (code, label) in enumerate(
        [("Delete", "Del"), ("End", "End"), ("PageDown", "PgDn")]
    ):
        keys.append(key(code, 15.25 + i, 2.25, base=label, role="nav"))
    keys.append(key("ArrowUp", 16.25, 4.25, base="▲", role="nav"))
    keys.append(key("ArrowLeft", 15.25, 5.25, base="◀", role="nav"))
    keys.append(key("ArrowDown", 16.25, 5.25, base="▼", role="nav"))
    keys.append(key("ArrowRight", 17.25, 5.25, base="▶", role="nav"))

    # Numeric keypad
    keys.append(key("NumLock", 18.5, 1.25, base="Num", role="toggle"))
    keys.append(key("NumpadDivide", 19.5, 1.25, base="/", role="numpad"))
    keys.append(key("NumpadMultiply", 20.5, 1.25, base="*", role="numpad"))
    keys.append(key("NumpadSubtract", 21.5, 1.25, base="-", role="numpad"))
    for i, code in enumerate(["Numpad7", "Numpad8", "Numpad9"]):
        keys.append(key(code, 18.5 + i, 2.25, base=code[-1], role="numpad"))
    keys.append(key("NumpadAdd", 21.5, 2.25, h=2, base="+", role="numpad"))
    for i, code in enumerate(["Numpad4", "Numpad5", "Numpad6"]):
        keys.append(key(code, 18.5 + i, 3.25, base=code[-1], role="numpad"))
    for i, code in enumerate(["Numpad1", "Numpad2", "Numpad3"]):
        keys.append(key(code, 18.5 + i, 4.25, base=code[-1], role="numpad"))
    keys.append(key("NumpadEnter", 21.5, 4.25, h=2, base="Enter", role="numpad"))
    keys.append(key("Numpad0", 18.5, 5.25, w=2, base="0", role="numpad"))
    keys.append(key("NumpadDecimal", 20.5, 5.25, base=".", role="numpad"))

    return keys


# --------------------------------------------------------------------------
# Laptop boards
# --------------------------------------------------------------------------

def thinkpad():
    """Modelled on current mainstream ThinkPad T/X series boards.

    Distinguishing traits: Fn is the leftmost key on the bottom row, Home/End/
    Insert/Delete live at the right end of the function row, and PgUp/PgDn flank
    the arrow cluster. Arrow up/down are half-height, stacked.
    """
    keys: list[dict] = []
    width = 15.5

    top = ([("Escape", "Esc")] + FN_KEYS +
           [("Home", "Home"), ("End", "End"), ("Insert", "Ins"), ("Delete", "Del")])
    fw = width / len(top)
    keys += run(top, 0, 0, w=fw, role="system")

    keys = alpha_block(keys, iso_enter=False)
    keys = shift_row(keys, iso=False, right_shift_w=1.5)
    keys.append(key("PageUp", 13.75, 4.25, w=0.875, base="PgUp", role="nav"))
    keys.append(key("PageDown", 14.625, 4.25, w=0.875, base="PgDn", role="nav"))

    keys.append(key("Fn", 0, 5.25, base="Fn", role="modifier"))
    keys.append(key("ControlLeft", 1, 5.25, base="Ctrl", role="modifier"))
    keys.append(key("MetaLeft", 2, 5.25, base="Win", role="modifier"))
    keys.append(key("AltLeft", 3, 5.25, w=1.25, base="Alt", role="modifier"))
    keys.append(key("Space", 4.25, 5.25, w=5.5, base=""))
    keys.append(key("AltRight", 9.75, 5.25, base="Alt", role="modifier"))
    keys.append(key("PrintScreen", 10.75, 5.25, base="PrtSc", role="system"))
    keys.append(key("ControlRight", 11.75, 5.25, base="Ctrl", role="modifier"))

    # Rounded before use, so the three arrow columns tile exactly rather than
    # each inheriting a different rounding of the same fraction.
    aw = round((width - 12.75) / 3, 4)
    keys.append(key("ArrowLeft", 12.75, 5.25, w=aw, base="◀", role="nav"))
    keys.append(key("ArrowUp", 12.75 + aw, 5.25, w=aw, h=0.5, base="▲", role="nav"))
    keys.append(key("ArrowDown", 12.75 + aw, 5.75, w=aw, h=0.5, base="▼", role="nav"))
    keys.append(key("ArrowRight", 12.75 + 2 * aw, 5.25, w=aw, base="▶", role="nav"))
    return keys, width


def asus():
    """Modelled on current Asus Zenbook / Vivobook compact boards.

    Distinguishing traits: Ctrl is leftmost with Fn second, Delete sits at the
    right end of the function row, there is no dedicated nav column, and the
    arrow cluster is an inverted-T with half-height up/down.
    """
    keys: list[dict] = []
    width = 15.0

    top = [("Escape", "Esc")] + FN_KEYS + [("Delete", "Del")]
    fw = width / len(top)
    keys += run(top, 0, 0, w=fw, role="system")

    keys = alpha_block(keys, iso_enter=False)
    keys = shift_row(keys, iso=False, right_shift_w=2.75)

    keys.append(key("ControlLeft", 0, 5.25, w=1.25, base="Ctrl", role="modifier"))
    keys.append(key("Fn", 1.25, 5.25, base="Fn", role="modifier"))
    keys.append(key("MetaLeft", 2.25, 5.25, base="Win", role="modifier"))
    keys.append(key("AltLeft", 3.25, 5.25, w=1.25, base="Alt", role="modifier"))
    keys.append(key("Space", 4.5, 5.25, w=5.5, base=""))
    keys.append(key("AltRight", 10, 5.25, base="Alt", role="modifier"))
    keys.append(key("ControlRight", 11, 5.25, base="Ctrl", role="modifier"))
    keys.append(key("ArrowLeft", 12, 5.25, base="◀", role="nav"))
    keys.append(key("ArrowUp", 13, 5.25, h=0.5, base="▲", role="nav"))
    keys.append(key("ArrowDown", 13, 5.75, h=0.5, base="▼", role="nav"))
    keys.append(key("ArrowRight", 14, 5.25, base="▶", role="nav"))
    return keys, width


# --------------------------------------------------------------------------

def document(layout_id, name, description, keys, w=None, h=None):
    # Derive the extent from the keys themselves: a fractional key width
    # rounded for the file can otherwise overhang a hand-written total.
    w = max(k["x"] + k.get("w", 1.0) for k in keys) if w is None else w
    h = max(k["y"] + k.get("h", 1.0) for k in keys) if h is None else h
    return {
        "schema": "mousetrapkeys/layout/1",
        "id": layout_id,
        "name": name,
        "description": description,
        "size": {"w": round(w, 4), "h": h},
        "keys": keys,
    }


def write(doc):
    path = OUT_DIR / f"{doc['id']}.json"
    path.write_text(json.dumps(doc, indent=2, ensure_ascii=False) + "\n",
                    encoding="utf-8")
    print(f"{path.name:24} {len(doc['keys']):3d} keys  "
          f"{doc['size']['w']}u x {doc['size']['h']}u")


def main():
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    tp_keys, tp_w = thinkpad()
    as_keys, as_w = asus()

    write(document(
        "us-ansi-104", "US Full-Size (ANSI 104)",
        "Standard US full-size desktop keyboard with function row, navigation "
        "cluster and numeric keypad.",
        full_size(iso=False)))

    write(document(
        "us-iso-105", "US-International Full-Size (ISO 105)",
        "Full-size ISO variant: tall Enter, short left Shift, and the extra "
        "IntlBackslash key. This is the board usually meant by \"105-key\".",
        full_size(iso=True)))

    write(document(
        "thinkpad-compact", "ThinkPad Compact (laptop)",
        "Compact laptop layout modelled on current ThinkPad T/X series boards. "
        "Fn is leftmost; Home/End/Ins/Del sit in the function row.",
        tp_keys))

    write(document(
        "asus-compact", "Asus Compact (laptop)",
        "Compact laptop layout modelled on current Asus Zenbook / Vivobook "
        "boards. Ctrl is leftmost, Fn second; Delete ends the function row.",
        as_keys))


if __name__ == "__main__":
    main()

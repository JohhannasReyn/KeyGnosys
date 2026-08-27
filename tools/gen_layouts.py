"""Generate the bundled keyboard layout documents.

The JSON files in ``data/layouts/`` are the source of truth at runtime -- this
script exists only to make *authoring* them tolerable, since a full-size board
is 104 hand-placed rectangles. Editing the generated JSON directly is fully
supported, and from milestone M6 so is the visual editor.

Geometry is in layout units (1u = one standard alphanumeric key), measured from
the top-left corner. A logical key is drawn from one or more rectangular
segments; only the ISO Enter needs more than one. See docs/SPEC.md section 4.1.

The laptop layouts are representative templates for their model families, not
model-exact reproductions. Correcting one is a JSON edit or a drag in the
editor, which is the point (SPEC 15.5).

Usage:  python tools/gen_layouts.py
"""

from __future__ import annotations

import json
import re
from pathlib import Path

OUT_DIR = Path(__file__).resolve().parent.parent / "data" / "layouts"
SCHEMA = "mousetrapkeys/layout/2"

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


# --------------------------------------------------------------------------
# Key construction
# --------------------------------------------------------------------------

def _slug(code: str) -> str:
    """A readable, stable key id derived from the physical code."""
    s = re.sub(r"(?<!^)(?=[A-Z])", "-", code).lower()
    return re.sub(r"[^a-z0-9-]+", "-", s).strip("-")


def segment(x, y, w=1.0, h=1.0, index=0):
    seg = {"id": f"s{index}", "x": round(x, 4), "y": round(y, 4)}
    if w != 1.0:
        seg["w"] = round(w, 4)
    if h != 1.0:
        seg["h"] = round(h, 4)
    return seg


def key(code, x=0.0, y=0.0, w=1.0, h=1.0, base=None, shift=None, sub=None,
        role="normal", segments=None):
    """Build one logical key. `segments` overrides the single-rectangle case."""
    legend = {"base": base if base is not None else code}
    if shift is not None:
        legend["shift"] = shift
    if sub is not None:
        legend["sub"] = sub

    k = {"id": _slug(code), "code": code, "legend": legend}
    if role != "normal":
        k["role"] = role
    k["segments"] = segments if segments else [segment(x, y, w, h)]
    return k


def run(keys, x, y, w=1.0, role="normal"):
    """Place a horizontal run of uniform-width keys.

    Positions step by the *rounded* width rather than the exact one, so that a
    fractional key width (a laptop function row divided across the board) tiles
    exactly instead of leaving sub-thousandth overlaps between neighbours.
    """
    w = round(w, 4)
    out = []
    for i, entry in enumerate(keys):
        code, base = entry[0], entry[1]
        shift = entry[2] if len(entry) > 2 else None
        out.append(key(code, x + i * w, y, w=w, base=base, shift=shift,
                       role=role))
    return out


# --------------------------------------------------------------------------
# Blocks
# --------------------------------------------------------------------------

def alpha_block(keys, *, iso_enter: bool):
    """Rows 1-3 of a standard 15u-wide alphanumeric block.

    Identical across every board we ship apart from the ANSI/ISO difference in
    how Enter and Backslash are arranged.
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
        # L-shape -- two segments, one logical key. The upper part reaches
        # 0.25u further left than the lower, because the row above it holds
        # one fewer key. That overhang is the whole shape.
        keys.append(key("Backslash", 12.75, 3.25, base="\\", shift="|"))
        keys.append(key("Enter", base="Enter", role="system", segments=[
            segment(13.5, 2.25, 1.5, 1, index=0),
            segment(13.75, 3.25, 1.25, 1, index=1),
        ]))
    else:
        keys.append(key("Backslash", 13.5, 2.25, w=1.5, base="\\", shift="|"))
        keys.append(key("Enter", 12.75, 3.25, w=2.25, base="Enter",
                        role="system"))
    return keys


def shift_row(keys, *, iso: bool, right_shift_w: float):
    if iso:
        keys.append(key("ShiftLeft", 0, 4.25, w=1.25, base="Shift",
                        role="modifier"))
        # Not part of Shift: the ISO board has a separate key here, so it is
        # modelled as the separate logical key it is.
        keys.append(key("IntlBackslash", 1.25, 4.25, base="\\", shift="|"))
    else:
        keys.append(key("ShiftLeft", 0, 4.25, w=2.25, base="Shift",
                        role="modifier"))
    keys += run(BOTTOM_ROW, 2.25, 4.25)
    keys.append(key("ShiftRight", 12.25, 4.25, w=right_shift_w,
                    base="Shift", role="modifier"))
    return keys


def numpad_block(keys, x0: float):
    """The standard 4-column numeric keypad, anchored at `x0`."""
    keys.append(key("NumLock", x0, 1.25, base="Num", role="toggle"))
    keys.append(key("NumpadDivide", x0 + 1, 1.25, base="/", role="numpad"))
    keys.append(key("NumpadMultiply", x0 + 2, 1.25, base="*", role="numpad"))
    keys.append(key("NumpadSubtract", x0 + 3, 1.25, base="-", role="numpad"))
    for i, code in enumerate(["Numpad7", "Numpad8", "Numpad9"]):
        keys.append(key(code, x0 + i, 2.25, base=code[-1], role="numpad"))
    keys.append(key("NumpadAdd", x0 + 3, 2.25, h=2, base="+", role="numpad"))
    for i, code in enumerate(["Numpad4", "Numpad5", "Numpad6"]):
        keys.append(key(code, x0 + i, 3.25, base=code[-1], role="numpad"))
    for i, code in enumerate(["Numpad1", "Numpad2", "Numpad3"]):
        keys.append(key(code, x0 + i, 4.25, base=code[-1], role="numpad"))
    keys.append(key("NumpadEnter", x0 + 3, 4.25, h=2, base="Enter",
                    role="numpad"))
    keys.append(key("Numpad0", x0, 5.25, w=2, base="0", role="numpad"))
    keys.append(key("NumpadDecimal", x0 + 2, 5.25, base=".", role="numpad"))
    return keys


def laptop_bottom_row(keys, *, fn_first: bool, width: float):
    """A laptop modifier row. ThinkPads put Fn leftmost; most others put Ctrl.

    Arrows are the inverted-T every compact laptop uses: full-height left and
    right, half-height up and down stacked between them.
    """
    if fn_first:
        keys.append(key("Fn", 0, 5.25, base="Fn", role="modifier"))
        keys.append(key("ControlLeft", 1, 5.25, base="Ctrl", role="modifier"))
        keys.append(key("MetaLeft", 2, 5.25, base="Win", role="modifier"))
        keys.append(key("AltLeft", 3, 5.25, w=1.25, base="Alt", role="modifier"))
        keys.append(key("Space", 4.25, 5.25, w=5.5, base=""))
        keys.append(key("AltRight", 9.75, 5.25, base="Alt", role="modifier"))
        keys.append(key("PrintScreen", 10.75, 5.25, base="PrtSc", role="system"))
        keys.append(key("ControlRight", 11.75, 5.25, base="Ctrl",
                        role="modifier"))
        arrow_x = 12.75
    else:
        keys.append(key("ControlLeft", 0, 5.25, w=1.25, base="Ctrl",
                        role="modifier"))
        keys.append(key("Fn", 1.25, 5.25, base="Fn", role="modifier"))
        keys.append(key("MetaLeft", 2.25, 5.25, base="Win", role="modifier"))
        keys.append(key("AltLeft", 3.25, 5.25, w=1.25, base="Alt",
                        role="modifier"))
        keys.append(key("Space", 4.5, 5.25, w=5.5, base=""))
        keys.append(key("AltRight", 10, 5.25, base="Alt", role="modifier"))
        keys.append(key("ControlRight", 11, 5.25, base="Ctrl", role="modifier"))
        arrow_x = 12.0

    aw = round((width - arrow_x) / 3, 4)
    keys.append(key("ArrowLeft", arrow_x, 5.25, w=aw, base="◀", role="nav"))
    keys.append(key("ArrowUp", arrow_x + aw, 5.25, w=aw, h=0.5, base="▲",
                    role="nav"))
    keys.append(key("ArrowDown", arrow_x + aw, 5.75, w=aw, h=0.5, base="▼",
                    role="nav"))
    keys.append(key("ArrowRight", arrow_x + 2 * aw, 5.25, w=aw, base="▶",
                    role="nav"))
    return keys


# --------------------------------------------------------------------------
# Full-size desktop boards
# --------------------------------------------------------------------------

def full_size(iso: bool):
    keys: list[dict] = []

    keys.append(key("Escape", 0, 0, base="Esc", role="system"))
    keys += run(FN_KEYS[0:4], 2, 0, role="system")
    keys += run(FN_KEYS[4:8], 6.5, 0, role="system")
    keys += run(FN_KEYS[8:12], 11, 0, role="system")
    keys.append(key("PrintScreen", 15.25, 0, base="PrtSc", role="system"))
    keys.append(key("ScrollLock", 16.25, 0, base="Scroll", role="toggle"))
    keys.append(key("Pause", 17.25, 0, base="Pause", role="system"))

    keys = alpha_block(keys, iso_enter=iso)
    keys = shift_row(keys, iso=iso, right_shift_w=2.75)

    keys.append(key("ControlLeft", 0, 5.25, w=1.25, base="Ctrl", role="modifier"))
    keys.append(key("MetaLeft", 1.25, 5.25, w=1.25, base="Win", role="modifier"))
    keys.append(key("AltLeft", 2.5, 5.25, w=1.25, base="Alt", role="modifier"))
    keys.append(key("Space", 3.75, 5.25, w=6.25, base=""))
    keys.append(key("AltRight", 10, 5.25, w=1.25, base="Alt", role="modifier"))
    keys.append(key("MetaRight", 11.25, 5.25, w=1.25, base="Win",
                    role="modifier"))
    keys.append(key("ContextMenu", 12.5, 5.25, w=1.25, base="Menu",
                    role="system"))
    keys.append(key("ControlRight", 13.75, 5.25, w=1.25, base="Ctrl",
                    role="modifier"))

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

    keys = numpad_block(keys, 18.5)
    return keys


# --------------------------------------------------------------------------
# Laptop boards
# --------------------------------------------------------------------------

def thinkpad():
    """Modelled on current ThinkPad T-series boards, including the T16.

    Distinguishing traits: Fn is the leftmost key on the bottom row,
    Home/End/Insert/Delete live at the right end of the function row, and
    PgUp/PgDn flank the arrow cluster. No numeric keypad, on the 14-inch and
    16-inch chassis alike.
    """
    keys: list[dict] = []
    width = 15.5

    top = ([("Escape", "Esc")] + FN_KEYS +
           [("Home", "Home"), ("End", "End"), ("Insert", "Ins"),
            ("Delete", "Del")])
    keys += run(top, 0, 0, w=width / len(top), role="system")

    keys = alpha_block(keys, iso_enter=False)
    keys = shift_row(keys, iso=False, right_shift_w=1.5)
    keys.append(key("PageUp", 13.75, 4.25, w=0.875, base="PgUp", role="nav"))
    keys.append(key("PageDown", 14.625, 4.25, w=0.875, base="PgDn", role="nav"))

    keys = laptop_bottom_row(keys, fn_first=True, width=width)
    return keys


def zenbook():
    """Modelled on Asus Zenbook and the smaller Vivobooks -- no numeric keypad.

    Ctrl is leftmost with Fn second, and Delete ends the function row.
    """
    keys: list[dict] = []
    width = 15.0

    top = [("Escape", "Esc")] + FN_KEYS + [("Delete", "Del")]
    keys += run(top, 0, 0, w=width / len(top), role="system")

    keys = alpha_block(keys, iso_enter=False)
    keys = shift_row(keys, iso=False, right_shift_w=2.75)
    keys = laptop_bottom_row(keys, fn_first=False, width=width)
    return keys


def vivobook_s():
    """Modelled on the Asus Vivobook S 15 / S 16.

    The larger Vivobooks carry a numeric keypad, which is what separates this
    from the Zenbook template: four extra columns on the right, plus a nav row
    above them.
    """
    keys: list[dict] = []
    alpha_width = 15.0
    numpad_x = 15.25

    top = [("Escape", "Esc")] + FN_KEYS + [("Delete", "Del")]
    keys += run(top, 0, 0, w=alpha_width / len(top), role="system")
    for i, (code, label) in enumerate(
        [("PrintScreen", "PrtSc"), ("Home", "Home"), ("End", "End"),
         ("Insert", "Ins")]
    ):
        keys.append(key(code, numpad_x + i, 0, base=label, role="nav"))

    keys = alpha_block(keys, iso_enter=False)
    keys = shift_row(keys, iso=False, right_shift_w=2.75)
    keys = laptop_bottom_row(keys, fn_first=False, width=alpha_width)
    keys = numpad_block(keys, numpad_x)
    return keys


# --------------------------------------------------------------------------

def document(layout_id, name, description, keys, *, model=None,
             author="MouseTrapKeys"):
    # The extent follows from the keys themselves: a fractional key width
    # rounded for the file can otherwise overhang a hand-written total.
    def right(k):
        return max(s["x"] + s.get("w", 1.0) for s in k["segments"])

    def bottom(k):
        return max(s["y"] + s.get("h", 1.0) for s in k["segments"])

    return {
        "schema": SCHEMA,
        "id": layout_id,
        "name": name,
        "description": description,
        "metadata": {
            "author": author,
            "source_template": None,
            "model": model,
            "revision": 1,
        },
        "size": {
            "w": round(max(right(k) for k in keys), 4),
            "h": round(max(bottom(k) for k in keys), 4),
        },
        "keys": keys,
    }


def write(doc):
    path = OUT_DIR / f"{doc['id']}.json"
    path.write_text(json.dumps(doc, indent=2, ensure_ascii=False) + "\n",
                    encoding="utf-8")
    segments = sum(len(k["segments"]) for k in doc["keys"])
    shaped = sum(1 for k in doc["keys"] if len(k["segments"]) > 1)
    extra = f"  ({shaped} multi-segment)" if shaped else ""
    print(f"{path.name:24} {len(doc['keys']):3d} keys  {segments:3d} seg{extra}"
          f"  {doc['size']['w']}x{doc['size']['h']}u")


def main():
    OUT_DIR.mkdir(parents=True, exist_ok=True)

    write(document(
        "us-ansi-104", "US Full-Size (ANSI 104)",
        "Standard US full-size desktop keyboard with function row, navigation "
        "cluster and numeric keypad.",
        full_size(iso=False), model="Generic ANSI full-size"))

    write(document(
        "us-iso-105", "US-International Full-Size (ISO 105)",
        "Full-size ISO variant: the tall L-shaped Enter, a short left Shift, "
        "and the extra IntlBackslash key beside it. This is the board usually "
        "meant by \"105-key\".",
        full_size(iso=True), model="Generic ISO full-size"))

    write(document(
        "thinkpad-compact", "ThinkPad (laptop)",
        "Compact laptop layout modelled on current ThinkPad T-series boards, "
        "14-inch and 16-inch alike. Fn is leftmost; Home/End/Ins/Del sit in "
        "the function row; no numeric keypad. A starting template, not a "
        "model-exact reproduction.",
        thinkpad(), model="ThinkPad T14 / T16 Gen 5 class"))

    write(document(
        "asus-zenbook", "Asus Zenbook (laptop)",
        "Compact laptop layout modelled on Asus Zenbook and the smaller "
        "Vivobook boards. Ctrl is leftmost, Fn second; Delete ends the "
        "function row; no numeric keypad.",
        zenbook(), model="Asus Zenbook / Vivobook 14 class"))

    write(document(
        "asus-vivobook-s", "Asus Vivobook S 15/16 (laptop)",
        "Asus Vivobook S 15 / S 16, which unlike the Zenbook carries a "
        "numeric keypad and a navigation row above it. A starting template, "
        "not a model-exact reproduction.",
        vivobook_s(), model="Asus Vivobook S 15 / S 16"))


if __name__ == "__main__":
    main()

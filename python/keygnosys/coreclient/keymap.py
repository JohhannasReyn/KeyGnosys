"""Qt key events -> the W3C UI Events `code` vocabulary.

Used only by the mock client. The real backends each own their own table
(evdev codes on Linux, scancodes on Windows) as required by principle P2 --
this one exists because Qt is a third input source with its own conventions.

Qt does not distinguish left from right modifiers through `Qt.Key`, so those are
resolved from the native scancode where one is available, defaulting to the left
variant. That is a known and accepted imprecision of the mock backend.
"""

from __future__ import annotations

from PySide6.QtCore import Qt

# --------------------------------------------------------------------------
# Non-modifier keys
# --------------------------------------------------------------------------

_SIMPLE: dict[int, str] = {
    Qt.Key_Escape: "Escape",
    Qt.Key_Tab: "Tab",
    Qt.Key_Backtab: "Tab",
    Qt.Key_Backspace: "Backspace",
    Qt.Key_Return: "Enter",
    Qt.Key_Enter: "NumpadEnter",
    Qt.Key_Insert: "Insert",
    Qt.Key_Delete: "Delete",
    Qt.Key_Pause: "Pause",
    Qt.Key_Print: "PrintScreen",
    Qt.Key_Home: "Home",
    Qt.Key_End: "End",
    Qt.Key_Left: "ArrowLeft",
    Qt.Key_Up: "ArrowUp",
    Qt.Key_Right: "ArrowRight",
    Qt.Key_Down: "ArrowDown",
    Qt.Key_PageUp: "PageUp",
    Qt.Key_PageDown: "PageDown",
    Qt.Key_CapsLock: "CapsLock",
    Qt.Key_NumLock: "NumLock",
    Qt.Key_ScrollLock: "ScrollLock",
    Qt.Key_Space: "Space",
    Qt.Key_Menu: "ContextMenu",
    Qt.Key_Minus: "Minus",
    Qt.Key_Equal: "Equal",
    Qt.Key_BracketLeft: "BracketLeft",
    Qt.Key_BracketRight: "BracketRight",
    Qt.Key_Backslash: "Backslash",
    Qt.Key_Semicolon: "Semicolon",
    Qt.Key_Apostrophe: "Quote",
    Qt.Key_QuoteLeft: "Backquote",
    Qt.Key_Comma: "Comma",
    Qt.Key_Period: "Period",
    Qt.Key_Slash: "Slash",
}

# Shifted punctuation arrives as a different Qt.Key but is the same physical
# key, and the vocabulary is positional -- so both must resolve identically.
_SHIFTED_PUNCTUATION: dict[int, str] = {
    Qt.Key_Exclam: "Digit1", Qt.Key_At: "Digit2", Qt.Key_NumberSign: "Digit3",
    Qt.Key_Dollar: "Digit4", Qt.Key_Percent: "Digit5",
    Qt.Key_AsciiCircum: "Digit6", Qt.Key_Ampersand: "Digit7",
    Qt.Key_Asterisk: "Digit8", Qt.Key_ParenLeft: "Digit9",
    Qt.Key_ParenRight: "Digit0",
    Qt.Key_Underscore: "Minus", Qt.Key_Plus: "Equal",
    Qt.Key_BraceLeft: "BracketLeft", Qt.Key_BraceRight: "BracketRight",
    Qt.Key_Bar: "Backslash", Qt.Key_Colon: "Semicolon",
    Qt.Key_QuoteDbl: "Quote", Qt.Key_AsciiTilde: "Backquote",
    Qt.Key_Less: "Comma", Qt.Key_Greater: "Period", Qt.Key_Question: "Slash",
}

_NUMPAD: dict[int, str] = {
    Qt.Key_0: "Numpad0", Qt.Key_1: "Numpad1", Qt.Key_2: "Numpad2",
    Qt.Key_3: "Numpad3", Qt.Key_4: "Numpad4", Qt.Key_5: "Numpad5",
    Qt.Key_6: "Numpad6", Qt.Key_7: "Numpad7", Qt.Key_8: "Numpad8",
    Qt.Key_9: "Numpad9",
    Qt.Key_Asterisk: "NumpadMultiply", Qt.Key_Plus: "NumpadAdd",
    Qt.Key_Minus: "NumpadSubtract", Qt.Key_Slash: "NumpadDivide",
    Qt.Key_Period: "NumpadDecimal", Qt.Key_Comma: "NumpadDecimal",
    Qt.Key_Insert: "Numpad0", Qt.Key_End: "Numpad1", Qt.Key_Down: "Numpad2",
    Qt.Key_PageDown: "Numpad3", Qt.Key_Left: "Numpad4",
    Qt.Key_Clear: "Numpad5", Qt.Key_Right: "Numpad6", Qt.Key_Home: "Numpad7",
    Qt.Key_Up: "Numpad8", Qt.Key_PageUp: "Numpad9",
    Qt.Key_Delete: "NumpadDecimal",
}

#: Windows scancode set 1 values for the modifier keys, used to tell left from
#: right. Qt sets bit 8 for extended keys, which is what distinguishes the
#: right-hand Ctrl and Alt from their left-hand twins.
_MODIFIER_SCANCODES: dict[int, str] = {
    0x2A: "ShiftLeft", 0x36: "ShiftRight",
    0x1D: "ControlLeft", 0x11D: "ControlRight",
    0x38: "AltLeft", 0x138: "AltRight",
    0x15B: "MetaLeft", 0x15C: "MetaRight",
    0x5B: "MetaLeft", 0x5C: "MetaRight",
}

_MODIFIER_DEFAULTS: dict[int, str] = {
    Qt.Key_Shift: "ShiftLeft",
    Qt.Key_Control: "ControlLeft",
    Qt.Key_Alt: "AltLeft",
    Qt.Key_AltGr: "AltRight",
    Qt.Key_Meta: "MetaLeft",
}


def qt_key_to_code(key: int, modifiers, scancode: int = 0) -> str | None:
    """Resolve a Qt key event to a vocabulary code, or None if unmapped."""
    if key in _MODIFIER_DEFAULTS:
        named = _MODIFIER_SCANCODES.get(scancode)
        return named or _MODIFIER_DEFAULTS[key]

    if modifiers & Qt.KeypadModifier:
        mapped = _NUMPAD.get(key)
        if mapped:
            return mapped
        if key == Qt.Key_Enter:
            return "NumpadEnter"

    if Qt.Key_F1 <= key <= Qt.Key_F24:
        return f"F{key - Qt.Key_F1 + 1}"

    if Qt.Key_A <= key <= Qt.Key_Z:
        return f"Key{chr(key)}"

    if Qt.Key_0 <= key <= Qt.Key_9:
        return f"Digit{chr(key)}"

    return _SIMPLE.get(key) or _SHIFTED_PUNCTUATION.get(key)

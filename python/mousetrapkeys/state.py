"""Runtime state and legend resolution.

Deliberately free of Qt imports: this is the logic that decides *what* each key
says and how it should be drawn, which makes it unit-testable without a display.
The widgets consult it; they do not duplicate it.

Legend resolution follows docs/SPEC.md section 9.4.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum

from . import actions
from .documents import BindingSet, Layout, Profile, canonical_modifiers


class LegendLayer(str, Enum):
    BASE = "base"
    SHIFT = "shift"
    MODIFIER = "modifier"
    CURSOR = "cursor"


class KeyStyle(str, Enum):
    """How a key should be drawn, independent of theme."""
    NORMAL = "normal"
    PRESSED = "pressed"      # physically down right now
    LATCHED = "latched"      # held modifier, or an engaged toggle
    UNBOUND = "unbound"      # cursor layer is on and this key does nothing


@dataclass
class KeyRender:
    """Everything the painter needs about one key, already resolved."""
    text: str
    style: KeyStyle
    dim: bool = False
    sub: str | None = None
    led: bool | None = None      # None = no LED, else on/off


#: Legend length caps. The painter wraps multi-word labels onto two lines, so
#: these are generous enough for real shortcut names ("Reopen closed tab") and
#: real window titles, and only bite on genuinely runaway text.
SHORTCUT_LIMIT = 22
SLOT_NAME_LIMIT = 16

MODIFIER_CODES = {
    "ShiftLeft": "Shift", "ShiftRight": "Shift",
    "ControlLeft": "Control", "ControlRight": "Control",
    "AltLeft": "Alt", "AltRight": "Alt",
    "MetaLeft": "Meta", "MetaRight": "Meta",
}


@dataclass
class AppState:
    """Everything the overlay knows about the world right now."""

    layout: Layout | None = None
    binding_set: BindingSet | None = None
    profile: Profile | None = None

    cursor_layer: bool = False
    caps_lock_led: bool = False
    num_lock_led: bool = True
    scroll_lock_led: bool = False

    #: Physical key codes currently down.
    pressed: set[str] = field(default_factory=set)
    #: Codes fading out after release, mapped to a 0..1 fade progress.
    fading: dict[str, float] = field(default_factory=dict)

    #: Window slot index -> display name, for window.slot legends.
    slot_names: dict[int, str] = field(default_factory=dict)

    focus_process: str | None = None
    focus_wm_class: str | None = None
    focus_title: str | None = None

    connected: bool = False
    backend_name: str = "none"

    # -- derived modifier state -------------------------------------------

    def held_modifiers(self) -> set[str]:
        return {MODIFIER_CODES[c] for c in self.pressed if c in MODIFIER_CODES}

    def modifier_combo(self) -> str:
        """The canonical modifier string for app-shortcut lookup.

        Shift alone is deliberately excluded: a lone Shift selects the shift
        legend layer rather than a shortcut layer (SPEC section 4.4).
        """
        mods = self.held_modifiers()
        if mods == {"Shift"} or not mods:
            return ""
        return canonical_modifiers(mods)

    def active_layer(self) -> LegendLayer:
        if self.cursor_layer:
            return LegendLayer.CURSOR
        if self.modifier_combo():
            return LegendLayer.MODIFIER
        if "Shift" in self.held_modifiers():
            return LegendLayer.SHIFT
        return LegendLayer.BASE

    # -- legend resolution -------------------------------------------------

    def render_key(self, code: str, base: str, shift: str | None,
                   sub: str | None, role: str) -> KeyRender:
        """Resolve one key to its text and style for the current state."""
        layer = self.active_layer()

        if layer is LegendLayer.CURSOR:
            text, dim = self._cursor_legend(code)
            style = self._style_for(code, role, unbound=dim)
            return KeyRender(text=text, style=style, dim=dim,
                             sub=None, led=self._led_for(code))

        if layer is LegendLayer.MODIFIER:
            text, dim = self._shortcut_legend(code, base)
            return KeyRender(text=text, style=self._style_for(code, role),
                             dim=dim, sub=None, led=self._led_for(code))

        if layer is LegendLayer.SHIFT:
            text = shift if shift is not None else base.upper()
            return KeyRender(text=text, style=self._style_for(code, role),
                             dim=False, sub=sub, led=self._led_for(code))

        return KeyRender(text=base, style=self._style_for(code, role),
                         dim=False, sub=sub, led=self._led_for(code))

    def _cursor_legend(self, code: str) -> tuple[str, bool]:
        """(text, dim) for a key while the cursor layer is engaged."""
        if self.binding_set is None:
            return "", True
        binding = self.binding_set.bindings.get(code)
        if binding is None:
            return "", True

        # A window-slot binding is far more useful showing the app that is
        # actually in that slot than showing "App 3".
        if binding.action == "window.slot":
            index = binding.params.get("index")
            name = self.slot_names.get(index)
            if name:
                return _elide(name, SLOT_NAME_LIMIT), False

        if binding.legend:
            return binding.legend, False
        return actions.default_legend(binding.action, binding.params), False

    def _shortcut_legend(self, code: str, base: str) -> tuple[str, bool]:
        """(text, dim) for a key while a shortcut modifier is held.

        On a miss we return the base legend dimmed rather than nothing: a
        keyboard that goes blank reads as a bug, not as "no shortcut here".
        """
        combo = self.modifier_combo()
        if self.profile is None or not combo:
            return base, True
        table = self.profile.shortcuts.get(combo)
        if not table:
            return base, True
        text = table.get(code)
        if not text:
            return base, True
        return _elide(text, SHORTCUT_LIMIT), False

    def _style_for(self, code: str, role: str, unbound: bool = False) -> KeyStyle:
        if code in self.pressed:
            if role == "modifier":
                return KeyStyle.LATCHED
            return KeyStyle.PRESSED
        # Toggles report their state through the LED dot, not by lighting the
        # whole key: NumLock is on almost always, and a permanently highlighted
        # key trains the eye to ignore highlighting.
        if code == "CapsLock" and self.cursor_layer:
            return KeyStyle.LATCHED
        if unbound:
            return KeyStyle.UNBOUND
        return KeyStyle.NORMAL

    def _led_for(self, code: str) -> bool | None:
        if code == "CapsLock":
            # While the layer is engaged the LED reports the layer, not the
            # OS caps state -- the layer is what the user needs to see.
            return self.cursor_layer or self.caps_lock_led
        if code == "NumLock":
            return self.num_lock_led
        if code == "ScrollLock":
            return self.scroll_lock_led
        return None

    # -- mutation ---------------------------------------------------------

    def press(self, code: str) -> None:
        self.pressed.add(code)
        self.fading.pop(code, None)

    def release(self, code: str) -> None:
        if code in self.pressed:
            self.pressed.discard(code)
            self.fading[code] = 1.0

    def release_all(self) -> None:
        for code in list(self.pressed):
            self.release(code)

    def update_profile(self, registry) -> bool:
        """Re-match the app profile against the focused window.

        Returns True if the active profile changed.
        """
        new = registry.match_profile(self.focus_process, self.focus_wm_class,
                                     self.focus_title)
        if new is not self.profile:
            self.profile = new
            return True
        return False


def _elide(text: str, limit: int) -> str:
    return text if len(text) <= limit else text[: limit - 1] + "…"

"""Runtime state and legend resolution.

Deliberately free of Qt imports: this is the logic that decides *what* each key
says and how it should be drawn, which makes it unit-testable without a display.
The widgets consult it; they do not duplicate it.

Legend resolution follows docs/SPEC.md section 9.4.
"""

from __future__ import annotations

import time
from dataclasses import dataclass, field
from enum import Enum

from . import actions
from .documents import BindingSet, Key, Layout, Profile, canonical_modifiers


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

#: How long the unprotected banner flashes after a refused drag lock.
PROTECTION_FLASH_S = 2.0

UNPROTECTED_BANNER = "Unprotected — drag lock off"

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
    #: The core cannot see keys because an elevated window has focus.
    inert: bool = False
    #: "not_applicable" | "protected" | "unprotected" (crash-release guardian).
    protection: str = "not_applicable"
    protection_reason: str = ""
    #: time.monotonic() deadline of a refused-drag-lock flash; 0 = none.
    flash_until: float = 0.0
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

    def render_key(self, key: Key) -> KeyRender:
        """Resolve one logical key to its text and style for the current state.

        Takes the whole key rather than its parts because a key is one thing:
        one identity, one label, one highlight -- however many segments it is
        drawn from.
        """
        code, role = key.code, key.role
        layer = self.active_layer()

        if layer is LegendLayer.CURSOR:
            text, dim = self._cursor_legend(code)
            # Inert: the layer is not acting, so no key is live. Keep the text
            # so the user still sees what the layer would do.
            dim = dim or self.inert
            style = self._style_for(code, role, unbound=dim)
            return KeyRender(text=text, style=style, dim=dim,
                             sub=None, led=self._led_for(code))

        if layer is LegendLayer.MODIFIER:
            text, dim = self._shortcut_legend(code, key.base)
            return KeyRender(text=text, style=self._style_for(code, role),
                             dim=dim, sub=None, led=self._led_for(code))

        # Rules 3 and 4 both consult the profile first and NEITHER dims on a
        # miss (SPEC section 9.4). An application that commands with bare keys
        # -- Blender's G/R/S, Tab for edit mode -- is the case the modifier-only
        # lookup was blind to, and it is the case where naming the key's action
        # is worth most. Dimming every unclaimed letter would make ordinary
        # typing look broken, so a miss simply leaves the key as it was.
        if layer is LegendLayer.SHIFT:
            text = self._profile_shortcut("Shift", code)
            if text is None:
                text = key.shift if key.shift is not None else key.base.upper()
            return KeyRender(text=text, style=self._style_for(code, role),
                             dim=False, sub=key.sub, led=self._led_for(code))

        text = self._profile_shortcut("", code)
        return KeyRender(text=key.base if text is None else text,
                         style=self._style_for(code, role),
                         dim=False, sub=key.sub, led=self._led_for(code))

    def _profile_shortcut(self, combo: str, code: str) -> str | None:
        """The active profile's description for `combo` + `code`, or None.

        `combo` is a canonical modifier string; `""` is the bare key.
        """
        if self.profile is None:
            return None
        table = self.profile.shortcuts.get(combo)
        if not table:
            return None
        text = table.get(code)
        return _elide(text, SHORTCUT_LIMIT) if text else None

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
        if not combo:
            return base, True
        text = self._profile_shortcut(combo, code)
        return (base, True) if text is None else (text, False)

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

    def status_text(self) -> tuple[str, str] | None:
        """(status, tooltip) overriding the connection status, or None."""
        if self.inert:
            return (
                "layer inactive: admin window has focus",
                "Windows does not let a non-elevated program read keys typed "
                "into an elevated (administrator) window. The layer resumes "
                "when an ordinary window has focus.",
            )
        if self.protection == "unprotected":
            reason = self.protection_reason or "unknown"
            return (
                "unprotected: drag lock off",
                "Nothing will lift a held mouse button if the core crashes, "
                f"so drag lock is disabled. Reason: {reason}.",
            )
        return None

    def flash_protection(self, now: float | None = None) -> None:
        """Show the unprotected banner briefly (a drag lock was refused)."""
        now = time.monotonic() if now is None else now
        self.flash_until = now + PROTECTION_FLASH_S

    def banner_flashing(self, now: float | None = None) -> bool:
        """True while a refused drag lock is being signalled. The unprotected
        banner is already standing whenever a refusal can happen, so the view
        must draw it differently for the flash to be seen at all."""
        if self.inert:
            return False
        now = time.monotonic() if now is None else now
        return now < self.flash_until

    def banner_text(self, now: float | None = None) -> str | None:
        """Large on-keyboard notice. Inert wins over unprotected."""
        if self.inert:
            return "Layer inactive: admin window has focus"
        if self.protection == "unprotected":
            return UNPROTECTED_BANNER
        now = time.monotonic() if now is None else now
        if now < self.flash_until:
            return UNPROTECTED_BANNER
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

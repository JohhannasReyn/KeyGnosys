"""Resolved colours for the painter.

Themes are colour-token documents rather than stylesheets (SPEC section 4.3),
so this module is the one place that knows how a token becomes a QColor -- and
the one place that applies the user's global opacity and accent override.
"""

from __future__ import annotations

from dataclasses import dataclass

from PySide6.QtGui import QColor

from ..documents import KeyStyle, Theme


def parse_color(value: str, opacity: float = 1.0) -> QColor:
    """Parse ``#RRGGBB`` or ``#RRGGBBAA``, scaling alpha by `opacity`."""
    text = (value or "").strip().lstrip("#")
    if len(text) == 6:
        r, g, b, a = *(int(text[i:i + 2], 16) for i in (0, 2, 4)), 255
    elif len(text) == 8:
        r, g, b, a = (int(text[i:i + 2], 16) for i in (0, 2, 4, 6))
    else:
        return QColor(255, 0, 255)          # loud magenta: a bad token shows
    return QColor(r, g, b, max(0, min(255, round(a * opacity))))


@dataclass
class ResolvedTheme:
    """A theme flattened to QColors, ready for the painter."""

    surface: QColor
    key_face: QColor
    key_face_alt: QColor
    key_border: QColor
    key_text: QColor
    key_text_dim: QColor
    key_sub_text: QColor
    accent: QColor
    accent_text: QColor
    latched: QColor
    led_on: QColor
    led_off: QColor
    bar_surface: QColor
    bar_text: QColor
    bar_border: QColor
    is_dark: bool

    @classmethod
    def build(cls, theme: Theme, opacity: float = 1.0,
              accent_override: str | None = None) -> "ResolvedTheme":
        tokens = dict(theme.tokens)
        if accent_override:
            tokens["accent"] = accent_override

        # Text and borders keep full alpha: fading them with the surface makes
        # the legends unreadable long before the background is subtle.
        opaque = {"key_text", "key_text_dim", "key_sub_text", "accent",
                  "accent_text", "latched", "led_on", "bar_text"}

        def token(name: str, fallback: str = "#ff00ff") -> QColor:
            raw = tokens.get(name, fallback)
            return parse_color(raw, 1.0 if name in opaque else opacity)

        return cls(
            surface=token("surface"),
            key_face=token("key_face"),
            key_face_alt=token("key_face_alt"),
            key_border=token("key_border"),
            key_text=token("key_text"),
            key_text_dim=token("key_text_dim"),
            key_sub_text=token("key_sub_text"),
            accent=token("accent"),
            accent_text=token("accent_text"),
            latched=token("latched"),
            led_on=token("led_on"),
            led_off=token("led_off"),
            bar_surface=token("bar_surface"),
            bar_text=token("bar_text"),
            bar_border=token("bar_border", tokens.get("key_border", "#333333")),
            is_dark=theme.base == "dark",
        )

    def face_for_role(self, role: str) -> QColor:
        """Modifier and system keys sit visually behind the character keys."""
        if role in ("modifier", "system", "toggle", "nav", "numpad"):
            return self.key_face_alt
        return self.key_face

    # -- per-key overrides (SPEC 4.1.4) -----------------------------------
    #
    # A layout may set colours on an individual key, so a user can mark up
    # their own board -- tinting the keys they are still learning, say --
    # without forking an entire theme. Anything unset falls through.

    def key_face_override(self, style: KeyStyle | None, role: str) -> QColor:
        if style is not None and style.face:
            return parse_color(style.face)
        return self.face_for_role(role)

    def key_text_override(self, style: KeyStyle | None,
                          dim: bool = False) -> QColor:
        if style is not None and style.text:
            return parse_color(style.text)
        return self.key_text_dim if dim else self.key_text

    def key_border_override(self, style: KeyStyle | None) -> QColor:
        if style is not None and style.border:
            return parse_color(style.border)
        return self.key_border

    def accent_override(self, style: KeyStyle | None) -> QColor:
        """This key's feedback colour."""
        if style is not None and style.accent:
            return parse_color(style.accent)
        return self.accent


def system_is_dark() -> bool:
    """Best-effort read of the OS colour preference.

    Qt 6.5+ reports this through the style hints; older builds fall back to
    dark, which is the more forgiving default for a translucent overlay.
    """
    try:
        from PySide6.QtGui import QGuiApplication
        from PySide6.QtCore import Qt as _Qt

        hints = QGuiApplication.styleHints()
        scheme = hints.colorScheme()
        if scheme == _Qt.ColorScheme.Light:
            return False
        if scheme == _Qt.ColorScheme.Dark:
            return True
    except (ImportError, AttributeError):
        pass
    return True

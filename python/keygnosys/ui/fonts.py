"""The overlay's font.

Key legends are mostly one or two characters at small point sizes, and they sit
on a translucent background -- so the face matters more here than it usually
would. The stack prefers each platform's UI font, then falls back to whatever
the system offers rather than leaving Qt to pick an unstyled default.

Legends also use box-drawing and arrow glyphs, which not every face carries.
`has_glyphs` reports what is actually renderable so the caller can substitute
text rather than silently drawing tofu.
"""

from __future__ import annotations

import sys

from PySide6.QtGui import QFont, QFontDatabase

#: Preferred families, best first.
FAMILIES = {
    "win32": ["Segoe UI", "Tahoma", "Arial"],
    "darwin": ["SF Pro Text", "Helvetica Neue", "Helvetica"],
}
FALLBACK = ["Noto Sans", "DejaVu Sans", "Liberation Sans", "Cantarell", "Arial"]


def ui_font(point_size: float = 9.0) -> QFont:
    """A concrete font for the overlay, with a real fallback chain."""
    candidates = FAMILIES.get(sys.platform, []) + FALLBACK
    available = set(QFontDatabase.families())
    for family in candidates:
        if family in available:
            font = QFont(family, int(point_size))
            font.setStyleStrategy(QFont.PreferAntialias)
            return font

    # Nothing on the preferred list exists (a bare container, say). Take the
    # system's own default rather than naming a family that does not resolve.
    font = QFont()
    font.setPointSizeF(point_size)
    return font


def has_glyphs(font: QFont, text: str) -> bool:
    """True if `font` can actually draw every character of `text`."""
    from PySide6.QtGui import QFontMetricsF
    metrics = QFontMetricsF(font)
    return all(metrics.inFont(ch) for ch in text if ch.strip())

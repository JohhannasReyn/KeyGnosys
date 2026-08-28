"""The on-screen keyboard, drawn as a single custom-painted widget.

One widget rather than one widget per key: a full-size board is 104 keys, and
every keystroke would otherwise mean a style recalculation and a layout pass
across all of them. Painting also lets the units-based geometry map to
sub-pixel positions, which per-widget layout cannot express.

All *decisions* about what a key says and how it should look live in
``state.AppState`` (SPEC section 9.4). This module only draws them.
"""

from __future__ import annotations

from PySide6.QtCore import QRectF, Qt, QTimer, Signal
from PySide6.QtGui import QColor, QFont, QFontMetricsF, QPainter, QPainterPath
from PySide6.QtWidgets import QWidget

from ..documents import Key, Layout, Segment
from ..state import AppState, KeyStyle
from .theme import ResolvedTheme

#: Pixels per layout unit at scale 1.0.
BASE_UNIT_PX = 44.0
#: Gap between adjacent keys, in pixels at scale 1.0.
KEY_GAP_PX = 3.0
#: Padding around the whole board.
BOARD_PAD_PX = 8.0
#: Frames per second for the release fade animation.
FADE_FPS = 60


class KeyboardView(QWidget):
    """Renders a Layout for the current AppState."""

    keyClicked = Signal(str)

    def __init__(self, state: AppState, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self._state = state
        self._theme: ResolvedTheme | None = None
        self._layout: Layout | None = None
        self._scale = 1.0
        self._show_numpad = True
        self._fade_ms = 220
        self.setAttribute(Qt.WA_TranslucentBackground, True)
        self.setMouseTracking(False)

        #: key id -> outline path, invalidated whenever geometry changes.
        self._path_cache: dict[str, QPainterPath] = {}

        self._fade_timer = QTimer(self)
        self._fade_timer.setInterval(int(1000 / FADE_FPS))
        self._fade_timer.timeout.connect(self._advance_fade)

    # -- configuration ----------------------------------------------------

    def set_layout_doc(self, layout: Layout | None) -> None:
        self._layout = layout
        self._path_cache.clear()
        self.updateGeometry()
        self._resize_to_layout()
        self.update()

    def set_theme(self, theme: ResolvedTheme) -> None:
        self._theme = theme
        self.update()

    def set_scale(self, scale: float) -> None:
        self._scale = max(0.4, min(2.0, scale))
        self._path_cache.clear()
        self._resize_to_layout()
        self.update()

    def set_show_numpad(self, show: bool) -> None:
        self._show_numpad = show
        self._resize_to_layout()
        self.update()

    def set_fade_ms(self, ms: int) -> None:
        self._fade_ms = max(0, ms)

    # -- geometry ---------------------------------------------------------

    @property
    def unit(self) -> float:
        return BASE_UNIT_PX * self._scale

    def _visible_keys(self) -> list[Key]:
        if self._layout is None:
            return []
        if self._show_numpad:
            return self._layout.keys
        return [k for k in self._layout.keys if k.role != "numpad"]

    def _content_size(self) -> tuple[float, float]:
        keys = self._visible_keys()
        if not keys:
            return 320.0, 120.0
        width = max(k.right for k in keys)
        height = max(k.bottom for k in keys)
        pad = BOARD_PAD_PX * self._scale
        return width * self.unit + pad * 2, height * self.unit + pad * 2

    def _resize_to_layout(self) -> None:
        w, h = self._content_size()
        self.setFixedSize(int(round(w)), int(round(h)))

    def sizeHint(self):
        w, h = self._content_size()
        from PySide6.QtCore import QSize
        return QSize(int(round(w)), int(round(h)))

    def segment_rect(self, segment: Segment) -> QRectF:
        pad = BOARD_PAD_PX * self._scale
        gap = KEY_GAP_PX * self._scale
        return QRectF(
            pad + segment.x * self.unit + gap / 2,
            pad + segment.y * self.unit + gap / 2,
            segment.w * self.unit - gap,
            segment.h * self.unit - gap,
        )

    def key_rect(self, key: Key) -> QRectF:
        """The key's bounding box, for damage regions and hit tests."""
        rects = [self.segment_rect(s) for s in key.segments]
        out = rects[0]
        for r in rects[1:]:
            out = out.united(r)
        return out

    def key_path(self, key: Key) -> QPainterPath:
        """The key's outline: the union of its segments, drawn as one shape.

        Interior corners -- where two segments meet -- must stay square. A real
        ISO Enter has a sharp inner corner, and rounding it produces a visible
        pinch. Uniting rounded rectangles alone would round every corner, so
        each shared edge gets a plain "bridge" rectangle spanning it, which
        fills the notches the rounding would otherwise leave.

        Paths are cached per key: this is called on every repaint, and a union
        is not cheap enough to redo for a keystroke.
        """
        cached = self._path_cache.get(key.id)
        if cached is not None:
            return cached

        radius = 5.0 * self._scale
        rects = [self.segment_rect(s) for s in key.segments]

        path = QPainterPath()
        for rect in rects:
            rounded = QPainterPath()
            rounded.addRoundedRect(rect, radius, radius)
            path = path.united(rounded)

        for i, a in enumerate(rects):
            for b in rects[i + 1:]:
                bridge = _bridge(a, b, radius)
                if bridge is not None:
                    filler = QPainterPath()
                    filler.addRect(bridge)
                    path = path.united(filler)

        self._path_cache[key.id] = path
        return path

    def label_rect(self, key: Key) -> QRectF:
        """Where the legend goes: the largest segment, not the bounding box.

        The centre of an L-shape's bounding box is in the notch, outside the
        key entirely.
        """
        return self.segment_rect(key.largest_segment)

    def key_at(self, pos) -> Key | None:
        """A point inside any segment hits the whole key."""
        for key in self._visible_keys():
            if any(self.segment_rect(s).contains(pos) for s in key.segments):
                return key
        return None

    # -- feedback ---------------------------------------------------------

    def note_key_event(self, code: str, state: str) -> None:
        """Repaint just the affected key, plus start the fade timer if needed."""
        if state == "down":
            self._state.press(code)
        elif state == "up":
            self._state.release(code)
            if self._fade_ms > 0 and not self._fade_timer.isActive():
                self._fade_timer.start()
        self._repaint_key(code)

    def _repaint_key(self, code: str) -> None:
        for key in self._visible_keys():
            if key.code == code:
                # Margin covers the glow drawn outside the key's own rect.
                self.update(self.key_rect(key).adjusted(-4, -4, 4, 4).toRect())

    def _advance_fade(self) -> None:
        if self._fade_ms <= 0:
            self._state.fading.clear()
            self._fade_timer.stop()
            self.update()
            return
        step = (1000 / FADE_FPS) / self._fade_ms
        done = []
        for code in list(self._state.fading):
            self._state.fading[code] -= step
            if self._state.fading[code] <= 0:
                done.append(code)
            self._repaint_key(code)
        for code in done:
            self._state.fading.pop(code, None)
            self._repaint_key(code)
        if not self._state.fading:
            self._fade_timer.stop()

    # -- painting ---------------------------------------------------------

    def paintEvent(self, event) -> None:
        if self._theme is None or self._layout is None:
            return
        painter = QPainter(self)
        painter.setRenderHint(QPainter.Antialiasing, True)
        painter.setRenderHint(QPainter.TextAntialiasing, True)

        theme = self._theme
        radius = 5.0 * self._scale

        board = QRectF(self.rect())
        path = QPainterPath()
        path.addRoundedRect(board, radius * 1.8, radius * 1.8)
        painter.fillPath(path, theme.surface)

        damaged = QRectF(event.rect())
        for key in self._visible_keys():
            rect = self.key_rect(key)
            if not rect.adjusted(-4, -4, 4, 4).intersects(damaged):
                continue
            self._paint_key(painter, key, rect, radius)

    def _paint_key(self, painter: QPainter, key: Key, rect: QRectF,
                   radius: float) -> None:
        theme = self._theme
        render = self._state.render_key(key)
        fade = self._state.fading.get(key.code, 0.0)

        # A key may override theme colours for itself, so a user can mark up
        # their own board without forking a whole theme (SPEC 4.1.4).
        face = theme.key_face_override(key.style, key.role)
        text_color = theme.key_text_override(
            key.style, dim=render.dim)
        border = theme.key_border_override(key.style)
        accent = theme.accent_override(key.style)

        if render.style is KeyStyle.PRESSED:
            face = accent
            text_color = theme.accent_text
            border = accent
        elif render.style is KeyStyle.LATCHED:
            face = theme.latched
            text_color = theme.accent_text
            border = theme.latched
        elif fade > 0:
            face = _blend(face, accent, fade)
            text_color = _blend(text_color, theme.accent_text, fade)
            border = _blend(border, accent, fade)
        elif render.style is KeyStyle.UNBOUND:
            face = _with_alpha(face, face.alpha() * 0.45)
            text_color = theme.key_sub_text

        painter.setPen(border)
        painter.setBrush(face)
        if len(key.segments) == 1:
            # The common case must not pay for the rare one: a plain rounded
            # rect, no path union, no cache lookup.
            painter.drawRoundedRect(rect, radius, radius)
        else:
            painter.drawPath(self.key_path(key))

        # Everything below is positioned on the largest segment, so an L-shaped
        # key labels itself in its wide part rather than in the notch.
        label_rect = self.label_rect(key)

        if render.led is not None:
            self._paint_led(painter, label_rect, render.led)

        if render.text:
            self._paint_label(painter, label_rect, render.text, text_color, key)

        if render.sub:
            painter.setPen(theme.key_sub_text)
            painter.setFont(self._font(7.0))
            painter.drawText(label_rect.adjusted(0, 3 * self._scale,
                                                 -5 * self._scale, 0),
                             Qt.AlignRight | Qt.AlignTop, render.sub)

    def _paint_led(self, painter: QPainter, rect: QRectF, on: bool) -> None:
        theme = self._theme
        d = 4.0 * self._scale
        dot = QRectF(rect.right() - d - 4 * self._scale,
                     rect.top() + 4 * self._scale, d, d)
        painter.setPen(Qt.NoPen)
        painter.setBrush(theme.led_on if on else theme.led_off)
        painter.drawEllipse(dot)

    def _paint_label(self, painter: QPainter, rect: QRectF, text: str,
                     color: QColor, key: Key) -> None:
        """Fit a legend into a key.

        Base legends are one or two characters and always fit. Cursor-layer and
        app-shortcut legends are words -- "Address bar", "Reopen closed tab" --
        and on a 1u key they need help. Shrinking alone bottoms out at an
        unreadable size, so multi-word labels wrap to two lines first and only
        then shrink.
        """
        painter.setPen(color)
        avail = rect.adjusted(2.5 * self._scale, 1.0 * self._scale,
                              -2.5 * self._scale, -1.0 * self._scale)

        size = 10.0 if len(text) <= 2 else 8.0
        wrappable = " " in text.strip()

        while size > 5.5:
            font = self._font(size)
            metrics = QFontMetricsF(font)
            if metrics.horizontalAdvance(text) <= avail.width():
                painter.setFont(font)
                painter.drawText(avail, Qt.AlignCenter, text)
                return
            if wrappable:
                bounds = metrics.boundingRect(
                    avail, Qt.AlignCenter | Qt.TextWordWrap, text)
                if (bounds.width() <= avail.width()
                        and bounds.height() <= avail.height()):
                    painter.setFont(font)
                    painter.drawText(avail, Qt.AlignCenter | Qt.TextWordWrap,
                                     text)
                    return
            size -= 0.5

        # Nothing fits cleanly; draw at the floor size and let Qt elide.
        font = self._font(5.5)
        painter.setFont(font)
        flags = Qt.AlignCenter | (Qt.TextWordWrap if wrappable else 0)
        painter.drawText(avail, flags, text)

    def _font(self, point_size: float) -> QFont:
        font = QFont(self.font())
        font.setPointSizeF(max(5.0, point_size * self._scale))
        font.setWeight(QFont.Medium)
        return font

    # -- interaction (only reachable when click-through is off) -----------

    def mousePressEvent(self, event) -> None:
        key = self.key_at(event.position())
        if key is not None:
            self.keyClicked.emit(key.code)
        else:
            super().mousePressEvent(event)


def _bridge(a: QRectF, b: QRectF, radius: float) -> QRectF | None:
    """A filler rectangle spanning the edge two segments share, or None.

    Uniting rounded rectangles rounds every corner, including the ones on the
    inside of an L where two segments meet. This rectangle covers that seam so
    the joint reads as one continuous key.

    Returns None for segments that do not share an edge -- including ones that
    merely touch at a corner, where there is no seam to fill.
    """
    eps = 0.5   # sub-pixel: adjacent segments rarely land exactly equal

    ox1, ox2 = max(a.left(), b.left()), min(a.right(), b.right())
    oy1, oy2 = max(a.top(), b.top()), min(a.bottom(), b.bottom())

    overlap_x = ox2 - ox1
    overlap_y = oy2 - oy1

    if overlap_x > eps and overlap_y > -eps:
        # Stacked vertically, sharing a horizontal edge.
        return QRectF(ox1, oy1 - radius, overlap_x, overlap_y + radius * 2)
    if overlap_y > eps and overlap_x > -eps:
        # Side by side, sharing a vertical edge.
        return QRectF(ox1 - radius, oy1, overlap_x + radius * 2, overlap_y)
    return None


def _blend(a: QColor, b: QColor, t: float) -> QColor:
    t = max(0.0, min(1.0, t))
    return QColor(
        round(a.red() + (b.red() - a.red()) * t),
        round(a.green() + (b.green() - a.green()) * t),
        round(a.blue() + (b.blue() - a.blue()) * t),
        round(a.alpha() + (b.alpha() - a.alpha()) * t),
    )


def _with_alpha(color: QColor, alpha: float) -> QColor:
    out = QColor(color)
    out.setAlpha(max(0, min(255, round(alpha))))
    return out

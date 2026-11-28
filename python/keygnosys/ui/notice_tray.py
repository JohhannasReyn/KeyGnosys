"""A small strip that slides out under the keyboard when an action is blocked.

It says what was blocked and why, unasked. A tooltip cannot do that job: it
needs a hover, and in click-through mode the overlay must be clicked first.
"""

from __future__ import annotations

from PySide6.QtCore import QRect, Qt, QTimer
from PySide6.QtGui import QColor, QPainter
from PySide6.QtWidgets import QHBoxLayout, QLabel, QWidget

#: Gap between the keyboard's bottom edge and the tray.
TRAY_GAP = 4

#: Background and text colour per notice level (operator, 2026-11-28): green
#: for something that worked, yellow for a warning, red for an error.
LEVEL_COLOURS: dict[str, tuple[QColor, str]] = {
    "success": (QColor(46, 125, 50, 235), "white"),
    "warning": (QColor(255, 213, 74, 240), "#1a1a1a"),
    "error": (QColor(200, 30, 30, 235), "white"),
}


class NoticeTray(QWidget):
    """Frameless, non-activating, top-most strip docked under the keyboard."""

    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle("KeyGnosys notice")
        self.setWindowFlags(
            Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.Tool
            | Qt.WindowTransparentForInput)
        self.setAttribute(Qt.WA_TranslucentBackground, True)
        self.setAttribute(Qt.WA_ShowWithoutActivating, True)

        self._label = QLabel(self)
        # Notices can quote a file's own text (a map name, a refusal reason).
        self._label.setTextFormat(Qt.PlainText)
        self._level = "error"
        self._apply_level()
        font = self._label.font()
        font.setPointSizeF(font.pointSizeF() * 1.15)
        self._label.setFont(font)
        layout = QHBoxLayout(self)
        layout.setContentsMargins(16, 8, 16, 8)
        layout.addWidget(self._label)

        self._timer = QTimer(self)
        self._timer.setSingleShot(True)
        self._timer.timeout.connect(self.hide)
        self._anchor = QRect()

    def text(self) -> str:
        return self._label.text()

    def level(self) -> str:
        return self._level

    def background(self) -> QColor:
        return LEVEL_COLOURS[self._level][0]

    def _apply_level(self) -> None:
        text_colour = LEVEL_COLOURS[self._level][1]
        self._label.setStyleSheet(f"color: {text_colour}; font-weight: 600;")

    def show_notice(self, text: str, anchor: QRect, duration_s: float,
                    level: str = "error") -> None:
        """Show `text` under `anchor` (the keyboard's frame geometry) for
        `duration_s`; a new notice replaces the old and restarts the timer.
        `level` is "success", "warning" or "error" and sets the colour."""
        self._level = level if level in LEVEL_COLOURS else "error"
        self._apply_level()
        self._label.setText(text)
        self.adjustSize()
        self.dock_under(anchor)
        self.show()
        self.raise_()
        self._timer.start(int(duration_s * 1000))

    def dock_under(self, anchor: QRect) -> None:
        self._anchor = QRect(anchor)
        x = anchor.x() + (anchor.width() - self.width()) // 2
        self.move(x, anchor.y() + anchor.height() + TRAY_GAP)

    def paintEvent(self, _event) -> None:
        painter = QPainter(self)
        painter.setRenderHint(QPainter.Antialiasing, True)
        painter.setPen(Qt.NoPen)
        painter.setBrush(self.background())
        radius = self.height() * 0.3
        painter.drawRoundedRect(self.rect(), radius, radius)

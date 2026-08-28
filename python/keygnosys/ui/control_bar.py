"""The control bar: the one surface that stays clickable.

While click-through is on, the keyboard window passes every mouse event to the
application beneath it -- which would leave the user no way to turn it back off.
The control bar is a separate always-interactive window docked to the keyboard's
top edge, and it is the handle by which the overlay is moved and configured.
"""

from __future__ import annotations

from PySide6.QtCore import QPoint, Qt, Signal
from PySide6.QtWidgets import (
    QComboBox, QFrame, QHBoxLayout, QLabel, QSlider, QToolButton, QWidget,
)

from .theme import ResolvedTheme


class ControlBar(QWidget):
    """Docked toolbar for the overlay."""

    pinToggled = Signal(bool)
    clickThroughToggled = Signal(bool)
    opacityChanged = Signal(float)
    layoutChanged = Signal(str)
    themeChanged = Signal(str)
    hideRequested = Signal()
    quitRequested = Signal()
    dragged = Signal(QPoint)

    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.setWindowFlags(
            Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.Tool
        )
        self.setAttribute(Qt.WA_TranslucentBackground, True)
        self._drag_origin: QPoint | None = None

        root = QHBoxLayout(self)
        root.setContentsMargins(8, 5, 8, 5)
        root.setSpacing(6)

        self._grip = QLabel("⠿")
        self._grip.setToolTip("Drag to move the overlay")
        self._grip.setCursor(Qt.SizeAllCursor)
        root.addWidget(self._grip)

        self._title = QLabel("KeyGnosys")
        self._title.setObjectName("title")
        root.addWidget(self._title)

        root.addWidget(_separator())

        self._layout_box = QComboBox()
        self._layout_box.setToolTip("Keyboard layout")
        self._layout_box.setMinimumWidth(150)
        self._layout_box.currentIndexChanged.connect(self._emit_layout)
        root.addWidget(self._layout_box)

        self._theme_box = QComboBox()
        self._theme_box.setToolTip("Theme")
        self._theme_box.setMinimumWidth(90)
        self._theme_box.currentIndexChanged.connect(self._emit_theme)
        root.addWidget(self._theme_box)

        root.addWidget(_separator())

        self._opacity = QSlider(Qt.Horizontal)
        self._opacity.setToolTip("Opacity")
        self._opacity.setRange(15, 100)
        self._opacity.setFixedWidth(80)
        self._opacity.valueChanged.connect(
            lambda v: self.opacityChanged.emit(v / 100.0))
        root.addWidget(QLabel("◐"))
        root.addWidget(self._opacity)

        root.addWidget(_separator())

        self._pin = _toggle("📌", "Keep the overlay above other windows")
        self._pin.toggled.connect(self.pinToggled)
        root.addWidget(self._pin)

        self._click = _toggle("👆", "Click through the keyboard to the window "
                                    "beneath it")
        self._click.toggled.connect(self.clickThroughToggled)
        root.addWidget(self._click)

        self._hide = _button("▁", "Hide the keyboard (the bar stays)")
        self._hide.clicked.connect(self.hideRequested)
        root.addWidget(self._hide)

        self._quit = _button("✕", "Quit KeyGnosys")
        self._quit.clicked.connect(self.quitRequested)
        root.addWidget(self._quit)

        root.addWidget(_separator())

        self._status = QLabel()
        self._status.setObjectName("status")
        root.addWidget(self._status)

    # -- population -------------------------------------------------------

    def populate(self, layouts: list[tuple[str, str]], current_layout: str,
                 themes: list[tuple[str, str]], current_theme: str) -> None:
        """Fill the pickers. Ids are stored as item data, names are shown."""
        for box, items, current in (
            (self._layout_box, layouts, current_layout),
            (self._theme_box, themes, current_theme),
        ):
            box.blockSignals(True)
            box.clear()
            for item_id, name in items:
                box.addItem(name, item_id)
            index = box.findData(current)
            box.setCurrentIndex(index if index >= 0 else 0)
            box.blockSignals(False)

    def set_toggles(self, pinned: bool, click_through: bool,
                    opacity: float) -> None:
        for widget, value in ((self._pin, pinned), (self._click, click_through)):
            widget.blockSignals(True)
            widget.setChecked(value)
            widget.blockSignals(False)
        self._opacity.blockSignals(True)
        self._opacity.setValue(int(round(opacity * 100)))
        self._opacity.blockSignals(False)

    def disable_click_through(self, reason: str) -> None:
        """Turn the control off and say why, rather than lying about it."""
        self._click.blockSignals(True)
        self._click.setChecked(False)
        self._click.setEnabled(False)
        self._click.setToolTip(reason)
        self._click.blockSignals(False)

    def set_status(self, text: str, tooltip: str = "") -> None:
        self._status.setText(text)
        self._status.setToolTip(tooltip)

    def apply_theme(self, theme: ResolvedTheme) -> None:
        bar = theme.bar_surface
        text = theme.bar_text
        border = theme.bar_border
        accent = theme.accent
        self.setStyleSheet(f"""
            ControlBar {{
                background: rgba({bar.red()},{bar.green()},{bar.blue()},{bar.alpha()});
                border: 1px solid {border.name()};
                border-radius: 7px;
            }}
            QLabel {{ color: {text.name()}; font-size: 11px; }}
            QLabel#title {{ font-weight: 600; }}
            QLabel#status {{ color: {theme.key_text_dim.name()}; font-size: 10px; }}
            QToolButton {{
                color: {text.name()};
                border: 1px solid transparent;
                border-radius: 4px;
                padding: 2px 5px;
                font-size: 12px;
            }}
            QToolButton:hover {{ border-color: {border.name()}; }}
            QToolButton:checked {{
                background: {accent.name()};
                color: {theme.accent_text.name()};
            }}
            QToolButton:disabled {{ color: {theme.key_sub_text.name()}; }}
            QComboBox {{
                color: {text.name()};
                background: {theme.key_face_alt.name()};
                border: 1px solid {border.name()};
                border-radius: 4px;
                padding: 2px 6px;
                font-size: 11px;
            }}
            QComboBox QAbstractItemView {{
                color: {text.name()};
                background: {theme.key_face.name()};
                selection-background-color: {accent.name()};
            }}
            QSlider::groove:horizontal {{
                height: 3px; background: {border.name()}; border-radius: 2px;
            }}
            QSlider::handle:horizontal {{
                width: 10px; margin: -4px 0;
                background: {accent.name()}; border-radius: 5px;
            }}
        """)

    # -- dragging ---------------------------------------------------------

    def mousePressEvent(self, event) -> None:
        if event.button() == Qt.LeftButton:
            self._drag_origin = event.globalPosition().toPoint() - self.pos()
            event.accept()
            return
        super().mousePressEvent(event)

    def mouseMoveEvent(self, event) -> None:
        if self._drag_origin is not None and event.buttons() & Qt.LeftButton:
            self.dragged.emit(event.globalPosition().toPoint() - self._drag_origin)
            event.accept()
            return
        super().mouseMoveEvent(event)

    def mouseReleaseEvent(self, event) -> None:
        self._drag_origin = None
        super().mouseReleaseEvent(event)

    # -- signal plumbing --------------------------------------------------

    def _emit_layout(self, index: int) -> None:
        data = self._layout_box.itemData(index)
        if data:
            self.layoutChanged.emit(str(data))

    def _emit_theme(self, index: int) -> None:
        data = self._theme_box.itemData(index)
        if data:
            self.themeChanged.emit(str(data))


def _toggle(text: str, tooltip: str) -> QToolButton:
    button = _button(text, tooltip)
    button.setCheckable(True)
    return button


def _button(text: str, tooltip: str) -> QToolButton:
    button = QToolButton()
    button.setText(text)
    button.setToolTip(tooltip)
    button.setFocusPolicy(Qt.NoFocus)
    return button


def _separator() -> QFrame:
    line = QFrame()
    line.setFrameShape(QFrame.VLine)
    line.setFixedWidth(1)
    return line

"""The define-mode side picker: choose a command for the selected key.

Docked beside the keyboard while define mode is open. It never edits the
binding set itself: every user decision leaves as a signal, and the overlay
(Task 6) calls the `BindingEditor`. It only reads `editor.bset`, fresh on every
refresh, because an undo replaces that object.
"""

from __future__ import annotations

import json
from typing import Callable

from PySide6.QtCore import QMimeData, QRect, Qt, Signal
from PySide6.QtGui import QColor
from PySide6.QtWidgets import (
    QComboBox, QDoubleSpinBox, QFormLayout, QLabel, QLineEdit, QListWidget,
    QListWidgetItem, QMessageBox, QPushButton, QSpinBox, QStyledItemDelegate,
    QToolButton, QVBoxLayout, QWidget,
)

from ..actions import default_legend
from ..binding_editor import UNASSIGNED, BindingEditor, PickerEntry
from ..documents import Binding
from .theme import ResolvedTheme

#: Drag payload type: the JSON of `Binding.to_dict()`.
MIME_TYPE = "application/x-keygnosys-binding"

#: Gap between the keyboard's right edge and the picker.
DOCK_GAP = 4

RESET_QUESTION = "Discard your edits to this map and return to the shipped one?"

#: Item role flagging an Unassigned entry, drawn with an orange left border.
_UNASSIGNED_ROLE = Qt.UserRole + 1
_UNASSIGNED_COLOUR = QColor(230, 140, 30)


def _ask(question: str) -> bool:
    answer = QMessageBox.question(None, "KeyGnosys", question)
    return answer == QMessageBox.Yes


class _EntryList(QListWidget):
    """The grouped entry list; each entry item drags as a Binding."""

    def __init__(self, picker: "BindingPicker") -> None:
        super().__init__()
        self._picker = picker
        self.setDragEnabled(True)
        self.setDragDropMode(QListWidget.DragOnly)

    def mimeTypes(self) -> list[str]:
        return [MIME_TYPE]

    def mimeData(self, items) -> QMimeData | None:
        for item in items:
            binding = self._picker._binding_for(item.data(Qt.UserRole),
                                                for_drag=True)
            if binding is not None:
                mime = QMimeData()
                mime.setData(MIME_TYPE,
                             json.dumps(binding.to_dict()).encode("utf-8"))
                return mime
        return None


class _EntryDelegate(QStyledItemDelegate):
    def paint(self, painter, option, index) -> None:
        super().paint(painter, option, index)
        if index.data(_UNASSIGNED_ROLE):
            painter.save()
            r = option.rect
            painter.fillRect(QRect(r.left(), r.top(), 3, r.height()),
                             _UNASSIGNED_COLOUR)
            painter.restore()


class BindingPicker(QWidget):
    """Frameless, top-most tool window that accepts input."""

    assignRequested = Signal(str, object)
    armed = Signal(object)
    clearRequested = Signal(str)
    legendEdited = Signal(str, str)
    settingEdited = Signal(str, object)
    resetRequested = Signal()

    def __init__(self, confirm: Callable[[str], bool] = _ask) -> None:
        super().__init__()
        self.setWindowTitle("KeyGnosys bindings")
        self.setWindowFlags(
            Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.Tool)
        self._confirm = confirm
        self._editor: BindingEditor | None = None
        self._key: str | None = None
        self._armed = False

        root = QVBoxLayout(self)
        root.setContentsMargins(10, 8, 10, 8)
        root.setSpacing(6)

        self.header = QLabel("Pick a key")
        self.header.setObjectName("header")
        root.addWidget(self.header)

        self.search = QLineEdit()
        self.search.setPlaceholderText("Search commands")
        self.search.setClearButtonEnabled(True)
        self.search.textChanged.connect(lambda _t: self._fill_list())
        root.addWidget(self.search)

        self.list = _EntryList(self)
        self.list.setItemDelegate(_EntryDelegate(self.list))
        self.list.currentItemChanged.connect(lambda *_: self._on_entry_changed())
        self.list.itemClicked.connect(self._on_item_clicked)
        root.addWidget(self.list, 1)

        self.choice = QComboBox()
        self.choice.setVisible(False)
        self.choice.activated.connect(lambda _i: self._choose())
        root.addWidget(self.choice)

        self.assign_button = QPushButton("Assign")
        self.assign_button.clicked.connect(self._choose)
        root.addWidget(self.assign_button)

        self.label_edit = QLineEdit()
        self.label_edit.setPlaceholderText("Key label")
        self.label_edit.editingFinished.connect(self._emit_legend)
        root.addWidget(self.label_edit)

        self.clear_button = QPushButton("Clear key")
        self.clear_button.clicked.connect(self._emit_clear)
        root.addWidget(self.clear_button)

        self.settings_toggle = QToolButton()
        self.settings_toggle.setText("Layer settings")
        self.settings_toggle.setCheckable(True)
        self.settings_toggle.setToolButtonStyle(Qt.ToolButtonTextBesideIcon)
        self.settings_toggle.setArrowType(Qt.RightArrow)
        self.settings_toggle.toggled.connect(self._toggle_settings)
        root.addWidget(self.settings_toggle)

        self.settings_body = QWidget()
        self._settings_form = QFormLayout(self.settings_body)
        self._settings_form.setContentsMargins(0, 0, 0, 0)
        self.settings_body.setVisible(False)
        root.addWidget(self.settings_body)
        self.setting_boxes: dict[str, QSpinBox | QDoubleSpinBox] = {}

        self.reset_button = QPushButton("Reset to shipped")
        self.reset_button.clicked.connect(self._emit_reset)
        self.reset_button.setVisible(False)
        root.addWidget(self.reset_button)

        self.refresh()

    # -- public API -------------------------------------------------------

    def set_editor(self, editor: BindingEditor | None) -> None:
        self._editor = editor
        self.refresh()

    def set_selected_key(self, code: str | None) -> None:
        # A label typed but not yet committed belongs to the key it was
        # typed for; commit it before the field switches keys.
        self._emit_legend()
        if code is not None and self._armed:
            self._armed = False
            self.armed.emit(None)
        self._key = code
        self.refresh()

    def set_can_reset(self, can: bool) -> None:
        self.reset_button.setVisible(can)

    def dock_beside(self, anchor: QRect) -> None:
        self.move(anchor.right() + DOCK_GAP, anchor.top())

    def refresh(self) -> None:
        self._update_header()
        self._fill_list()
        self._update_key_controls()
        self._update_settings()

    def is_marked_unassigned(self, item: QListWidgetItem) -> bool:
        return bool(item.data(_UNASSIGNED_ROLE))

    def apply_theme(self, theme: ResolvedTheme) -> None:
        bar = theme.bar_surface
        text = theme.bar_text
        border = theme.bar_border
        accent = theme.accent
        self.setStyleSheet(f"""
            BindingPicker {{
                background: rgba({bar.red()},{bar.green()},{bar.blue()},{bar.alpha()});
                border: 1px solid {border.name()};
            }}
            QLabel, QToolButton {{ color: {text.name()}; font-size: 11px; }}
            QLabel#header {{ font-weight: 600; }}
            QLineEdit, QComboBox, QListWidget, QSpinBox, QDoubleSpinBox {{
                color: {text.name()};
                background: {theme.key_face_alt.name()};
                border: 1px solid {border.name()};
                border-radius: 4px;
                font-size: 11px;
            }}
            QListWidget::item:selected {{
                background: {accent.name()};
                color: {theme.accent_text.name()};
            }}
            QComboBox QAbstractItemView {{
                color: {text.name()};
                background: {theme.key_face.name()};
                selection-background-color: {accent.name()};
            }}
            QPushButton {{
                color: {text.name()};
                background: {theme.key_face.name()};
                border: 1px solid {border.name()};
                border-radius: 4px;
                padding: 3px 8px;
                font-size: 11px;
            }}
            QPushButton:hover {{ border-color: {accent.name()}; }}
            QPushButton:disabled {{ color: {theme.key_sub_text.name()}; }}
            QToolButton {{ border: none; }}
        """)

    # -- reading the editor -----------------------------------------------

    def _current_binding(self) -> Binding | None:
        if self._editor is None or self._key is None:
            return None
        return self._editor.bset.bindings.get(self._key)

    def _current_entry(self) -> PickerEntry | None:
        item = self.list.currentItem()
        return item.data(Qt.UserRole) if item is not None else None

    def _binding_for(self, entry: PickerEntry | None,
                     for_drag: bool = False) -> Binding | None:
        """The Binding choosing (or dragging) `entry` would request."""
        if entry is None:
            return None
        if entry.stored is not None:
            return entry.stored
        if entry.action == "key.passthrough":
            # Armed or dragged, the target key isn't known here: the receiver
            # fills in {"code": <target key>}.
            if self._key and not for_drag:
                return Binding(entry.action, {"code": self._key})
            return Binding(entry.action, {})
        if not entry.choices:
            return Binding(entry.action, {})
        # The choice box speaks for the current entry; a dragged entry that
        # isn't current takes its first choice.
        index = (self.choice.currentIndex()
                 if entry is self._current_entry() else 0)
        index = max(0, min(index, len(entry.choices) - 1))
        return Binding(entry.action, dict(entry.choices[index]))

    # -- view updates -----------------------------------------------------

    def _update_header(self) -> None:
        if self._key is None:
            self.header.setText("Pick a key")
            return
        current = self._current_binding()
        if current is None:
            now = "unbound"
        else:
            now = current.legend or default_legend(current.action, current.params)
        self.header.setText(f"Assign to {self._key} (now: {now})")

    def _fill_list(self) -> None:
        previous = self._current_entry()
        keep = ((previous.group, previous.action, previous.label)
                if previous is not None else None)
        self.list.blockSignals(True)
        self.list.clear()
        restored: QListWidgetItem | None = None
        if self._editor is not None:
            group = None
            for entry in self._editor.entries(self.search.text()):
                if entry.group != group:
                    group = entry.group
                    heading = QListWidgetItem(group)
                    heading.setFlags(Qt.NoItemFlags)
                    font = heading.font()
                    font.setBold(True)
                    heading.setFont(font)
                    self.list.addItem(heading)
                item = QListWidgetItem(entry.label)
                item.setData(Qt.UserRole, entry)
                item.setToolTip(entry.action)
                item.setFlags(Qt.ItemIsEnabled | Qt.ItemIsSelectable
                              | Qt.ItemIsDragEnabled)
                if entry.group == UNASSIGNED:
                    item.setData(_UNASSIGNED_ROLE, True)
                self.list.addItem(item)
                if keep == (entry.group, entry.action, entry.label):
                    restored = item
        if restored is not None:
            self.list.setCurrentItem(restored)
        self.list.blockSignals(False)
        self._on_entry_changed()

    def _on_entry_changed(self) -> None:
        entry = self._current_entry()
        previous_params = self.choice.currentData()
        self.choice.clear()
        if entry is not None and entry.stored is None and len(entry.choices) > 1:
            for i, params in enumerate(entry.choices):
                text = default_legend(entry.action, params)
                keys = entry.bound_on.get(i)
                if keys:
                    text += f" (on {', '.join(keys)})"
                self.choice.addItem(text, params)
            index = self.choice.findData(previous_params)
            self.choice.setCurrentIndex(index if index >= 0 else 0)
            self.choice.setVisible(True)
        else:
            self.choice.setVisible(False)
        self._update_assign_button()

    def _update_assign_button(self) -> None:
        entry = self._current_entry()
        self.assign_button.setText("Assign" if self._key else "Arm")
        self.assign_button.setEnabled(self._binding_for(entry) is not None)

    def _update_key_controls(self) -> None:
        current = self._current_binding()
        self.label_edit.blockSignals(True)
        self.label_edit.setText((current.legend or "") if current else "")
        self.label_edit.setPlaceholderText(
            default_legend(current.action, current.params) if current
            else "Key label")
        self.label_edit.blockSignals(False)
        self.label_edit.setEnabled(current is not None)
        self.clear_button.setEnabled(current is not None)
        self._update_assign_button()

    def _update_settings(self) -> None:
        settings = self._editor.bset.settings if self._editor else {}
        if set(settings) != set(self.setting_boxes):
            while self._settings_form.rowCount():
                self._settings_form.removeRow(0)
            self.setting_boxes = {}
            for name, value in settings.items():
                box = _spin_box_for(value)
                box.valueChanged.connect(
                    lambda v, n=name: self.settingEdited.emit(n, v))
                self._settings_form.addRow(name.replace("_", " "), box)
                self.setting_boxes[name] = box
        for name, box in self.setting_boxes.items():
            box.blockSignals(True)
            box.setValue(settings[name])
            box.blockSignals(False)

    def _toggle_settings(self, open_: bool) -> None:
        self.settings_toggle.setArrowType(Qt.DownArrow if open_ else Qt.RightArrow)
        self.settings_body.setVisible(open_)

    # -- signal plumbing --------------------------------------------------

    def _on_item_clicked(self, item: QListWidgetItem) -> None:
        entry = item.data(Qt.UserRole)
        if entry is None:
            return
        # One click chooses, unless the entry needs a choice first: then the
        # combo is showing and picking from it chooses.
        if entry.stored is not None or len(entry.choices) <= 1:
            self._choose()

    def _choose(self) -> None:
        binding = self._binding_for(self._current_entry())
        if binding is None:
            return
        if self._key is None:
            self._armed = True
            self.armed.emit(binding)
        else:
            self.assignRequested.emit(self._key, binding)

    def _emit_legend(self) -> None:
        current = self._current_binding()
        if current is None:
            return
        text = self.label_edit.text()
        if text != (current.legend or ""):
            self.legendEdited.emit(self._key, text)

    def _emit_clear(self) -> None:
        if self._current_binding() is not None:
            self.clearRequested.emit(self._key)

    def _emit_reset(self) -> None:
        if self._confirm(RESET_QUESTION):
            self.resetRequested.emit()


def _spin_box_for(value: float | int) -> QSpinBox | QDoubleSpinBox:
    if isinstance(value, float):
        box = QDoubleSpinBox()
        box.setDecimals(4)
        box.setRange(-1e6, 1e6)
        box.setSingleStep(0.05)
    else:
        box = QSpinBox()
        box.setRange(-1_000_000, 1_000_000)
    box.setKeyboardTracking(False)
    return box

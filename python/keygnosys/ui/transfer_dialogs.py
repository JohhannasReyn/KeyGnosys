"""Preview dialogs for importing and exporting a binding map."""

from __future__ import annotations

from datetime import datetime

from PySide6.QtCore import Qt
from PySide6.QtGui import QGuiApplication
from PySide6.QtWidgets import (QDialog, QDialogButtonBox, QLabel, QLineEdit,
                               QPushButton, QScrollArea, QVBoxLayout,
                               QWidget)

from keygnosys.bundle import Change, ImportPlan

_MONTHS = ("Jan", "Feb", "Mar", "Apr", "May", "Jun",
           "Jul", "Aug", "Sep", "Oct", "Nov", "Dec")

INTRO = ("This replaces your current map. Your current map is kept as a "
         "backup, and Undo brings it straight back.")


def max_dialog_height() -> int:
    """About 60% of the available screen height, with a fixed fallback."""
    screen = QGuiApplication.primaryScreen()
    if screen is None:
        return 480
    return int(screen.availableGeometry().height() * 0.6)


def _plain(text: str) -> QLabel:
    """A word-wrapped label that never interprets its text as markup."""
    label = QLabel(text)
    label.setTextFormat(Qt.PlainText)
    label.setWordWrap(True)
    return label


def _bold(text: str) -> QLabel:
    """A section heading: plain text, in bold."""
    label = _plain(text)
    font = label.font()
    font.setBold(True)
    label.setFont(font)
    return label


def _format_date(iso: str | None) -> str:
    if not iso:
        return ""
    try:
        d = datetime.fromisoformat(iso.replace("Z", "+00:00"))
    except ValueError:
        return ""
    return f"{d.day} {_MONTHS[d.month - 1]} {d.year}"


def _change_line(c: Change) -> str:
    return f"{c.name}  {c.before}  →  {c.after}"


class ImportPreview(QDialog):
    def __init__(self, plan: ImportPlan, changes: list[Change], parent=None):
        super().__init__(parent)
        self.setWindowTitle("Import bindings")
        self._header = self._header_text(plan)
        self._intro = INTRO
        # (title, lines) pairs; later projects can append further sections.
        self._sections: list[tuple[str, list[str]]] = []

        key_changes = [c for c in changes if c.kind == "key"]
        if changes:
            n = len(key_changes)
            title = (f"Changes ({n} {'key' if n == 1 else 'keys'})"
                     if n else "Changes")
            self._sections.append(
                (title,
                 [_change_line(c) for c in key_changes]
                 + [_change_line(c) for c in changes if c.kind != "key"]))
        for kind, title in (("unusable", "Kept but not usable in this version"),
                            ("off_layout", "Not on your keyboard"),
                            ("setting", "Settings adjusted")):
            texts = [f.text for f in plan.findings if f.kind == kind]
            if texts:
                self._sections.append((f"{title} ({len(texts)})", texts))

        layout = QVBoxLayout(self)
        self.header_label = _plain(self._header)
        layout.addWidget(self.header_label)
        layout.addWidget(_plain(self._intro))
        self._labels: list[QLabel] = []
        self._max_height = max_dialog_height()
        self.setMaximumHeight(self._max_height)
        # Sections scroll; the buttons stay outside so they are always visible.
        self.scroll = QScrollArea()
        self.scroll.setWidgetResizable(True)
        self.scroll.setMaximumHeight(max(120, self._max_height - 280))
        inner = QWidget()
        inner_layout = QVBoxLayout(inner)
        self.scroll.setWidget(inner)
        layout.addWidget(self.scroll)
        if not changes:
            note = _plain("Nothing changes: this map is the same as your "
                          "current one.")
            inner_layout.addWidget(note)
            self._labels.append(note)
        for title, lines in self._sections:
            head = _bold(title)
            inner_layout.addWidget(head)
            body = _plain("\n".join(lines))
            body.setTextInteractionFlags(Qt.TextSelectableByMouse)
            inner_layout.addWidget(body)
            self._labels.extend([head, body])

        buttons = QDialogButtonBox()
        self.import_button: QPushButton | None = None
        if changes:
            self.import_button = buttons.addButton(
                "Import", QDialogButtonBox.AcceptRole)
            self.import_button.setDefault(True)
            self.import_button.clicked.connect(self.accept)
            self.close_button = buttons.addButton(
                "Cancel", QDialogButtonBox.RejectRole)
        else:
            self.close_button = buttons.addButton(
                "Close", QDialogButtonBox.RejectRole)
            self.close_button.setDefault(True)
        self.close_button.clicked.connect(self.reject)
        layout.addWidget(buttons)

    @staticmethod
    def _header_text(plan: ImportPlan) -> str:
        parts = [plan.name]
        if plan.author:
            parts.append(f"by {plan.author}")
        date = _format_date(plan.exported)
        if date:
            parts.append(date)
        return "  ·  ".join(parts)

    def max_height(self) -> int:
        return self._max_height

    def header_text(self) -> str:
        return self._header

    def intro_text(self) -> str:
        return self._intro

    def sections(self) -> dict[str, list[str]]:
        return {title: list(lines) for title, lines in self._sections}

    def all_text(self) -> str:
        return "\n".join(l.text() for l in self._labels)


class ExportPreview(QDialog):
    def __init__(self, name: str, key_count: int, warnings: list[str],
                 parent=None):
        super().__init__(parent)
        self.setWindowTitle("Export bindings")
        layout = QVBoxLayout(self)
        self._labels: list[QLabel] = []
        summary = _plain(f"Exporting '{name}': {key_count} keys.")
        layout.addWidget(summary)
        self._labels.append(summary)
        self.warnings_heading: QLabel | None = None
        if warnings:
            self.warnings_heading = _bold("Warnings")
            layout.addWidget(self.warnings_heading)
            self._labels.append(self.warnings_heading)
        for w in warnings:
            lab = _plain(w)
            layout.addWidget(lab)
            self._labels.append(lab)
        self.author_label = QLabel("Your name (for credit)")
        layout.addWidget(self.author_label)
        self.author_field = QLineEdit()
        layout.addWidget(self.author_field)
        buttons = QDialogButtonBox()
        self.save_button = buttons.addButton(
            "Save…", QDialogButtonBox.AcceptRole)
        self.save_button.setDefault(True)
        self.save_button.clicked.connect(self.accept)
        self.close_button = buttons.addButton(
            "Cancel", QDialogButtonBox.RejectRole)
        self.close_button.clicked.connect(self.reject)
        layout.addWidget(buttons)

    def author(self) -> str:
        return self.author_field.text().strip()

    def all_text(self) -> str:
        return "\n".join(l.text() for l in self._labels)


def _run(dlg: QDialog) -> bool:
    dlg.setWindowFlag(Qt.WindowStaysOnTopHint, True)
    dlg.raise_()
    dlg.activateWindow()
    return dlg.exec() == QDialog.Accepted


def ask_import(plan: ImportPlan, changes: list[Change],
               parent: QWidget | None = None) -> bool:
    return _run(ImportPreview(plan, changes, parent))


def ask_export(name: str, key_count: int, warnings: list[str],
               parent: QWidget | None = None) -> str | None:
    dlg = ExportPreview(name, key_count, warnings, parent)
    return dlg.author() if _run(dlg) else None

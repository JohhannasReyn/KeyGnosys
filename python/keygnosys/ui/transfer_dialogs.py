"""Preview dialogs for importing and exporting a binding map."""

from __future__ import annotations

from datetime import datetime

from PySide6.QtCore import Qt
from PySide6.QtWidgets import (QDialog, QDialogButtonBox, QLabel, QLineEdit,
                               QPushButton, QVBoxLayout, QWidget)

from keygnosys.bundle import Change, ImportPlan

_MONTHS = ("Jan", "Feb", "Mar", "Apr", "May", "Jun",
           "Jul", "Aug", "Sep", "Oct", "Nov", "Dec")

INTRO = ("This replaces your current map. Your current map is kept as a "
         "backup, and Undo brings it straight back.")


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
            self._sections.append(
                (f"Changes ({len(key_changes)} keys)",
                 [_change_line(c) for c in key_changes]
                 + [_change_line(c) for c in changes if c.kind != "key"]))
        for kind, title in (("unusable", "Kept but not usable in this version"),
                            ("off_layout", "Not on your keyboard"),
                            ("setting", "Settings adjusted")):
            texts = [f.text for f in plan.findings if f.kind == kind]
            if texts:
                self._sections.append((f"{title} ({len(texts)})", texts))

        layout = QVBoxLayout(self)
        layout.addWidget(QLabel(self._header))
        intro = QLabel(self._intro)
        intro.setWordWrap(True)
        layout.addWidget(intro)
        self._labels: list[QLabel] = []
        if not changes:
            note = QLabel("Nothing changes: this map is the same as your "
                          "current one.")
            note.setWordWrap(True)
            layout.addWidget(note)
            self._labels.append(note)
        for title, lines in self._sections:
            head = QLabel(title)
            font = head.font()
            font.setBold(True)
            head.setFont(font)
            layout.addWidget(head)
            body = QLabel("\n".join(lines))
            body.setTextFormat(Qt.PlainText)
            body.setWordWrap(True)
            body.setTextInteractionFlags(Qt.TextSelectableByMouse)
            layout.addWidget(body)
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
        summary = QLabel(f"Exporting '{name}': {key_count} keys.")
        summary.setWordWrap(True)
        layout.addWidget(summary)
        self._labels.append(summary)
        for w in warnings:
            lab = QLabel(w)
            lab.setWordWrap(True)
            layout.addWidget(lab)
            self._labels.append(lab)
        layout.addWidget(QLabel("Author (optional)"))
        self.author_field = QLineEdit()
        layout.addWidget(self.author_field)
        buttons = QDialogButtonBox()
        self.save_button = buttons.addButton(
            "Save...", QDialogButtonBox.AcceptRole)
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

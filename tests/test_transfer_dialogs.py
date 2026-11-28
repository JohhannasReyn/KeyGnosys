"""The import and export preview dialogs (never exec()'d here)."""

from __future__ import annotations

from PySide6.QtCore import Qt
from PySide6.QtWidgets import QDialog

from keygnosys.bundle import Change, Finding, ImportPlan
from keygnosys.documents import BindingSet
from keygnosys.ui.transfer_dialogs import ExportPreview, ImportPreview

INTRO = ("This replaces your current map. Your current map is kept as a "
         "backup, and Undo brings it straight back.")


def make_plan(findings=(), author="Sam", exported="2026-11-27T14:03:00Z"):
    return ImportPlan(bset=BindingSet.from_dict(
        {"schema": "keygnosys/bindings/2", "id": "default", "name": "Mine",
         "bindings": {}}),
        name="Mine", author=author, exported=exported,
        findings=list(findings))


def two_changes():
    return [Change("key", "A", "Copy", "Paste"),
            Change("key", "B", "(nothing)", "Undo")]


def test_import_preview_sections(qtbot):
    plan = make_plan([Finding("unusable", "F1 needs a newer build"),
                      Finding("off_layout", "Key § is not on your keyboard")])
    dlg = ImportPreview(plan, two_changes())
    qtbot.addWidget(dlg)
    sections = dlg.sections()
    assert list(sections) == ["Changes (2 keys)",
                              "Kept but not usable in this version (1)",
                              "Not on your keyboard (1)"]
    assert sections["Changes (2 keys)"] == ["A  Copy  →  Paste",
                                            "B  (nothing)  →  Undo"]
    assert sections["Not on your keyboard (1)"] == [
        "Key § is not on your keyboard"]
    assert "Mine" in dlg.header_text()
    assert "by Sam" in dlg.header_text()
    assert "27 Nov 2026" in dlg.header_text()
    assert dlg.intro_text() == INTRO


def test_import_preview_header_without_author_or_date(qtbot):
    dlg = ImportPreview(make_plan(author="", exported=None), two_changes())
    qtbot.addWidget(dlg)
    assert "by" not in dlg.header_text()
    dlg2 = ImportPreview(make_plan(exported="garbage"), two_changes())
    qtbot.addWidget(dlg2)
    assert "garbage" not in dlg2.header_text()
    assert "by Sam" in dlg2.header_text()


def test_import_preview_setting_findings_section(qtbot):
    plan = make_plan([Finding("setting", "Opacity limited to 90")])
    dlg = ImportPreview(plan, two_changes() + [
        Change("setting", "Opacity", "50", "90")])
    qtbot.addWidget(dlg)
    sections = dlg.sections()
    assert sections["Settings adjusted (1)"] == ["Opacity limited to 90"]
    assert sections["Changes (2 keys)"][-1] == "Opacity  50  →  90"


def test_import_preview_no_changes_offers_close_only(qtbot):
    dlg = ImportPreview(make_plan(), [])
    qtbot.addWidget(dlg)
    assert dlg.import_button is None
    assert dlg.close_button is not None
    assert "Nothing changes" in dlg.all_text()


def test_import_is_default_and_escape_cancels(qtbot):
    dlg = ImportPreview(make_plan(), two_changes())
    qtbot.addWidget(dlg)
    dlg.setModal(False)
    dlg.show()
    assert dlg.import_button.isDefault()
    qtbot.keyClick(dlg, Qt.Key_Escape)
    assert dlg.result() == QDialog.Rejected


def test_export_preview_author_empty_by_default_and_stripped(qtbot):
    dlg = ExportPreview("Mine", 12, [])
    qtbot.addWidget(dlg)
    assert dlg.author() == ""
    dlg.author_field.setText("  Sam ")
    assert dlg.author() == "Sam"
    assert dlg.save_button.isDefault()


def test_export_preview_lists_warnings(qtbot):
    dlg = ExportPreview("Mine", 12, ["Key X has no label"])
    qtbot.addWidget(dlg)
    assert "Key X has no label" in dlg.all_text()
    assert "12" in dlg.all_text()


def test_file_derived_text_is_plain(qtbot):
    plan = make_plan(author="<i>me</i>")
    plan.name = "<b>x</b>"
    dlg = ImportPreview(plan, two_changes())
    qtbot.addWidget(dlg)
    assert dlg.header_label.textFormat() == Qt.PlainText
    assert "<b>x</b>" in dlg.header_label.text()
    assert dlg.header_label.wordWrap()
    exp = ExportPreview("<b>n</b>", 1, ["<u>w</u>"])
    qtbot.addWidget(exp)
    assert "<b>n</b>" in exp.all_text() and "<u>w</u>" in exp.all_text()
    assert all(l.textFormat() == Qt.PlainText for l in exp._labels)


def test_many_changes_scroll_and_buttons_stay_outside(qtbot):
    changes = [Change("key", f"K{i}", "a", "b") for i in range(300)]
    plan = make_plan([Finding("unusable", "u"), Finding("setting", "s")])
    dlg = ImportPreview(plan, changes)
    qtbot.addWidget(dlg)
    inner = dlg.scroll.widget()
    assert dlg._labels
    for label in dlg._labels:
        assert inner.isAncestorOf(label)
    assert not inner.isAncestorOf(dlg.import_button)
    assert not inner.isAncestorOf(dlg.close_button)
    assert not inner.isAncestorOf(dlg.header_label)
    assert dlg.maximumHeight() == dlg.max_height()
    assert dlg.scroll.maximumHeight() < dlg.max_height()
    # The content is far taller than the viewport, so it scrolls.
    assert inner.sizeHint().height() > dlg.scroll.maximumHeight()
    assert dlg.sizeHint().height() <= dlg.max_height()


def test_changes_title_is_pluralised(qtbot):
    one = ImportPreview(make_plan(), two_changes()[:1])
    qtbot.addWidget(one)
    assert list(one.sections())[0] == "Changes (1 key)"
    settings_only = ImportPreview(make_plan(), [
        Change("setting", "Opacity", "50", "90")])
    qtbot.addWidget(settings_only)
    assert list(settings_only.sections())[0] == "Changes"


def test_export_preview_matches_the_spec(qtbot):
    dlg = ExportPreview("Mine", 12, ["Key X has no label"])
    qtbot.addWidget(dlg)
    assert dlg.author_label.text() == "Your name (for credit)"
    assert dlg.save_button.text() == "Save\u2026"
    assert dlg.warnings_heading is not None
    assert dlg.warnings_heading.text() == "Warnings"
    assert dlg.warnings_heading.font().bold()
    quiet = ExportPreview("Mine", 12, [])
    qtbot.addWidget(quiet)
    assert quiet.warnings_heading is None
    assert "Warnings" not in quiet.all_text()

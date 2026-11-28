"""The keyboard view's drag and drop: picker commands and dropped files."""

from __future__ import annotations

import json
from pathlib import Path

import pytest
from PySide6.QtCore import QMimeData, QPoint, QPointF, Qt, QUrl
from PySide6.QtGui import (
    QDragEnterEvent, QDragLeaveEvent, QDragMoveEvent, QDropEvent,
)

from keygnosys import paths
from keygnosys.documents import Binding, Registry
from keygnosys.state import AppState
from keygnosys.ui.binding_picker import MIME_TYPE
from keygnosys.ui.keyboard_view import KeyboardView, dropped_file


def file_mime(*files: Path) -> QMimeData:
    mime = QMimeData()
    mime.setUrls([QUrl.fromLocalFile(str(f)) for f in files])
    return mime


@pytest.fixture
def view(qtbot, tmp_path, monkeypatch):
    monkeypatch.setattr(paths, "user_root", lambda: tmp_path / "user")
    registry = Registry().load_all()
    v = KeyboardView(AppState())
    v.set_layout_doc(next(iter(registry.layouts.values())))
    qtbot.addWidget(v)
    return v


# -- dropped_file -----------------------------------------------------------

def test_one_local_json_file_is_accepted(tmp_path):
    path = tmp_path / "a.json"
    assert dropped_file(file_mime(path)) == path


def test_keygnosys_extension_is_accepted(tmp_path):
    path = tmp_path / "a.keygnosys.json"
    assert dropped_file(file_mime(path)) == path


def test_extension_is_case_insensitive(tmp_path):
    path = tmp_path / "A.JSON"
    assert dropped_file(file_mime(path)) == path


def test_two_files_refused(tmp_path):
    assert dropped_file(file_mime(tmp_path / "a.json",
                                  tmp_path / "b.json")) is None


def test_other_extension_refused(tmp_path):
    assert dropped_file(file_mime(tmp_path / "a.txt")) is None


def test_remote_url_refused():
    mime = QMimeData()
    mime.setUrls([QUrl("https://example.com/a.json")])
    assert dropped_file(mime) is None


def test_directory_refused(tmp_path):
    folder = tmp_path / "folder.json"
    folder.mkdir()
    assert dropped_file(file_mime(folder)) is None


def test_no_urls_refused():
    mime = QMimeData()
    mime.setText("C:/x/a.json")
    assert dropped_file(mime) is None


def test_mixed_mime_with_one_local_json_is_accepted(tmp_path):
    path = tmp_path / "a.json"
    mime = file_mime(path)
    mime.setText("dragged from a browser")
    assert dropped_file(mime) == path


# -- drag events ------------------------------------------------------------

def drag_enter(view, mime) -> QDragEnterEvent:
    event = QDragEnterEvent(QPoint(5, 5), Qt.CopyAction, mime,
                            Qt.LeftButton, Qt.NoModifier)
    event.setAccepted(False)
    view.dragEnterEvent(event)
    return event


def drag_move(view, mime, pos=QPoint(5, 5)) -> QDragMoveEvent:
    event = QDragMoveEvent(pos, Qt.CopyAction, mime,
                           Qt.LeftButton, Qt.NoModifier)
    event.setAccepted(False)
    view.dragMoveEvent(event)
    return event


def drop(view, mime, pos=QPointF(5, 5)) -> QDropEvent:
    event = QDropEvent(pos, Qt.CopyAction, mime, Qt.LeftButton, Qt.NoModifier)
    event.setAccepted(False)
    view.dropEvent(event)
    return event


def test_file_drag_accepted_outside_and_inside_define_mode(view, tmp_path):
    mime = file_mime(tmp_path / "a.keygnosys.json")
    for define in (False, True):
        view.set_define_mode(define)
        assert view.acceptDrops()
        assert drag_enter(view, mime).isAccepted()
        assert view.file_drag_active
        assert drag_move(view, mime).isAccepted()
        view.dragLeaveEvent(QDragLeaveEvent())
        assert not view.file_drag_active


def test_refused_file_drag_shows_no_highlight(view, tmp_path):
    for define in (False, True):
        view.set_define_mode(define)
        mime = file_mime(tmp_path / "a.txt")
        assert not drag_enter(view, mime).isAccepted()
        assert not drag_move(view, mime).isAccepted()
        assert not view.file_drag_active


def test_file_drop_emits_file_dropped_and_clears_highlight(view, tmp_path,
                                                           qtbot):
    path = tmp_path / "a.keygnosys.json"
    mime = file_mime(path)
    drag_enter(view, mime)
    with qtbot.assertNotEmitted(view.bindingDropped), \
            qtbot.waitSignal(view.fileDropped) as blocker:
        assert drop(view, mime).isAccepted()
        assert not view.file_drag_active
    assert blocker.args == [path]


def test_refused_drop_emits_nothing(view, tmp_path, qtbot):
    with qtbot.assertNotEmitted(view.fileDropped):
        assert not drop(view, file_mime(tmp_path / "a.txt")).isAccepted()
        qtbot.wait(20)


def test_picker_drag_still_assigns(view, qtbot):
    view.set_define_mode(True)
    key = next(k for k in view._visible_keys() if k.code == "KeyD")
    centre = view.key_rect(key).center()
    mime = QMimeData()
    mime.setData(MIME_TYPE, json.dumps(
        {"action": "button.click", "params": {"button": "right"}}
    ).encode("utf-8"))

    assert drag_enter(view, mime).isAccepted()
    assert not view.file_drag_active
    assert drag_move(view, mime, centre.toPoint()).isAccepted()
    with qtbot.assertNotEmitted(view.fileDropped):
        with qtbot.waitSignal(view.bindingDropped) as blocker:
            assert view.drop_at(centre, mime)
        assert blocker.args == ["KeyD", Binding("button.click",
                                                {"button": "right"})]
        with qtbot.waitSignal(view.bindingDropped):
            assert drop(view, mime, centre).isAccepted()
        qtbot.wait(20)

    # Outside define mode the picker's data is refused.
    view.set_define_mode(False)
    assert not drag_enter(view, mime).isAccepted()

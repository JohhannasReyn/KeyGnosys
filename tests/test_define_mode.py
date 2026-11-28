"""Define mode on the overlay: select a key, choose a command, save, reload."""

from __future__ import annotations

import json

import pytest
from PySide6.QtCore import QMimeData, QPointF, Qt

from keygnosys import binding_store, paths
from keygnosys.coreclient.base import CoreClient
from keygnosys.documents import Binding, Registry
from keygnosys.settings import Settings
from keygnosys.ui.binding_picker import MIME_TYPE
from keygnosys.ui.overlay import Overlay
from keygnosys.ui.platform import clickthrough

LEFT = {"button": "left"}
RIGHT = {"button": "right"}


class FakeClient(CoreClient):
    """Records commands; tests emit its signals by hand."""

    name = "fake"

    def __init__(self, bindings_id: str | None = "default") -> None:
        super().__init__()
        self.reloads = 0
        self._id = bindings_id

    def start(self) -> None:
        pass

    def stop(self) -> None:
        pass

    def reload_config(self) -> None:
        self.reloads += 1

    @property
    def bindings_id(self) -> str | None:
        return self._id

    def connect_core(self) -> None:
        self.connected.emit({"backends": {"input": "fake"}})


@pytest.fixture
def user_root(tmp_path, monkeypatch):
    root = tmp_path / "user"
    monkeypatch.setattr(paths, "user_root", lambda: root)
    return root


@pytest.fixture
def click_calls(monkeypatch):
    calls: list[bool] = []

    def record(_window, enabled: bool) -> bool:
        calls.append(enabled)
        return True
    monkeypatch.setattr(clickthrough, "set_click_through", record)
    monkeypatch.setattr(clickthrough, "set_no_activate", lambda _w: True)
    return calls


@pytest.fixture
def make_overlay(qtbot, user_root, click_calls):
    made: list[Overlay] = []

    def make(client: CoreClient | None = None) -> Overlay:
        client = client or FakeClient()
        overlay = Overlay(Registry().load_all(), Settings(), client)
        overlay.define.confirm = lambda _q: True
        made.append(overlay)
        return overlay
    yield make
    for overlay in made:
        for widget in (overlay.window, overlay.bar, overlay.tray,
                       overlay.define.picker):
            widget.hide()
            widget.deleteLater()


def user_file(user_root):
    return user_root / "bindings" / "default.json"


def enter(overlay: Overlay) -> None:
    overlay.bar.define_button.click()
    assert overlay.define.active


def select(overlay: Overlay, code: str) -> None:
    overlay.window.view.keyClicked.emit(code)


def assign_from_picker(overlay: Overlay, action: str, params: dict) -> None:
    picker = overlay.define.picker
    for row in range(picker.list.count()):
        item = picker.list.item(row)
        entry = item.data(Qt.UserRole)
        if entry is not None and entry.action == action and entry.stored is None:
            picker.list.setCurrentItem(item)
            break
    else:
        raise AssertionError(f"{action} not listed")
    for i in range(picker.choice.count()):
        if picker.choice.itemData(i) == params:
            picker.choice.setCurrentIndex(i)
    picker.assign_button.click()


def saved_binding(user_root, code: str) -> dict:
    doc = json.loads(user_file(user_root).read_text("utf-8"))
    return doc["bindings"][code]


# -- entering and leaving ---------------------------------------------------

def test_entering_define_mode_turns_click_through_off_and_exiting_restores_it(
        make_overlay, click_calls, user_root):
    overlay = make_overlay()
    assert overlay.settings.get("window.click_through") is True
    enter(overlay)
    assert click_calls == [False]
    assert overlay.define.picker.isVisible()
    assert overlay.bar.define_indicator.isVisibleTo(overlay.bar)
    assert overlay.bar.define_button.text() == "Done"

    overlay.bar.define_button.click()
    assert not overlay.define.active
    assert click_calls == [False, True]
    assert not overlay.define.picker.isVisible()
    assert not overlay.bar.define_indicator.isVisibleTo(overlay.bar)
    # The stored preference was never touched.
    assert overlay.settings.get("window.click_through") is True
    assert not paths.settings_file().exists()


def test_exiting_restores_click_through_off_when_it_was_off(make_overlay,
                                                           click_calls):
    overlay = make_overlay()
    overlay.settings.set("window.click_through", False)
    enter(overlay)
    overlay.bar.define_button.click()
    assert click_calls == [False, False]


def test_undo_and_redo_are_hidden_outside_define_mode(make_overlay):
    overlay = make_overlay()
    bar = overlay.bar
    assert not bar.undo_button.isVisibleTo(bar)
    assert not bar.redo_button.isVisibleTo(bar)
    enter(overlay)
    assert bar.undo_button.isVisibleTo(bar) and not bar.undo_button.isEnabled()
    assert bar.redo_button.isVisibleTo(bar) and not bar.redo_button.isEnabled()


# -- selecting and assigning ------------------------------------------------

def test_clicking_a_key_then_choosing_assigns_saves_and_redraws(make_overlay,
                                                                user_root):
    overlay = make_overlay()
    enter(overlay)
    select(overlay, "KeyD")
    assert overlay.window.view.selected == "KeyD"
    assign_from_picker(overlay, "button.click", RIGHT)

    assert saved_binding(user_root, "KeyD") == {"action": "button.click",
                                                "params": RIGHT}
    assert overlay.state.binding_set.bindings["KeyD"].params == RIGHT
    assert overlay.state.binding_set is overlay.define.editor.bset
    assert overlay.bar.undo_button.isEnabled()


def test_the_activation_key_cannot_be_selected(make_overlay, qtbot):
    overlay = make_overlay()
    enter(overlay)
    select(overlay, "CapsLock")
    assert overlay.window.view.selected is None
    assert overlay.define.picker.header.text() == "Pick a key"

    # Nor can it be clicked on the drawn keyboard in define mode.
    view = overlay.window.view
    caps = next(k for k in view._visible_keys() if k.code == "CapsLock")
    with qtbot.assertNotEmitted(view.keyClicked):
        qtbot.mouseClick(view, Qt.LeftButton,
                         pos=view.key_rect(caps).center().toPoint())


def test_an_armed_command_lands_on_the_next_key_clicked(make_overlay, user_root):
    overlay = make_overlay()
    enter(overlay)
    overlay.define.picker.armed.emit(Binding("key.passthrough", {}))
    select(overlay, "KeyQ")
    # Ruling R6: an armed passthrough gets the target key's own code.
    assert saved_binding(user_root, "KeyQ") == {
        "action": "key.passthrough", "params": {"code": "KeyQ"}}
    # Disarmed: the next click only selects.
    before = overlay.state.binding_set.bindings.get("KeyE")
    select(overlay, "KeyE")
    assert overlay.state.binding_set.bindings.get("KeyE") == before


def test_a_dropped_command_is_assigned_to_the_key_under_it(make_overlay,
                                                           user_root):
    overlay = make_overlay()
    enter(overlay)
    overlay.window.view.bindingDropped.emit("KeyD",
                                            Binding("key.passthrough", {}))
    assert saved_binding(user_root, "KeyD") == {
        "action": "key.passthrough", "params": {"code": "KeyD"}}


def test_the_view_turns_a_drop_into_binding_dropped(make_overlay, qtbot):
    overlay = make_overlay()
    enter(overlay)
    view = overlay.window.view
    assert view.acceptDrops()
    key = next(k for k in view._visible_keys() if k.code == "KeyD")
    mime = QMimeData()
    mime.setData(MIME_TYPE, json.dumps(
        {"action": "button.click", "params": RIGHT}).encode("utf-8"))
    with qtbot.waitSignal(view.bindingDropped) as blocker:
        assert view.drop_at(view.key_rect(key).center(), mime)
    assert blocker.args == ["KeyD", Binding("button.click", RIGHT)]
    caps = next(k for k in view._visible_keys() if k.code == "CapsLock")
    assert not view.drop_at(view.key_rect(caps).center(), mime)
    assert not view.drop_at(QPointF(-5, -5), mime)


def test_undo_from_the_bar_reverts_and_saves(make_overlay, user_root):
    overlay = make_overlay()
    enter(overlay)
    select(overlay, "KeyD")
    header_before = overlay.define.picker.header.text()
    assign_from_picker(overlay, "button.click", RIGHT)
    assert overlay.define.picker.header.text() != header_before
    overlay.bar.undo_button.click()

    assert overlay.state.binding_set.bindings["KeyD"].params == LEFT
    assert overlay.state.binding_set is overlay.define.editor.bset
    assert saved_binding(user_root, "KeyD")["params"] == LEFT
    assert not overlay.bar.undo_button.isEnabled()
    assert overlay.bar.redo_button.isEnabled()
    assert overlay.define.picker.header.text() == header_before

    overlay.bar.redo_button.click()
    assert overlay.state.binding_set.bindings["KeyD"].params == RIGHT
    assert saved_binding(user_root, "KeyD")["params"] == RIGHT


# -- reloading the core -----------------------------------------------------

def test_a_burst_of_edits_reloads_once(make_overlay, qtbot):
    client = FakeClient()
    overlay = make_overlay(client)
    client.connect_core()
    enter(overlay)
    editor = overlay.define.editor
    for button in ("right", "middle", "back"):
        editor.assign("KeyD", Binding("button.click", {"button": button}))
        qtbot.wait(30)
    assert client.reloads == 0
    qtbot.wait(400)
    assert client.reloads == 1


def test_edits_while_disconnected_reload_once_on_reconnect(make_overlay, qtbot):
    client = FakeClient()
    overlay = make_overlay(client)
    enter(overlay)
    overlay.define.editor.assign("KeyD", Binding("button.click", RIGHT))
    qtbot.wait(400)
    assert client.reloads == 0
    client.connect_core()
    assert client.reloads == 1
    client.connect_core()
    assert client.reloads == 1


def test_the_mock_says_when_edits_will_apply(make_overlay, qtbot):
    client = FakeClient(bindings_id=None)
    client.name = "mock"
    overlay = make_overlay(client)
    client.connect_core()
    enter(overlay)
    overlay.define.editor.assign("KeyD", Binding("button.click", RIGHT))
    qtbot.wait(400)
    assert client.reloads == 0
    assert overlay.tray.text() == \
        "Saved — changes apply when the core is running."


def test_exiting_fires_a_pending_reload_at_once(make_overlay):
    client = FakeClient()
    overlay = make_overlay(client)
    client.connect_core()
    enter(overlay)
    overlay.define.editor.assign("KeyD", Binding("button.click", RIGHT))
    overlay.bar.define_button.click()
    assert client.reloads == 1


def test_losing_the_core_keeps_define_mode_open(make_overlay):
    client = FakeClient()
    overlay = make_overlay(client)
    client.connect_core()
    enter(overlay)
    select(overlay, "KeyD")
    client.disconnected.emit("lost")
    assert overlay.define.active
    assert overlay.define.picker.isVisible()
    assert overlay.window.view.selected == "KeyD"
    assert overlay.bar.define_indicator.isVisibleTo(overlay.bar)


def test_a_core_switch_to_another_id_closes_define_mode(make_overlay, qtbot,
                                                       user_root):
    client = FakeClient()
    overlay = make_overlay(client)
    client.connect_core()
    enter(overlay)
    select(overlay, "KeyD")
    # Our own reload echoing back with the same id leaves define mode open.
    client.configChanged.emit({"kinds": ["bindings"], "bindings_id": "default"})
    assert overlay.define.active

    overlay.define.picker.label_edit.setText("Uncommitted")
    client.configChanged.emit({"kinds": ["bindings"], "bindings_id": "other"})
    assert not overlay.define.active
    assert overlay.tray.text() == \
        "The core switched to bindings 'other'; define mode closed."
    assert not overlay.define.picker.isVisible()
    assert overlay.window.view.selected is None
    # Nothing was written into the map the core no longer runs.
    assert not user_file(user_root).exists()
    qtbot.wait(400)
    assert client.reloads == 0


def test_config_changed_outside_define_mode_reloads_the_drawn_map(
        make_overlay, user_root):
    client = FakeClient()
    overlay = make_overlay(client)
    bset = binding_store.load("default")
    bset.assign("KeyD", Binding("button.click", RIGHT))
    binding_store.save(bset)
    client.configChanged.emit({"kinds": ["bindings"], "bindings_id": "default"})
    assert overlay.state.binding_set.bindings["KeyD"].params == RIGHT


# -- a broken user file -----------------------------------------------------

def test_a_broken_user_file_offers_only_reset(make_overlay, user_root):
    broken = user_file(user_root)
    broken.parent.mkdir(parents=True)
    broken.write_text('{"schema": "keygnosys/bindings/2", oops', encoding="utf-8")
    before = broken.read_bytes()

    overlay = make_overlay()
    enter(overlay)
    picker = overlay.define.picker
    assert overlay.define.editor is None
    assert overlay.tray.text().startswith("Your bindings file has an error (")
    assert overlay.tray.text().endswith(
        "). Fix it, or use Reset to shipped.")
    assert picker.reset_button.isVisible()
    assert not any(picker.list.item(r).data(Qt.UserRole)
                   for r in range(picker.list.count()))
    select(overlay, "KeyD")
    assert overlay.window.view.selected is None

    overlay.bar.define_button.click()
    assert broken.read_bytes() == before


def test_reset_moves_the_broken_file_aside_and_reloads(make_overlay, user_root):
    broken = user_file(user_root)
    broken.parent.mkdir(parents=True)
    broken.write_text("not json", encoding="utf-8")
    client = FakeClient()
    overlay = make_overlay(client)
    client.connect_core()
    enter(overlay)

    overlay.define.picker.reset_button.click()
    assert not broken.exists()
    moved = list(broken.parent.glob("default.broken-*.json"))
    assert len(moved) == 1 and moved[0].read_text("utf-8") == "not json"
    assert overlay.define.editor is not None
    assert overlay.state.binding_set is overlay.define.editor.bset
    assert client.reloads == 1


# -- diagnostics ------------------------------------------------------------

def test_a_binding_invalid_diagnostic_shows_in_the_tray(make_overlay):
    client = FakeClient()
    overlay = make_overlay(client)
    client.diagnostic.emit({"level": "error", "code": "binding.invalid",
                            "message": "KeyD: unknown action",
                            "file": "default.json"})
    assert overlay.tray.isVisible()
    assert overlay.tray.text() == "Bindings not applied — KeyD: unknown action"


# -- fix round 1 ------------------------------------------------------------

def test_a_late_connect_to_another_id_closes_define_mode(make_overlay, qtbot,
                                                         user_root):
    # Entered with the core down, so the id came from settings ("default").
    client = FakeClient(bindings_id=None)
    overlay = make_overlay(client)
    enter(overlay)
    assert overlay.define.bindings_id == "default"
    select(overlay, "KeyD")
    assign_from_picker(overlay, "button.click", RIGHT)
    qtbot.wait(400)
    assert overlay.define.reload_pending
    overlay.define.picker.label_edit.setText("Uncommitted")

    client._id = "other"
    client.connect_core()
    assert not overlay.define.active
    assert overlay.tray.text() ==         "The core switched to bindings 'other'; define mode closed."
    assert client.reloads == 0
    assert not overlay.define.reload_pending
    assert "legend" not in saved_binding(user_root, "KeyD")
    qtbot.wait(400)
    assert client.reloads == 0


def test_a_late_connect_to_the_same_id_sends_the_pending_reload(make_overlay,
                                                                qtbot):
    client = FakeClient(bindings_id=None)
    overlay = make_overlay(client)
    enter(overlay)
    overlay.define.editor.assign("KeyD", Binding("button.click", RIGHT))
    qtbot.wait(400)
    client._id = "default"
    client.connect_core()
    assert overlay.define.active
    assert client.reloads == 1


def test_window_slots_repaint_the_keyboard_in_define_mode(make_overlay,
                                                          monkeypatch):
    client = FakeClient()
    overlay = make_overlay(client)
    enter(overlay)
    repaints = []
    monkeypatch.setattr(overlay.window.view, "update",
                        lambda *a: repaints.append(a))
    client.windowsChanged.emit([{"index": 1, "title": "Editor"}])
    assert repaints


def test_a_failed_click_through_restore_disables_the_control(make_overlay,
                                                             monkeypatch):
    overlay = make_overlay()
    enter(overlay)
    monkeypatch.setattr(clickthrough, "set_click_through",
                        lambda _w, _on: False)
    monkeypatch.setattr(clickthrough, "unavailable_reason", lambda: None)
    overlay.bar.define_button.click()
    assert not overlay.define.active
    click = overlay.bar._click
    assert not click.isEnabled() and not click.isChecked()
    assert click.toolTip() == "Click-through failed on this platform."

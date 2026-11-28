"""Define mode on the overlay: select a key, choose a command, save, reload."""

from __future__ import annotations

import json

import pytest
from PySide6.QtCore import QMimeData, QPointF, Qt

from keygnosys import binding_store, bundle, paths
from keygnosys.coreclient.base import CoreClient
from keygnosys.documents import Binding, BindingSet, Registry
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
    # It is the one thing to do, so it is highlighted.
    assert picker.reset_button.property("urgent") is True
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


# -- final review fixes -----------------------------------------------------

def write_user_doc(user_root, doc: dict, name: str = "default") -> None:
    path = user_root / "bindings" / f"{name}.json"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(doc), encoding="utf-8")


def shipped_default() -> dict:
    return json.loads((paths.BUNDLED_ROOT / "bindings" / "default.json")
                      .read_text("utf-8"))


def test_define_mode_opens_with_a_string_setting_and_keeps_it(make_overlay,
                                                              user_root):
    doc = shipped_default()
    doc["settings"]["pointer_curve"] = "smooth"
    write_user_doc(user_root, doc)
    overlay = make_overlay()
    enter(overlay)
    assert overlay.define.editor is not None
    assert overlay.define.picker.isVisible()
    assert "pointer_curve" not in overlay.define.picker.setting_boxes
    select(overlay, "KeyQ")
    assign_from_picker(overlay, "overlay.toggle", {})
    saved = json.loads(user_file(user_root).read_text("utf-8"))
    assert saved["settings"]["pointer_curve"] == "smooth"
    assert saved["bindings"]["KeyQ"]["action"] == "overlay.toggle"


def test_a_failure_opening_the_editor_leaves_define_mode_closed(
        make_overlay, click_calls, monkeypatch):
    overlay = make_overlay()

    def boom(_editor):
        raise TypeError("picker exploded")
    monkeypatch.setattr(overlay.define.picker, "set_editor", boom)
    overlay.bar.define_button.click()
    assert not overlay.define.active
    assert overlay.define.editor is None
    assert click_calls == [False, True]
    assert not overlay.state.define_mode
    assert not overlay.define.picker.isVisible()
    assert overlay.bar.define_button.text() == "Define"
    assert not overlay.bar.define_button.isChecked()
    assert "picker exploded" in overlay.tray.text()


def test_changing_params_keeps_the_label(make_overlay, user_root):
    overlay = make_overlay()
    enter(overlay)
    select(overlay, "Digit1")
    picker = overlay.define.picker
    picker.label_edit.setText("Mail")
    picker.label_edit.editingFinished.emit()
    index = picker.params_combo.findData({"index": 4})
    picker.params_combo.setCurrentIndex(index)
    picker.params_combo.activated.emit(index)
    bound = overlay.state.binding_set.bindings["Digit1"]
    assert bound.params == {"index": 4} and bound.legend == "Mail"
    assert saved_binding(user_root, "Digit1") == {
        "action": "window.slot", "params": {"index": 4}, "legend": "Mail"}


def test_the_picker_names_keys_as_the_layout_draws_them(make_overlay):
    overlay = make_overlay()
    enter(overlay)
    select(overlay, "KeyD")
    assert overlay.define.picker.header.text().startswith("Assign to D (now: ")
    overlay.define.editor.clear("Digit1")
    labels = [e.label for e in overlay.define.editor.entries()
              if e.stored is not None]
    assert any(label.endswith("(was on 1)") for label in labels), labels


def test_an_edit_while_the_core_is_down_says_when_it_applies(make_overlay,
                                                             qtbot):
    client = FakeClient()
    overlay = make_overlay(client)
    enter(overlay)
    notices = []
    real = overlay.show_notice
    overlay.show_notice = lambda text: (notices.append(text), real(text))
    overlay.define.editor.assign("KeyD", Binding("button.click", RIGHT))
    overlay.define.editor.assign("KeyD", Binding("button.click", LEFT))
    assert notices == ["Saved — changes apply when the core is running."]
    # A new disconnected stretch says it again.
    client.connect_core()
    client.disconnected.emit("lost")
    overlay.define.editor.assign("KeyD", Binding("button.click", RIGHT))
    assert notices == ["Saved — changes apply when the core is running."] * 2


def test_an_edit_while_connected_shows_no_offline_notice(make_overlay):
    client = FakeClient()
    overlay = make_overlay(client)
    client.connect_core()
    enter(overlay)
    notices = []
    overlay.show_notice = notices.append
    overlay.define.editor.assign("KeyD", Binding("button.click", RIGHT))
    assert notices == []


def test_a_broken_user_file_with_no_shipped_copy_offers_no_reset(make_overlay,
                                                                 user_root):
    broken = user_root / "bindings" / "mine.json"
    broken.parent.mkdir(parents=True)
    broken.write_text("not json", encoding="utf-8")
    overlay = make_overlay(FakeClient(bindings_id="mine"))
    enter(overlay)
    assert overlay.define.editor is None
    assert not overlay.define.picker.reset_button.isVisible()


def test_a_user_file_whose_id_differs_is_saved_in_place(make_overlay,
                                                        user_root):
    doc = shipped_default()
    doc["id"] = "custom"
    write_user_doc(user_root, doc)
    overlay = make_overlay()
    enter(overlay)
    select(overlay, "KeyQ")
    assign_from_picker(overlay, "overlay.toggle", {})
    assert saved_binding(user_root, "KeyQ")["action"] == "overlay.toggle"
    assert not (user_root / "bindings" / "custom.json").exists()


def test_hiding_the_keyboard_from_the_bar_exits_define_mode(make_overlay,
                                                            click_calls):
    overlay = make_overlay()
    overlay.window.show()
    enter(overlay)
    overlay.bar._hide.click()
    assert not overlay.window.isVisible()
    assert not overlay.define.active
    assert not overlay.define.picker.isVisible()
    assert click_calls == [False, True]


def test_overlay_toggle_from_the_core_exits_define_mode(make_overlay):
    client = FakeClient()
    overlay = make_overlay(client)
    overlay.window.show()
    enter(overlay)
    client.overlayToggleRequested.emit()
    assert not overlay.window.isVisible()
    assert not overlay.define.active


# -- export and import ------------------------------------------------------

RESET_FIRST = "Reset to shipped first"


class Transfer:
    """Stands in for every export and import dialog; records the calls."""

    def __init__(self, overlay: Overlay, *, open_path=None, save_path=None,
                 import_answer: bool = True,
                 export_answer: str | None = "") -> None:
        self.open_path = open_path
        self.save_path = save_path
        self.import_answer = import_answer
        self.export_answer = export_answer
        self.opened = 0
        self.suggested: list[str] = []
        self.imports: list[tuple] = []
        self.exports: list[tuple] = []
        d = overlay.define
        d.choose_open_path = self._open
        d.choose_save_path = self._save
        d.ask_import = self._ask_import
        d.ask_export = self._ask_export

    def _open(self):
        self.opened += 1
        return self.open_path

    def _save(self, suggested: str):
        self.suggested.append(suggested)
        return self.save_path

    def _ask_import(self, plan, changes) -> bool:
        self.imports.append((plan, changes))
        return self.import_answer

    def _ask_export(self, name, key_count, warnings):
        self.exports.append((name, key_count, warnings))
        return self.export_answer


def import_file(tmp_path, *, name: str = "Sam's layer", author: str = "Sam",
                code: str = "KeyD", binding: Binding | None = None):
    """A bundle as another overlay would export it: the shipped map with one
    key changed, under another id and name (ruling R2)."""
    bset = BindingSet.from_dict(shipped_default())
    bset.id = "sams"
    bset.name = name
    bset.metadata.author = ""
    bset.assign(code, binding or Binding("button.click", RIGHT))
    path = tmp_path / f"incoming{bundle.EXTENSION}"
    bundle.write_bundle(path, bundle.export_bundle(bset, author=author))
    return path


def saved_doc(user_root) -> dict:
    return json.loads(user_file(user_root).read_text("utf-8"))


def backups(user_root) -> list:
    return list(user_file(user_root).parent.glob("default.backup-*.json"))


def test_buttons_visible_only_in_define_mode(make_overlay):
    overlay = make_overlay()
    bar = overlay.bar
    for button in (bar.export_button, bar.import_button):
        assert not button.isVisibleTo(bar)
    enter(overlay)
    for button in (bar.export_button, bar.import_button):
        assert button.isVisibleTo(bar) and button.isEnabled()
        assert RESET_FIRST not in button.toolTip()
    assert overlay.define.can_transfer
    overlay.bar.define_button.click()
    for button in (bar.export_button, bar.import_button):
        assert not button.isVisibleTo(bar)


def test_buttons_disabled_while_broken_with_tooltip(make_overlay, user_root,
                                                    tmp_path):
    broken = user_file(user_root)
    broken.parent.mkdir(parents=True)
    broken.write_text("not json", encoding="utf-8")
    overlay = make_overlay()
    transfer = Transfer(overlay)
    enter(overlay)
    bar = overlay.bar
    assert not overlay.define.can_transfer
    for button in (bar.export_button, bar.import_button):
        assert button.isVisibleTo(bar) and not button.isEnabled()
        assert RESET_FIRST in button.toolTip()
    # Called anyway (as a drop would), it refuses with the same words.
    assert overlay.define.import_(import_file(tmp_path)) is False
    assert overlay.tray.text() == "Reset to shipped first."
    assert transfer.imports == []
    assert broken.read_text("utf-8") == "not json"
    # Reset brings them back.
    overlay.define.picker.reset_button.click()
    assert bar.import_button.isEnabled() and bar.export_button.isEnabled()
    assert RESET_FIRST not in bar.import_button.toolTip()


def test_export_writes_bundle_and_makes_no_save(make_overlay, user_root,
                                                tmp_path, qtbot):
    client = FakeClient()
    overlay = make_overlay(client)
    client.connect_core()
    out = tmp_path / "out" / f"mine{bundle.EXTENSION}"
    out.parent.mkdir()
    transfer = Transfer(overlay, save_path=out, export_answer="Sam")
    enter(overlay)
    running = overlay.define.editor.bset
    overlay.bar.export_button.click()

    plan = bundle.read_import(out)
    assert plan.bset.to_dict() == running.to_dict()
    assert json.loads(out.read_text("utf-8"))["author"] == "Sam"
    assert transfer.suggested == [f"{running.name}.keygnosys.json"]
    name, key_count, warnings = transfer.exports[0]
    assert name == running.name
    assert key_count == len(running.bindings) + len(running.extra_bindings)
    assert warnings == []
    qtbot.wait(400)
    assert client.reloads == 0
    assert not user_file(user_root).exists()
    assert not overlay.define.editor.can_undo
    assert overlay.tray.text() == f"Exported to mine{bundle.EXTENSION}."


def test_export_warns_about_unknown_commands(make_overlay, user_root,
                                             tmp_path):
    doc = shipped_default()
    doc["bindings"]["KeyD"] = {"action": "window.snap", "params": {}}
    write_user_doc(user_root, doc)
    overlay = make_overlay()
    transfer = Transfer(overlay, save_path=tmp_path / "x.keygnosys.json")
    enter(overlay)
    overlay.define.export()
    assert transfer.exports[0][2] == [
        'D: unknown command "window.snap" (exported unchanged)']


def test_export_failure_shows_a_notice(make_overlay, tmp_path):
    overlay = make_overlay()
    Transfer(overlay, save_path=tmp_path / "missing" / "x.keygnosys.json")
    enter(overlay)
    overlay.define.export()
    assert overlay.tray.text().startswith("Export failed — ")


def test_export_cancelled_writes_nothing(make_overlay, user_root, tmp_path):
    out_dir = tmp_path / "out"
    out_dir.mkdir()
    overlay = make_overlay()
    transfer = Transfer(overlay, save_path=out_dir / "x.keygnosys.json",
                        export_answer=None)
    enter(overlay)
    overlay.define.export()
    assert transfer.suggested == []

    transfer.export_answer = ""
    transfer.save_path = None
    overlay.define.export()
    assert len(transfer.suggested) == 1
    assert list(out_dir.iterdir()) == []
    assert not user_file(user_root).exists()
    assert not overlay.tray.text().startswith("Exported")


def test_export_commits_pending_label_first(make_overlay, tmp_path):
    out = tmp_path / "x.keygnosys.json"
    overlay = make_overlay()
    Transfer(overlay, save_path=out)
    enter(overlay)
    select(overlay, "KeyD")
    overlay.define.picker.label_edit.setText("Mine")
    overlay.define.export()
    plan = bundle.read_import(out)
    assert plan.bset.bindings["KeyD"].legend == "Mine"


def test_import_replaces_saves_and_reloads_once(make_overlay, user_root,
                                                tmp_path, qtbot):
    path = import_file(tmp_path)
    client = FakeClient()
    overlay = make_overlay(client)
    client.connect_core()
    transfer = Transfer(overlay, open_path=path)
    enter(overlay)
    overlay.bar.import_button.click()

    assert transfer.opened == 1
    plan, changes = transfer.imports[0]
    assert [c.name for c in changes] == ["D"]
    saved = saved_doc(user_root)
    assert saved["id"] == "default"
    assert saved["bindings"]["KeyD"] == {"action": "button.click",
                                         "params": RIGHT}
    assert saved["metadata"]["imported_from"] == {
        "id": "sams", "name": "Sam's layer", "author": "Sam",
        "exported": plan.exported}
    # The map's empty author is filled from the bundle header.
    assert saved["metadata"]["author"] == "Sam"
    assert overlay.state.binding_set is overlay.define.editor.bset
    assert overlay.define.active
    qtbot.waitUntil(lambda: client.reloads == 1)
    qtbot.wait(400)
    assert client.reloads == 1
    assert overlay.tray.text() == (
        "Imported 'Sam's layer'. Undo or Reset to shipped brings back the "
        "previous map.")


def test_import_writes_backup_when_user_copy_exists(make_overlay, user_root,
                                                    tmp_path):
    write_user_doc(user_root, shipped_default())
    before = user_file(user_root).read_bytes()
    overlay = make_overlay()
    Transfer(overlay)
    enter(overlay)
    assert overlay.define.import_(import_file(tmp_path)) is True
    made = backups(user_root)
    assert len(made) == 1
    assert made[0].read_bytes() == before
    assert overlay.tray.text() == (
        f"Imported 'Sam's layer'. Your previous map is saved as "
        f"{made[0].name}.")


def test_import_cancel_changes_nothing(make_overlay, user_root, tmp_path,
                                       qtbot):
    write_user_doc(user_root, shipped_default())
    before = user_file(user_root).read_bytes()
    client = FakeClient()
    overlay = make_overlay(client)
    client.connect_core()
    transfer = Transfer(overlay, import_answer=False)
    enter(overlay)
    assert overlay.define.import_(import_file(tmp_path)) is False
    assert len(transfer.imports) == 1
    assert backups(user_root) == []
    assert user_file(user_root).read_bytes() == before
    assert not overlay.define.editor.can_undo
    qtbot.wait(400)
    assert client.reloads == 0


def test_import_with_no_file_chosen_does_nothing(make_overlay, user_root):
    overlay = make_overlay()
    transfer = Transfer(overlay, open_path=None)
    enter(overlay)
    assert overlay.define.import_() is False
    assert transfer.opened == 1 and transfer.imports == []
    assert not user_file(user_root).exists()


def test_refused_import_writes_nothing(make_overlay, user_root, tmp_path):
    write_user_doc(user_root, shipped_default())
    before = user_file(user_root).read_bytes()
    bad = tmp_path / "bad.keygnosys.json"
    bad.write_text('{"schema": "keygnosys/bundle/9", "documents": []}',
                   encoding="utf-8")
    overlay = make_overlay()
    transfer = Transfer(overlay)
    enter(overlay)
    assert overlay.define.import_(bad) is False
    assert "This file was made by a newer KeyGnosys." in overlay.tray.text()
    assert transfer.imports == []
    assert backups(user_root) == []
    assert user_file(user_root).read_bytes() == before


def test_identical_import_changes_nothing(make_overlay, user_root, tmp_path,
                                          qtbot):
    write_user_doc(user_root, shipped_default())
    before = user_file(user_root).read_bytes()
    client = FakeClient()
    overlay = make_overlay(client)
    client.connect_core()
    out = tmp_path / "same.keygnosys.json"
    enter(overlay)
    # Whatever the dialog answers, an import that changes nothing does nothing.
    for answer in (False, True):
        transfer = Transfer(overlay, save_path=out, import_answer=answer)
        overlay.define.export()
        assert overlay.define.import_(out) is False
        assert transfer.imports[0][1] == []
    assert backups(user_root) == []
    assert user_file(user_root).read_bytes() == before
    assert not overlay.define.editor.can_undo
    qtbot.wait(400)
    assert client.reloads == 0


def test_undo_reverts_import(make_overlay, user_root, tmp_path, qtbot):
    write_user_doc(user_root, shipped_default())
    client = FakeClient()
    overlay = make_overlay(client)
    client.connect_core()
    Transfer(overlay)
    enter(overlay)
    previous = overlay.define.editor.bset.to_dict()
    assert overlay.define.import_(import_file(tmp_path))
    qtbot.waitUntil(lambda: client.reloads == 1)

    overlay.bar.undo_button.click()
    assert overlay.state.binding_set.bindings["KeyD"].params == LEFT
    saved = saved_doc(user_root)
    assert "imported_from" not in saved["metadata"]
    assert saved["bindings"] == previous["bindings"]
    assert saved["name"] == previous["name"]
    assert not overlay.define.editor.can_undo
    qtbot.waitUntil(lambda: client.reloads == 2)


def test_import_while_offline_reloads_on_connect(make_overlay, user_root,
                                                 tmp_path, qtbot):
    client = FakeClient()
    overlay = make_overlay(client)
    client.connect_core()
    Transfer(overlay)
    enter(overlay)
    client.disconnected.emit("lost")
    notices = []
    real = overlay.show_notice
    overlay.show_notice = lambda text: (notices.append(text), real(text))
    assert overlay.define.import_(import_file(tmp_path))
    assert notices.count(
        "Saved — changes apply when the core is running.") == 1
    qtbot.wait(400)
    assert client.reloads == 0
    assert saved_binding(user_root, "KeyD") == {"action": "button.click",
                                                "params": RIGHT}
    client.connect_core()
    assert client.reloads == 1


def test_import_commits_pending_label_first(make_overlay, user_root, tmp_path):
    overlay = make_overlay()
    Transfer(overlay)
    enter(overlay)
    select(overlay, "KeyF")
    overlay.define.picker.label_edit.setText("Typed")
    assert overlay.define.import_(import_file(tmp_path))
    assert overlay.state.binding_set.bindings["KeyF"].legend != "Typed"
    overlay.define.undo()
    assert overlay.state.binding_set.bindings["KeyF"].legend == "Typed"
    assert saved_binding(user_root, "KeyF")["legend"] == "Typed"


def test_backup_failure_aborts_import(make_overlay, user_root, tmp_path,
                                      monkeypatch, qtbot):
    write_user_doc(user_root, shipped_default())
    before = user_file(user_root).read_bytes()
    client = FakeClient()
    overlay = make_overlay(client)
    client.connect_core()
    Transfer(overlay)
    enter(overlay)
    running = overlay.define.editor.bset.to_dict()

    def fail(_id):
        raise binding_store.StoreError("disk full")
    monkeypatch.setattr(binding_store, "backup", fail)
    assert overlay.define.import_(import_file(tmp_path)) is False
    assert "disk full" in overlay.tray.text()
    assert overlay.define.editor.bset.to_dict() == running
    assert not overlay.define.editor.can_undo
    assert user_file(user_root).read_bytes() == before
    qtbot.wait(400)
    assert client.reloads == 0

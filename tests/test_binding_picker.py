"""The define-mode side picker: choose a command for the selected key."""

from __future__ import annotations

import json

import pytest
from PySide6.QtCore import QRect, Qt

from keygnosys import paths
from keygnosys.binding_editor import BindingEditor
from keygnosys.documents import Binding, BindingSet
from keygnosys.ui.binding_picker import MIME_TYPE, BindingPicker


@pytest.fixture
def editor():
    doc = json.loads((paths.BUNDLED_ROOT / "bindings" / "default.json")
                     .read_text(encoding="utf-8"))
    return BindingEditor(BindingSet.from_dict(doc), lambda b: None)


@pytest.fixture
def confirms():
    return []


def make_picker(qtbot, editor, answer=True, asked=None):
    def confirm(question: str) -> bool:
        if asked is not None:
            asked.append(question)
        return answer
    picker = BindingPicker(confirm=confirm)
    qtbot.addWidget(picker)
    picker.set_editor(editor)
    return picker


def entry_items(picker):
    out = []
    for row in range(picker.list.count()):
        item = picker.list.item(row)
        entry = item.data(Qt.UserRole)
        if entry is not None:
            out.append((item, entry))
    return out


def pick(picker, action, *, group=None):
    for item, entry in entry_items(picker):
        if entry.action == action and (group is None or entry.group == group):
            picker.list.setCurrentItem(item)
            return entry
    raise AssertionError(f"{action} not listed")


def choose_choice(picker, params):
    for i in range(picker.choice.count()):
        if picker.choice.itemData(i) == params:
            picker.choice.setCurrentIndex(i)
            return
    raise AssertionError(f"{params} not offered")


def test_choosing_an_entry_with_a_key_selected_requests_assignment(qtbot, editor):
    picker = make_picker(qtbot, editor)
    picker.set_selected_key("KeyD")
    pick(picker, "button.click")
    choose_choice(picker, {"button": "right"})
    with qtbot.waitSignal(picker.assignRequested) as blocker:
        picker.assign_button.click()
    assert blocker.args == ["KeyD", Binding("button.click", {"button": "right"})]


def test_choosing_with_no_key_selected_arms_instead(qtbot, editor):
    picker = make_picker(qtbot, editor)
    picker.set_selected_key(None)
    assert picker.header.text() == "Pick a key"
    pick(picker, "overlay.toggle")
    with qtbot.assertNotEmitted(picker.assignRequested):
        with qtbot.waitSignal(picker.armed) as blocker:
            picker.assign_button.click()
    assert blocker.args == [Binding("overlay.toggle", {})]


def test_a_parameterised_entry_asks_for_its_choice_first(qtbot, editor):
    picker = make_picker(qtbot, editor)
    picker.show()
    picker.set_selected_key("KeyQ")
    pick(picker, "window.slot")
    assert picker.choice.isVisible() and picker.choice.count() == 9
    # Choices already bound say where.
    assert "(on Digit1)" in picker.choice.itemText(0)
    choose_choice(picker, {"index": 4})
    with qtbot.waitSignal(picker.assignRequested) as blocker:
        picker.assign_button.click()
    code, binding = blocker.args
    assert code == "KeyQ"
    assert binding.action == "window.slot" and binding.params == {"index": 4}

    pick(picker, "overlay.toggle")
    assert not picker.choice.isVisible()


def test_passthrough_targets_the_selected_key(qtbot, editor):
    picker = make_picker(qtbot, editor)
    picker.set_selected_key("KeyD")
    pick(picker, "key.passthrough")
    with qtbot.waitSignal(picker.assignRequested) as blocker:
        picker.assign_button.click()
    assert blocker.args == ["KeyD", Binding("key.passthrough", {"code": "KeyD"})]


def test_passthrough_arms_without_a_code_when_no_key_is_selected(qtbot, editor):
    picker = make_picker(qtbot, editor)
    picker.set_selected_key(None)
    pick(picker, "key.passthrough")
    assert picker.assign_button.isEnabled()
    with qtbot.waitSignal(picker.armed) as blocker:
        picker.assign_button.click()
    assert blocker.args == [Binding("key.passthrough", {})]


def test_passthrough_drags_without_a_code(qtbot, editor):
    picker = make_picker(qtbot, editor)
    picker.set_selected_key("KeyD")
    item = next(i for i, e in entry_items(picker) if e.action == "key.passthrough")
    assert item.flags() & Qt.ItemIsDragEnabled
    mime = picker.list.mimeData([item])
    payload = json.loads(bytes(mime.data(MIME_TYPE)).decode("utf-8"))
    assert payload == Binding("key.passthrough", {}).to_dict()


def click_item(qtbot, picker, action):
    item = next(i for i, e in entry_items(picker) if e.action == action)
    picker.list.scrollToItem(item)
    rect = picker.list.visualItemRect(item)
    qtbot.mouseClick(picker.list.viewport(), Qt.LeftButton, pos=rect.center())


def test_a_single_click_assigns_a_parameterless_entry(qtbot, editor):
    picker = make_picker(qtbot, editor)
    picker.show()
    picker.set_selected_key("KeyQ")
    with qtbot.waitSignal(picker.assignRequested) as blocker:
        click_item(qtbot, picker, "overlay.toggle")
    assert blocker.args == ["KeyQ", Binding("overlay.toggle", {})]


def test_a_click_on_a_parameterised_entry_waits_for_its_choice(qtbot, editor):
    picker = make_picker(qtbot, editor)
    picker.show()
    picker.set_selected_key("KeyQ")
    with qtbot.assertNotEmitted(picker.assignRequested):
        click_item(qtbot, picker, "window.slot")
    assert picker.choice.isVisible()
    index = picker.choice.findData({"index": 4})
    picker.choice.setCurrentIndex(index)
    with qtbot.waitSignal(picker.assignRequested) as blocker:
        picker.choice.activated.emit(index)      # the user's pick
    assert blocker.args == ["KeyQ", Binding("window.slot", {"index": 4})]


def test_selecting_a_key_while_armed_disarms(qtbot, editor):
    picker = make_picker(qtbot, editor)
    picker.set_selected_key(None)
    pick(picker, "overlay.toggle")
    picker.assign_button.click()
    with qtbot.waitSignal(picker.armed) as blocker:
        picker.set_selected_key("KeyQ")
    assert blocker.args == [None]
    with qtbot.assertNotEmitted(picker.armed):
        picker.set_selected_key("KeyW")


def test_an_uncommitted_label_is_kept_when_the_key_changes(qtbot, editor):
    picker = make_picker(qtbot, editor)
    picker.set_selected_key("Digit1")
    picker.label_edit.setText("Mail")
    with qtbot.waitSignal(picker.legendEdited) as blocker:
        picker.set_selected_key("Digit2")
    assert blocker.args == ["Digit1", "Mail"]


def test_settings_do_not_round_and_showing_one_never_rewrites_it(qtbot, editor):
    editor.bset.settings["precision_factor"] = 0.1234
    editor.bset.settings["pointer_ramp_ms"] = -5
    picker = make_picker(qtbot, editor)
    with qtbot.assertNotEmitted(picker.settingEdited):
        picker.refresh()
    assert picker.setting_boxes["precision_factor"].value() == pytest.approx(0.1234)
    # Out of the core's range: shown as the core runs it (clamped to 0), but
    # the document keeps what the user wrote until they edit it.
    assert picker.setting_boxes["pointer_ramp_ms"].value() == 0
    assert editor.bset.settings["pointer_ramp_ms"] == -5


def test_an_unassigned_entry_assigns_the_stored_command_with_its_legend(qtbot, editor):
    editor.set_legend("Digit1", "Mail")
    editor.clear("Digit1")
    stored = editor.bset.unassigned[-1]
    picker = make_picker(qtbot, editor)
    picker.set_selected_key("KeyQ")
    for item, entry in entry_items(picker):
        if entry.stored is stored:
            picker.list.setCurrentItem(item)
            break
    else:
        raise AssertionError("stored command not listed")
    with qtbot.waitSignal(picker.assignRequested) as blocker:
        picker.assign_button.click()
    assert blocker.args[0] == "KeyQ" and blocker.args[1] is stored
    assert blocker.args[1].legend == "Mail"


def test_header_names_the_key_and_its_current_legend(qtbot, editor):
    picker = make_picker(qtbot, editor)
    picker.set_selected_key("Digit1")
    assert picker.header.text() == "Assign to Digit1 (now: App 1)"


def test_refresh_reads_a_replaced_binding_set(qtbot, editor):
    picker = make_picker(qtbot, editor)
    picker.set_selected_key("KeyQ")
    editor.assign("KeyQ", Binding("overlay.toggle"))
    picker.refresh()
    assert picker.header.text() == "Assign to KeyQ (now: Map)"
    editor.undo()                      # replaces editor.bset
    picker.refresh()
    assert "(now: Map)" not in picker.header.text()


def test_label_edit_and_clear_emit_for_the_selected_key(qtbot, editor):
    picker = make_picker(qtbot, editor)
    picker.set_selected_key("Digit1")
    picker.label_edit.setText("Mail")
    with qtbot.waitSignal(picker.legendEdited) as blocker:
        picker.label_edit.editingFinished.emit()
    assert blocker.args == ["Digit1", "Mail"]
    with qtbot.waitSignal(picker.clearRequested) as blocker:
        picker.clear_button.click()
    assert blocker.args == ["Digit1"]


def test_settings_get_typed_spin_boxes(qtbot, editor):
    from PySide6.QtWidgets import QDoubleSpinBox, QSpinBox
    picker = make_picker(qtbot, editor)
    boxes = picker.setting_boxes
    assert set(boxes) == set(editor.bset.settings)
    assert isinstance(boxes["precision_factor"], QDoubleSpinBox)
    assert isinstance(boxes["pointer_max_speed"], QDoubleSpinBox)
    assert isinstance(boxes["pointer_ramp_ms"], QSpinBox)
    assert boxes["pointer_max_speed"].value() == editor.bset.settings["pointer_max_speed"]
    with qtbot.waitSignal(picker.settingEdited) as blocker:
        boxes["pointer_max_speed"].setValue(40)
    assert blocker.args == ["pointer_max_speed", 40.0]


def test_settings_section_collapses(qtbot, editor):
    picker = make_picker(qtbot, editor)
    picker.show()
    assert not picker.settings_body.isVisible()
    picker.settings_toggle.click()
    assert picker.settings_body.isVisible()


def test_reset_needs_confirmation(qtbot, editor):
    asked = []
    picker = make_picker(qtbot, editor, answer=False, asked=asked)
    picker.show()
    assert not picker.reset_button.isVisible()
    picker.set_can_reset(True)
    assert picker.reset_button.isVisible()
    with qtbot.assertNotEmitted(picker.resetRequested):
        picker.reset_button.click()
    assert asked == ["Discard your edits to this map and return to the shipped one?"]

    picker2 = make_picker(qtbot, editor, answer=True)
    picker2.set_can_reset(True)
    fired = []
    picker2.resetRequested.connect(lambda: fired.append(1))
    picker2.reset_button.click()
    assert fired == [1]


def test_search_narrows_the_list(qtbot, editor):
    picker = make_picker(qtbot, editor)
    everything = len(entry_items(picker))
    picker.search.setText("window slot")
    narrowed = entry_items(picker)
    assert 0 < len(narrowed) < everything
    assert all("slot" in e.label.lower() or "slot" in e.action for _, e in narrowed)
    picker.search.setText("")
    assert len(entry_items(picker)) == everything


def test_list_items_carry_a_binding_drag_payload(qtbot, editor):
    picker = make_picker(qtbot, editor)
    item = next(i for i, e in entry_items(picker) if e.action == "overlay.toggle")
    mime = picker.list.mimeData([item])
    assert mime.hasFormat(MIME_TYPE)
    payload = json.loads(bytes(mime.data(MIME_TYPE)).decode("utf-8"))
    assert payload == Binding("overlay.toggle").to_dict()


def test_unassigned_items_are_marked(qtbot, editor):
    editor.clear("Digit1")
    picker = make_picker(qtbot, editor)
    marked = [e for i, e in entry_items(picker) if picker.is_marked_unassigned(i)]
    assert marked and all(e.group == "Unassigned" for e in marked)


def test_it_docks_to_the_right_of_the_keyboard(qtbot, editor):
    picker = make_picker(qtbot, editor)
    anchor = QRect(100, 200, 1000, 300)
    picker.dock_beside(anchor)
    assert picker.pos().x() == anchor.right() + 4
    assert picker.pos().y() == anchor.top()


def test_it_accepts_input(qtbot, editor):
    picker = make_picker(qtbot, editor)
    assert not picker.windowFlags() & Qt.WindowTransparentForInput
    assert picker.windowFlags() & Qt.Tool


# -- final review fixes -----------------------------------------------------

def test_known_settings_get_core_bounded_boxes(qtbot, editor):
    from PySide6.QtWidgets import QDoubleSpinBox, QSpinBox
    picker = make_picker(qtbot, editor)
    boxes = picker.setting_boxes
    for name in ("pointer_base_speed", "pointer_max_speed",
                 "scroll_base_speed", "scroll_max_speed"):
        box = boxes[name]
        assert isinstance(box, QDoubleSpinBox), name
        assert (box.minimum(), box.maximum()) == (0.01, 10000.0)
    for name in ("pointer_ramp_ms", "scroll_ramp_ms"):
        box = boxes[name]
        assert isinstance(box, QSpinBox), name
        assert (box.minimum(), box.maximum()) == (0, 60000)
    box = boxes["precision_factor"]
    assert isinstance(box, QDoubleSpinBox)
    assert (box.minimum(), box.maximum()) == (0.001, 1.0)


def test_unknown_and_non_numeric_settings_get_no_box(qtbot, editor):
    editor.bset.settings["pointer_curve"] = "smooth"      # a newer release's
    editor.bset.settings["x_flag"] = True
    editor.bset.settings["x_future_number"] = 3
    editor.bset.settings["precision_factor"] = "high"     # known, wrong type
    editor.bset.settings["pointer_ramp_ms"] = False       # a bool is not an int
    picker = make_picker(qtbot, editor)                   # must not raise
    for name in ("pointer_curve", "x_flag", "x_future_number",
                 "precision_factor", "pointer_ramp_ms"):
        assert name not in picker.setting_boxes, name
    assert "pointer_max_speed" in picker.setting_boxes


def test_a_bound_parameterised_key_offers_its_params(qtbot, editor):
    picker = make_picker(qtbot, editor)
    picker.set_selected_key("Digit1")                    # window.slot index 1
    assert picker.params_combo.isVisibleTo(picker)
    assert picker.params_combo.count() == 9
    assert picker.params_combo.currentData() == {"index": 1}
    with qtbot.waitSignal(picker.paramsEdited) as blocker:
        picker.params_combo.setCurrentIndex(2)
        picker.params_combo.activated.emit(2)
    assert blocker.args == ["Digit1", {"index": 3}]


def test_params_are_not_offered_for_a_single_choice_or_unbound_key(qtbot, editor):
    picker = make_picker(qtbot, editor)
    picker.set_selected_key("KeyQ")                       # unbound
    assert not picker.params_combo.isVisibleTo(picker)
    editor.assign("KeyQ", Binding("overlay.toggle"))
    picker.refresh()
    assert not picker.params_combo.isVisibleTo(picker)


def test_choosing_the_current_params_emits_nothing(qtbot, editor):
    picker = make_picker(qtbot, editor)
    picker.set_selected_key("Digit1")
    with qtbot.assertNotEmitted(picker.paramsEdited):
        picker.params_combo.activated.emit(0)


def test_header_uses_the_key_display_name(qtbot):
    doc = json.loads((paths.BUNDLED_ROOT / "bindings" / "default.json")
                     .read_text(encoding="utf-8"))
    editor = BindingEditor(BindingSet.from_dict(doc), lambda b: None,
                           display=lambda code: {"KeyD": "D"}.get(code, code))
    picker = make_picker(qtbot, editor)
    picker.set_selected_key("KeyD")
    assert picker.header.text().startswith("Assign to D (now: ")


def test_header_names_a_preserved_unknown_action(qtbot, editor):
    editor.bset.bindings.pop("KeyQ", None)
    editor.bset.extra_bindings["KeyQ"] = {"action": "pointer.teleport",
                                          "params": {}}
    picker = make_picker(qtbot, editor)
    picker.set_selected_key("KeyQ")
    assert picker.header.text() == \
        "Assign to KeyQ (now: pointer.teleport (not supported by this version))"

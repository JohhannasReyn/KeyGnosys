"""The binding editor model: targets, picker entries, undoable edits."""

from __future__ import annotations

import json

import pytest

from keygnosys import actions, paths
from keygnosys.binding_editor import GROUPS, NOT_TARGETS, BindingEditor
from keygnosys.documents import Binding, BindingSet


@pytest.fixture
def calls():
    return []


FREE = "KeyQ"  # not bound on the shipped default


@pytest.fixture
def editor(calls):
    doc = json.loads((paths.BUNDLED_ROOT / "bindings" / "default.json")
                     .read_text(encoding="utf-8"))
    return BindingEditor(BindingSet.from_dict(doc), lambda b: calls.append(b))


def test_activation_and_modifier_keys_are_not_targets(editor):
    assert NOT_TARGETS == {"CapsLock", "ShiftLeft", "ShiftRight", "ControlLeft",
                           "ControlRight", "AltLeft", "AltRight", "MetaLeft",
                           "MetaRight"}
    for code in NOT_TARGETS:
        assert not editor.is_target(code)
        with pytest.raises(ValueError):
            editor.assign(code, Binding("overlay.toggle"))
    assert editor.is_target(FREE)


def test_groups_map_every_catalog_prefix():
    assert {a.split(".")[0] for a in actions.CATALOG} <= set(GROUPS)
    assert GROUPS["button"] == "Clicks" and GROUPS["key"] == "Layer & system"


def test_a_parameterised_action_is_one_entry_with_choices(editor):
    e = [x for x in editor.entries() if x.action == "window.slot"]
    assert len(e) == 1 and len(e[0].choices) == 9
    assert e[0].bound_on[0] == ["Digit1"]
    assert e[0].group == "Windows" and e[0].stored is None


def test_passthrough_is_one_entry_with_no_choices(editor):
    e = [x for x in editor.entries() if x.action == "key.passthrough"]
    assert len(e) == 1 and e[0].choices == []


def test_entries_follow_group_order(editor):
    order = list(dict.fromkeys(GROUPS.values()))
    seen = [x.group for x in editor.entries() if x.group != "Unassigned"]
    assert [g for g in order if g in seen] == list(dict.fromkeys(seen))


def test_unassigned_entries_come_first_and_show_where_they_were(editor):
    code = next(iter(editor.bset.bindings))
    cmd = editor.bset.bindings[code]
    editor.clear(code)
    es = editor.entries()
    assert es[0].group == "Unassigned"
    assert es[0].stored is not None and es[0].stored.action == cmd.action
    assert f"was on {code}" in es[0].label
    assert all(x.stored is None for x in es if x.group != "Unassigned")
    assert sum(1 for x in es if x.group == "Unassigned") == len(editor.bset.unassigned)


def test_editing_params_displaces_the_old_identity(editor):
    editor.set_params("Digit1", {"index": 5})
    assert editor.bset.bindings["Digit1"].params == {"index": 5}
    assert editor.bset.bindings["Digit5"].params == {"index": 5}
    assert any(u.params == {"index": 1} for u in editor.bset.unassigned)


def test_set_legend_empty_string_clears(editor):
    editor.set_legend("Digit1", "Hi")
    assert editor.bset.bindings["Digit1"].legend == "Hi"
    editor.set_legend("Digit1", "")
    assert editor.bset.bindings["Digit1"].legend is None


def test_set_setting_rejects_unknown_names(editor, calls):
    with pytest.raises(KeyError):
        editor.set_setting("nope", 1)
    assert calls == [] and not editor.can_undo


def test_undo_restores_the_exact_document_including_settings(editor):
    before = editor.bset.to_dict()
    editor.assign(FREE, Binding("button.click", {"button": "right"}))
    editor.set_legend(FREE, "Menu")
    editor.set_setting("pointer_max_speed", 40)
    while editor.undo():
        pass
    assert editor.bset.to_dict() == before and editor.can_redo
    while editor.redo():
        pass
    assert editor.bset.bindings[FREE].legend == "Menu"
    assert editor.bset.settings["pointer_max_speed"] == 40


def test_each_mutation_notifies_once_and_redo_is_cleared_by_a_new_edit(editor, calls):
    editor.assign(FREE, Binding("overlay.toggle"))
    assert len(calls) == 1 and calls[0] is editor.bset
    editor.set_legend(FREE, "x")
    assert len(calls) == 2
    assert editor.undo() and len(calls) == 3 and calls[-1] is editor.bset
    assert editor.can_redo
    editor.clear(FREE)
    assert not editor.can_redo and len(calls) == 4
    assert editor.redo() is False and len(calls) == 4


def test_undo_with_nothing_to_undo_is_false_and_silent(editor, calls):
    assert editor.undo() is False and editor.redo() is False and calls == []


def test_failed_assign_leaves_history_untouched(editor, calls):
    with pytest.raises(ValueError):
        editor.assign("CapsLock", Binding("overlay.toggle"))
    assert calls == [] and not editor.can_undo


def test_search_filters_on_label_and_action_name(editor):
    by_action = editor.entries("WINDOW.slot")
    assert by_action and all("window.slot" in e.action or "window slot" in e.label.lower()
                             for e in by_action)
    assert any(e.action == "window.slot" for e in by_action)
    label = next(e for e in editor.entries() if e.action == "button.click").label
    assert any(e.action == "button.click" for e in editor.entries(label.upper()))
    assert editor.entries("zzzz-no-such") == []


@pytest.mark.parametrize("case", ["from_unassigned", "parameterised", "clear", "empty_key"])
def test_table_driven_edits(editor, case):
    before = {c: (b.action, b.params, b.legend) for c, b in editor.bset.bindings.items()}
    n_un = len(editor.bset.unassigned)

    def bindings():
        return {c: (b.action, b.params, b.legend) for c, b in editor.bset.bindings.items()}

    if case == "from_unassigned":
        editor.clear("Digit1")
        stored = editor.bset.unassigned[-1]
        assert stored.was_on == "Digit1"
        editor.assign(FREE, Binding(stored.action, dict(stored.params)))
        expected = dict(before)
        del expected["Digit1"]
        expected[FREE] = ("window.slot", {"index": 1}, None)
        assert bindings() == expected
        assert len(editor.bset.unassigned) == n_un
        assert all(u.was_on is None for u in editor.bset.unassigned)
        assert editor.bset.bindings[FREE].was_on is None
    elif case == "parameterised":
        keys_before = editor.bset.keys_for(Binding("warp.grid", {"cell": 3}))
        editor.assign(FREE, Binding("warp.grid", {"cell": 3}))
        assert bindings() == {**before, FREE: ("warp.grid", {"cell": 3}, None)}
        assert editor.bset.keys_for(Binding("warp.grid", {"cell": 3})) == keys_before + [FREE]
        assert len(editor.bset.unassigned) == n_un
        assert editor.bset.bindings[FREE].was_on is None
    elif case == "clear":
        editor.clear("Digit1")
        expected = dict(before)
        del expected["Digit1"]
        assert bindings() == expected
        assert len(editor.bset.unassigned) == n_un + 1
        last = editor.bset.unassigned[-1]
        assert (last.action, last.params, last.was_on) == ("window.slot", {"index": 1}, "Digit1")
    else:
        assert FREE not in before
        editor.assign(FREE, Binding("overlay.toggle"))
        assert bindings() == {**before, FREE: ("overlay.toggle", {}, None)}
        assert len(editor.bset.unassigned) == n_un
        assert editor.bset.bindings[FREE].was_on is None
        assert all(u.was_on is None for u in editor.bset.unassigned[:n_un])


def test_unchanged_legend_or_setting_is_a_silent_no_op(editor, calls):
    editor.set_legend("Digit1", None)
    editor.set_legend("Digit1", "")
    editor.set_setting("pointer_max_speed", editor.bset.settings["pointer_max_speed"])
    assert calls == [] and not editor.can_undo
    editor.set_legend("Digit1", "Hi")
    editor.set_legend("Digit1", "Hi")
    assert len(calls) == 1 and editor.undo() and not editor.can_undo

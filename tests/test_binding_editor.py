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


# -- final review fixes -----------------------------------------------------

def test_undo_restores_content_but_keeps_the_live_metadata(editor):
    editor.assign(FREE, Binding("overlay.toggle"))
    # A save between edits forks and bumps the revision on the live object.
    editor.bset.metadata.source_template = "default"
    editor.bset.metadata.revision = 7
    assert editor.undo()
    assert FREE not in editor.bset.bindings
    assert editor.bset.metadata.source_template == "default"
    assert editor.bset.metadata.revision == 7
    editor.bset.metadata.revision = 8
    assert editor.redo()
    assert FREE in editor.bset.bindings
    assert editor.bset.metadata.revision == 8


def test_unassigned_labels_use_the_display_name(calls):
    doc = json.loads((paths.BUNDLED_ROOT / "bindings" / "default.json")
                     .read_text(encoding="utf-8"))
    editor = BindingEditor(BindingSet.from_dict(doc), lambda b: calls.append(b),
                           display=lambda code: {"Digit1": "1"}.get(code, code))
    editor.clear("Digit1")
    labels = [e.label for e in editor.entries() if e.stored is not None]
    assert any(label.endswith("(was on 1)") for label in labels), labels
    assert editor.display("KeyQ") == "KeyQ"


def test_without_a_display_name_the_code_is_shown(editor):
    editor.clear("Digit1")
    labels = [e.label for e in editor.entries() if e.stored is not None]
    assert any(label.endswith("(was on Digit1)") for label in labels), labels


def test_set_params_keeps_the_legend(editor):
    editor.set_legend("Digit1", "Mail")
    editor.set_params("Digit1", {"index": 4})
    bound = editor.bset.bindings["Digit1"]
    assert bound.params == {"index": 4} and bound.legend == "Mail"


# -- replace_all -------------------------------------------------------------

def _imported():
    return BindingSet.from_dict({
        "schema": "keygnosys/bindings/2", "id": "other", "name": "Theirs",
        "metadata": {"author": "Sam", "revision": 9, "y": 2},
        "bindings": {FREE: {"action": "overlay.toggle", "params": {}}},
    })


def test_replace_all_sets_import_metadata(editor, calls):
    editor.bset.metadata.revision = 4
    editor.bset.metadata.source_template = "default"
    new = _imported()
    before = new.to_dict()
    info = {"id": "other", "name": "Theirs", "author": "Sam", "exported": "t"}
    editor.replace_all(new, info)
    meta = editor.bset.metadata
    assert editor.bset.id == "default"
    assert meta.revision == 4
    assert meta.source_template == "default"
    assert meta.author == "Sam"
    assert meta.extra["y"] == 2
    assert meta.extra["imported_from"] == info
    assert FREE in editor.bset.bindings
    assert len(calls) == 1
    assert new.to_dict() == before


def test_undo_of_import_restores_metadata_but_not_revision(editor):
    editor.bset.metadata.revision = 4
    editor.bset.metadata.source_template = "default"
    original = editor.bset.to_dict()
    editor.replace_all(_imported(), {"id": "other"})
    editor.bset.metadata.revision = 5
    assert editor.undo()
    expected = dict(original, metadata=dict(original["metadata"], revision=5))
    assert editor.bset.to_dict() == expected
    assert "imported_from" not in editor.bset.metadata.extra
    editor.bset.metadata.revision = 6
    assert editor.redo()
    assert editor.bset.metadata.extra["imported_from"] == {"id": "other"}
    assert editor.bset.metadata.author == "Sam"
    assert editor.bset.metadata.revision == 6


def test_ordinary_undo_still_keeps_live_metadata(editor):
    editor.assign(FREE, Binding("overlay.toggle"))
    editor.bset.metadata.author = "live"
    editor.bset.metadata.revision = 9
    assert editor.undo()
    assert editor.bset.metadata.author == "live"
    assert editor.bset.metadata.revision == 9


def test_history_is_not_fooled_by_document_fields(editor):
    # Top-level fields a file may carry, named like the history's own marker.
    original = editor.bset.to_dict()
    doc = _imported().to_dict()
    doc["with_metadata"] = True
    doc["doc"] = {"schema": "keygnosys/bindings/2", "id": "decoy",
                  "name": "Decoy", "bindings": {}}
    editor.replace_all(BindingSet.from_dict(doc), {"id": "other"})
    imported = editor.bset.to_dict()
    assert imported["with_metadata"] is True and "doc" in imported
    editor.assign("KeyW", Binding("overlay.toggle"))
    assert editor.undo()
    assert editor.bset.to_dict() == imported
    assert editor.undo()
    assert editor.bset.to_dict() == original
    assert not editor.can_undo
    assert editor.redo() and editor.bset.to_dict() == imported

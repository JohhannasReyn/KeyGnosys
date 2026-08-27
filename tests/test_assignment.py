"""Binding reassignment: silent, and never destructive.

Assigning a command to an occupied key displaces the old one without a prompt.
The displaced command is not lost -- it moves to the unassigned list, keeping
its parameters and its custom label.

The asymmetry that makes this well-defined: a key holds one command, but a
command may be bound to several keys. See docs/SPEC.md section 10.2.
"""

from __future__ import annotations

import pytest

from mousetrapkeys.actions import catalog_commands
from mousetrapkeys.documents import Binding, BindingSet, DiagnosticSink, Registry


def make_set(**bindings: Binding) -> BindingSet:
    return BindingSet.from_dict({
        "schema": "mousetrapkeys/bindings/2",
        "id": "t",
        "bindings": {code: {"action": b.action, "params": b.params,
                            **({"legend": b.legend} if b.legend else {})}
                     for code, b in bindings.items()},
    })


LEFT = Binding("pointer.move", {"dir": "left"})
RIGHT = Binding("pointer.move", {"dir": "right"})
CLICK = Binding("button.click", {"button": "left"})


# -- identity ---------------------------------------------------------------

def test_parameters_are_part_of_a_commands_identity() -> None:
    """`window.slot index=1` and `index=2` are different commands."""
    assert (Binding("window.slot", {"index": 1}).identity()
            != Binding("window.slot", {"index": 2}).identity())


def test_labels_are_not_part_of_identity() -> None:
    """Relabelling a command does not make it a different command."""
    plain = Binding("window.move_to_monitor", {"target": "next"})
    labelled = Binding("window.move_to_monitor", {"target": "next"},
                       legend="Send right")
    assert plain.identity() == labelled.identity()


def test_parameter_order_does_not_affect_identity() -> None:
    a = Binding("warp.grid", {"cell": 1})
    b = Binding("warp.grid", dict(reversed(list({"cell": 1}.items()))))
    assert a.identity() == b.identity()


# -- reassignment -----------------------------------------------------------

def test_assigning_over_an_occupied_key_displaces_the_old_command() -> None:
    bindings = make_set(KeyH=LEFT)
    displaced = bindings.assign("KeyH", CLICK)
    assert displaced.identity() == LEFT.identity()
    assert bindings.bindings["KeyH"].identity() == CLICK.identity()


def test_the_displaced_command_lands_in_the_unassigned_list() -> None:
    bindings = make_set(KeyH=LEFT)
    bindings.assign("KeyH", CLICK)
    assert [b.identity() for b in bindings.unassigned] == [LEFT.identity()]


def test_assigning_to_an_empty_key_displaces_nothing() -> None:
    bindings = make_set()
    assert bindings.assign("KeyH", LEFT) is None
    assert bindings.unassigned == []


def test_a_command_on_two_keys_survives_losing_one() -> None:
    """Only the key side is exclusive, so this is not a displacement at all.

    The shipped defaults bind left-click to both Space and KeyD, so getting
    this wrong would silently unassign a working binding.
    """
    bindings = make_set(KeyD=CLICK, Space=CLICK)
    bindings.assign("KeyD", LEFT)
    assert bindings.keys_for(CLICK) == ["Space"]
    assert bindings.unassigned == [], "still bound elsewhere; not unassigned"


def test_a_command_reaches_the_list_only_when_its_last_key_goes() -> None:
    bindings = make_set(KeyD=CLICK, Space=CLICK)
    bindings.assign("KeyD", LEFT)
    bindings.assign("Space", RIGHT)
    assert [b.identity() for b in bindings.unassigned] == [CLICK.identity()]


def test_assigning_from_the_unassigned_list_removes_it_from_there() -> None:
    bindings = make_set(KeyH=LEFT)
    bindings.assign("KeyH", CLICK)
    assert len(bindings.unassigned) == 1
    bindings.assign("KeyJ", LEFT)
    assert bindings.unassigned == []
    assert bindings.bindings["KeyJ"].identity() == LEFT.identity()


def test_displaced_commands_keep_their_params_and_custom_legend() -> None:
    """A user who wrote a label should not have to retype it after a move."""
    custom = Binding("window.move_to_monitor", {"target": "prev"},
                     legend="Send to left screen")
    bindings = make_set(KeyB=custom)
    bindings.assign("KeyB", CLICK)
    stored = bindings.unassigned[0]
    assert stored.legend == "Send to left screen"
    assert stored.params == {"target": "prev"}


def test_unassigning_a_key_moves_its_command_to_the_list() -> None:
    bindings = make_set(KeyH=LEFT)
    removed = bindings.unassign("KeyH")
    assert removed.identity() == LEFT.identity()
    assert "KeyH" not in bindings.bindings
    assert [b.identity() for b in bindings.unassigned] == [LEFT.identity()]


def test_the_unassigned_list_never_holds_duplicates() -> None:
    bindings = make_set(KeyH=LEFT, KeyJ=RIGHT)
    bindings.assign("KeyH", CLICK)
    bindings.assign("KeyJ", LEFT)     # LEFT leaves the list...
    bindings.assign("KeyJ", RIGHT)    # ...and comes straight back
    identities = [b.identity() for b in bindings.unassigned]
    assert len(identities) == len(set(identities))


def test_reassignment_is_reversible() -> None:
    """Nothing is destroyed, which is why no confirmation is warranted.

    Putting the displaced command back restores the key exactly. The command
    that had taken its place is then the homeless one, which is correct: it is
    now bound nowhere.
    """
    bindings = make_set(KeyH=LEFT)
    bindings.assign("KeyH", CLICK)
    bindings.assign("KeyH", bindings.unassigned[0])
    assert bindings.bindings["KeyH"].identity() == LEFT.identity()
    assert [b.identity() for b in bindings.unassigned] == [CLICK.identity()]


# -- the available half of the list -----------------------------------------

def test_available_commands_exclude_bound_ones(registry_default) -> None:
    available = {b.identity() for b in registry_default.available_commands()}
    bound = {b.identity() for b in registry_default.bindings.values()}
    assert not (available & bound)


def test_available_commands_exclude_the_stored_unassigned_ones() -> None:
    bindings = make_set(KeyH=LEFT)
    bindings.assign("KeyH", CLICK)
    available = {b.identity() for b in bindings.available_commands()}
    assert LEFT.identity() not in available


def test_available_commands_are_derived_not_stored() -> None:
    """This is what keeps upgrades additive (SPEC 3.4.5).

    A command added to the catalog in a later release must appear for a user
    whose document predates it, without them re-forking anything.
    """
    bindings = make_set()
    assert len(bindings.available_commands()) == len(catalog_commands())


def test_every_catalog_command_is_valid() -> None:
    from mousetrapkeys.actions import validate_binding
    for action, params in catalog_commands():
        assert validate_binding(action, params) is None, (action, params)


# -- document round-trip ----------------------------------------------------

def test_unassigned_entries_load_from_the_document() -> None:
    doc = {
        "schema": "mousetrapkeys/bindings/2", "id": "t",
        "bindings": {"KeyH": {"action": "pointer.move",
                              "params": {"dir": "left"}}},
        "unassigned": [{"action": "button.click", "params": {"button": "right"},
                        "legend": "Context"}],
    }
    bindings = BindingSet.from_dict(doc)
    assert len(bindings.unassigned) == 1
    assert bindings.unassigned[0].legend == "Context"


def test_a_command_both_bound_and_unassigned_keeps_the_binding() -> None:
    """Only reachable by hand-editing, but it must resolve one way."""
    sink = DiagnosticSink()
    doc = {
        "schema": "mousetrapkeys/bindings/2", "id": "t",
        "bindings": {"KeyH": {"action": "pointer.move",
                              "params": {"dir": "left"}}},
        "unassigned": [{"action": "pointer.move", "params": {"dir": "left"}}],
    }
    bindings = BindingSet.from_dict(doc, sink)
    assert bindings.unassigned == []
    assert "KeyH" in bindings.bindings
    assert [d for d in sink if d.code == "binding.unassigned_conflict"]


def test_invalid_unassigned_entries_are_dropped_not_fatal() -> None:
    sink = DiagnosticSink()
    doc = {
        "schema": "mousetrapkeys/bindings/2", "id": "t",
        "bindings": {"KeyH": {"action": "pointer.move",
                              "params": {"dir": "left"}}},
        "unassigned": [{"action": "pointer.teleport", "params": {}},
                       {"action": "button.click", "params": {"button": "right"}}],
    }
    bindings = BindingSet.from_dict(doc, sink)
    assert len(bindings.unassigned) == 1
    assert "KeyH" in bindings.bindings


def test_schema_1_bindings_get_an_empty_unassigned_list() -> None:
    bindings = BindingSet.from_dict({
        "schema": "mousetrapkeys/bindings/1", "id": "t",
        "bindings": {"KeyH": {"action": "pointer.move",
                              "params": {"dir": "left"}}},
    })
    assert bindings.schema_major == 1
    assert bindings.unassigned == []


@pytest.fixture(scope="module")
def registry_default() -> BindingSet:
    return Registry().load_all().bindings["default"]

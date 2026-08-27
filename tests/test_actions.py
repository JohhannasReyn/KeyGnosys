"""The action catalog is the contract between bindings, core and overlay."""

from __future__ import annotations

import pytest

from mousetrapkeys.actions import (
    CATALOG, action_names, default_legend, is_held, validate_binding,
)
from mousetrapkeys.documents import Registry


def test_unknown_action_is_rejected() -> None:
    assert "unknown action" in validate_binding("pointer.teleport", {})


def test_every_action_rejects_bad_parameters() -> None:
    bad = {
        "pointer.move": {"dir": "sideways"},
        "button.click": {"button": "thumb"},
        "button.double_click": {"button": None},
        "button.drag_lock": {},
        "scroll.scroll": {"dir": "in"},
        "scroll.page": {"dir": "left"},
        "warp.grid": {"cell": 0},
        "warp.corner": {"corner": "middle"},
        "warp.monitor": {"target": "somewhere"},
        "window.cycle": {"dir": "up"},
        "window.slot": {"index": 10},
        "window.focus_monitor": {"target": -1},
        "window.move_to_monitor": {"target": 1.5},
        "key.passthrough": {"code": ""},
    }
    for action, params in bad.items():
        assert validate_binding(action, params) is not None, action


def test_grid_cells_accept_the_full_range() -> None:
    for cell in range(1, 10):
        assert validate_binding("warp.grid", {"cell": cell}) is None
    assert validate_binding("warp.grid", {"cell": 10}) is not None


def test_booleans_are_not_accepted_as_integers() -> None:
    """`True == 1` in Python, and a bool in a config file is a mistake."""
    assert validate_binding("warp.grid", {"cell": True}) is not None


def test_every_action_produces_a_legend() -> None:
    samples = {
        "dir": "up", "button": "left", "cell": 1, "corner": "tl",
        "target": "next", "index": 1, "code": "KeyA",
    }
    for name in action_names():
        legend = default_legend(name, samples)
        assert legend and legend != "?", name


def test_held_actions_are_the_ones_that_repeat() -> None:
    assert is_held("pointer.move")
    assert is_held("scroll.scroll")
    assert is_held("pointer.precision")
    assert not is_held("button.click")
    assert not is_held("warp.grid")
    assert not is_held("scroll.page")


def test_default_bindings_only_use_catalogued_actions() -> None:
    registry = Registry().load_all()
    for code, binding in registry.bindings["default"].bindings.items():
        assert binding.action in CATALOG, f"{code} -> {binding.action}"


def test_bindings_file_lost_nothing_to_validation() -> None:
    """The shipped default set must load in full, not partially.

    A silently-dropped binding here would leave a dead key on the map, which is
    exactly the failure mode the diagnostics exist to make visible.
    """
    import json
    from mousetrapkeys import paths

    raw = json.loads(
        (paths.BUNDLED_ROOT / "bindings" / "default.json").read_text("utf-8"))
    registry = Registry().load_all()
    assert len(registry.bindings["default"].bindings) == len(raw["bindings"])

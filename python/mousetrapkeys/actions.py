"""The action catalog.

This is the single authority on which actions exist, what parameters they take,
and what the overlay writes on a key when a binding does not supply its own
legend. Both the binding loader and the legend renderer consult it, so a new
action is added in exactly one place.

Mirrors docs/SPEC.md section 7. The native core implements the same catalog;
keeping the two in step is checked by tests/test_actions.py.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any, Callable

ARROWS = {"up": "▲", "down": "▼", "left": "◀", "right": "▶"}
DIRECTIONS = ("up", "down", "left", "right")
BUTTONS = ("left", "right", "middle")
CORNERS = {"tl": "↖", "tr": "↗", "bl": "↙", "br": "↘",
           "center": "⊙"}


@dataclass(frozen=True)
class ActionSpec:
    name: str
    #: Returns None if params are valid, else a human-readable problem.
    validate: Callable[[dict[str, Any]], str | None]
    #: Default legend text for a binding that does not override it.
    legend: Callable[[dict[str, Any]], str]
    #: True for actions that do something for as long as the key is held.
    held: bool = False
    summary: str = ""


# --------------------------------------------------------------------------
# Parameter validators
# --------------------------------------------------------------------------

def _one_of(key: str, allowed) -> Callable[[dict], str | None]:
    def check(params: dict) -> str | None:
        value = params.get(key)
        if value not in allowed:
            return (f"parameter {key!r} must be one of "
                    f"{', '.join(map(str, allowed))}; found {value!r}")
        return None
    return check


def _target(params: dict) -> str | None:
    """A monitor target: 'next', 'prev', or a 0-based monitor index."""
    value = params.get("target")
    if value in ("next", "prev"):
        return None
    if isinstance(value, int) and value >= 0:
        return None
    return f"parameter 'target' must be 'next', 'prev' or an index; found {value!r}"


def _int_range(key: str, lo: int, hi: int) -> Callable[[dict], str | None]:
    def check(params: dict) -> str | None:
        value = params.get(key)
        if not isinstance(value, int) or isinstance(value, bool):
            return f"parameter {key!r} must be an integer; found {value!r}"
        if not lo <= value <= hi:
            return f"parameter {key!r} must be {lo}-{hi}; found {value}"
        return None
    return check


def _no_params(params: dict) -> str | None:
    return None


def _target_legend(prefix: str) -> Callable[[dict], str]:
    def legend(params: dict) -> str:
        target = params.get("target")
        if target == "next":
            return f"{prefix} ▶"
        if target == "prev":
            return f"{prefix} ◀"
        return f"{prefix} {target}"
    return legend


# --------------------------------------------------------------------------
# The catalog
# --------------------------------------------------------------------------

CATALOG: dict[str, ActionSpec] = {}


def _register(spec: ActionSpec) -> None:
    CATALOG[spec.name] = spec


_register(ActionSpec(
    "pointer.move", _one_of("dir", DIRECTIONS),
    lambda p: ARROWS.get(p.get("dir", ""), "?"), held=True,
    summary="Move the pointer while held. Two directions held at once produce "
            "a diagonal, so no diagonal binding is needed."))

_register(ActionSpec(
    "pointer.precision", _no_params, lambda p: "Slow", held=True,
    summary="While held, scale pointer and scroll speed down for fine work."))

_register(ActionSpec(
    "button.click", _one_of("button", BUTTONS),
    lambda p: {"left": "Click", "right": "R-Click",
               "middle": "M-Click"}[p["button"]],
    summary="Press on key-down and release on key-up, so click-and-hold works."))

_register(ActionSpec(
    "button.double_click", _one_of("button", BUTTONS),
    lambda p: "Dbl Click" if p["button"] == "left" else f"Dbl {p['button'][0].upper()}",
    summary="Two press/release pairs within the OS double-click interval."))

_register(ActionSpec(
    "button.drag_lock", _one_of("button", BUTTONS),
    lambda p: "Drag" if p["button"] == "left" else f"Drag {p['button'][0].upper()}",
    summary="Toggle: hold the button down, move freely, press again to drop."))

_register(ActionSpec(
    "scroll.scroll", _one_of("dir", DIRECTIONS),
    lambda p: f"Scroll {ARROWS.get(p.get('dir', ''), '?')}", held=True,
    summary="Scroll while held, with its own acceleration ramp."))

_register(ActionSpec(
    "scroll.page", _one_of("dir", ("up", "down")),
    lambda p: f"Page {ARROWS.get(p.get('dir', ''), '?')}",
    summary="One large discrete scroll per press."))

_register(ActionSpec(
    "warp.grid", _int_range("cell", 1, 9),
    lambda p: f"⌗{p['cell']}",
    summary="Jump to that cell of a 3x3 grid over the current monitor."))

_register(ActionSpec(
    "warp.corner", _one_of("corner", tuple(CORNERS)),
    lambda p: CORNERS[p["corner"]],
    summary="Jump to a corner or the centre of the current monitor."))

_register(ActionSpec(
    "warp.monitor", _target, _target_legend("Mon"),
    summary="Move the pointer to the centre of another monitor."))

_register(ActionSpec(
    "window.cycle", _one_of("dir", ("next", "prev")),
    lambda p: "App ▶" if p["dir"] == "next" else "App ◀",
    summary="Focus the next or previous window in stable slot order."))

_register(ActionSpec(
    "window.slot", _int_range("index", 1, 9),
    lambda p: f"App {p['index']}",
    summary="Focus the window occupying that slot. The overlay replaces this "
            "legend with the slot's actual application name when known."))

_register(ActionSpec(
    "window.focus_monitor", _target, _target_legend("Screen"),
    summary="Focus the topmost window on another monitor and warp there."))

_register(ActionSpec(
    "window.move_to_monitor", _target, _target_legend("Send"),
    summary="Move the focused window to another monitor, preserving its "
            "relative position and maximised state."))

_register(ActionSpec(
    "layer.release", _no_params, lambda p: "Exit",
    summary="Leave the cursor layer and release everything held."))

_register(ActionSpec(
    "overlay.toggle", _no_params, lambda p: "Map",
    summary="Show or hide the on-screen keyboard."))

_register(ActionSpec(
    "system.reload", _no_params, lambda p: "Reload",
    summary="Reload layouts, bindings, themes and profiles from disk."))

_register(ActionSpec(
    "key.passthrough",
    lambda p: None if isinstance(p.get("code"), str) and p["code"]
    else "parameter 'code' must be a key code string",
    lambda p: p.get("code", "Key"),
    summary="Send a literal key even while the cursor layer is engaged."))


# --------------------------------------------------------------------------
# Public helpers
# --------------------------------------------------------------------------

def validate_binding(action: str, params: dict[str, Any]) -> str | None:
    """Return None if the binding is usable, else why it is not."""
    spec = CATALOG.get(action)
    if spec is None:
        return f"unknown action {action!r}"
    return spec.validate(params or {})


def default_legend(action: str, params: dict[str, Any]) -> str:
    """The text to write on a key when the binding supplies no legend."""
    spec = CATALOG.get(action)
    if spec is None:
        return "?"
    try:
        return spec.legend(params or {})
    except (KeyError, TypeError):
        return "?"


def is_held(action: str) -> bool:
    spec = CATALOG.get(action)
    return bool(spec and spec.held)


def action_names() -> list[str]:
    return sorted(CATALOG)

"""The binding editor's model: no Qt, so it can be tested without a display.

Holds a live `BindingSet`, the picker's grouped entries, and an undo/redo
history of whole-document snapshots. Every edit goes through
`BindingSet.assign` / `unassign`, which own the silent-reassignment rule
(SPEC 10.2.2); nothing here reimplements it.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Callable

from .actions import CATALOG, COMMAND_PARAMS, default_legend
from .documents import Binding, BindingSet

#: The activation key and the eight modifiers. Modifiers are always forwarded
#: in the layer (SPEC 6), so a binding on one would draw a legend the layer
#: never honours.
NOT_TARGETS = frozenset({
    "CapsLock",
    "ShiftLeft", "ShiftRight", "ControlLeft", "ControlRight",
    "AltLeft", "AltRight", "MetaLeft", "MetaRight",
})

#: Action prefix -> picker group label. Dict order is the display order.
GROUPS: dict[str, str] = {
    "button": "Clicks",
    "pointer": "Pointer",
    "scroll": "Scroll",
    "warp": "Warp",
    "window": "Windows",
    "layer": "Layer & system",
    "overlay": "Layer & system",
    "system": "Layer & system",
    "key": "Layer & system",
}

UNASSIGNED = "Unassigned"


@dataclass
class PickerEntry:
    group: str
    label: str
    action: str
    #: Parameter sets to choose between; empty for a parameterless action, and
    #: for `key.passthrough`, whose params (`{"code": <key>}`) the caller fills.
    choices: list[dict] = field(default_factory=list)
    #: Choice index -> keys currently holding that choice.
    bound_on: dict[int, list[str]] = field(default_factory=dict)
    #: For the "Unassigned" group: the stored command.
    stored: Binding | None = None


def _label(action: str) -> str:
    prefix, _, name = action.partition(".")
    return f"{prefix} {name.replace('_', ' ')}".capitalize()


class BindingEditor:
    def __init__(self, bset: BindingSet,
                 on_change: Callable[[BindingSet], None],
                 display: Callable[[str], str] | None = None) -> None:
        self.bset = bset
        self._on_change = on_change
        #: Key code -> the name the user sees on the keyboard ("KeyD" -> "D").
        self.display: Callable[[str], str] = display or (lambda code: code)
        self._undo: list[dict] = []
        self._redo: list[dict] = []

    # -- queries ------------------------------------------------------------

    def is_target(self, code: str) -> bool:
        return code not in NOT_TARGETS

    @property
    def can_undo(self) -> bool:
        return bool(self._undo)

    @property
    def can_redo(self) -> bool:
        return bool(self._redo)

    def entries(self, query: str = "") -> list[PickerEntry]:
        out: list[PickerEntry] = []
        for stored in self.bset.unassigned:
            label = stored.legend or default_legend(stored.action, stored.params)
            if stored.was_on:
                label = f"{label} (was on {self.display(stored.was_on)})"
            out.append(PickerEntry(UNASSIGNED, label, stored.action,
                                   [dict(stored.params)], {}, stored))

        rank = list(GROUPS)
        actions = sorted(CATALOG, key=lambda a: (rank.index(a.split(".")[0])
                         if a.split(".")[0] in GROUPS else len(rank), a))
        for action in actions:
            group = GROUPS.get(action.split(".")[0], "Layer & system")
            choices = [dict(p) for p in COMMAND_PARAMS.get(action, [])]
            bound_on: dict[int, list[str]] = {}
            for i, params in enumerate(choices):
                keys = self.bset.keys_for(Binding(action, params))
                if keys:
                    bound_on[i] = keys
            out.append(PickerEntry(group, _label(action), action,
                                   choices, bound_on))

        q = query.strip().lower()
        if q:
            out = [e for e in out
                   if q in e.label.lower() or q in e.action.lower()]
        return out

    # -- edits --------------------------------------------------------------

    def assign(self, code: str, binding: Binding) -> None:
        if not self.is_target(code):
            raise ValueError(f"{code} cannot be assigned")
        self._edit(lambda: self.bset.assign(code, binding))

    def clear(self, code: str) -> None:
        self._edit(lambda: self.bset.unassign(code))

    def set_legend(self, code: str, legend: str | None) -> None:
        current = self.bset.bindings[code]
        legend = legend or None
        if current.legend == legend:
            return
        self._edit(lambda: setattr(current, "legend", legend))

    def set_params(self, code: str, params: dict) -> None:
        current = self.bset.bindings[code]
        self.assign(code, Binding(current.action, dict(params), current.legend,
                                  extra=dict(current.extra)))

    def set_setting(self, name: str, value: float | int) -> None:
        if name not in self.bset.settings:
            raise KeyError(name)
        if self.bset.settings[name] == value:
            return
        self._edit(lambda: self.bset.settings.__setitem__(name, value))

    # -- history ------------------------------------------------------------

    def undo(self) -> bool:
        return self._travel(self._undo, self._redo)

    def redo(self) -> bool:
        return self._travel(self._redo, self._undo)

    def _travel(self, source: list[dict], sink: list[dict]) -> bool:
        if not source:
            return False
        sink.append(self.bset.to_dict())
        # Content travels; provenance does not. A save between edits forked
        # and bumped the revision, and undoing an edit must not rewind that.
        metadata = self.bset.metadata
        self.bset = BindingSet.from_dict(source.pop())
        self.bset.metadata = metadata
        self._on_change(self.bset)
        return True

    def _edit(self, mutate: Callable[[], object]) -> None:
        snapshot = self.bset.to_dict()
        mutate()
        self._undo.append(snapshot)
        self._redo.clear()
        self._on_change(self.bset)

"""Persistent user settings.

Deep-merged over defaults so a settings file written by an older build keeps
working, and unknown keys survive a round-trip so a file written by a *newer*
build is not silently stripped. Out-of-range values are clamped rather than
rejected, and the clamp is reported (SPEC section 4.5).
"""

from __future__ import annotations

import copy
import json
from typing import Any

from . import paths

DEFAULTS: dict[str, Any] = {
    "schema": "keygnosys/settings/1",
    "appearance": {
        "theme": "system",
        "accent": None,
        "opacity": 0.85,
        "scale": 1.0,
        "show_numpad": True,
        "feedback_fade_ms": 220,
        "overlay_visible": True,
        "position": {"x": None, "y": None},
    },
    "behavior": {
        "activation_mode": "hybrid",
        "hybrid_tap_ms": 200,
        "grace_ms": 50,
        "real_capslock_gesture": "Shift+CapsLock",
        "layout": "us-ansi-104",
        "bindings": "default",
        "start_minimized": False,
    },
    "window": {
        "pinned": True,
        "click_through": True,
    },
}

#: (dotted path, low, high) for every numeric setting with a sane range.
RANGES = [
    ("appearance.opacity", 0.15, 1.0),
    ("appearance.scale", 0.4, 2.0),
    ("appearance.feedback_fade_ms", 0, 2000),
    ("behavior.hybrid_tap_ms", 50, 1000),
    ("behavior.grace_ms", 0, 400),
]

CHOICES = {
    "behavior.activation_mode": ("toggle", "hold", "hybrid"),
    "behavior.real_capslock_gesture": ("Shift+CapsLock", "double-tap", "none"),
}


def _get(doc: dict, path: str) -> Any:
    node: Any = doc
    for part in path.split("."):
        if not isinstance(node, dict) or part not in node:
            return None
        node = node[part]
    return node


def _set(doc: dict, path: str, value: Any) -> None:
    parts = path.split(".")
    node = doc
    for part in parts[:-1]:
        node = node.setdefault(part, {})
    node[parts[-1]] = value


def _deep_merge(base: dict, over: dict) -> dict:
    """Merge `over` onto a copy of `base`, recursing into nested dicts."""
    out = copy.deepcopy(base)
    for key, value in over.items():
        if isinstance(value, dict) and isinstance(out.get(key), dict):
            out[key] = _deep_merge(out[key], value)
        else:
            out[key] = value
    return out


class Settings:
    """User settings with dotted-path access."""

    def __init__(self, data: dict[str, Any] | None = None) -> None:
        self.data = _deep_merge(DEFAULTS, data or {})
        self.clamped: list[str] = []
        self._normalise()

    # -- persistence ------------------------------------------------------

    @classmethod
    def load(cls) -> "Settings":
        path = paths.settings_file()
        try:
            raw = json.loads(path.read_text(encoding="utf-8"))
        except FileNotFoundError:
            return cls()
        except (OSError, json.JSONDecodeError):
            # A corrupt settings file must not stop the app from starting;
            # defaults are always a usable state.
            return cls()
        if not isinstance(raw, dict):
            return cls()
        return cls(raw)

    def save(self) -> None:
        paths.ensure_user_dirs()
        path = paths.settings_file()
        tmp = path.with_suffix(".json.tmp")
        tmp.write_text(json.dumps(self.data, indent=2) + "\n", encoding="utf-8")
        tmp.replace(path)          # atomic, so a crash mid-write cannot corrupt

    # -- access -----------------------------------------------------------

    def get(self, path: str, default: Any = None) -> Any:
        value = _get(self.data, path)
        return default if value is None else value

    def set(self, path: str, value: Any) -> None:
        _set(self.data, path, value)
        self._normalise()

    # -- validation -------------------------------------------------------

    def _normalise(self) -> None:
        self.clamped = []
        for path, lo, hi in RANGES:
            value = _get(self.data, path)
            if not isinstance(value, (int, float)) or isinstance(value, bool):
                _set(self.data, path, _get(DEFAULTS, path))
                self.clamped.append(path)
                continue
            clamped = min(max(value, lo), hi)
            if clamped != value:
                _set(self.data, path, clamped)
                self.clamped.append(path)

        for path, allowed in CHOICES.items():
            if _get(self.data, path) not in allowed:
                _set(self.data, path, _get(DEFAULTS, path))
                self.clamped.append(path)

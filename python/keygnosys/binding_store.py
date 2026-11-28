"""Loading and saving binding sets in the user config directory.

A bundled set is read-only. The first save of an edited bundled set forks it
into the user directory under the same id, where it shadows the original
(SPEC 3.1). A user file that exists but cannot be read is never overwritten:
the only way past it is `reset_to_shipped`, which moves it aside.
"""

from __future__ import annotations

import json
import os
import time
from pathlib import Path

from . import actions, paths
from .documents import BindingSet, DocumentError


class StoreError(Exception):
    """The store could not complete the request."""


class BrokenUserCopy(StoreError):
    """The user's copy exists but does not parse or validate."""

    def __init__(self, path: Path, reason: str) -> None:
        super().__init__(f"{path}: {reason}")
        self.path = path
        self.reason = reason


def user_path(bindings_id: str) -> Path:
    return paths.user_root() / "bindings" / f"{bindings_id}.json"


def shipped_path(bindings_id: str) -> Path:
    return paths.BUNDLED_ROOT / "bindings" / f"{bindings_id}.json"


def _read(path: Path) -> BindingSet:
    """Parse one file; any failure is raised as ValueError with a reason."""
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:   # JSONDecodeError, bad UTF-8
        raise ValueError(str(exc)) from exc
    if not isinstance(raw, dict):
        raise ValueError("document is not a JSON object")
    try:
        bset = BindingSet.from_dict(raw)
    except DocumentError as exc:
        raise ValueError(str(exc)) from exc
    problem = _first_problem(bset)
    if problem:
        raise ValueError(problem)
    return bset


def _first_problem(bset: BindingSet) -> str | None:
    # Only modelled entries are checked; ones this build cannot model are
    # preserved verbatim in the extra_* fields.
    for code, b in bset.bindings.items():
        why = actions.validate_binding(b.action, b.params)
        if why:
            return f"{code}: {why}"
    for b in bset.unassigned:
        why = actions.validate_binding(b.action, b.params)
        if why:
            return f"unassigned {b.action}: {why}"
    return None


def load(bindings_id: str) -> BindingSet:
    user = user_path(bindings_id)
    if user.exists():
        try:
            return _read(user)
        except ValueError as exc:
            raise BrokenUserCopy(user, str(exc)) from exc
    shipped = shipped_path(bindings_id)
    if shipped.exists():
        try:
            return _read(shipped)
        except ValueError as exc:
            raise StoreError(
                f"shipped bindings {bindings_id!r} unreadable: {exc}") from exc
    raise StoreError(f"no bindings named {bindings_id!r}")


def shadows_shipped(bindings_id: str) -> bool:
    return user_path(bindings_id).exists() and shipped_path(bindings_id).exists()


def save(bset: BindingSet) -> Path:
    problem = _first_problem(bset)
    if problem:
        raise StoreError(f"refusing to save an invalid binding: {problem}")

    path = user_path(bset.id)
    forking = False
    if path.exists():
        try:
            _read(path)
        except ValueError as exc:
            raise BrokenUserCopy(path, str(exc)) from exc
    else:
        forking = shipped_path(bset.id).exists()

    old_source = bset.metadata.source_template
    old_revision = bset.metadata.revision
    if forking:
        bset.metadata.source_template = bset.id
    bset.metadata.revision = old_revision + 1
    try:
        _write_atomic(path, json.dumps(bset.to_dict(), indent=2) + "\n")
    except BaseException:
        bset.metadata.source_template = old_source
        bset.metadata.revision = old_revision
        raise
    return path


def _write_atomic(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(".json.tmp")
    try:
        tmp.write_text(text, encoding="utf-8")
        os.replace(tmp, path)       # atomic, so a crash cannot leave a partial file
    except BaseException:
        try:
            tmp.unlink()
        except OSError:
            pass
        raise


def reset_to_shipped(bindings_id: str) -> BindingSet:
    shipped = shipped_path(bindings_id)
    if not shipped.exists():
        raise StoreError(f"no shipped bindings named {bindings_id!r}")
    user = user_path(bindings_id)
    if user.exists():
        try:
            _read(user)
        except ValueError:
            user.replace(user.with_name(
                f"{bindings_id}.broken-{int(time.time())}.json"))
        else:
            user.unlink()
    return load(bindings_id)

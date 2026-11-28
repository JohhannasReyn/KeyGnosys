"""Export and import files: `keygnosys/bundle/1`.

A bundle is a small header around whole documents embedded verbatim. This
module is pure Python (no Qt): it builds a bundle from a map, writes it
atomically, and reads a file back into an `ImportPlan` -- or refuses it with
reasons. Nothing here touches the user's own copy; applying a plan is the
caller's business (design 2026-11-27-export-import, sections 5 and 6).
"""

from __future__ import annotations

import json
import os
from collections.abc import Callable, Sequence
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path

import keygnosys

from .actions import CATALOG, KNOWN_SETTINGS, default_legend
from .documents import BindingSet, DocumentError

BUNDLE_SCHEMA = "keygnosys/bundle/1"
EXTENSION = ".keygnosys.json"
MAX_BYTES = 5 * 1024 * 1024

#: Highest major of each document kind this build reads.
_BUNDLE_MAJOR = 1
_BINDINGS_MAJOR = 2

_NOT_OURS = "This is not a KeyGnosys file."
_NEWER = "This file was made by a newer KeyGnosys."
_DAMAGED = "This map is damaged and cannot be read."


class ImportRefused(Exception):
    """The file cannot be imported. `reasons` are human-readable lines."""

    def __init__(self, reasons: list[str]) -> None:
        super().__init__("; ".join(reasons))
        self.reasons = reasons


@dataclass(frozen=True)
class Finding:
    """Something worth telling the user that does not stop the import.

    kind is "unusable" (kept but this build cannot run it), "off_layout"
    (a key the current keyboard layout lacks) or "setting" (a layer setting
    outside this build's bounds).
    """

    kind: str
    text: str


@dataclass
class ImportPlan:
    bset: BindingSet
    name: str
    author: str
    exported: str | None
    findings: list[Finding] = field(default_factory=list)


# -- writing ---------------------------------------------------------------

def export_bundle(bset: BindingSet, author: str = "") -> dict:
    """A `keygnosys/bundle/1` holding `bset` exactly as the editor saves it.

    The author lives only in the header, so exporting never alters the map.
    """
    exported = (datetime.now(timezone.utc).replace(microsecond=0)
                .strftime("%Y-%m-%dT%H:%M:%SZ"))
    return {
        "schema": BUNDLE_SCHEMA,
        "name": bset.name,
        "author": author,
        "exported": exported,
        "generator": f"KeyGnosys {keygnosys.__version__}",
        "documents": [bset.to_dict()],
    }


def write_bundle(path: Path, bundle: dict) -> None:
    """Write atomically: a temp file beside the target, then a rename."""
    path = Path(path)
    tmp = path.with_name(path.name + ".tmp")
    try:
        tmp.write_text(json.dumps(bundle, indent=2) + "\n", encoding="utf-8")
        os.replace(tmp, path)
    except BaseException:
        try:
            tmp.unlink()
        except OSError:
            pass
        raise


# -- reading ---------------------------------------------------------------

def _parse_schema(value: object) -> tuple[str, int] | None:
    """("bundle", 1) from "keygnosys/bundle/1"; None if not that shape."""
    if not isinstance(value, str):
        return None
    parts = value.split("/")
    if len(parts) != 3 or parts[0] != "keygnosys" or not parts[1]:
        return None
    try:
        return parts[1], int(parts[2].split(".")[0])
    except ValueError:
        return None


def _load_json(path: Path) -> object:
    try:
        if path.stat().st_size > MAX_BYTES:
            raise ImportRefused(
                [f"This file is too large to be a KeyGnosys map "
                 f"(over {MAX_BYTES // (1024 * 1024)} MB)."])
        raw = path.read_bytes()
        # utf-8-sig: a file hand-edited and saved with a BOM still reads.
        return json.loads(raw.decode("utf-8-sig"))
    except (OSError, UnicodeDecodeError, ValueError):
        # JSONDecodeError is a ValueError.
        raise ImportRefused([_NOT_OURS]) from None
    except RecursionError:
        raise ImportRefused([_NOT_OURS]) from None


def read_import(path: Path,
                layout_codes: set[str] | None = None) -> ImportPlan:
    """Validate a file and return what importing it would bring in.

    Raises `ImportRefused` for anything blocking. A known command whose
    parameters this build rejects is not blocking: it is kept and reported,
    like an unknown command.
    """
    data = _load_json(Path(path))
    top = _parse_schema(data.get("schema")) if isinstance(data, dict) else None
    if top is None or top[0] not in ("bundle", "bindings"):
        raise ImportRefused([_NOT_OURS])
    kind, major = top

    if kind == "bundle":
        if major > _BUNDLE_MAJOR:
            raise ImportRefused([_NEWER])
        if major < 1:
            raise ImportRefused([_NOT_OURS])
        doc, header = _pick_map(data), data
    else:
        if major > _BINDINGS_MAJOR:
            raise ImportRefused([_NEWER])
        doc, header = data, {}

    try:
        return _plan(doc, header, layout_codes)
    except DocumentError as exc:
        raise ImportRefused([str(exc)]) from None
    except Exception:       # noqa: BLE001 -- the file is untrusted
        # A wrong type, an absurd number or deep nesting: whatever it is,
        # it must reach the user as a refusal, never escape as a crash.
        raise ImportRefused([_DAMAGED]) from None


def _plan(doc: dict, header: dict,
          layout_codes: set[str] | None) -> ImportPlan:
    bset = BindingSet.from_dict(doc)
    name = header.get("name")
    if not isinstance(name, str) or not name:
        name = bset.name
    header_author = header.get("author")
    author = bset.metadata.author or (
        header_author if isinstance(header_author, str) else "")
    exported = header.get("exported")
    return ImportPlan(
        bset=bset,
        name=name,
        author=author,
        exported=exported if isinstance(exported, str) else None,
        findings=_findings(bset, layout_codes),
    )


def _pick_map(bundle: dict) -> dict:
    """The one bindings document in a bundle, or refuse."""
    docs = bundle.get("documents")
    if not isinstance(docs, list):
        raise ImportRefused(["This bundle holds no binding map."])
    reasons: list[str] = []
    maps: list[dict] = []
    for doc in docs:
        parsed = _parse_schema(doc.get("schema")) if isinstance(doc, dict) else None
        if parsed is None:
            reasons.append("This bundle holds a document that is not a "
                           "KeyGnosys document.")
        elif parsed[0] != "bindings":
            reasons.append(f"This bundle holds a '{parsed[0]}' document. "
                           f"{_NEWER}")
        elif parsed[1] > _BINDINGS_MAJOR:
            reasons.append(f"The binding map is version {parsed[1]}. {_NEWER}")
        else:
            maps.append(doc)
    if reasons:
        raise ImportRefused(reasons)
    if not maps:
        raise ImportRefused(["This bundle holds no binding map."])
    if len(maps) > 1:
        raise ImportRefused(
            ["This bundle holds more than one binding map. This version of "
             "KeyGnosys has a single map and cannot choose between them."])
    return maps[0]


def _findings(bset: BindingSet,
              layout_codes: set[str] | None) -> list[Finding]:
    out: list[Finding] = []

    def unusable(where: str, spec: dict) -> None:
        action = spec.get("action")
        if action in CATALOG:
            out.append(Finding(
                "unusable",
                f"{where}: command `{action}` has settings this version "
                f"does not accept"))
        else:
            out.append(Finding("unusable",
                               f"{where}: unknown command `{action}`"))

    for code, spec in bset.extra_bindings.items():
        unusable(code, spec)
    for spec in bset.extra_unassigned:
        unusable("Unassigned", spec)

    if layout_codes is not None:
        absent = sorted(c for c in {*bset.bindings, *bset.extra_bindings}
                        if c not in layout_codes)
        if absent:
            out.append(Finding(
                "off_layout",
                f"Not on your keyboard: {', '.join(absent)}"))

    for name, value in bset.settings.items():
        limits = KNOWN_SETTINGS.get(name)
        if (limits is None or isinstance(value, bool)
                or not isinstance(value, (int, float))):
            continue
        kind, low, high, _step = limits
        used = min(max(value, low), high)
        if kind == "int":
            used = int(used)
        if used != value:
            out.append(Finding(
                "setting",
                f"{name}: {value} is outside {low} to {high}; "
                f"{used} will be used"))
    return out


# -- comparing -------------------------------------------------------------

@dataclass(frozen=True)
class Change:
    """One difference between two maps. kind is "key" or "setting"."""

    kind: str
    name: str
    before: str
    after: str


_NOTHING = "(nothing)"
_NEW = "—"


def _entry(bset: BindingSet, code: str) -> tuple[dict, str, str | None] | None:
    """(comparable dict, description, legend) for a key, or None if unbound."""
    binding = bset.bindings.get(code)
    if binding is not None:
        legend = binding.legend
        text = legend if legend else default_legend(binding.action,
                                                    binding.params)
        return binding.to_dict(), text, legend
    raw = bset.extra_bindings.get(code)
    if raw is not None:
        legend = raw.get("legend")
        return (raw, str(raw.get("action")),
                legend if isinstance(legend, str) else None)
    return None


def _compact(spec: dict) -> str:
    return json.dumps(spec, sort_keys=True, separators=(",", ":"))


def diff(old: BindingSet, new: BindingSet,
         display: Callable[[str], str] = str,
         order: Sequence[str] = ()) -> list[Change]:
    """What importing `new` over `old` would change, key by key.

    Key changes follow `order` (the layout), then any other codes sorted;
    setting changes come last, sorted by name.
    """
    codes = {*old.bindings, *old.extra_bindings,
             *new.bindings, *new.extra_bindings}
    rank = {code: i for i, code in enumerate(order)}
    ordered = sorted(codes, key=lambda c: (rank.get(c, len(rank)), c))

    changes: list[Change] = []
    for code in ordered:
        before, after = _entry(old, code), _entry(new, code)
        if before is None and after is None:
            continue
        if before is not None and after is not None:
            if before[0] == after[0]:
                continue
            b_text, a_text = before[1], after[1]
            if b_text == a_text:
                if (before[0].get("action") == after[0].get("action")
                        and before[0].get("params") == after[0].get("params")):
                    # Only the legend differs.
                    b_text = f'{b_text} "{before[2] or ""}"'
                    a_text = f'{a_text} "{after[2] or ""}"'
                else:
                    # Same words, different parameters: show the parameters.
                    b_text = f"{b_text} {before[0].get('params', {})}"
                    a_text = f"{a_text} {after[0].get('params', {})}"
            if b_text == a_text:
                # Only fields this build does not show differ: show them all.
                b_text, a_text = _compact(before[0]), _compact(after[0])
        else:
            b_text = before[1] if before else _NEW
            a_text = after[1] if after else _NOTHING
        changes.append(Change("key", display(code), b_text, a_text))

    missing = object()
    for name in sorted({*old.settings, *new.settings}):
        b = old.settings.get(name, missing)
        a = new.settings.get(name, missing)
        if b == a:
            continue
        changes.append(Change("setting", name,
                              _NEW if b is missing else str(b),
                              _NOTHING if a is missing else str(a)))
    return changes

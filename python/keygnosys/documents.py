"""Loading and validation of the four JSON document kinds.

Layouts, bindings, themes and profiles are all plain JSON discovered at runtime
(principle P1). This module turns them into typed objects and, critically,
*refuses to let one bad file break the rest* (principle P6): every failure is
recorded as a diagnostic and the offending document is skipped.

See docs/SPEC.md section 4 for the formats and section 11 for diagnostic codes.
"""

from __future__ import annotations

import json
import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterable

from . import paths

#: Document ids: lowercase, used in settings and the shadowing rule.
ID_RE = re.compile(r"^[a-z0-9][a-z0-9-]*$")
#: Key and segment ids: scoped to their document, so the rules are looser.
SUB_ID_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_-]*$")

#: Tolerance for layout geometry comparisons, in units.
GEOMETRY_EPS = 1e-6

#: Schema majors this build writes. Older majors are still read (SPEC 3.4.4).
LAYOUT_SCHEMA_MAJOR = 2
BINDINGS_SCHEMA_MAJOR = 2

#: Canonical modifier ordering for app-profile shortcut lookup keys.
MODIFIER_ORDER = ("Control", "Alt", "Shift", "Meta")


def _vocabulary() -> frozenset[str]:
    """The W3C UI Events `code` values of SPEC section 2.1.

    A layout may name a code outside this set -- it loads and renders, it just
    can never highlight, because no backend will ever emit it. That is a
    warning, not a rejection.
    """
    codes = {f"Key{c}" for c in "ABCDEFGHIJKLMNOPQRSTUVWXYZ"}
    codes |= {f"Digit{d}" for d in "0123456789"}
    codes |= {f"F{i}" for i in range(1, 25)}
    codes |= {f"Numpad{d}" for d in "0123456789"}
    codes |= {
        "ShiftLeft", "ShiftRight", "ControlLeft", "ControlRight",
        "AltLeft", "AltRight", "MetaLeft", "MetaRight", "CapsLock",
        "Space", "Tab", "Enter", "Backspace",
        "Minus", "Equal", "BracketLeft", "BracketRight", "Backslash",
        "Semicolon", "Quote", "Backquote", "Comma", "Period", "Slash",
        "IntlBackslash",
        "Insert", "Delete", "Home", "End", "PageUp", "PageDown",
        "ArrowUp", "ArrowDown", "ArrowLeft", "ArrowRight",
        "NumpadAdd", "NumpadSubtract", "NumpadMultiply", "NumpadDivide",
        "NumpadDecimal", "NumpadEnter", "NumLock",
        "Escape", "PrintScreen", "ScrollLock", "Pause", "ContextMenu",
        # Laptop keys handled in firmware; rendered, never promised as
        # bindable (SPEC 8.4).
        "Fn", "FnLock",
    }
    return frozenset(codes)


KNOWN_CODES = _vocabulary()


class DocumentError(Exception):
    """A document failed validation and must be skipped."""


@dataclass
class Diagnostic:
    level: str          # "info" | "warn" | "error"
    code: str           # stable machine-readable code, see SPEC section 11
    message: str
    file: str | None = None

    def __str__(self) -> str:
        where = f" [{self.file}]" if self.file else ""
        return f"{self.level.upper()}: {self.code}: {self.message}{where}"


# ---------------------------------------------------------------------------
# Schema helpers
# ---------------------------------------------------------------------------

def _schema_major(doc: dict, kind: str, supported: tuple[int, ...]) -> int:
    """Validate the schema field and return its major version.

    Every major version this build has ever shipped stays readable; upgrading
    happens in memory and never rewrites the file (SPEC 3.4.4).
    """
    got = doc.get("schema")
    want = f"keygnosys/{kind}/"
    newest = max(supported)
    if not isinstance(got, str) or not got.startswith(want):
        raise DocumentError(f"expected schema {want}{newest}, found {got!r}")
    try:
        got_major = int(got[len(want):].split(".")[0])
    except ValueError as exc:
        raise DocumentError(f"unparsable schema version in {got!r}") from exc
    if got_major not in supported:
        raise DocumentError(
            f"schema major version {got_major} is not supported (this build "
            f"reads {', '.join(map(str, sorted(supported)))})"
        )
    return got_major


def _check_schema(doc: dict, kind: str, major: int = 1) -> None:
    _schema_major(doc, kind, (major,))


def _check_id(doc: dict) -> str:
    doc_id = doc.get("id")
    if not isinstance(doc_id, str) or not ID_RE.match(doc_id):
        raise DocumentError(
            f"id must match {ID_RE.pattern!r}, found {doc_id!r}"
        )
    return doc_id


# ---------------------------------------------------------------------------
# Layout
# ---------------------------------------------------------------------------

@dataclass
class Segment:
    """One axis-aligned rectangle of a logical key's drawn shape.

    Most keys have exactly one. An ISO Enter has two, forming its L. Segments
    are a *drawing* detail: they have no identity outside the layout editor,
    and every consumer treats the key they belong to as a single thing.
    """

    id: str
    x: float
    y: float
    w: float = 1.0
    h: float = 1.0

    @property
    def right(self) -> float:
        return self.x + self.w

    @property
    def bottom(self) -> float:
        return self.y + self.h

    @property
    def area(self) -> float:
        return self.w * self.h

    def contains(self, px: float, py: float) -> bool:
        return (self.x <= px <= self.right) and (self.y <= py <= self.bottom)

    def overlaps(self, other: "Segment") -> bool:
        """True if two rectangles share area, not merely an edge.

        Geometry is floating point and layouts are commonly generated, so
        adjacent keys routinely land a few ulps apart. Without the tolerance,
        a perfectly tiled row reads as 17 overlapping keys.
        """
        return not (
            self.right <= other.x + GEOMETRY_EPS
            or other.right <= self.x + GEOMETRY_EPS
            or self.bottom <= other.y + GEOMETRY_EPS
            or other.bottom <= self.y + GEOMETRY_EPS
        )


@dataclass
class KeyStyle:
    """Per-key colour overrides. Anything None falls through to the theme."""

    face: str | None = None
    text: str | None = None
    border: str | None = None
    accent: str | None = None

    @classmethod
    def from_dict(cls, raw: Any) -> "KeyStyle | None":
        if not isinstance(raw, dict):
            return None
        style = cls(
            face=raw.get("face"), text=raw.get("text"),
            border=raw.get("border"), accent=raw.get("accent"),
        )
        # An object with nothing usable in it is the same as no object; not
        # collapsing it would mean carrying a meaningless override around.
        if not any((style.face, style.text, style.border, style.accent)):
            return None
        return style

    def to_dict(self) -> dict:
        return {k: v for k, v in {
            "face": self.face, "text": self.text,
            "border": self.border, "accent": self.accent,
        }.items() if v is not None}


@dataclass
class Key:
    """One logical key: one identity, one label, one highlight, one target."""

    id: str
    code: str
    segments: list[Segment]
    base: str = ""
    shift: str | None = None
    sub: str | None = None
    role: str = "normal"
    style: KeyStyle | None = None

    # -- bounding box -----------------------------------------------------
    #
    # Single-segment keys are the overwhelming majority, so these read as the
    # key's own geometry and callers that never meet an L-shape need not know
    # segments exist.

    @property
    def x(self) -> float:
        return min(s.x for s in self.segments)

    @property
    def y(self) -> float:
        return min(s.y for s in self.segments)

    @property
    def right(self) -> float:
        return max(s.right for s in self.segments)

    @property
    def bottom(self) -> float:
        return max(s.bottom for s in self.segments)

    @property
    def w(self) -> float:
        return self.right - self.x

    @property
    def h(self) -> float:
        return self.bottom - self.y

    @property
    def largest_segment(self) -> Segment:
        """The segment a label should be centred on.

        The centroid of an L-shape's bounding box lands in the notch, which is
        outside the key.
        """
        return max(self.segments, key=lambda s: s.area)

    def contains(self, px: float, py: float) -> bool:
        """A point inside *any* segment hits the key."""
        return any(s.contains(px, py) for s in self.segments)

    def overlaps(self, other: "Key") -> bool:
        """True if any segment of this key overlaps any segment of another.

        Only meaningful between *different* logical keys. A key's own segments
        may touch or overlap freely -- sharing an edge is how an L is built.
        """
        return any(a.overlaps(b) for a in self.segments for b in other.segments)

    def translate(self, dx: float, dy: float) -> None:
        """Move every segment together, preserving their relative offsets."""
        for seg in self.segments:
            seg.x += dx
            seg.y += dy


@dataclass
class Metadata:
    """Provenance. Records where a document came from, for reset-to-template."""

    author: str = ""
    source_template: str | None = None
    model: str | None = None
    revision: int = 1

    @classmethod
    def from_dict(cls, raw: Any) -> "Metadata":
        if not isinstance(raw, dict):
            return cls()
        try:
            revision = int(raw.get("revision", 1))
        except (TypeError, ValueError):
            revision = 1
        return cls(
            author=str(raw.get("author") or ""),
            source_template=raw.get("source_template"),
            model=raw.get("model"),
            revision=revision,
        )


def _synth_id(raw: Any, prefix: str, index: int) -> str:
    """Use a document's id if it has a usable one, else synthesise by index.

    Hand-written layouts should not be required to invent ids, but the editor
    needs something stable to hold on to, so we always end up with one.
    """
    if isinstance(raw, str) and SUB_ID_RE.match(raw):
        return raw
    return f"{prefix}{index}"


@dataclass
class Layout:
    id: str
    name: str
    description: str
    width: float
    height: float
    keys: list[Key]
    metadata: Metadata = field(default_factory=Metadata)
    schema_major: int = LAYOUT_SCHEMA_MAJOR

    @classmethod
    def from_dict(cls, doc: dict, sink: "DiagnosticSink | None" = None,
                  origin: str | None = None) -> "Layout":
        major = _schema_major(doc, "layout", (1, 2))
        doc_id = _check_id(doc)

        raw_keys = doc.get("keys")
        if not isinstance(raw_keys, list) or not raw_keys:
            raise DocumentError("'keys' must be a non-empty list")

        def warn(code: str, message: str) -> None:
            if sink is not None:
                sink.warn(code, message, origin)

        keys: list[Key] = []
        seen_ids: set[str] = set()

        for i, rk in enumerate(raw_keys):
            # A malformed *key* drops that key and keeps the layout; only a
            # malformed document is fatal (SPEC 4.1.5).
            if not isinstance(rk, dict):
                warn("layout.key_invalid", f"keys[{i}] is not an object")
                continue
            try:
                key = _key_from_dict(rk, i, major)
            except DocumentError as exc:
                warn("layout.key_invalid", str(exc))
                continue
            if key.id in seen_ids:
                warn("layout.key_invalid",
                     f"duplicate key id '{key.id}'; the later one is dropped")
                continue
            seen_ids.add(key.id)
            keys.append(key)

        if not keys:
            raise DocumentError("no valid keys")

        size = doc.get("size") or {}
        width = float(size.get("w") or max(k.right for k in keys))
        height = float(size.get("h") or max(k.bottom for k in keys))

        layout = cls(
            id=doc_id,
            name=str(doc.get("name") or doc_id),
            description=str(doc.get("description") or ""),
            width=width, height=height, keys=keys,
            metadata=Metadata.from_dict(doc.get("metadata")),
            schema_major=major,
        )
        if major < LAYOUT_SCHEMA_MAJOR and sink:
            sink.info("layout.upgraded",
                      f"'{doc_id}' is schema {major}; upgraded in memory "
                      f"(the file is left untouched)", origin)
        layout.report_warnings(sink, origin)
        return layout

    # -- validation -------------------------------------------------------

    def report_warnings(self, sink: "DiagnosticSink | None" = None,
                        origin: str | None = None) -> list[Diagnostic]:
        """Non-fatal problems: the layout still renders, but something is off.

        The editor shows these in its problems panel and requires them to be
        acknowledged before a save that introduces one. Nothing here is ever
        corrected automatically -- silently rewriting someone's layout is worse
        than letting them ship an odd one.
        """
        found: list[Diagnostic] = []

        def add(code: str, message: str) -> None:
            diag = Diagnostic("warn", code, message, origin)
            found.append(diag)
            if sink is not None:
                sink.items.append(diag)

        by_code: dict[str, list[Key]] = {}
        for key in self.keys:
            by_code.setdefault(key.code, []).append(key)
        for code, group in by_code.items():
            if len(group) > 1:
                add("layout.duplicate_code",
                    f"{len(group)} keys claim code '{code}' "
                    f"({', '.join(k.id for k in group)}); all would light up "
                    f"together")

        unknown = sorted({k.code for k in self.keys} - KNOWN_CODES)
        if unknown:
            add("layout.unknown_code",
                f"codes outside the vocabulary, which can never highlight: "
                f"{', '.join(unknown)}")

        # Between *different* logical keys only. A key's own segments may
        # touch or overlap freely.
        for i, a in enumerate(self.keys):
            for b in self.keys[i + 1:]:
                if a.overlaps(b):
                    add("layout.overlap",
                        f"keys '{a.id}' ({a.code}) and '{b.id}' ({b.code}) "
                        f"overlap")

        out_of_bounds = [
            k.id for k in self.keys
            if k.right > self.width + GEOMETRY_EPS
            or k.bottom > self.height + GEOMETRY_EPS
        ]
        if out_of_bounds:
            add("layout.out_of_bounds",
                f"keys extending past the declared size "
                f"{self.width}x{self.height}u: {', '.join(out_of_bounds)}")

        return found

    def codes(self) -> set[str]:
        return {k.code for k in self.keys}

    def key_by_id(self, key_id: str) -> Key | None:
        return next((k for k in self.keys if k.id == key_id), None)


def _key_from_dict(rk: dict, index: int, major: int) -> Key:
    """Build one logical key, from either schema version."""
    code = rk.get("code")
    if not isinstance(code, str) or not code:
        raise DocumentError(f"keys[{index}] has no 'code'")

    legend = rk.get("legend")
    if not isinstance(legend, dict) or "base" not in legend:
        raise DocumentError(f"keys[{index}] ({code}) has no 'legend.base'")

    raw_segments = rk.get("segments")
    if raw_segments is None:
        # Schema 1: geometry sat directly on the key. Promote it to a single
        # segment, which is exactly what schema 2 would have said (SPEC 4.1.7).
        raw_segments = [{"id": "s0", "x": rk.get("x"), "y": rk.get("y"),
                         "w": rk.get("w", 1.0), "h": rk.get("h", 1.0)}]
    if not isinstance(raw_segments, list) or not raw_segments:
        raise DocumentError(f"keys[{index}] ({code}) has no segments")

    segments: list[Segment] = []
    for j, rs in enumerate(raw_segments):
        if not isinstance(rs, dict):
            raise DocumentError(f"keys[{index}] ({code}) segment {j} is not an object")
        try:
            x, y = float(rs["x"]), float(rs["y"])
            w, h = float(rs.get("w", 1.0)), float(rs.get("h", 1.0))
        except (KeyError, TypeError, ValueError) as exc:
            raise DocumentError(
                f"keys[{index}] ({code}) segment {j} has bad geometry: {exc}"
            ) from exc
        if not (w > 0 and h > 0) or not all(map(_finite, (x, y, w, h))):
            raise DocumentError(
                f"keys[{index}] ({code}) segment {j} has non-positive or "
                f"non-finite dimensions ({w}x{h})")
        segments.append(Segment(id=_synth_id(rs.get("id"), "s", j),
                                x=x, y=y, w=w, h=h))

    return Key(
        id=_synth_id(rk.get("id"), "k", index),
        code=code,
        segments=segments,
        base=str(legend["base"]),
        shift=legend.get("shift"),
        sub=legend.get("sub"),
        role=rk.get("role", "normal"),
        style=KeyStyle.from_dict(rk.get("style")),
    )


def _finite(value: float) -> bool:
    return value == value and value not in (float("inf"), float("-inf"))


# ---------------------------------------------------------------------------
# Bindings
# ---------------------------------------------------------------------------

DEFAULT_BINDING_SETTINGS = {
    "pointer_base_speed": 2,
    "pointer_max_speed": 28,
    "pointer_ramp_ms": 420,
    "precision_factor": 0.25,
    "scroll_base_speed": 1,
    "scroll_max_speed": 6,
    "scroll_ramp_ms": 500,
}


@dataclass
class Binding:
    """One command: an action, its parameters, and an optional custom label."""

    action: str
    params: dict[str, Any] = field(default_factory=dict)
    legend: str | None = None

    def identity(self) -> tuple:
        """What makes two commands "the same command".

        Parameters are part of it -- `window.slot index=1` and `index=2` are
        different commands. The label is not: relabelling does not create a
        new command.
        """
        return (self.action,
                tuple(sorted((self.params or {}).items(), key=lambda kv: kv[0])))


@dataclass
class BindingSet:
    id: str
    name: str
    description: str
    settings: dict[str, Any]
    bindings: dict[str, Binding]
    #: Commands the user configured that currently sit on no key. The stored
    #: half of the editor's unassigned list; the rest is derived from the
    #: action catalog at runtime, so a command added in a later release still
    #: reaches users whose document predates it (SPEC 3.4.5).
    unassigned: list[Binding] = field(default_factory=list)
    metadata: Metadata = field(default_factory=Metadata)
    schema_major: int = BINDINGS_SCHEMA_MAJOR

    @classmethod
    def from_dict(cls, doc: dict, sink: "DiagnosticSink | None" = None,
                  origin: str | None = None) -> "BindingSet":
        major = _schema_major(doc, "bindings", (1, 2))
        doc_id = _check_id(doc)

        raw = doc.get("bindings")
        if not isinstance(raw, dict):
            raise DocumentError("'bindings' must be an object")

        def warn(message: str, code: str = "binding.unknown_action") -> None:
            if sink is not None:
                sink.warn(code, message, origin)

        bindings: dict[str, Binding] = {}
        for code, spec in raw.items():
            binding = _binding_from_dict(spec, str(code), warn)
            if binding is not None:
                bindings[code] = binding

        bound = {b.identity() for b in bindings.values()}
        unassigned: list[Binding] = []
        seen: set[tuple] = set()
        for i, spec in enumerate(doc.get("unassigned") or []):
            binding = _binding_from_dict(spec, f"unassigned[{i}]", warn)
            if binding is None:
                continue
            identity = binding.identity()
            if identity in bound:
                # Only reachable by hand-editing. A command cannot be both
                # assigned and unassigned; the binding wins.
                warn(f"{binding.action} is both bound and listed as "
                     f"unassigned; keeping the binding",
                     "binding.unassigned_conflict")
                continue
            if identity in seen:
                continue
            seen.add(identity)
            unassigned.append(binding)

        settings = dict(DEFAULT_BINDING_SETTINGS)
        settings.update(doc.get("settings") or {})

        return cls(
            id=doc_id,
            name=str(doc.get("name") or doc_id),
            description=str(doc.get("description") or ""),
            settings=settings,
            bindings=bindings,
            unassigned=unassigned,
            metadata=Metadata.from_dict(doc.get("metadata")),
            schema_major=major,
        )

    # -- assignment (SPEC 10.2.2) -----------------------------------------

    def keys_for(self, command: Binding) -> list[str]:
        """Every key currently holding this command. May be more than one."""
        identity = command.identity()
        return [code for code, b in self.bindings.items()
                if b.identity() == identity]

    def assign(self, code: str, command: Binding) -> Binding | None:
        """Bind `command` to key `code`, displacing whatever was there.

        Silent by design: no prompt, no warning. The user named a key and a
        command, there is one possible meaning, and nothing is destroyed --
        anything displaced lands in `unassigned`, one click away.

        Returns the displaced command, if the key held one.
        """
        displaced = self.bindings.get(code)
        self.bindings[code] = command

        # A command may legitimately sit on several keys, so it only becomes
        # unassigned when its *last* key is taken.
        if displaced is not None and not self.keys_for(displaced):
            self._remember_unassigned(displaced)

        # If the incoming command came from the unassigned list, it is now
        # assigned and must leave it.
        identity = command.identity()
        self.unassigned = [b for b in self.unassigned
                           if b.identity() != identity]
        return displaced

    def unassign(self, code: str) -> Binding | None:
        """Clear a key, moving its command to the unassigned list."""
        removed = self.bindings.pop(code, None)
        if removed is not None and not self.keys_for(removed):
            self._remember_unassigned(removed)
        return removed

    def _remember_unassigned(self, command: Binding) -> None:
        identity = command.identity()
        if any(b.identity() == identity for b in self.unassigned):
            return
        # Kept with its params and custom legend intact: a user who wrote
        # "Send to left screen" on a key should not have to retype it because
        # they moved it.
        self.unassigned.append(command)

    def available_commands(self) -> list[Binding]:
        """Catalog commands neither bound nor already in `unassigned`.

        Derived, never stored. This is the half of the editor's unassigned list
        that keeps upgrades additive -- a command added in a later release
        appears here without the user re-forking anything.
        """
        from .actions import catalog_commands

        taken = {b.identity() for b in self.bindings.values()}
        taken |= {b.identity() for b in self.unassigned}
        return [Binding(action=a, params=p) for a, p in catalog_commands()
                if Binding(action=a, params=p).identity() not in taken]


def _binding_from_dict(spec: Any, where: str, warn) -> Binding | None:
    """Validate one command, or warn and return None."""
    if not isinstance(spec, dict) or "action" not in spec:
        warn(f"{where} is not an action object")
        return None

    # Imported lazily: the catalog is the authority on what is valid, and
    # keeping the import local says so.
    from .actions import validate_binding

    action = str(spec["action"])
    params = spec.get("params") or {}
    problem = validate_binding(action, params)
    if problem:
        # A bad command is skipped; every other one in the file still loads.
        warn(f"{where}: {problem}")
        return None
    return Binding(action=action, params=params, legend=spec.get("legend"))


# ---------------------------------------------------------------------------
# Theme
# ---------------------------------------------------------------------------

REQUIRED_TOKENS = (
    "surface", "key_face", "key_face_alt", "key_border", "key_text",
    "key_text_dim", "key_sub_text", "accent", "accent_text", "latched",
    "led_on", "led_off", "bar_surface", "bar_text",
)


@dataclass
class Theme:
    id: str
    name: str
    base: str                      # "dark" | "light"
    tokens: dict[str, str]

    @classmethod
    def from_dict(cls, doc: dict) -> "Theme":
        _check_schema(doc, "theme")
        doc_id = _check_id(doc)
        tokens = doc.get("tokens")
        if not isinstance(tokens, dict):
            raise DocumentError("'tokens' must be an object")
        missing = [t for t in REQUIRED_TOKENS if t not in tokens]
        if missing:
            raise DocumentError(f"missing colour tokens: {', '.join(missing)}")
        base = doc.get("base", "dark")
        if base not in ("dark", "light"):
            raise DocumentError(f"'base' must be 'dark' or 'light', found {base!r}")
        return cls(id=doc_id, name=str(doc.get("name") or doc_id),
                   base=base, tokens=dict(tokens))


# ---------------------------------------------------------------------------
# App profile
# ---------------------------------------------------------------------------

@dataclass
class Profile:
    id: str
    name: str
    priority: int
    processes: list[str]
    wm_classes: list[str]
    title_regex: re.Pattern | None
    shortcuts: dict[str, dict[str, str]]

    @classmethod
    def from_dict(cls, doc: dict) -> "Profile":
        _check_schema(doc, "profile")
        doc_id = _check_id(doc)
        match = doc.get("match") or {}
        raw_title = match.get("title_regex")
        try:
            title = re.compile(raw_title, re.IGNORECASE) if raw_title else None
        except re.error as exc:
            raise DocumentError(f"bad title_regex: {exc}") from exc

        shortcuts = doc.get("shortcuts") or {}
        if not isinstance(shortcuts, dict):
            raise DocumentError("'shortcuts' must be an object")
        # Canonicalise the modifier-combination keys so lookup is a plain dict
        # hit rather than a search. See SPEC section 4.4.
        canonical: dict[str, dict[str, str]] = {}
        for combo, keys in shortcuts.items():
            if not isinstance(keys, dict):
                raise DocumentError(f"shortcuts[{combo!r}] must be an object")
            parts = [p.strip() for p in str(combo).split("+") if p.strip()]
            unknown = [p for p in parts if p not in MODIFIER_ORDER]
            if unknown:
                raise DocumentError(
                    f"shortcuts key {combo!r} names unknown modifiers: {unknown}"
                )
            key = canonical_modifiers(parts)
            canonical.setdefault(key, {}).update(
                {str(k): str(v) for k, v in keys.items()}
            )

        return cls(
            id=doc_id,
            name=str(doc.get("name") or doc_id),
            priority=int(doc.get("priority", 50)),
            processes=[p.lower() for p in (match.get("process") or [])],
            wm_classes=[c.lower() for c in (match.get("wm_class") or [])],
            title_regex=title,
            shortcuts=canonical,
        )

    def matches(self, process: str | None, wm_class: str | None,
                title: str | None) -> bool:
        if process and process.lower() in self.processes:
            return True
        if wm_class and wm_class.lower() in self.wm_classes:
            return True
        if title and self.title_regex and self.title_regex.search(title):
            return True
        return False


def canonical_modifiers(mods: Iterable[str]) -> str:
    """Join modifier names in the one canonical order (SPEC section 4.4)."""
    present = set(mods)
    return "+".join(m for m in MODIFIER_ORDER if m in present)


# ---------------------------------------------------------------------------
# Registry
# ---------------------------------------------------------------------------

class DiagnosticSink:
    """Collects diagnostics instead of raising, so loading never aborts."""

    def __init__(self) -> None:
        self.items: list[Diagnostic] = []

    def add(self, level: str, code: str, message: str,
            file: str | None = None) -> None:
        self.items.append(Diagnostic(level, code, message, file))

    def warn(self, code: str, message: str, file: str | None = None) -> None:
        self.add("warn", code, message, file)

    def info(self, code: str, message: str, file: str | None = None) -> None:
        self.add("info", code, message, file)

    def error(self, code: str, message: str, file: str | None = None) -> None:
        self.add("error", code, message, file)

    def __iter__(self):
        return iter(self.items)

    def __len__(self) -> int:
        return len(self.items)

    def __bool__(self) -> bool:
        # Without this, `if sink:` is False while the sink is still empty --
        # which is exactly when the first diagnostic is about to be added.
        # Callers should test `sink is not None`, but a sink that lies about
        # its own existence is a trap worth closing at the source.
        return True


_LOADERS = {
    "layouts": (Layout, "layout.invalid"),
    "themes": (Theme, "theme.invalid"),
    "profiles": (Profile, "profile.invalid"),
}


class Registry:
    """All loaded documents, keyed by kind then id.

    Loading is ordered lowest-precedence-first, so a user document silently
    replaces a bundled one with the same id -- wholesale, never field-merged.
    See docs/SPEC.md section 3.2 for why merging is rejected.
    """

    def __init__(self) -> None:
        self.layouts: dict[str, Layout] = {}
        self.bindings: dict[str, BindingSet] = {}
        self.themes: dict[str, Theme] = {}
        self.profiles: dict[str, Profile] = {}
        self.diagnostics = DiagnosticSink()

    # -- loading ----------------------------------------------------------

    def load_all(self) -> "Registry":
        for kind in paths.KINDS:
            for directory in paths.search_dirs(kind):
                self._load_dir(kind, directory)
        self._cross_validate()
        return self

    def _load_dir(self, kind: str, directory: Path) -> None:
        if not directory.is_dir():
            return
        target = getattr(self, kind)
        for path in sorted(directory.glob("*.json")):
            try:
                doc = json.loads(path.read_text(encoding="utf-8"))
            except (OSError, json.JSONDecodeError) as exc:
                self.diagnostics.warn(f"{kind[:-1]}.invalid",
                                      f"unreadable: {exc}", str(path))
                continue
            try:
                if kind in ("bindings", "layouts"):
                    # These two report per-entry problems without failing the
                    # whole document, so they need the sink.
                    loader = BindingSet if kind == "bindings" else Layout
                    obj = loader.from_dict(doc, self.diagnostics, str(path))
                else:
                    cls, _code = _LOADERS[kind]
                    obj = cls.from_dict(doc)
            except DocumentError as exc:
                self.diagnostics.warn(f"{kind[:-1]}.invalid", str(exc), str(path))
                continue

            if obj.id in target:
                self.diagnostics.info(
                    f"{kind[:-1]}.duplicate_id",
                    f"'{obj.id}' replaces an earlier document of the same id",
                    str(path))
            target[obj.id] = obj

    def _cross_validate(self) -> None:
        """Report bindings that reference keys no bundled layout provides.

        Not an error -- a user may bind numpad keys and use a laptop layout most
        of the time -- but it is worth saying out loud, because the binding will
        simply never appear on the map.
        """
        if not self.layouts:
            return
        all_codes: set[str] = set()
        for layout in self.layouts.values():
            all_codes |= layout.codes()
        for bset in self.bindings.values():
            unknown = sorted(set(bset.bindings) - all_codes)
            if unknown:
                self.diagnostics.info(
                    "binding.unknown_key",
                    f"bindings '{bset.id}' reference keys absent from every "
                    f"loaded layout: {', '.join(unknown)}")

    # -- lookup -----------------------------------------------------------

    def layout(self, layout_id: str) -> Layout | None:
        return self.layouts.get(layout_id)

    def binding_set(self, bindings_id: str) -> BindingSet | None:
        return self.bindings.get(bindings_id)

    def theme(self, theme_id: str, system_is_dark: bool = True) -> Theme | None:
        """Resolve a theme id, expanding the pseudo-id 'system'."""
        if theme_id == "system":
            want = "dark" if system_is_dark else "light"
            for theme in self.themes.values():
                if theme.base == want:
                    return theme
            return next(iter(self.themes.values()), None)
        return self.themes.get(theme_id)

    def match_profile(self, process: str | None, wm_class: str | None,
                      title: str | None) -> Profile | None:
        """Highest-priority profile matching the focused window, if any."""
        candidates = [p for p in self.profiles.values()
                      if p.matches(process, wm_class, title)]
        if not candidates:
            return None
        return max(candidates, key=lambda p: p.priority)

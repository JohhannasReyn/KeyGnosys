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

ID_RE = re.compile(r"^[a-z0-9][a-z0-9-]*$")

#: Tolerance for layout geometry comparisons, in units.
GEOMETRY_EPS = 1e-6

#: Canonical modifier ordering for app-profile shortcut lookup keys.
MODIFIER_ORDER = ("Control", "Alt", "Shift", "Meta")


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

def _check_schema(doc: dict, kind: str, major: int = 1) -> None:
    got = doc.get("schema")
    want = f"mousetrapkeys/{kind}/"
    if not isinstance(got, str) or not got.startswith(want):
        raise DocumentError(f"expected schema {want}{major}, found {got!r}")
    try:
        got_major = int(got[len(want):].split(".")[0])
    except ValueError as exc:
        raise DocumentError(f"unparsable schema version in {got!r}") from exc
    if got_major != major:
        raise DocumentError(
            f"schema major version {got_major} is not supported (this build "
            f"understands {major})"
        )


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
class Key:
    code: str
    x: float
    y: float
    w: float = 1.0
    h: float = 1.0
    base: str = ""
    shift: str | None = None
    sub: str | None = None
    role: str = "normal"

    @property
    def right(self) -> float:
        return self.x + self.w

    @property
    def bottom(self) -> float:
        return self.y + self.h

    def overlaps(self, other: "Key") -> bool:
        """True if two keys share area, not merely an edge.

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
class Layout:
    id: str
    name: str
    description: str
    width: float
    height: float
    keys: list[Key]

    @classmethod
    def from_dict(cls, doc: dict) -> "Layout":
        _check_schema(doc, "layout")
        doc_id = _check_id(doc)

        raw_keys = doc.get("keys")
        if not isinstance(raw_keys, list) or not raw_keys:
            raise DocumentError("'keys' must be a non-empty list")

        keys: list[Key] = []
        for i, rk in enumerate(raw_keys):
            if not isinstance(rk, dict):
                raise DocumentError(f"keys[{i}] is not an object")
            code = rk.get("code")
            if not isinstance(code, str) or not code:
                raise DocumentError(f"keys[{i}] has no 'code'")
            legend = rk.get("legend")
            if not isinstance(legend, dict) or "base" not in legend:
                raise DocumentError(f"keys[{i}] ({code}) has no 'legend.base'")
            try:
                keys.append(Key(
                    code=code,
                    x=float(rk["x"]), y=float(rk["y"]),
                    w=float(rk.get("w", 1.0)), h=float(rk.get("h", 1.0)),
                    base=str(legend["base"]),
                    shift=legend.get("shift"),
                    sub=legend.get("sub"),
                    role=rk.get("role", "normal"),
                ))
            except (KeyError, TypeError, ValueError) as exc:
                raise DocumentError(f"keys[{i}] ({code}) has bad geometry: {exc}") from exc

        # Two keys sharing a code AND overlapping means the layout is drawing
        # the same physical key twice, which would double-render feedback.
        # Distinct codes may legitimately share neither, and duplicate codes at
        # different positions are fine (a board with two Fn keys, say).
        by_code: dict[str, list[Key]] = {}
        for k in keys:
            by_code.setdefault(k.code, []).append(k)
        for code, group in by_code.items():
            for a, b in zip(group, group[1:]):
                if a.overlaps(b):
                    raise DocumentError(f"duplicate overlapping keys for {code}")

        size = doc.get("size") or {}
        width = float(size.get("w") or max((k.right for k in keys), default=1.0))
        height = float(size.get("h") or max((k.bottom for k in keys), default=1.0))

        return cls(
            id=doc_id,
            name=str(doc.get("name") or doc_id),
            description=str(doc.get("description") or ""),
            width=width, height=height, keys=keys,
        )

    def codes(self) -> set[str]:
        return {k.code for k in self.keys}


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
    action: str
    params: dict[str, Any] = field(default_factory=dict)
    legend: str | None = None


@dataclass
class BindingSet:
    id: str
    name: str
    description: str
    settings: dict[str, Any]
    bindings: dict[str, Binding]

    @classmethod
    def from_dict(cls, doc: dict, sink: "DiagnosticSink | None" = None,
                  origin: str | None = None) -> "BindingSet":
        _check_schema(doc, "bindings")
        doc_id = _check_id(doc)

        raw = doc.get("bindings")
        if not isinstance(raw, dict):
            raise DocumentError("'bindings' must be an object")

        # Imported lazily: actions.py imports nothing from here, but keeping the
        # import local documents that the catalog is the authority on validity.
        from .actions import validate_binding

        bindings: dict[str, Binding] = {}
        for code, spec in raw.items():
            if not isinstance(spec, dict) or "action" not in spec:
                if sink:
                    sink.warn("binding.unknown_action",
                              f"binding for {code} is not an action object", origin)
                continue
            action = str(spec["action"])
            params = spec.get("params") or {}
            problem = validate_binding(action, params)
            if problem:
                # A bad binding is skipped; every other binding still loads.
                if sink:
                    sink.warn("binding.unknown_action",
                              f"{code}: {problem}", origin)
                continue
            bindings[code] = Binding(action=action, params=params,
                                     legend=spec.get("legend"))

        settings = dict(DEFAULT_BINDING_SETTINGS)
        settings.update(doc.get("settings") or {})

        return cls(
            id=doc_id,
            name=str(doc.get("name") or doc_id),
            description=str(doc.get("description") or ""),
            settings=settings,
            bindings=bindings,
        )


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
                if kind == "bindings":
                    obj = BindingSet.from_dict(doc, self.diagnostics, str(path))
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

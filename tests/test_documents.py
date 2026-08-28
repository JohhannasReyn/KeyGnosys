"""Every bundled document must load, and one bad file must not break the rest."""

from __future__ import annotations

import json

import pytest

from keygnosys import paths
from keygnosys.documents import (
    Binding, BindingSet, DiagnosticSink, DocumentError, Layout, Profile,
    Registry, Segment, Theme, canonical_modifiers,
)


def _flat_key(code: str = "KeyA", x: float = 0) -> dict:
    """A schema-1 style key: geometry directly on the key, no segments."""
    return {"code": code, "x": x, "y": 0, "legend": {"base": code[-1]}}


@pytest.fixture(scope="module")
def registry() -> Registry:
    return Registry().load_all()


# -- bundled content --------------------------------------------------------

def test_all_bundled_documents_load(registry: Registry) -> None:
    errors = [d for d in registry.diagnostics if d.level in ("warn", "error")]
    assert not errors, "bundled documents must load cleanly:\n" + "\n".join(
        str(e) for e in errors)


def test_expected_layouts_present(registry: Registry) -> None:
    for layout_id in ("us-ansi-104", "us-iso-105", "thinkpad-compact",
                      "asus-zenbook", "asus-vivobook-s"):
        assert layout_id in registry.layouts


def test_key_counts_match_their_names(registry: Registry) -> None:
    """The 104/105 distinction is the point of shipping both, so assert it."""
    assert len(registry.layouts["us-ansi-104"].keys) == 104
    assert len(registry.layouts["us-iso-105"].keys) == 105


def test_no_layout_has_overlapping_keys(registry: Registry) -> None:
    """Between *different* logical keys. A key's own segments are exempt."""
    for layout in registry.layouts.values():
        keys = layout.keys
        for i, a in enumerate(keys):
            for b in keys[i + 1:]:
                assert not a.overlaps(b), (
                    f"{layout.id}: {a.id} overlaps {b.id}")


def test_keys_stay_within_declared_size(registry: Registry) -> None:
    for layout in registry.layouts.values():
        for key in layout.keys:
            assert key.right <= layout.width + 1e-6, f"{layout.id}: {key.code}"
            assert key.bottom <= layout.height + 1e-6, f"{layout.id}: {key.code}"


def test_default_bindings_reference_real_keys(registry: Registry) -> None:
    """Every bound key must exist on at least one shipped layout.

    A binding nothing can draw is invisible on the map, which defeats the
    overlay's whole purpose.
    """
    all_codes: set[str] = set()
    for layout in registry.layouts.values():
        all_codes |= layout.codes()
    unknown = sorted(set(registry.bindings["default"].bindings) - all_codes)
    assert not unknown, f"bound keys on no layout: {unknown}"


def test_profiles_canonicalise_modifier_combinations(registry: Registry) -> None:
    for profile in registry.profiles.values():
        for combo in profile.shortcuts:
            assert combo == canonical_modifiers(combo.split("+")), (
                f"{profile.id}: {combo!r} is not in canonical order")


# -- validation -------------------------------------------------------------

def test_layout_rejects_unknown_schema_major() -> None:
    with pytest.raises(DocumentError, match="not supported"):
        Layout.from_dict({"schema": "keygnosys/layout/9", "id": "x",
                          "keys": [_flat_key()]})


def test_layout_rejects_bad_id() -> None:
    with pytest.raises(DocumentError, match="id must match"):
        Layout.from_dict({"schema": "keygnosys/layout/2", "id": "Not Valid",
                          "keys": [_flat_key()]})


def test_layout_rejects_empty_keys() -> None:
    with pytest.raises(DocumentError, match="non-empty"):
        Layout.from_dict({"schema": "keygnosys/layout/2", "id": "x",
                          "keys": []})


def test_bad_key_is_dropped_but_the_layout_still_loads() -> None:
    """A key-level failure must never blank the whole keyboard (P6)."""
    sink = DiagnosticSink()
    layout = Layout.from_dict({
        "schema": "keygnosys/layout/2", "id": "x",
        "keys": [
            _flat_key("KeyA", 0),
            {"code": "KeyB", "segments": [{"x": 1, "y": 0}]},   # no legend
            {"code": "KeyC", "legend": {"base": "C"},
             "segments": [{"x": 2, "y": 0, "w": 0}]},           # zero width
            _flat_key("KeyD", 3),
        ],
    }, sink)
    assert [k.code for k in layout.keys] == ["KeyA", "KeyD"]
    assert sum(1 for d in sink if d.code == "layout.key_invalid") == 2


def test_layout_with_no_usable_keys_is_rejected() -> None:
    with pytest.raises(DocumentError, match="no valid keys"):
        Layout.from_dict({"schema": "keygnosys/layout/2", "id": "x",
                          "keys": [{"code": "KeyA", "segments": []}]})


def test_theme_rejects_missing_tokens() -> None:
    with pytest.raises(DocumentError, match="missing colour tokens"):
        Theme.from_dict({"schema": "keygnosys/theme/1", "id": "x",
                         "tokens": {"surface": "#000000"}})


def test_profile_rejects_unknown_modifier() -> None:
    with pytest.raises(DocumentError, match="unknown modifiers"):
        Profile.from_dict({"schema": "keygnosys/profile/1", "id": "x",
                           "shortcuts": {"Hyper": {"KeyA": "nope"}}})


def test_bad_binding_is_skipped_but_the_document_still_loads() -> None:
    """Principle P6: one bad entry never takes the rest down with it."""
    doc = {
        "schema": "keygnosys/bindings/1",
        "id": "mixed",
        "bindings": {
            "KeyH": {"action": "pointer.move", "params": {"dir": "left"}},
            "KeyJ": {"action": "pointer.teleport", "params": {}},
            "KeyK": {"action": "pointer.move", "params": {"dir": "sideways"}},
            "KeyL": {"action": "pointer.move", "params": {"dir": "right"}},
        },
    }
    bindings = BindingSet.from_dict(doc)
    assert set(bindings.bindings) == {"KeyH", "KeyL"}


def test_one_unreadable_file_does_not_stop_the_rest(tmp_path, monkeypatch) -> None:
    good = {
        "schema": "keygnosys/layout/1", "id": "good", "name": "Good",
        "keys": [{"code": "KeyA", "x": 0, "y": 0, "legend": {"base": "A"}}],
    }
    (tmp_path / "layouts").mkdir()
    (tmp_path / "layouts" / "good.json").write_text(json.dumps(good))
    (tmp_path / "layouts" / "broken.json").write_text("{ not json at all")

    monkeypatch.setattr(paths, "search_dirs",
                        lambda kind: [tmp_path / kind] if kind == "layouts" else [])
    reg = Registry().load_all()

    assert "good" in reg.layouts
    assert any(d.code == "layout.invalid" for d in reg.diagnostics)


def test_user_document_shadows_bundled_one_by_id(tmp_path, monkeypatch) -> None:
    def make(directory, name):
        directory.mkdir(parents=True, exist_ok=True)
        (directory / "layout.json").write_text(json.dumps({
            "schema": "keygnosys/layout/1", "id": "shared", "name": name,
            "keys": [{"code": "KeyA", "x": 0, "y": 0, "legend": {"base": "A"}}],
        }))

    make(tmp_path / "bundled", "Bundled")
    make(tmp_path / "user", "User")
    monkeypatch.setattr(
        paths, "search_dirs",
        lambda kind: [tmp_path / "bundled", tmp_path / "user"]
        if kind == "layouts" else [])

    reg = Registry().load_all()
    assert reg.layouts["shared"].name == "User"

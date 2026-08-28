"""Segmented keys: a key is one thing however many rectangles draw it.

See docs/SPEC.md sections 4.1.3 (the one-key rule) and 4.1.5 (validation).
"""

from __future__ import annotations

import json

import pytest

from keygnosys.documents import (
    DiagnosticSink, DocumentError, Key, Layout, Registry, Segment,
)


@pytest.fixture(scope="module")
def registry() -> Registry:
    return Registry().load_all()


def _doc(keys: list[dict], **extra) -> dict:
    return {"schema": "keygnosys/layout/2", "id": "t", "keys": keys, **extra}


def _seg(x, y, w=1.0, h=1.0, sid="s0") -> dict:
    return {"id": sid, "x": x, "y": y, "w": w, "h": h}


# -- the shipped ISO Enter --------------------------------------------------

def test_iso_enter_is_two_segments_and_ansi_enter_is_one(registry) -> None:
    iso = registry.layouts["us-iso-105"].key_by_id("enter")
    ansi = registry.layouts["us-ansi-104"].key_by_id("enter")
    assert len(iso.segments) == 2
    assert len(ansi.segments) == 1


def test_iso_enter_upper_part_reaches_further_left(registry) -> None:
    """That overhang is the whole shape of an ISO Enter.

    The row above Enter holds one fewer key, so the upper segment starts
    0.25u to the left of the lower one. Get this backwards and the L points
    the wrong way.
    """
    upper, lower = sorted(registry.layouts["us-iso-105"].key_by_id("enter").segments,
                          key=lambda s: s.y)
    assert upper.x < lower.x
    assert upper.w > lower.w
    assert upper.right == pytest.approx(lower.right)


def test_iso_enter_segments_are_connected(registry) -> None:
    upper, lower = sorted(registry.layouts["us-iso-105"].key_by_id("enter").segments,
                          key=lambda s: s.y)
    assert upper.bottom == pytest.approx(lower.y), "segments must share an edge"


def test_only_the_iso_board_has_a_multi_segment_key(registry) -> None:
    for layout in registry.layouts.values():
        shaped = [k.id for k in layout.keys if len(k.segments) > 1]
        expected = ["enter"] if layout.id == "us-iso-105" else []
        assert shaped == expected, f"{layout.id}: {shaped}"


def test_iso_left_shift_is_not_l_shaped(registry) -> None:
    """It is a short rectangle with a separate key beside it, not an L."""
    layout = registry.layouts["us-iso-105"]
    shift = layout.key_by_id("shift-left")
    extra = layout.key_by_id("intl-backslash")
    assert len(shift.segments) == 1
    assert extra is not None and extra.code == "IntlBackslash"
    assert shift.right == pytest.approx(extra.x)


# -- the one-key rule -------------------------------------------------------

def test_bounding_box_spans_every_segment() -> None:
    key = Key(id="k", code="Enter", base="Enter", segments=[
        Segment("s0", 13.5, 2.25, 1.5, 1.0),
        Segment("s1", 13.75, 3.25, 1.25, 1.0),
    ])
    assert key.x == 13.5 and key.y == 2.25
    assert key.right == 15.0 and key.bottom == 4.25
    assert key.w == 1.5 and key.h == 2.0


def test_a_point_in_any_segment_hits_the_key() -> None:
    key = Key(id="k", code="Enter", base="Enter", segments=[
        Segment("s0", 13.5, 2.25, 1.5, 1.0),
        Segment("s1", 13.75, 3.25, 1.25, 1.0),
    ])
    assert key.contains(13.6, 2.5)      # upper only
    assert key.contains(14.0, 3.5)      # lower only
    assert not key.contains(13.6, 3.5)  # the notch: inside the bbox, not the key


def test_label_goes_on_the_largest_segment() -> None:
    """The centre of an L's bounding box is in the notch, outside the key."""
    key = Key(id="k", code="Enter", base="Enter", segments=[
        Segment("s0", 13.5, 2.25, 1.5, 1.0),
        Segment("s1", 13.75, 3.25, 1.25, 1.0),
    ])
    assert key.largest_segment.id == "s0"


def test_translate_moves_every_segment_together() -> None:
    key = Key(id="k", code="Enter", base="Enter", segments=[
        Segment("s0", 10.0, 0.0, 1.5, 1.0),
        Segment("s1", 10.25, 1.0, 1.25, 1.0),
    ])
    key.translate(2.0, 3.0)
    assert [(s.x, s.y) for s in key.segments] == [(12.0, 3.0), (12.25, 4.0)]


# -- validation -------------------------------------------------------------

def test_a_keys_own_segments_may_overlap_without_warning() -> None:
    """Sharing an edge is how an L is assembled, so it cannot be a problem."""
    sink = DiagnosticSink()
    layout = Layout.from_dict(_doc([
        {"code": "Enter", "legend": {"base": "Enter"},
         "segments": [_seg(0, 0, 1.5, 1, "a"), _seg(0.25, 1, 1.25, 1, "b")]},
    ]), sink)
    assert len(layout.keys) == 1
    assert not [d for d in sink if d.code == "layout.overlap"]


def test_overlapping_different_keys_warn_but_still_load() -> None:
    sink = DiagnosticSink()
    layout = Layout.from_dict(_doc([
        {"code": "KeyA", "legend": {"base": "A"}, "segments": [_seg(0, 0)]},
        {"code": "KeyB", "legend": {"base": "B"}, "segments": [_seg(0.5, 0)]},
    ]), sink)
    assert len(layout.keys) == 2, "a warning must never drop a key"
    assert [d.code for d in sink if d.code == "layout.overlap"]


def test_keys_that_merely_share_an_edge_do_not_warn() -> None:
    sink = DiagnosticSink()
    Layout.from_dict(_doc([
        {"code": "KeyA", "legend": {"base": "A"}, "segments": [_seg(0, 0)]},
        {"code": "KeyB", "legend": {"base": "B"}, "segments": [_seg(1, 0)]},
    ]), sink)
    assert not [d for d in sink if d.code == "layout.overlap"]


def test_duplicate_physical_codes_warn() -> None:
    sink = DiagnosticSink()
    layout = Layout.from_dict(_doc([
        {"id": "a", "code": "KeyA", "legend": {"base": "A"},
         "segments": [_seg(0, 0)]},
        {"id": "b", "code": "KeyA", "legend": {"base": "A"},
         "segments": [_seg(2, 0)]},
    ]), sink)
    assert len(layout.keys) == 2
    assert [d for d in sink if d.code == "layout.duplicate_code"]


def test_unknown_codes_warn_but_render() -> None:
    sink = DiagnosticSink()
    layout = Layout.from_dict(_doc([
        {"code": "VendorKey", "legend": {"base": "?"}, "segments": [_seg(0, 0)]},
    ]), sink)
    assert len(layout.keys) == 1
    assert [d for d in sink if d.code == "layout.unknown_code"]


def test_out_of_bounds_segments_warn() -> None:
    sink = DiagnosticSink()
    Layout.from_dict(_doc(
        [{"code": "KeyA", "legend": {"base": "A"}, "segments": [_seg(0, 0, 5, 1)]}],
        size={"w": 2, "h": 1}), sink)
    assert [d for d in sink if d.code == "layout.out_of_bounds"]


def test_negative_and_non_finite_dimensions_drop_the_key() -> None:
    for bad in ({"w": -1}, {"h": 0}, {"w": float("inf")}):
        seg = _seg(0, 0)
        seg.update(bad)
        with pytest.raises(DocumentError):
            Layout.from_dict(_doc([
                {"code": "KeyA", "legend": {"base": "A"}, "segments": [seg]},
            ]))


# -- identity and schema ----------------------------------------------------

def test_ids_are_synthesised_when_absent() -> None:
    layout = Layout.from_dict(_doc([
        {"code": "KeyA", "legend": {"base": "A"}, "segments": [{"x": 0, "y": 0}]},
        {"code": "KeyB", "legend": {"base": "B"}, "segments": [{"x": 1, "y": 0}]},
    ]))
    assert [k.id for k in layout.keys] == ["k0", "k1"]
    assert layout.keys[0].segments[0].id == "s0"


def test_every_bundled_key_and_segment_id_is_unique(registry) -> None:
    for layout in registry.layouts.values():
        ids = [k.id for k in layout.keys]
        assert len(ids) == len(set(ids)), f"{layout.id} has duplicate key ids"
        for key in layout.keys:
            seg_ids = [s.id for s in key.segments]
            assert len(seg_ids) == len(set(seg_ids)), f"{layout.id}/{key.id}"


def test_duplicate_key_ids_drop_the_later_one() -> None:
    sink = DiagnosticSink()
    layout = Layout.from_dict(_doc([
        {"id": "dup", "code": "KeyA", "legend": {"base": "A"},
         "segments": [_seg(0, 0)]},
        {"id": "dup", "code": "KeyB", "legend": {"base": "B"},
         "segments": [_seg(1, 0)]},
    ]), sink)
    assert [k.code for k in layout.keys] == ["KeyA"]
    assert [d for d in sink if d.code == "layout.key_invalid"]


def test_schema_1_upgrades_to_a_single_segment_per_key() -> None:
    """A pre-segment document must behave exactly as it always did."""
    sink = DiagnosticSink()
    layout = Layout.from_dict({
        "schema": "keygnosys/layout/1", "id": "old",
        "keys": [{"code": "KeyA", "x": 1.5, "y": 2.0, "w": 2.25, "h": 1,
                  "legend": {"base": "A"}}],
    }, sink)
    key = layout.keys[0]
    assert layout.schema_major == 1
    assert len(key.segments) == 1
    assert (key.x, key.y, key.w, key.h) == (1.5, 2.0, 2.25, 1.0)
    assert [d.code for d in sink if d.code == "layout.upgraded"]


def test_schema_1_and_2_produce_identical_geometry() -> None:
    old = Layout.from_dict({
        "schema": "keygnosys/layout/1", "id": "a",
        "keys": [{"code": "KeyA", "x": 3, "y": 4, "w": 1.75,
                  "legend": {"base": "A"}}],
    })
    new = Layout.from_dict(_doc([
        {"code": "KeyA", "legend": {"base": "A"},
         "segments": [_seg(3, 4, 1.75, 1)]},
    ]))
    a, b = old.keys[0], new.keys[0]
    assert (a.x, a.y, a.w, a.h) == (b.x, b.y, b.w, b.h)


def test_loading_an_old_document_never_rewrites_it(tmp_path, monkeypatch) -> None:
    """Upgrades happen in memory. The file on disk is not touched (SPEC 3.4.4)."""
    from keygnosys import paths

    directory = tmp_path / "layouts"
    directory.mkdir()
    path = directory / "old.json"
    original = json.dumps({
        "schema": "keygnosys/layout/1", "id": "old",
        "keys": [{"code": "KeyA", "x": 0, "y": 0, "legend": {"base": "A"}}],
    })
    path.write_text(original)
    before = path.stat().st_mtime_ns

    monkeypatch.setattr(paths, "search_dirs",
                        lambda kind: [directory] if kind == "layouts" else [])
    Registry().load_all()

    assert path.read_text() == original
    assert path.stat().st_mtime_ns == before


def test_per_key_style_overrides_survive_a_load() -> None:
    layout = Layout.from_dict(_doc([
        {"code": "KeyA", "legend": {"base": "A"}, "segments": [_seg(0, 0)],
         "style": {"face": "#112233", "accent": "#ff7043"}},
        {"code": "KeyB", "legend": {"base": "B"}, "segments": [_seg(1, 0)],
         "style": {}},
    ]))
    assert layout.keys[0].style.face == "#112233"
    assert layout.keys[0].style.accent == "#ff7043"
    assert layout.keys[0].style.text is None
    # An override object with nothing in it is the same as no override.
    assert layout.keys[1].style is None

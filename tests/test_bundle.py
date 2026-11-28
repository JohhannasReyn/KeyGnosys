"""The bundle module: read, validate and write keygnosys/bundle/1."""

from __future__ import annotations

import copy
import json
import os
from pathlib import Path

import pytest

import keygnosys
from keygnosys.bundle import (BUNDLE_SCHEMA, MAX_BYTES, Finding,
                              ImportRefused, diff, export_bundle, read_import,
                              write_bundle)
from keygnosys.actions import default_legend
from keygnosys.documents import Binding, BindingSet

DEFAULT = Path(__file__).resolve().parent.parent / "data" / "bindings" / "default.json"


def _default() -> BindingSet:
    return BindingSet.from_dict(json.loads(DEFAULT.read_text("utf-8")))


def _doc(**over) -> dict:
    doc = {"schema": "keygnosys/bindings/2", "id": "mine", "name": "Mine",
           "bindings": {"KeyD": {"action": "button.click",
                                 "params": {"button": "left"}}}}
    doc.update(over)
    return doc


def _bundle(*docs, **over) -> dict:
    b = {"schema": BUNDLE_SCHEMA, "name": "B", "author": "",
         "exported": "2026-11-27T00:00:00Z", "generator": "x",
         "documents": list(docs)}
    b.update(over)
    return b


def _write(tmp_path, obj, name="f.keygnosys.json") -> Path:
    p = tmp_path / name
    p.write_text(json.dumps(obj), encoding="utf-8")
    return p


def _refused(path, **kw) -> list[str]:
    with pytest.raises(ImportRefused) as err:
        read_import(path, **kw)
    return err.value.reasons


def _has(reasons, text) -> bool:
    return any(text in r for r in reasons)


def test_export_then_read_round_trips(tmp_path):
    bset = _default()
    bset.extra_bindings["F7"] = {"action": "window.snap"}
    bset.metadata.extra["x"] = 1
    path = tmp_path / "out.keygnosys.json"
    write_bundle(path, export_bundle(bset))
    assert read_import(path).bset.to_dict() == bset.to_dict()


def test_export_header():
    bset = _default()
    before = copy.deepcopy(bset.to_dict())
    b = export_bundle(bset)
    assert b["schema"] == BUNDLE_SCHEMA
    assert b["name"] == bset.name
    assert b["author"] == ""
    assert b["exported"].endswith("Z")
    assert b["generator"] == f"KeyGnosys {keygnosys.__version__}"
    assert len(b["documents"]) == 1
    signed = export_bundle(bset, author="Sam")
    assert signed["author"] == "Sam"
    assert (signed["documents"][0]["metadata"]["author"]
            == before["metadata"]["author"])
    assert bset.to_dict() == before


@pytest.mark.parametrize("major", [1, 2])
def test_reads_bare_bindings_v1_and_v2(tmp_path, major):
    plan = read_import(_write(tmp_path, _doc(schema=f"keygnosys/bindings/{major}")))
    assert plan.name == "Mine"
    assert plan.author == ""
    assert plan.exported is None


def test_refuses_not_json(tmp_path):
    p = tmp_path / "f.json"
    p.write_text("{nope", encoding="utf-8")
    assert _has(_refused(p), "not a KeyGnosys file")


def test_refuses_non_utf8(tmp_path):
    p = tmp_path / "f.json"
    p.write_bytes(b"\xff\xfe\x00")
    assert _has(_refused(p), "not a KeyGnosys file")


def test_refuses_missing_file(tmp_path):
    assert _has(_refused(tmp_path / "nope.json"), "not a KeyGnosys file")


def test_refuses_unknown_schema(tmp_path):
    assert _has(_refused(_write(tmp_path, {"schema": "foo/1"})),
                "not a KeyGnosys file")


def test_refuses_non_object(tmp_path):
    assert _has(_refused(_write(tmp_path, [1, 2])), "not a KeyGnosys file")


def test_refuses_newer_bundle_major(tmp_path):
    p = _write(tmp_path, _bundle(_doc(), schema="keygnosys/bundle/2"))
    assert _has(_refused(p), "newer KeyGnosys")


def test_refuses_newer_bindings_major(tmp_path):
    p = _write(tmp_path, _bundle(_doc(schema="keygnosys/bindings/3")))
    assert _has(_refused(p), "newer KeyGnosys")
    p = _write(tmp_path, _doc(schema="keygnosys/bindings/3"), "g.json")
    assert _has(_refused(p), "newer KeyGnosys")


def test_refuses_unknown_document_kind(tmp_path):
    p = _write(tmp_path, _bundle(_doc(), {"schema": "keygnosys/appmap/1"}))
    reasons = _refused(p)
    assert any("appmap" in r and "newer KeyGnosys" in r for r in reasons)


def test_refuses_no_map(tmp_path):
    assert _has(_refused(_write(tmp_path, _bundle())), "no binding map")


def test_refuses_two_maps(tmp_path):
    p = _write(tmp_path, _bundle(_doc(), _doc(id="other")))
    assert _has(_refused(p), "more than one binding map")


def test_refuses_broken_map(tmp_path):
    p = _write(tmp_path, _bundle(_doc(bindings=[])))
    assert _has(_refused(p), "'bindings' must be an object")


def test_refuses_oversize_without_reading(tmp_path, monkeypatch):
    p = tmp_path / "big.json"
    with p.open("wb") as f:
        f.truncate(MAX_BYTES + 1)

    def boom(self):
        raise AssertionError("read_bytes called")
    monkeypatch.setattr(Path, "read_bytes", boom)
    assert _has(_refused(p), "too large")


def test_known_command_with_rejected_params_is_reported_not_refused(tmp_path):
    doc = _doc(bindings={"KeyD": {"action": "button.click",
                                  "params": {"button": "sideways"}}})
    plan = read_import(_write(tmp_path, doc))
    unusable = [f for f in plan.findings if f.kind == "unusable"]
    assert len(unusable) == 1 and "KeyD" in unusable[0].text
    assert "KeyD" in plan.bset.extra_bindings


def test_unknown_command_reported(tmp_path):
    doc = _doc(bindings={"F7": {"action": "window.snap"}})
    plan = read_import(_write(tmp_path, doc))
    (f,) = [f for f in plan.findings if f.kind == "unusable"]
    assert "F7" in f.text and "window.snap" in f.text


def test_unknown_unassigned_command_reported(tmp_path):
    doc = _doc(unassigned=[{"action": "window.snap"}])
    plan = read_import(_write(tmp_path, doc))
    (f,) = [f for f in plan.findings if f.kind == "unusable"]
    assert "window.snap" in f.text


def test_off_layout_keys_reported(tmp_path):
    click = {"action": "button.click", "params": {"button": "left"}}
    doc = _doc(bindings={"KeyD": click, "NumpadAdd": click})
    plan = read_import(_write(tmp_path, doc), layout_codes={"KeyD"})
    (f,) = [f for f in plan.findings if f.kind == "off_layout"]
    assert "NumpadAdd" in f.text and "KeyD" not in f.text
    assert "NumpadAdd" in plan.bset.bindings
    unchecked = read_import(_write(tmp_path, doc, "h.json"))
    assert not [f for f in unchecked.findings if f.kind == "off_layout"]


def test_setting_out_of_bounds_reported(tmp_path):
    doc = _doc(settings={"precision_factor": 5.0, "pointer_ramp_ms": 100})
    plan = read_import(_write(tmp_path, doc))
    (f,) = [f for f in plan.findings if f.kind == "setting"]
    assert isinstance(f, Finding)
    assert "precision_factor" in f.text and "1.0" in f.text
    assert plan.bset.settings["precision_factor"] == 5.0


def test_bundle_author_fills_empty_map_author(tmp_path):
    d = _doc(metadata={"author": ""})
    plan = read_import(_write(tmp_path, _bundle(d, author="Sam")))
    assert plan.author == "Sam"
    d = _doc(metadata={"author": "Lee"})
    plan = read_import(_write(tmp_path, _bundle(d, author="Sam"), "g.json"))
    assert plan.author == "Lee"


def test_bundle_plan_carries_header(tmp_path):
    plan = read_import(_write(tmp_path, _bundle(_doc(), name="Sam's")))
    assert plan.name == "Sam's"
    assert plan.exported == "2026-11-27T00:00:00Z"


def test_write_bundle_is_atomic(tmp_path, monkeypatch):
    path = tmp_path / "out.keygnosys.json"
    path.write_text("old", encoding="utf-8")

    def boom(*a, **k):
        raise OSError("nope")
    monkeypatch.setattr(os, "replace", boom)
    with pytest.raises(OSError):
        write_bundle(path, export_bundle(_default()))
    assert path.read_text("utf-8") == "old"
    assert list(tmp_path.glob("*.tmp")) == []


# -- diff ------------------------------------------------------------------

def _pair():
    old = _default()
    old.bindings["KeyD"] = Binding("button.click", {"button": "left"})
    old.bindings["KeyQ"] = Binding("button.click", {"button": "left"}, "Old")
    old.bindings["KeyE"] = Binding("window.slot", {"index": 1})
    old.bindings["KeyF"] = Binding("button.click", {"button": "left"})
    old.bindings.pop("KeyZ", None)
    old.settings["precision_factor"] = 0.25
    new = copy.deepcopy(old)
    new.bindings["KeyD"] = Binding("button.click", {"button": "right"})
    new.bindings["KeyQ"] = Binding("button.click", {"button": "left"}, "New")
    new.bindings["KeyE"] = Binding("window.slot", {"index": 2})
    del new.bindings["KeyF"]
    new.bindings["KeyZ"] = Binding("button.click", {"button": "left"})
    new.settings["precision_factor"] = 0.5
    return old, new


def _short(code: str) -> str:
    return code.removeprefix("Key")


def test_diff_identical_is_empty():
    assert diff(_default(), _default()) == []


def test_diff_each_kind_once():
    old, new = _pair()
    changes = diff(old, new, display=_short)
    assert len(changes) == 6
    by = {c.name: c for c in changes}
    assert set(by) == {"D", "Q", "E", "F", "Z", "precision_factor"}
    assert by["F"].after == "(nothing)"
    assert by["Z"].before == "—"
    assert by["Q"].before != by["Q"].after
    assert by["E"].before != by["E"].after
    assert by["D"].before != by["D"].after
    assert (by["precision_factor"].before, by["precision_factor"].after) == (
        "0.25", "0.5")
    assert by["precision_factor"].kind == "setting"


def test_diff_follows_layout_order():
    old, new = _pair()
    changes = diff(old, new, display=_short, order=["KeyZ", "KeyD"])
    names = [c.name for c in changes]
    assert names[:2] == ["Z", "D"]
    assert names[2:5] == sorted(["E", "F", "Q"])
    assert names[-1] == "precision_factor"


def test_diff_counts_extra_bindings():
    old = _default()
    new = copy.deepcopy(old)
    new.extra_bindings["F7"] = {"action": "window.snap"}
    changes = diff(old, new)
    assert len(changes) == 1
    assert "window.snap" in changes[0].after


# -- damaged maps (final review I1) ----------------------------------------

_DAMAGED = "This map is damaged and cannot be read."


def _deep(levels: int) -> object:
    value: object = 1
    for _ in range(levels):
        value = [value]
    return value


@pytest.mark.parametrize("over", [
    {"settings": [1]},
    {"settings": "ab"},
    {"unassigned": 5},
    {"metadata": {"revision": float("inf")}},
    {"bindings": {"F7": {"action": "window.snap", "params": {"x": _deep(600)}}}},
], ids=["settings-list", "settings-str", "unassigned-int", "revision-huge",
        "deep-nesting"])
def test_refuses_damaged_map(tmp_path, over):
    doc = _doc(**over)
    p = tmp_path / "f.keygnosys.json"
    # json.dumps writes inf as Infinity; the file says 1e400, as a hand edit
    # would, and json.loads reads that back as inf too.
    p.write_text(json.dumps(_bundle(doc)).replace("Infinity", "1e400"),
                 encoding="utf-8")
    assert _refused(p) == [_DAMAGED]


def test_document_error_keeps_its_own_message(tmp_path):
    p = _write(tmp_path, _bundle(_doc(bindings=[])))
    reasons = _refused(p)
    assert reasons != [_DAMAGED]
    assert _has(reasons, "'bindings' must be an object")


# -- byte order mark (final review M4) -------------------------------------

def test_reads_a_file_with_a_utf8_bom(tmp_path):
    p = tmp_path / "bom.keygnosys.json"
    p.write_bytes(b"\xef\xbb\xbf" + json.dumps(_bundle(_doc())).encode("utf-8"))
    assert read_import(p).bset.name == "Mine"


# -- diff text always differs (final review M2) ----------------------------

def test_diff_legend_set_to_default_text_shows_both_sides():
    old = _default()
    old.bindings["KeyD"] = Binding("button.click", {"button": "left"})
    new = copy.deepcopy(old)
    words = default_legend("button.click", {"button": "left"})
    new.bindings["KeyD"] = Binding("button.click", {"button": "left"}, words)
    (c,) = diff(old, new)
    assert (c.before, c.after) == (f'{words} ""', f'{words} "{words}"')


def test_diff_extra_binding_legend_only_differs():
    old = _default()
    old.extra_bindings["F7"] = {"action": "window.snap", "legend": "A"}
    new = copy.deepcopy(old)
    new.extra_bindings["F7"] = {"action": "window.snap", "legend": "B"}
    (c,) = diff(old, new)
    assert c.before != c.after
    assert '"A"' in c.before and '"B"' in c.after


def test_diff_unknown_fields_only_differs():
    old = _default()
    old.extra_bindings["F7"] = {"action": "window.snap", "x": 1}
    new = copy.deepcopy(old)
    new.extra_bindings["F7"] = {"action": "window.snap", "x": 2}
    old.bindings["KeyD"] = Binding("button.click", {"button": "left"},
                                   extra={"y": 1})
    new.bindings["KeyD"] = Binding("button.click", {"button": "left"},
                                   extra={"y": 2})
    changes = diff(old, new)
    assert len(changes) == 2
    for c in changes:
        assert c.before != c.after

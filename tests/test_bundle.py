"""The bundle module: read, validate and write keygnosys/bundle/1."""

from __future__ import annotations

import copy
import json
import os
from pathlib import Path

import pytest

import keygnosys
from keygnosys.bundle import (BUNDLE_SCHEMA, MAX_BYTES, Finding,
                              ImportRefused, export_bundle, read_import,
                              write_bundle)
from keygnosys.documents import BindingSet

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

"""The binding store: fork on first save, atomic writes, broken-copy guard."""

from __future__ import annotations

import json

import pytest

from keygnosys import binding_store, paths
from keygnosys.documents import Binding

SHIPPED = {
    "schema": "keygnosys/bindings/2",
    "id": "default",
    "name": "Default",
    "metadata": {"author": "KeyGnosys", "revision": 3},
    "bindings": {"KeyW": {"action": "pointer.move", "params": {"dir": "up"}}},
}


@pytest.fixture
def roots(tmp_path, monkeypatch):
    user = tmp_path / "user"
    data = tmp_path / "data"
    (data / "bindings").mkdir(parents=True)
    shipped = data / "bindings" / "default.json"
    shipped.write_text(json.dumps(SHIPPED), encoding="utf-8")
    monkeypatch.setattr(paths, "user_root", lambda: user)
    monkeypatch.setattr(paths, "BUNDLED_ROOT", data)
    return user, shipped


def test_first_save_forks_under_the_same_id_and_leaves_shipped_untouched(roots):
    user, shipped = roots
    before = shipped.read_bytes()
    bset = binding_store.load("default")
    out = binding_store.save(bset)
    assert out == user / "bindings" / "default.json"
    assert out.exists()
    assert json.loads(out.read_text("utf-8"))["metadata"]["source_template"] == "default"
    assert shipped.read_bytes() == before


def test_save_bumps_revision(roots):
    bset = binding_store.load("default")
    original = bset.metadata.revision
    binding_store.save(bset)
    binding_store.save(binding_store.load("default"))
    assert binding_store.load("default").metadata.revision == original + 2


def test_load_prefers_the_user_copy(roots):
    bset = binding_store.load("default")
    bset.name = "Mine"
    binding_store.save(bset)
    assert binding_store.load("default").name == "Mine"
    assert binding_store.shadows_shipped("default")


def test_shadows_shipped_false_without_user_copy(roots):
    assert not binding_store.shadows_shipped("default")


def test_load_of_a_missing_id_is_a_store_error(roots):
    with pytest.raises(binding_store.StoreError):
        binding_store.load("nope")


def test_a_broken_user_copy_is_never_overwritten(roots):
    shipped_set = binding_store.load("default")
    path = binding_store.user_path("default")
    path.parent.mkdir(parents=True)
    path.write_text("{ nope", encoding="utf-8")
    with pytest.raises(binding_store.BrokenUserCopy) as info:
        binding_store.load("default")
    assert info.value.path == path
    with pytest.raises(binding_store.BrokenUserCopy):
        binding_store.save(shipped_set)
    assert path.read_text("utf-8") == "{ nope"


def test_reset_moves_a_broken_copy_aside(roots):
    shipped_set = binding_store.load("default")
    path = binding_store.user_path("default")
    path.parent.mkdir(parents=True)
    path.write_text("{ nope", encoding="utf-8")
    result = binding_store.reset_to_shipped("default")
    assert not path.exists()
    aside = list(path.parent.glob("default.broken-*.json"))
    assert len(aside) == 1
    assert aside[0].read_text("utf-8") == "{ nope"
    assert result.to_dict() == shipped_set.to_dict()


def test_reset_deletes_a_healthy_copy(roots):
    binding_store.save(binding_store.load("default"))
    path = binding_store.user_path("default")
    assert path.exists()
    binding_store.reset_to_shipped("default")
    assert not path.exists()
    assert not list(path.parent.glob("*.broken-*"))


def test_an_invalid_binding_is_refused_before_writing(roots):
    bset = binding_store.load("default")
    bset.bindings["KeyA"] = Binding("pointer.move", {"dir": "sideways"})
    with pytest.raises(binding_store.StoreError):
        binding_store.save(bset)
    assert not binding_store.user_path("default").exists()


def test_a_failed_write_leaves_the_previous_file_intact(roots, monkeypatch):
    bset = binding_store.load("default")
    path = binding_store.save(bset)
    first = json.loads(path.read_text("utf-8"))

    def boom(*a, **k):
        raise RuntimeError("disk on fire")

    monkeypatch.setattr(binding_store.json, "dumps", boom)
    bset.name = "Changed"
    with pytest.raises(RuntimeError):
        binding_store.save(bset)
    assert json.loads(path.read_text("utf-8")) == first
    assert not list(path.parent.glob("*.json.tmp"))


def test_a_failed_replace_removes_its_temp_file(roots, monkeypatch):
    bset = binding_store.load("default")
    path = binding_store.save(bset)
    first = path.read_bytes()

    def boom(*a, **k):
        raise OSError("locked")

    monkeypatch.setattr(binding_store.os, "replace", boom)
    with pytest.raises(OSError):
        binding_store.save(bset)
    assert path.read_bytes() == first
    assert not list(path.parent.glob("*.json.tmp"))


# -- final review fixes -----------------------------------------------------

def test_unmodelled_fields_on_bindings_and_metadata_round_trip(roots):
    user, _shipped = roots
    doc = dict(SHIPPED)
    doc["metadata"] = {"author": "me", "revision": 2, "x-origin": {"tool": "v9"}}
    doc["bindings"] = {
        "KeyW": {"action": "pointer.move", "params": {"dir": "up"},
                 "x-note": "keep me"},
    }
    doc["unassigned"] = [{"action": "overlay.toggle", "params": {},
                          "x-tag": 1}]
    path = user / "bindings" / "default.json"
    path.parent.mkdir(parents=True)
    path.write_text(json.dumps(doc), encoding="utf-8")

    bset = binding_store.load("default")
    assert bset.bindings["KeyW"].extra == {"x-note": "keep me"}
    # Not part of identity: the same command with no extras is the same.
    assert bset.bindings["KeyW"].identity() == \
        Binding("pointer.move", {"dir": "up"}).identity()
    binding_store.save(bset)

    saved = json.loads(path.read_text("utf-8"))
    assert saved["bindings"]["KeyW"]["x-note"] == "keep me"
    assert saved["metadata"]["x-origin"] == {"tool": "v9"}
    assert saved["unassigned"][0]["x-tag"] == 1


def test_save_writes_to_the_id_it_was_loaded_under(roots):
    user, _shipped = roots
    doc = dict(SHIPPED)
    doc["id"] = "custom"                     # the file name and id disagree
    path = user / "bindings" / "default.json"
    path.parent.mkdir(parents=True)
    path.write_text(json.dumps(doc), encoding="utf-8")

    bset = binding_store.load("default")
    assert bset.id == "custom"
    bset.assign("KeyQ", Binding("overlay.toggle"))
    out = binding_store.save(bset, "default")
    assert out == path
    assert not (user / "bindings" / "custom.json").exists()
    saved = json.loads(path.read_text("utf-8"))
    assert saved["id"] == "custom"
    assert "KeyQ" in saved["bindings"]
    # It already existed, so this is not a fork.
    assert "source_template" not in saved["metadata"]


def test_fork_detection_uses_the_given_id(roots):
    user, _shipped = roots
    bset = binding_store.load("default")
    bset.id = "renamed"
    out = binding_store.save(bset, "default")
    assert out == user / "bindings" / "default.json"
    saved = json.loads(out.read_text("utf-8"))
    assert saved["metadata"]["source_template"] == "default"

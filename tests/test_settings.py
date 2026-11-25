"""Settings loading: an old file must keep working after a setting is retired."""

from __future__ import annotations

import json

from keygnosys import paths
from keygnosys.settings import Settings


def test_an_old_settings_file_with_grace_ms_still_loads(tmp_path, monkeypatch):
    path = tmp_path / "settings.json"
    path.write_text(
        json.dumps({"behavior": {"grace_ms": 50, "hybrid_tap_ms": 300}}),
        encoding="utf-8",
    )
    monkeypatch.setattr(paths, "settings_file", lambda: path)

    settings = Settings.load()

    assert settings.get("behavior.hybrid_tap_ms") == 300
    assert settings.clamped == []

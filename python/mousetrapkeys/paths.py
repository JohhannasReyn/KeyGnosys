"""Filesystem locations for bundled and user configuration.

See docs/SPEC.md section 3.1. Bundled documents ship with the package and are
read-only; user documents live in the platform config directory and shadow
bundled ones by id.
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

#: The four document kinds, each of which gets a subdirectory in both roots.
KINDS = ("layouts", "bindings", "themes", "profiles")

#: Bundled data ships alongside the package, two levels up from this file.
BUNDLED_ROOT = Path(__file__).resolve().parent.parent.parent / "data"


def user_root() -> Path:
    """The user's writable configuration root."""
    if sys.platform == "win32":
        base = os.environ.get("APPDATA")
        if base:
            return Path(base) / "MouseTrapKeys"
        return Path.home() / "AppData" / "Roaming" / "MouseTrapKeys"

    base = os.environ.get("XDG_CONFIG_HOME")
    if base:
        return Path(base) / "mousetrapkeys"
    return Path.home() / ".config" / "mousetrapkeys"


def log_root() -> Path:
    if sys.platform == "win32":
        base = os.environ.get("LOCALAPPDATA")
        root = Path(base) if base else Path.home() / "AppData" / "Local"
        return root / "MouseTrapKeys" / "logs"

    base = os.environ.get("XDG_STATE_HOME")
    root = Path(base) if base else Path.home() / ".local" / "state"
    return root / "mousetrapkeys" / "logs"


def ipc_endpoint() -> str:
    """The address the native core listens on. See docs/SPEC.md section 5.1."""
    if sys.platform == "win32":
        return r"\\.\pipe\mousetrapkeys"
    base = os.environ.get("XDG_RUNTIME_DIR") or f"/tmp/mousetrapkeys-{os.getuid()}"
    return str(Path(base) / "mousetrapkeys" / "core.sock")


def settings_file() -> Path:
    return user_root() / "settings.json"


def search_dirs(kind: str) -> list[Path]:
    """Directories to scan for one document kind, lowest precedence first.

    Later directories shadow earlier ones by document id, so the user root is
    returned last.
    """
    if kind not in KINDS:
        raise ValueError(f"unknown document kind: {kind!r}")
    return [BUNDLED_ROOT / kind, user_root() / kind]


def ensure_user_dirs() -> Path:
    """Create the user configuration tree if it does not exist."""
    root = user_root()
    for kind in KINDS:
        (root / kind).mkdir(parents=True, exist_ok=True)
    return root

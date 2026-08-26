"""Clients that feed the overlay with input state.

`CoreClient` is the interface; `MockClient` sources events from Qt itself so the
UI can be developed and tested with no native core, and `IpcClient` speaks the
JSON Lines protocol to the real one.
"""

from .base import CoreClient, KeyState
from .mock import MockClient

__all__ = ["CoreClient", "KeyState", "MockClient"]

"""The interface between the overlay and whatever is producing input state.

Two implementations exist: `MockClient` (Qt key events, for development) and
`IpcClient` (the real native core). The overlay depends only on this interface,
which is what lets the entire UI be built and tested before the core exists.

Signal payloads mirror the IPC events in docs/SPEC.md section 5.3.
"""

from __future__ import annotations

from enum import Enum

from PySide6.QtCore import QObject, Signal


class KeyState(str, Enum):
    DOWN = "down"
    UP = "up"
    REPEAT = "repeat"


class CoreClient(QObject):
    """Abstract source of input and window state."""

    #: (hello payload) -- core version, platform, backends, limitations
    connected = Signal(dict)
    #: (reason)
    disconnected = Signal(str)

    #: (code, state, suppressed)
    keyEvent = Signal(str, str, bool)
    #: (cursor_layer_engaged, latched)
    modeChanged = Signal(bool, bool)
    #: (payload with process / wm_class / title)
    focusChanged = Signal(dict)
    #: (list of slot payloads)
    windowsChanged = Signal(list)
    #: (payload with level / code / message / file)
    diagnostic = Signal(dict)

    #: Human-readable name shown in the UI so it is never ambiguous which
    #: backend is live -- a mock that looks like the real thing is a trap.
    name = "abstract"

    def start(self) -> None:
        raise NotImplementedError

    def stop(self) -> None:
        raise NotImplementedError

    # -- commands (SPEC section 5.4) --------------------------------------

    def set_activation_mode(self, mode: str) -> None:
        """Override in clients that can actually change core behaviour."""

    def set_enabled(self, enabled: bool) -> None:
        """Master switch for interception."""

    def release_all(self) -> None:
        """Panic button: release every held key, button and drag lock."""

    def reload_config(self) -> None:
        """Ask the core to re-read configuration from disk."""

    @property
    def limitations(self) -> list[str]:
        """Things this backend genuinely cannot do, shown in the UI.

        Principle P6: an unavailable capability is reported, never faked.
        """
        return []

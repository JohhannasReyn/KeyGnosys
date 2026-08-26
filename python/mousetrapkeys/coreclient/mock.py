"""A core client backed by Qt's own key handling.

This exists so the entire overlay -- rendering, legend layers, feedback,
theming, click-through -- can be built, demonstrated and tested before the
native core exists, with no elevation and nothing platform-specific.

Its limits are inherent, and the UI states them plainly whenever it is active
(SPEC section 9.6):

  * it only sees keys while the application has keyboard focus;
  * it cannot suppress anything, so the cursor layer draws but does not drive;
  * it has no window, monitor or focused-application information.

It is a development tool, not a degraded mode of the product.
"""

from __future__ import annotations

import platform

from PySide6.QtCore import QEvent, QObject, Qt

from .base import CoreClient, KeyState
from .keymap import qt_key_to_code


class MockClient(CoreClient):
    name = "mock"

    def __init__(self, app: QObject, activation_mode: str = "hybrid") -> None:
        super().__init__()
        self._app = app
        self._activation_mode = activation_mode
        self._cursor_layer = False
        self._down: set[str] = set()
        self._filter_installed = False

    # -- lifecycle --------------------------------------------------------

    def start(self) -> None:
        if not self._filter_installed:
            self._app.installEventFilter(self)
            self._filter_installed = True
        self.connected.emit({
            "core_version": "mock",
            "protocol": "1.0",
            "platform": platform.system().lower(),
            "backends": {"input": "qt", "output": "none", "window": "none"},
            "capabilities": ["key_events"],
            "limitations": self.limitations,
        })

    def stop(self) -> None:
        if self._filter_installed:
            self._app.removeEventFilter(self)
            self._filter_installed = False
        self.release_all()
        self.disconnected.emit("stopped")

    @property
    def limitations(self) -> list[str]:
        return [
            "Only sees keys while this application has focus.",
            "Cannot suppress keys, so the cursor layer is a preview only.",
            "No window, monitor or focused-application information.",
        ]

    # -- commands ---------------------------------------------------------

    def set_activation_mode(self, mode: str) -> None:
        self._activation_mode = mode

    def release_all(self) -> None:
        for code in sorted(self._down):
            self.keyEvent.emit(code, KeyState.UP.value, False)
        self._down.clear()
        if self._cursor_layer:
            self._set_layer(False)

    # -- event plumbing ---------------------------------------------------

    def eventFilter(self, obj: QObject, event: QEvent) -> bool:
        etype = event.type()
        if etype not in (QEvent.KeyPress, QEvent.KeyRelease):
            return False

        code = qt_key_to_code(event.key(), event.modifiers(),
                              event.nativeScanCode())
        if code is None:
            return False

        if etype == QEvent.KeyPress:
            if event.isAutoRepeat():
                self.keyEvent.emit(code, KeyState.REPEAT.value,
                                   self._cursor_layer)
                return False
            self._on_press(code)
        else:
            if event.isAutoRepeat():
                return False
            self._on_release(code)

        # Never consume the event: the mock explicitly does not suppress, and
        # swallowing keys here would make the settings UI untypable.
        return False

    def _on_press(self, code: str) -> None:
        if code == "CapsLock":
            self._handle_capslock_press()
            return
        self._down.add(code)
        self.keyEvent.emit(code, KeyState.DOWN.value, self._cursor_layer)

    def _on_release(self, code: str) -> None:
        if code == "CapsLock":
            self._handle_capslock_release()
            return
        self._down.discard(code)
        self.keyEvent.emit(code, KeyState.UP.value, self._cursor_layer)

    def _handle_capslock_press(self) -> None:
        self._down.add("CapsLock")
        self.keyEvent.emit("CapsLock", KeyState.DOWN.value, True)
        if self._activation_mode == "toggle":
            self._set_layer(not self._cursor_layer)
        else:
            # `hold` and `hybrid` both engage on press; they differ only in
            # what release does.
            self._set_layer(True)

    def _handle_capslock_release(self) -> None:
        self._down.discard("CapsLock")
        self.keyEvent.emit("CapsLock", KeyState.UP.value, True)
        if self._activation_mode == "hold":
            self._set_layer(False)
        # `toggle` ignores release. `hybrid` would time the press to decide
        # between latch and momentary; without a real core to own that clock,
        # the mock latches, which is the more useful preview behaviour.

    def _set_layer(self, engaged: bool) -> None:
        if engaged == self._cursor_layer:
            return
        self._cursor_layer = engaged
        latched = engaged and self._activation_mode != "hold"
        self.modeChanged.emit(engaged, latched)

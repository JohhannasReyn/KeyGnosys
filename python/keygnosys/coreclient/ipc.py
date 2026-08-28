"""JSON Lines client for the native core.

`QLocalSocket` covers both transports the spec defines -- named pipes on Windows
and Unix domain sockets on Linux -- so the transport itself needs no platform
branching here. See docs/SPEC.md section 5.

The native core does not exist yet (milestones M2-M4). This client is complete
and correct against the specified protocol, so the day the core starts listening
the overlay connects to it without further work; until then it reports the
endpoint as unavailable and the app falls back to the mock backend.
"""

from __future__ import annotations

import json
import itertools

from PySide6.QtCore import QTimer
from PySide6.QtNetwork import QLocalSocket

from .. import paths
from .base import CoreClient

PROTOCOL_MAJOR = 1
RECONNECT_MS = 2000


class IpcClient(CoreClient):
    name = "ipc"

    def __init__(self, endpoint: str | None = None) -> None:
        super().__init__()
        self._endpoint = endpoint or paths.ipc_endpoint()
        self._socket = QLocalSocket(self)
        self._buffer = bytearray()
        self._ids = itertools.count(1)
        self._hello: dict = {}
        self._want_running = False

        self._socket.readyRead.connect(self._on_ready_read)
        self._socket.errorOccurred.connect(self._on_error)
        self._socket.disconnected.connect(self._on_disconnected)

        self._retry = QTimer(self)
        self._retry.setInterval(RECONNECT_MS)
        self._retry.timeout.connect(self._try_connect)

    # -- lifecycle --------------------------------------------------------

    def start(self) -> None:
        self._want_running = True
        self._try_connect()

    def stop(self) -> None:
        self._want_running = False
        self._retry.stop()
        self._socket.abort()

    def _try_connect(self) -> None:
        if self._socket.state() != QLocalSocket.UnconnectedState:
            return
        self._socket.connectToServer(self._endpoint)

    def _on_error(self, _err) -> None:
        if self._want_running and not self._retry.isActive():
            self._retry.start()

    def _on_disconnected(self) -> None:
        self._buffer.clear()
        self._hello = {}
        self.disconnected.emit("core disconnected")
        if self._want_running:
            self._retry.start()

    @property
    def limitations(self) -> list[str]:
        return list(self._hello.get("limitations", []))

    # -- reading ----------------------------------------------------------

    def _on_ready_read(self) -> None:
        self._buffer += bytes(self._socket.readAll())
        while b"\n" in self._buffer:
            line, _, rest = self._buffer.partition(b"\n")
            self._buffer = bytearray(rest)
            if not line.strip():
                continue
            try:
                msg = json.loads(line.decode("utf-8"))
            except (UnicodeDecodeError, json.JSONDecodeError):
                self.diagnostic.emit({
                    "level": "warn", "code": "ipc.bad_message",
                    "message": "core sent a line that is not valid JSON",
                })
                continue
            self._dispatch(msg)

    def _dispatch(self, msg: dict) -> None:
        if msg.get("t") != "event":
            return                      # replies are handled by their futures
        name = msg.get("n")
        data = msg.get("d") or {}

        if name == "hello":
            self._on_hello(data)
        elif name == "key":
            self.keyEvent.emit(data.get("code", ""), data.get("state", "down"),
                               bool(data.get("suppressed")))
        elif name == "mode":
            self.modeChanged.emit(data.get("mode") == "cursor",
                                  bool(data.get("latched")))
        elif name == "focus":
            self.focusChanged.emit(data)
        elif name == "windows":
            self.windowsChanged.emit(data.get("slots") or [])
        elif name == "overlay_toggle":
            self.overlayToggleRequested.emit()
        elif name == "diagnostic":
            self.diagnostic.emit(data)
        elif name == "shutdown":
            self.disconnected.emit(data.get("reason", "core shut down"))

    def _on_hello(self, data: dict) -> None:
        protocol = str(data.get("protocol", "0"))
        try:
            major = int(protocol.split(".")[0])
        except ValueError:
            major = 0
        if major != PROTOCOL_MAJOR:
            # A partial-compatibility mode would fail in ways the user cannot
            # diagnose. Refusing loudly is the specified behaviour.
            self.diagnostic.emit({
                "level": "error", "code": "ipc.version_mismatch",
                "message": (f"core speaks protocol {protocol}, this build "
                            f"speaks {PROTOCOL_MAJOR}.x -- refusing to connect"),
            })
            self._want_running = False
            self._socket.abort()
            return
        self._hello = data
        self._retry.stop()
        self.connected.emit(data)

    # -- writing ----------------------------------------------------------

    def _send(self, name: str, data: dict | None = None) -> None:
        if self._socket.state() != QLocalSocket.ConnectedState:
            return
        msg = {"v": 1, "t": "command", "n": name,
               "id": f"c{next(self._ids)}", "d": data or {}}
        self._socket.write(json.dumps(msg).encode("utf-8") + b"\n")

    def set_activation_mode(self, mode: str) -> None:
        self._send("set_activation_mode", {"mode": mode})

    def set_enabled(self, enabled: bool) -> None:
        self._send("set_enabled", {"enabled": bool(enabled)})

    def release_all(self) -> None:
        self._send("release_all")

    def reload_config(self) -> None:
        self._send("reload_config")

"""The overlay: a keyboard window and a control bar, moved as one.

Assembles the pieces and owns the wiring between the core client, the app state
and the two windows. Nothing here decides what a key *says* -- that is
``state.AppState`` -- and nothing here paints, which is ``KeyboardView``.
"""

from __future__ import annotations

from PySide6.QtCore import QPoint, Qt, QTimer
from PySide6.QtWidgets import QApplication, QVBoxLayout, QWidget

from .. import paths
from ..coreclient.base import CoreClient
from ..documents import Registry
from ..settings import Settings
from ..state import AppState
from .control_bar import ControlBar
from .keyboard_view import KeyboardView
from .platform import clickthrough
from .theme import ResolvedTheme, system_is_dark

#: Vertical gap between the control bar and the keyboard beneath it.
DOCK_GAP = 4
#: Margin from the bottom of the screen when no saved position exists.
SCREEN_MARGIN = 48


class KeyboardWindow(QWidget):
    """Frameless translucent top-level window holding the keyboard view."""

    def __init__(self, state: AppState) -> None:
        super().__init__()
        self.setWindowTitle("MouseTrapKeys")
        self.setWindowFlags(
            Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.Tool
        )
        self.setAttribute(Qt.WA_TranslucentBackground, True)
        self.setAttribute(Qt.WA_ShowWithoutActivating, True)

        self.view = KeyboardView(state, self)
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.addWidget(self.view)

    def sizeHint(self):
        return self.view.sizeHint()


class Overlay:
    """Coordinates the two windows, the state, and the core client."""

    def __init__(self, registry: Registry, settings: Settings,
                 client: CoreClient) -> None:
        self.registry = registry
        self.settings = settings
        self.client = client
        self.state = AppState()
        self.state.backend_name = client.name

        self.window = KeyboardWindow(self.state)
        self.bar = ControlBar()

        self._apply_documents()
        self._populate_bar()
        self._connect_bar()
        self._connect_client()

        self.window.view.set_fade_ms(
            int(settings.get("appearance.feedback_fade_ms", 220)))

    # -- setup ------------------------------------------------------------

    def _apply_documents(self) -> None:
        layout_id = self.settings.get("behavior.layout")
        layout = self.registry.layout(layout_id)
        if layout is None:
            # The configured layout is gone (uninstalled, renamed, typo).
            # Falling back beats starting with a blank window.
            layout = next(iter(self.registry.layouts.values()), None)
            if layout is not None:
                self.settings.set("behavior.layout", layout.id)
        self.state.layout = layout

        bindings_id = self.settings.get("behavior.bindings")
        self.state.binding_set = (self.registry.binding_set(bindings_id)
                                  or next(iter(self.registry.bindings.values()),
                                          None))

        self.window.view.set_show_numpad(
            bool(self.settings.get("appearance.show_numpad", True)))
        self.window.view.set_scale(float(self.settings.get("appearance.scale", 1.0)))
        self.window.view.set_layout_doc(layout)
        self.apply_theme()

    def apply_theme(self) -> None:
        theme_id = self.settings.get("appearance.theme", "system")
        theme = self.registry.theme(theme_id, system_is_dark())
        if theme is None:
            return
        resolved = ResolvedTheme.build(
            theme,
            opacity=float(self.settings.get("appearance.opacity", 0.85)),
            accent_override=self.settings.get("appearance.accent"),
        )
        self.window.view.set_theme(resolved)
        self.bar.apply_theme(resolved)

    def _populate_bar(self) -> None:
        layouts = sorted(
            ((l.id, l.name) for l in self.registry.layouts.values()),
            key=lambda pair: pair[1])
        themes = [("system", "System")] + sorted(
            ((t.id, t.name) for t in self.registry.themes.values()),
            key=lambda pair: pair[1])
        self.bar.populate(layouts, self.settings.get("behavior.layout"),
                          themes, self.settings.get("appearance.theme"))
        self.bar.set_toggles(
            pinned=bool(self.settings.get("window.pinned", True)),
            click_through=bool(self.settings.get("window.click_through", True)),
            opacity=float(self.settings.get("appearance.opacity", 0.85)),
        )
        reason = clickthrough.unavailable_reason()
        if reason:
            self.bar.disable_click_through(reason)
        self._update_status()

    def _update_status(self) -> None:
        limits = self.client.limitations
        if self.client.name == "mock":
            self.bar.set_status(
                "mock input",
                "Running without the native core.\n\n" + "\n".join(
                    f"• {line}" for line in limits))
        elif self.state.connected:
            self.bar.set_status("core connected", "\n".join(limits))
        else:
            self.bar.set_status(
                "core offline",
                f"Waiting for keygnosys-core on {paths.ipc_endpoint()}")

    # -- wiring -----------------------------------------------------------

    def _connect_bar(self) -> None:
        self.bar.pinToggled.connect(self._on_pin)
        self.bar.clickThroughToggled.connect(self._on_click_through)
        self.bar.opacityChanged.connect(self._on_opacity)
        self.bar.layoutChanged.connect(self._on_layout)
        self.bar.themeChanged.connect(self._on_theme)
        self.bar.hideRequested.connect(self._on_hide)
        self.bar.quitRequested.connect(self._on_quit)
        self.bar.dragged.connect(self._on_dragged)

    def _connect_client(self) -> None:
        self.client.keyEvent.connect(self._on_key)
        self.client.modeChanged.connect(self._on_mode)
        self.client.focusChanged.connect(self._on_focus)
        self.client.windowsChanged.connect(self._on_windows)
        self.client.connected.connect(self._on_connected)
        self.client.disconnected.connect(self._on_disconnected)

    # -- client events ----------------------------------------------------

    def _on_key(self, code: str, key_state: str, _suppressed: bool) -> None:
        if key_state == "repeat":
            return
        self.window.view.note_key_event(code, key_state)
        # A modifier changing swaps the whole legend layer, so the cheap
        # single-key repaint is not enough.
        if code in ("ShiftLeft", "ShiftRight", "ControlLeft", "ControlRight",
                    "AltLeft", "AltRight", "MetaLeft", "MetaRight"):
            self.window.view.update()

    def _on_mode(self, cursor_layer: bool, _latched: bool) -> None:
        self.state.cursor_layer = cursor_layer
        self.window.view.update()

    def _on_focus(self, payload: dict) -> None:
        self.state.focus_process = payload.get("process")
        self.state.focus_wm_class = payload.get("wm_class")
        self.state.focus_title = payload.get("title")
        if self.state.update_profile(self.registry):
            self.window.view.update()

    def _on_windows(self, slots: list) -> None:
        self.state.slot_names = {
            int(s["index"]): s.get("title") or s.get("process") or ""
            for s in slots if s.get("index") is not None
        }
        if self.state.cursor_layer:
            self.window.view.update()

    def _on_connected(self, payload: dict) -> None:
        self.state.connected = True
        self.state.backend_name = payload.get("backends", {}).get("input", "?")
        self._update_status()

    def _on_disconnected(self, _reason: str) -> None:
        self.state.connected = False
        self.state.release_all()
        self.window.view.update()
        self._update_status()

    # -- bar actions ------------------------------------------------------

    def _on_pin(self, pinned: bool) -> None:
        self.settings.set("window.pinned", pinned)
        for widget in (self.window, self.bar):
            widget.setWindowFlag(Qt.WindowStaysOnTopHint, pinned)
            widget.show()
        self._reapply_native_flags()
        self.settings.save()

    def _on_click_through(self, enabled: bool) -> None:
        ok = clickthrough.set_click_through(self.window, enabled)
        if not ok:
            self.bar.disable_click_through(
                clickthrough.unavailable_reason()
                or "Click-through failed on this platform.")
            return
        self.settings.set("window.click_through", enabled)
        self.settings.save()

    def _on_opacity(self, value: float) -> None:
        self.settings.set("appearance.opacity", value)
        self.apply_theme()
        self._save_soon()

    def _on_layout(self, layout_id: str) -> None:
        layout = self.registry.layout(layout_id)
        if layout is None:
            return
        self.settings.set("behavior.layout", layout_id)
        self.state.layout = layout
        self.state.release_all()
        self.window.view.set_layout_doc(layout)
        self.window.adjustSize()
        self._reposition()
        self.settings.save()

    def _on_theme(self, theme_id: str) -> None:
        self.settings.set("appearance.theme", theme_id)
        self.apply_theme()
        self.settings.save()

    def _on_hide(self) -> None:
        visible = not self.window.isVisible()
        self.window.setVisible(visible)
        self.settings.set("appearance.overlay_visible", visible)
        self.settings.save()

    def _on_quit(self) -> None:
        self.shutdown()
        QApplication.instance().quit()

    def _on_dragged(self, top_left: QPoint) -> None:
        self.bar.move(top_left)
        self.window.move(top_left.x(),
                         top_left.y() + self.bar.height() + DOCK_GAP)
        self._save_soon()

    # -- lifecycle --------------------------------------------------------

    def show(self) -> None:
        self.window.adjustSize()
        self.bar.adjustSize()
        self.bar.show()
        if self.settings.get("appearance.overlay_visible", True):
            self.window.show()
        self._reposition()

        # Native window flags need a real window handle, so they are applied
        # after show() rather than in the constructor.
        QTimer.singleShot(0, self._reapply_native_flags)

    def _reapply_native_flags(self) -> None:
        clickthrough.set_no_activate(self.window)
        if self.settings.get("window.click_through", True):
            if not clickthrough.set_click_through(self.window, True):
                self.bar.disable_click_through(
                    clickthrough.unavailable_reason()
                    or "Click-through is unavailable here.")

    def _reposition(self) -> None:
        saved_x = self.settings.get("appearance.position.x")
        saved_y = self.settings.get("appearance.position.y")
        if saved_x is not None and saved_y is not None:
            top_left = QPoint(int(saved_x), int(saved_y))
        else:
            screen = QApplication.primaryScreen().availableGeometry()
            total_h = self.bar.height() + DOCK_GAP + self.window.height()
            top_left = QPoint(
                screen.center().x() - self.window.width() // 2,
                screen.bottom() - total_h - SCREEN_MARGIN,
            )
        self.bar.move(top_left)
        self.window.move(top_left.x(),
                         top_left.y() + self.bar.height() + DOCK_GAP)

    def _save_soon(self) -> None:
        """Debounce writes so dragging does not hammer the disk."""
        if not hasattr(self, "_save_timer"):
            self._save_timer = QTimer()
            self._save_timer.setSingleShot(True)
            self._save_timer.setInterval(600)
            self._save_timer.timeout.connect(self._save_position)
        self._save_timer.start()

    def _save_position(self) -> None:
        pos = self.bar.pos()
        self.settings.set("appearance.position", {"x": pos.x(), "y": pos.y()})
        self.settings.save()

    def shutdown(self) -> None:
        self._save_position()
        self.client.stop()

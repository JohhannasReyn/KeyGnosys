"""Define mode: edit the running binding map from the overlay itself.

The overlay delegates here. This owns the editor, the side picker and the
reload debounce; it saves every edit to the user copy and asks the core to
re-read it. It never sends `set_bindings`: the overlay is a viewer (P4), and
the file on disk is the one source of truth both sides read.
"""

from __future__ import annotations

from pathlib import Path
from typing import TYPE_CHECKING, Callable

from PySide6.QtCore import QStandardPaths, QTimer
from PySide6.QtWidgets import QFileDialog

from .. import binding_store, bundle
from ..binding_editor import BindingEditor
from ..bundle import Change, ImportPlan
from ..documents import Binding, BindingSet
from . import transfer_dialogs
from .binding_picker import BindingPicker, _ask
from .control_bar import RESET_FIRST
from .platform import clickthrough

if TYPE_CHECKING:
    from .overlay import Overlay

#: Quiet time after the last edit before the core is asked to reload.
RELOAD_DEBOUNCE_MS = 300

BROKEN_NOTICE = "Your bindings file has an error ({reason}). Fix it, or use Reset to shipped."
MOCK_NOTICE = "Saved — changes apply when the core is running."
#: The same words when the real core is down (design 6.4.6).
OFFLINE_NOTICE = MOCK_NOTICE
SWITCHED_NOTICE = "The core switched to bindings '{id}'; define mode closed."
IMPORTED_BACKUP_NOTICE = ("Imported '{name}'. Your previous map is saved as "
                          "{backup}.")
IMPORTED_NOTICE = ("Imported '{name}'. Undo or Reset to shipped brings back "
                   "the previous map.")
EXPORTED_NOTICE = "Exported to {file}."
FILE_FILTER = "KeyGnosys files (*.keygnosys.json);;All JSON (*.json)"


class DefineMode:
    def __init__(self, overlay: "Overlay") -> None:
        self._o = overlay
        self.active = False
        self.bindings_id: str | None = None
        self.editor: BindingEditor | None = None
        #: Asked before Reset; tests replace it so no dialog ever opens.
        self.confirm: Callable[[str], bool] = _ask
        # Export and import dialogs, replaced the same way in tests.
        self.choose_open_path: Callable[[], Path | None] = self._choose_open_path
        #: Its argument is the suggested file name.
        self.choose_save_path: Callable[[str], Path | None] = self._choose_save_path
        self.ask_import: Callable[[ImportPlan, list[Change]], bool] = (
            lambda plan, changes: transfer_dialogs.ask_import(
                plan, changes, self._o.window))
        self.ask_export: Callable[[str, int, list[str]], str | None] = (
            lambda name, count, warnings: transfer_dialogs.ask_export(
                name, count, warnings, self._o.window))
        self.picker = BindingPicker(confirm=lambda q: self.confirm(q))
        self._armed: Binding | None = None
        self._click_through_was = True
        self.reload_pending = False
        #: The offline notice has been shown in this disconnected stretch.
        self._offline_noticed = False

        self._reload_timer = QTimer()
        self._reload_timer.setSingleShot(True)
        self._reload_timer.setInterval(RELOAD_DEBOUNCE_MS)
        self._reload_timer.timeout.connect(self._reload_now)

        p = self.picker
        p.assignRequested.connect(self._assign)
        p.armed.connect(self._on_armed)
        p.clearRequested.connect(lambda code: self._edit(
            lambda e: e.clear(code)))
        p.legendEdited.connect(lambda code, text: self._edit(
            lambda e: e.set_legend(code, text)))
        p.settingEdited.connect(lambda name, value: self._edit(
            lambda e: e.set_setting(name, value)))
        p.paramsEdited.connect(lambda code, params: self._edit(
            lambda e: e.set_params(code, params)))
        p.resetRequested.connect(self._reset)

    # -- entering and leaving ---------------------------------------------

    def set_active(self, on: bool) -> None:
        if on:
            self.enter()
        else:
            self.exit()

    def enter(self) -> None:
        if self.active:
            return
        o = self._o
        self.active = True
        # The keyboard must take clicks to be edited. The stored preference
        # is left alone: this is a mode, not a change of mind.
        self._click_through_was = bool(o.settings.get("window.click_through", True))
        clickthrough.set_click_through(o.window, False)

        drawn = o.state.binding_set
        try:
            self.bindings_id = (o.client.bindings_id
                                or o.settings.get("behavior.bindings"))
            self._open_editor()
            o.state.define_mode = True
            o.window.view.set_define_mode(True)
            self.picker.show()
            self.follow()
            self._refresh()
        except Exception as exc:    # noqa: BLE001 -- never leave it half-open
            self._abandon_enter(drawn, exc)

    def _abandon_enter(self, drawn: BindingSet | None, exc: Exception) -> None:
        """Undo a partial enter(): no editor, click-through as it was, and
        the bar back to "Define"."""
        o = self._o
        self.active = False
        self.editor = None
        self._armed = None
        # Nothing was saved, so nothing new waits on a reload; a reload still
        # pending from an earlier session (core down) is kept.
        try:
            self.picker.set_editor(None)
        except Exception:           # noqa: BLE001 -- it may be what failed
            pass
        self.picker.hide()
        o.state.binding_set = drawn
        o.state.define_mode = False
        o.window.view.set_selected(None)
        o.window.view.set_define_mode(False)
        if not clickthrough.set_click_through(o.window, self._click_through_was):
            o.bar.disable_click_through(
                clickthrough.unavailable_reason()
                or "Click-through failed on this platform.")
        o.bar.set_define_mode(False, False, False)
        o.show_notice(f"Define mode could not open — {exc}")

    def exit(self, notice: str | None = None) -> None:
        if not self.active:
            return
        o = self._o
        if notice is None:
            # A label typed but not committed is still the user's edit.
            self.picker.set_selected_key(None)
            if self._reload_timer.isActive():
                self._reload_timer.stop()
                self._reload_now()
        else:
            # Leaving because the map changed under us: nothing more may be
            # saved into a map the core no longer runs.
            self._reload_timer.stop()
            self.reload_pending = False
            self.picker.set_editor(None)
            self.picker.set_selected_key(None)
        self.active = False
        self.editor = None
        self._armed = None
        if not clickthrough.set_click_through(o.window, self._click_through_was):
            o.bar.disable_click_through(
                clickthrough.unavailable_reason()
                or "Click-through failed on this platform.")
        if self._click_through_was != bool(o.settings.get("window.click_through", True)):
            # The user flipped the control while define mode held it off.
            o.settings.set("window.click_through", self._click_through_was)
            o.settings.save()
        self.picker.hide()
        o.state.define_mode = False
        o.window.view.set_selected(None)
        o.window.view.set_define_mode(False)
        o.bar.set_define_mode(False, False, False)
        if notice:
            o.show_notice(notice)

    def _open_editor(self) -> None:
        o = self._o
        try:
            bset = binding_store.load(self.bindings_id)
        except binding_store.BrokenUserCopy as exc:
            # Never fork over the user's file: Reset, which moves it aside,
            # is the only way on.
            self._set_editor(
                None, can_reset=binding_store.shipped_path(self.bindings_id).exists(),
                urgent=True)
            o.show_notice(BROKEN_NOTICE.format(reason=exc.reason))
            return
        except binding_store.StoreError as exc:
            self._set_editor(None, can_reset=False)
            o.show_notice(f"Bindings unavailable — {exc}")
            return
        self._set_editor(bset, binding_store.shadows_shipped(self.bindings_id))

    def _set_editor(self, bset: BindingSet | None, can_reset: bool,
                    urgent: bool = False) -> None:
        self.editor = (BindingEditor(bset, on_change=self._on_binding_edit,
                                     display=self._display)
                       if bset is not None else None)
        if self.editor is not None:
            self._o.state.binding_set = self.editor.bset
        self.picker.set_editor(self.editor)
        self.picker.set_can_reset(can_reset, urgent=urgent)
        self._o.window.view.update()

    def _display(self, code: str) -> str:
        """The key's name as the overlay draws it ("KeyD" -> "D")."""
        layout = self._o.state.layout
        return layout.display_name(code) if layout is not None else code

    # -- user actions -----------------------------------------------------

    def on_key_clicked(self, code: str) -> None:
        if not self.active or self.editor is None or not self.editor.is_target(code):
            return
        armed, self._armed = self._armed, None
        self._o.window.view.set_selected(code)
        # Selecting disarms the picker (it emits armed(None)).
        self.picker.set_selected_key(code)
        if armed is not None:
            self._assign(code, armed)

    def on_binding_dropped(self, code: str, binding: Binding) -> None:
        self._assign(code, binding)

    def undo(self) -> None:
        self._edit(lambda e: e.undo())

    def redo(self) -> None:
        self._edit(lambda e: e.redo())

    def remember_click_through(self, enabled: bool) -> None:
        """The bar's click-through control, flipped while define mode holds
        click-through off: applied and stored on exit."""
        self._click_through_was = enabled

    def _on_armed(self, binding: Binding | None) -> None:
        self._armed = binding

    def _assign(self, code: str, binding: Binding) -> None:
        if self.editor is None or not self.editor.is_target(code):
            return
        if binding.action == "key.passthrough" and "code" not in binding.params:
            # Ruling R6: an armed or dragged passthrough does not know its
            # key until it lands; it passes through the key it lands on.
            binding = Binding(binding.action, {"code": code}, binding.legend)
        self._edit(lambda e: e.assign(code, binding))

    def _edit(self, change: Callable[[BindingEditor], object]) -> None:
        if self.active and self.editor is not None:
            change(self.editor)

    def _reset(self) -> None:
        try:
            bset = binding_store.reset_to_shipped(self.bindings_id)
        except binding_store.StoreError as exc:
            self._o.show_notice(f"Reset failed — {exc}")
            return
        self._set_editor(bset, can_reset=False)
        self._refresh()
        self._reload_timer.stop()
        self._reload_now()

    # -- export and import ------------------------------------------------

    @property
    def can_transfer(self) -> bool:
        """False while there is no editor: the user copy is broken (or
        unreadable), so there is no map to export and nothing may replace
        the file but Reset."""
        return self.active and self.editor is not None

    def export(self) -> None:
        """Write the running map to a bundle file. Changes nothing: no save,
        no reload, no undo entry."""
        if not self.can_transfer:
            self._o.show_notice(RESET_FIRST)
            return
        # The file should match what the user sees, typed label included.
        self.picker.commit_pending()
        bset = self.editor.bset
        warnings = [
            f'{self._display(code)}: unknown command "{spec.get("action")}" '
            f"(exported unchanged)"
            for code, spec in bset.extra_bindings.items()]
        warnings += [
            f'Unassigned: unknown command "{spec.get("action")}" '
            f"(exported unchanged)"
            for spec in bset.extra_unassigned]
        author = self.ask_export(
            bset.name, len(bset.bindings) + len(bset.extra_bindings), warnings)
        if author is None:
            return
        path = self.choose_save_path(f"{bset.name}{bundle.EXTENSION}")
        if path is None:
            return
        path = Path(path)
        try:
            bundle.write_bundle(path, bundle.export_bundle(bset, author))
        except OSError as exc:
            self._o.show_notice(f"Export failed — {exc}")
            return
        self._o.show_notice(EXPORTED_NOTICE.format(file=path.name))

    def import_(self, path: Path | None = None) -> bool:
        """Replace the running map with one from a file, as one undoable
        edit. Returns whether the map was replaced."""
        if not self.can_transfer:
            self._o.show_notice(RESET_FIRST)
            return False
        if path is None:
            path = self.choose_open_path()
            if path is None:
                return False
        # A half-typed label is part of the map being replaced, so Undo of
        # the import brings it back.
        self.picker.commit_pending()
        layout = self._o.state.layout
        try:
            plan = bundle.read_import(
                Path(path), layout.codes() if layout is not None else None)
        except bundle.ImportRefused as exc:
            self._o.show_notice(f"Not imported — {'; '.join(exc.reasons)}")
            return False
        # Ruling R1: the origin's id, before anything rewrites it.
        imported_from = {"id": plan.bset.id, "name": plan.name,
                         "author": plan.author, "exported": plan.exported}
        changes = bundle.diff(
            self.editor.bset, plan.bset, display=self._display,
            order=[k.code for k in layout.keys] if layout is not None else ())
        accepted = self.ask_import(plan, changes)
        if not changes or not accepted:
            # An identical map: the preview said so, and nothing is done.
            return False
        try:
            backup = binding_store.backup(self.bindings_id)
        except binding_store.StoreError as exc:
            self._o.show_notice(f"Not imported — {exc}")
            return False
        # An empty map author is filled from the bundle header.
        plan.bset.metadata.author = plan.author
        # Saves and schedules the reload through _on_binding_edit.
        self.editor.replace_all(plan.bset, imported_from)
        self._o.show_notice(
            IMPORTED_BACKUP_NOTICE.format(name=plan.name, backup=backup.name)
            if backup is not None else IMPORTED_NOTICE.format(name=plan.name))
        return True

    def on_file_dropped(self, path: Path) -> None:
        """A file dropped on the keyboard: the same import, with the same
        preview. Outside define mode it opens define mode for the import and
        closes it again if nothing was imported."""
        if not self.active:
            # Ruling R3: refuse a broken map before anything opens, so
            # nothing is entered and the file is never read.
            bindings_id = (self._o.client.bindings_id
                           or self._o.settings.get("behavior.bindings"))
            try:
                binding_store.load(bindings_id)
            except binding_store.BrokenUserCopy:
                self._o.show_notice(RESET_FIRST)
                return
            except binding_store.StoreError:
                pass    # enter() reports it; import_ then refuses.
            self.enter()
            imported = False
            try:
                imported = self.import_(path)
            finally:
                # Even if the import raised, define mode was opened only
                # for it, so it closes again.
                if not imported:
                    self.exit()
            return
        self.import_(path)

    def _choose_open_path(self) -> Path | None:
        chosen, _ = QFileDialog.getOpenFileName(
            self._o.window, "Import bindings", _documents_dir(), FILE_FILTER)
        return Path(chosen) if chosen else None

    def _choose_save_path(self, suggested: str) -> Path | None:
        chosen, _ = QFileDialog.getSaveFileName(
            self._o.window, "Export bindings",
            str(Path(_documents_dir()) / suggested), FILE_FILTER)
        if not chosen:
            return None
        path = Path(chosen)
        if not path.name.endswith(".json"):
            path = path.with_name(path.name + bundle.EXTENSION)
        return path

    # -- saving and reloading ---------------------------------------------

    def _on_binding_edit(self, bset: BindingSet) -> None:
        o = self._o
        # After undo or redo the editor holds a new object; always re-read.
        o.state.binding_set = self.editor.bset
        saved = True
        try:
            binding_store.save(bset, self.bindings_id)
        except binding_store.BrokenUserCopy as exc:
            saved = False
            o.show_notice(BROKEN_NOTICE.format(reason=exc.reason))
        except (binding_store.StoreError, OSError) as exc:
            saved = False
            o.show_notice(f"Bindings not saved — {exc}")
        o.window.view.update()
        self._refresh()
        if saved:
            if (o.client.name != "mock" and not o.state.connected
                    and not self._offline_noticed):
                self._offline_noticed = True
                o.show_notice(OFFLINE_NOTICE)
            self._reload_timer.start()

    def _reload_now(self) -> None:
        o = self._o
        if o.client.name == "mock":
            # The mock "connects", but no core reads this file.
            o.show_notice(MOCK_NOTICE)
        elif o.state.connected:
            o.client.reload_config()
        else:
            self.reload_pending = True

    def on_connected(self) -> None:
        # The next disconnected stretch gets its own notice.
        self._offline_noticed = False
        # Entered while the core was down, the id came from settings; the
        # core that just connected may be running another map.
        if self._switched_away(self._o.client.bindings_id):
            return
        if self.reload_pending:
            self.reload_pending = False
            self._o.client.reload_config()

    def on_config_changed(self, payload: dict) -> None:
        new_id = payload.get("bindings_id")
        if self.active:
            self._switched_away(new_id)
            return
        self._o.reload_binding_set(new_id)

    def _switched_away(self, new_id: str | None) -> bool:
        """Close define mode if the core runs a map other than the one being
        edited, rather than saving edits into a map it no longer runs."""
        if not (self.active and new_id and new_id != self.bindings_id):
            return False
        self.exit(SWITCHED_NOTICE.format(id=new_id))
        self._o.reload_binding_set(new_id)
        return True

    # -- view -------------------------------------------------------------

    def follow(self) -> None:
        """Keep the picker docked beside the keyboard."""
        if self.active:
            self.picker.dock_beside(self._o.window.frameGeometry())

    def _refresh(self) -> None:
        # The picker reads editor.bset fresh; without this its fields go stale.
        self.picker.refresh()
        e = self.editor
        self._o.bar.set_define_mode(self.active,
                                    bool(e and e.can_undo),
                                    bool(e and e.can_redo),
                                    self.can_transfer)


def _documents_dir() -> str:
    return QStandardPaths.writableLocation(QStandardPaths.DocumentsLocation)

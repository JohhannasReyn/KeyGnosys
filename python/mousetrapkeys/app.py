"""Application entry point."""

from __future__ import annotations

import argparse
import sys

from PySide6.QtCore import Qt
from PySide6.QtWidgets import QApplication

from . import paths
from .documents import Registry
from .settings import Settings


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="mousetrapkeys",
        description="Pinnable, click-through on-screen keyboard and "
                    "CapsLock cursor layer.",
    )
    parser.add_argument(
        "--backend", choices=("auto", "mock", "ipc"), default="auto",
        help="Input source. 'ipc' talks to the native core, 'mock' uses Qt's "
             "own key events for development, 'auto' tries ipc and falls back "
             "to mock (default).")
    parser.add_argument("--layout", help="Override the configured layout id.")
    parser.add_argument("--scale", type=float,
                        help="Override the configured display scale.")
    parser.add_argument("--list", action="store_true",
                        help="List discovered documents and exit.")
    parser.add_argument("--check", action="store_true",
                        help="Validate all documents, report diagnostics, "
                             "and exit non-zero if any failed to load.")
    return parser


def load_registry() -> Registry:
    paths.ensure_user_dirs()
    return Registry().load_all()


def cmd_list(registry: Registry) -> int:
    for kind in paths.KINDS:
        items = getattr(registry, kind)
        print(f"\n{kind} ({len(items)})")
        for doc_id, doc in sorted(items.items()):
            extra = ""
            if kind == "layouts":
                extra = f"  {len(doc.keys)} keys, {doc.width}x{doc.height}u"
            elif kind == "bindings":
                extra = f"  {len(doc.bindings)} bindings"
            elif kind == "profiles":
                total = sum(len(v) for v in doc.shortcuts.values())
                extra = f"  {total} shortcuts"
            print(f"  {doc_id:22} {doc.name}{extra}")
    return cmd_diagnostics(registry, quiet=True)


def cmd_diagnostics(registry: Registry, quiet: bool = False) -> int:
    problems = [d for d in registry.diagnostics if d.level in ("warn", "error")]
    if registry.diagnostics.items and not quiet:
        print("\ndiagnostics")
    for diag in registry.diagnostics:
        if quiet and diag.level == "info":
            continue
        print(f"  {diag}")
    if not problems and not quiet:
        print("  none — all documents loaded cleanly")
    return 1 if problems else 0


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)

    if args.list or args.check:
        registry = load_registry()
        return cmd_list(registry) if args.list else cmd_diagnostics(registry)

    # High-DPI rounding: floor rather than round, so a 150% display does not
    # snap the overlay to a size that no longer matches the layout geometry.
    QApplication.setHighDpiScaleFactorRoundingPolicy(
        Qt.HighDpiScaleFactorRoundingPolicy.PassThrough)

    app = QApplication(sys.argv if argv is None else [sys.argv[0], *argv])
    app.setApplicationName("MouseTrapKeys")
    app.setQuitOnLastWindowClosed(False)

    registry = load_registry()
    settings = Settings.load()
    if args.layout:
        settings.set("behavior.layout", args.layout)
    if args.scale:
        settings.set("appearance.scale", args.scale)

    for diag in registry.diagnostics:
        if diag.level in ("warn", "error"):
            print(diag, file=sys.stderr)

    client = _make_client(app, args.backend, settings)

    from .ui.overlay import Overlay
    overlay = Overlay(registry, settings, client)
    overlay.show()
    client.start()

    app.aboutToQuit.connect(overlay.shutdown)
    return app.exec()


def _make_client(app: QApplication, backend: str, settings: Settings):
    """Pick an input source.

    'auto' prefers the native core but falls back to the mock rather than
    starting an overlay that can never light up -- and the control bar always
    states which one is live, so the two are never confused.
    """
    from .coreclient.mock import MockClient

    mode = settings.get("behavior.activation_mode", "hybrid")

    if backend == "mock":
        return MockClient(app, mode)

    from .coreclient.ipc import IpcClient
    if backend == "ipc":
        return IpcClient()

    # auto: the core is not built yet (milestones M2-M4), so this resolves to
    # the mock today and to the core the moment it starts listening.
    import os
    endpoint = paths.ipc_endpoint()
    core_present = (sys.platform == "win32" and os.path.exists(endpoint)) or \
                   (sys.platform != "win32" and os.path.exists(endpoint))
    return IpcClient() if core_present else MockClient(app, mode)


if __name__ == "__main__":
    raise SystemExit(main())

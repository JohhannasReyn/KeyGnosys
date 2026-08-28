"""Render the overlay to PNG files without opening a window.

Used to eyeball the painter and the legend layers, and to produce the images in
the README. Runs on Qt's offscreen platform plugin, so it works headlessly and
in CI.

Usage:
    python tools/render_preview.py [--out docs/images] [--scale 1.0]
"""

from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "python"))

# Qt's offscreen plugin ships no fonts on some platforms, which renders every
# legend as tofu. Use it only where there is genuinely no display to render on.
if sys.platform.startswith("linux") and not os.environ.get("DISPLAY"):
    os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from PySide6.QtGui import QFont, QImage  # noqa: E402
from PySide6.QtWidgets import QApplication  # noqa: E402

from keygnosys.documents import Registry  # noqa: E402
from keygnosys.state import AppState  # noqa: E402
from keygnosys.ui.keyboard_view import KeyboardView  # noqa: E402
from keygnosys.ui.theme import ResolvedTheme  # noqa: E402
from keygnosys.ui.fonts import ui_font  # noqa: E402


def render(view: KeyboardView, path: Path) -> None:
    image = QImage(view.size(), QImage.Format_ARGB32_Premultiplied)
    image.fill(0)
    view.render(image)
    path.parent.mkdir(parents=True, exist_ok=True)
    image.save(str(path))
    print(f"{path.relative_to(ROOT)}  {image.width()}x{image.height()}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", default="docs/images")
    parser.add_argument("--scale", type=float, default=1.0)
    args = parser.parse_args()

    out = ROOT / args.out
    app = QApplication([])
    app.setFont(ui_font())
    registry = Registry().load_all()

    state = AppState()
    state.binding_set = registry.binding_set("default")
    state.profile = registry.profiles.get("chrome")
    state.slot_names = {1: "Chrome", 2: "VS Code", 3: "Terminal"}

    view = KeyboardView(state)
    view.set_scale(args.scale)

    dark = ResolvedTheme.build(registry.themes["dark"], opacity=1.0)
    light = ResolvedTheme.build(registry.themes["light"], opacity=1.0)

    # One image per layout, base legend, dark theme.
    view.set_theme(dark)
    for layout_id in sorted(registry.layouts):
        view.set_layout_doc(registry.layouts[layout_id])
        render(view, out / f"layout-{layout_id}.png")

    # The four legend layers, on the full-size board.
    view.set_layout_doc(registry.layouts["us-ansi-104"])

    state.pressed.clear()
    render(view, out / "legend-base.png")

    state.press("ShiftLeft")
    render(view, out / "legend-shift.png")
    state.pressed.clear()

    state.press("ControlLeft")
    render(view, out / "legend-shortcuts-chrome.png")
    state.pressed.clear()

    state.cursor_layer = True
    render(view, out / "legend-cursor.png")
    state.cursor_layer = False

    # Light theme, for the theme comparison.
    view.set_theme(light)
    render(view, out / "theme-light.png")

    del app
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

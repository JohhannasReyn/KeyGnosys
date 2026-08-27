"""Whole-window click-through.

Per-widget ``WA_TransparentForMouseEvents`` is not enough: it routes events
*within* the Qt window, but the OS still delivers them to that window rather
than to the application beneath it. Making the pointer genuinely pass through
requires telling the window system, which is what this module does.

This is also the reason the overlay is two windows -- the control bar must stay
clickable while the keyboard does not. See docs/SPEC.md section 9.2.

``set_click_through`` returns False when the platform cannot do it, and the
caller must then disable the control and say why rather than offering a toggle
that silently does nothing (principle P6).
"""

from __future__ import annotations

import sys

from PySide6.QtWidgets import QWidget

# Win32 constants
GWL_EXSTYLE = -20
WS_EX_LAYERED = 0x00080000
WS_EX_TRANSPARENT = 0x00000020
WS_EX_NOACTIVATE = 0x08000000
WS_EX_TOOLWINDOW = 0x00000080


def platform_name() -> str:
    if sys.platform == "win32":
        return "windows"
    if sys.platform.startswith("linux"):
        return "x11" if _is_x11() else "wayland"
    return sys.platform


def _is_x11() -> bool:
    try:
        from PySide6.QtGui import QGuiApplication
        return "xcb" in (QGuiApplication.platformName() or "").lower()
    except Exception:
        return False


def unavailable_reason() -> str | None:
    """Why click-through cannot work here, or None if it can."""
    name = platform_name()
    if name == "windows":
        return None
    if name == "x11":
        return None if _have_xlib() else (
            "Click-through on X11 needs the python-xlib package "
            "(pip install python-xlib)."
        )
    if name == "wayland":
        return (
            "Wayland does not allow an application to make its own window "
            "click-through. Log into an X11 session, or move the overlay out "
            "of the way instead."
        )
    return f"Click-through is not implemented for {name!r}."


def _have_xlib() -> bool:
    try:
        import Xlib  # noqa: F401
        return True
    except ImportError:
        return False


def set_click_through(widget: QWidget, enabled: bool) -> bool:
    """Make `widget` pass mouse input through to whatever is beneath it."""
    name = platform_name()
    if name == "windows":
        return _set_windows(widget, enabled)
    if name == "x11":
        return _set_x11(widget, enabled)
    return False


def set_no_activate(widget: QWidget) -> bool:
    """Stop the window from ever taking keyboard focus when shown or clicked."""
    if platform_name() != "windows":
        return False
    try:
        import ctypes
        hwnd = int(widget.winId())
        user32 = ctypes.windll.user32
        get_style = user32.GetWindowLongPtrW
        set_style = user32.SetWindowLongPtrW
        style = get_style(hwnd, GWL_EXSTYLE)
        set_style(hwnd, GWL_EXSTYLE, style | WS_EX_NOACTIVATE)
        return True
    except Exception:
        return False


# --------------------------------------------------------------------------
# Windows
# --------------------------------------------------------------------------

def _set_windows(widget: QWidget, enabled: bool) -> bool:
    try:
        import ctypes

        hwnd = int(widget.winId())
        user32 = ctypes.windll.user32
        # GetWindowLongPtrW is the 64-bit-safe form; the W suffix keeps the
        # style value from being truncated on 64-bit builds.
        get_style = user32.GetWindowLongPtrW
        set_style = user32.SetWindowLongPtrW
        get_style.restype = ctypes.c_longlong
        set_style.restype = ctypes.c_longlong

        style = get_style(hwnd, GWL_EXSTYLE)
        if enabled:
            style |= WS_EX_LAYERED | WS_EX_TRANSPARENT
        else:
            # WS_EX_LAYERED stays: Qt uses it for the translucent background,
            # and clearing it would make the window opaque.
            style &= ~WS_EX_TRANSPARENT
        set_style(hwnd, GWL_EXSTYLE, style)
        return True
    except Exception:
        return False


# --------------------------------------------------------------------------
# X11
# --------------------------------------------------------------------------

def _set_x11(widget: QWidget, enabled: bool) -> bool:
    """Set an empty input shape, so the X server routes clicks past us."""
    try:
        from Xlib import display as xdisplay
        from Xlib.ext import shape

        disp = xdisplay.Display()
        win = disp.create_resource_object("window", int(widget.winId()))
        if enabled:
            win.shape_rectangles(shape.SO.Set, shape.SK.Input, 0, 0, 0, [])
        else:
            from Xlib.protocol import rq  # noqa: F401
            win.shape_rectangles(
                shape.SO.Set, shape.SK.Input, 0, 0, 0,
                [(0, 0, widget.width(), widget.height())],
            )
        disp.sync()
        return True
    except Exception:
        return False

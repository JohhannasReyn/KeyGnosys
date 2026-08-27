"""Legend resolution: the rules from docs/SPEC.md section 9.4.

These are pure -- no Qt, no display -- which is the point of keeping the
decisions in AppState rather than in the painter.
"""

from __future__ import annotations

import pytest

from mousetrapkeys.documents import Key, Registry, Segment
from mousetrapkeys.state import (
    SLOT_NAME_LIMIT, AppState, KeyStyle, LegendLayer,
)


@pytest.fixture(scope="module")
def registry() -> Registry:
    return Registry().load_all()


@pytest.fixture()
def state(registry: Registry) -> AppState:
    st = AppState()
    st.layout = registry.layouts["us-ansi-104"]
    st.binding_set = registry.bindings["default"]
    st.profile = registry.profiles["chrome"]
    return st


def make_key(code: str, base: str, shift=None, sub=None, role="normal") -> Key:
    """A one-segment key, which is what all but the ISO Enter actually are."""
    return Key(id=code.lower(), code=code, base=base, shift=shift, sub=sub,
               role=role, segments=[Segment(id="s0", x=0, y=0)])


def render(state: AppState, code: str, base: str, shift=None, role="normal"):
    return state.render_key(make_key(code, base, shift, role=role))


# -- layer selection --------------------------------------------------------

def test_base_layer_by_default(state: AppState) -> None:
    assert state.active_layer() is LegendLayer.BASE
    assert render(state, "KeyA", "A").text == "A"


def test_shift_alone_selects_the_shift_layer(state: AppState) -> None:
    state.press("ShiftLeft")
    assert state.active_layer() is LegendLayer.SHIFT
    assert render(state, "KeyA", "A").text == "A"
    assert render(state, "Digit1", "1", shift="!").text == "!"


def test_shift_uppercases_when_no_shift_legend_is_given(state: AppState) -> None:
    state.press("ShiftRight")
    assert render(state, "KeyQ", "q").text == "Q"


def test_control_selects_the_shortcut_layer(state: AppState) -> None:
    state.press("ControlLeft")
    assert state.active_layer() is LegendLayer.MODIFIER
    assert render(state, "KeyT", "T").text == "New tab"


def test_shortcut_miss_shows_dimmed_base_not_blank(state: AppState) -> None:
    """A keyboard that goes blank reads as a bug, not as 'no shortcut here'."""
    state.press("ControlLeft")
    result = render(state, "KeyQ", "Q")
    assert result.text == "Q"
    assert result.dim is True


def test_no_matching_profile_dims_everything(state: AppState) -> None:
    state.profile = None
    state.press("ControlLeft")
    result = render(state, "KeyT", "T")
    assert result.text == "T" and result.dim is True


def test_modifier_combinations_are_canonical(state: AppState) -> None:
    state.press("ShiftLeft")
    state.press("ControlLeft")
    assert state.modifier_combo() == "Control+Shift"
    assert render(state, "KeyT", "T").text == "Reopen closed tab"


def test_left_and_right_modifiers_are_equivalent_for_lookup(state: AppState) -> None:
    state.press("ControlRight")
    assert state.modifier_combo() == "Control"


def test_cursor_layer_outranks_held_modifiers(state: AppState) -> None:
    state.cursor_layer = True
    state.press("ControlLeft")
    assert state.active_layer() is LegendLayer.CURSOR


# -- cursor layer -----------------------------------------------------------

def test_cursor_layer_shows_binding_legends(state: AppState) -> None:
    state.cursor_layer = True
    assert render(state, "KeyH", "H").text == "◀"
    assert render(state, "KeyD", "D").text == "Click"
    assert render(state, "KeyF", "F").text == "Slow"


def test_unbound_keys_are_blank_and_dimmed_in_the_cursor_layer(state: AppState) -> None:
    state.cursor_layer = True
    result = render(state, "KeyQ", "Q")
    assert result.text == ""
    assert result.dim is True
    assert result.style is KeyStyle.UNBOUND


def test_window_slots_prefer_the_real_application_name(state: AppState) -> None:
    state.cursor_layer = True
    assert render(state, "Digit1", "1").text == "App 1"
    state.slot_names = {1: "Google Chrome"}
    assert render(state, "Digit1", "1").text == "Google Chrome"


def test_long_slot_names_are_elided(state: AppState) -> None:
    state.cursor_layer = True
    state.slot_names = {1: "A very long window title that will never fit"}
    text = render(state, "Digit1", "1").text
    assert len(text) <= SLOT_NAME_LIMIT and text.endswith("…")


# -- styling ----------------------------------------------------------------

def test_pressed_key_is_highlighted(state: AppState) -> None:
    state.press("KeyA")
    assert render(state, "KeyA", "A").style is KeyStyle.PRESSED


def test_held_modifier_latches_rather_than_flashing(state: AppState) -> None:
    state.press("ControlLeft")
    assert render(state, "ControlLeft", "Ctrl",
                  role="modifier").style is KeyStyle.LATCHED


def test_capslock_latches_while_the_cursor_layer_is_engaged(state: AppState) -> None:
    state.cursor_layer = True
    assert render(state, "CapsLock", "Caps",
                  role="toggle").style is KeyStyle.LATCHED


def test_numlock_reports_through_its_led_not_by_lighting_up(state: AppState) -> None:
    state.num_lock_led = True
    result = render(state, "NumLock", "Num", role="toggle")
    assert result.led is True
    assert result.style is KeyStyle.NORMAL


def test_capslock_led_follows_the_layer_not_the_os_state(state: AppState) -> None:
    state.caps_lock_led = False
    state.cursor_layer = True
    assert render(state, "CapsLock", "Caps", role="toggle").led is True


# -- release semantics ------------------------------------------------------

def test_release_moves_a_key_into_the_fade_set(state: AppState) -> None:
    state.press("KeyA")
    state.release("KeyA")
    assert "KeyA" not in state.pressed
    assert state.fading["KeyA"] == 1.0


def test_release_all_clears_every_held_key(state: AppState) -> None:
    for code in ("KeyA", "KeyB", "ControlLeft"):
        state.press(code)
    state.release_all()
    assert not state.pressed


def test_pressing_a_fading_key_again_cancels_the_fade(state: AppState) -> None:
    state.press("KeyA")
    state.release("KeyA")
    state.press("KeyA")
    assert "KeyA" not in state.fading

#include "scancode_keymap.hpp"

namespace kgn::win {
namespace {

struct Entry {
    std::uint32_t scanCode;
    bool extended;
    const char* name;
};

// PC set-1 scancodes. Positional by construction: these do not move when the
// user's keyboard language changes, which is the whole reason SPEC section 2
// picks W3C `code` values over characters.
//
// `Fn` is deliberately absent. On most laptops it is handled in keyboard
// firmware and never produces a scancode the OS can see, so the layouts render
// it for fidelity while the core refuses to promise it is bindable
// (SPEC section 8.4).
constexpr Entry kTable[] = {
    {0x01, false, "Escape"},

    {0x02, false, "Digit1"}, {0x03, false, "Digit2"}, {0x04, false, "Digit3"},
    {0x05, false, "Digit4"}, {0x06, false, "Digit5"}, {0x07, false, "Digit6"},
    {0x08, false, "Digit7"}, {0x09, false, "Digit8"}, {0x0A, false, "Digit9"},
    {0x0B, false, "Digit0"},

    {0x0C, false, "Minus"}, {0x0D, false, "Equal"}, {0x0E, false, "Backspace"},
    {0x0F, false, "Tab"},

    {0x10, false, "KeyQ"}, {0x11, false, "KeyW"}, {0x12, false, "KeyE"},
    {0x13, false, "KeyR"}, {0x14, false, "KeyT"}, {0x15, false, "KeyY"},
    {0x16, false, "KeyU"}, {0x17, false, "KeyI"}, {0x18, false, "KeyO"},
    {0x19, false, "KeyP"},

    {0x1A, false, "BracketLeft"}, {0x1B, false, "BracketRight"},
    {0x1C, false, "Enter"},
    {0x1D, false, "ControlLeft"},

    {0x1E, false, "KeyA"}, {0x1F, false, "KeyS"}, {0x20, false, "KeyD"},
    {0x21, false, "KeyF"}, {0x22, false, "KeyG"}, {0x23, false, "KeyH"},
    {0x24, false, "KeyJ"}, {0x25, false, "KeyK"}, {0x26, false, "KeyL"},

    {0x27, false, "Semicolon"}, {0x28, false, "Quote"}, {0x29, false, "Backquote"},
    {0x2A, false, "ShiftLeft"}, {0x2B, false, "Backslash"},

    {0x2C, false, "KeyZ"}, {0x2D, false, "KeyX"}, {0x2E, false, "KeyC"},
    {0x2F, false, "KeyV"}, {0x30, false, "KeyB"}, {0x31, false, "KeyN"},
    {0x32, false, "KeyM"},

    {0x33, false, "Comma"}, {0x34, false, "Period"}, {0x35, false, "Slash"},
    {0x36, false, "ShiftRight"},
    {0x37, false, "NumpadMultiply"},
    {0x38, false, "AltLeft"},
    {0x39, false, "Space"},
    {0x3A, false, "CapsLock"},

    {0x3B, false, "F1"}, {0x3C, false, "F2"}, {0x3D, false, "F3"},
    {0x3E, false, "F4"}, {0x3F, false, "F5"}, {0x40, false, "F6"},
    {0x41, false, "F7"}, {0x42, false, "F8"}, {0x43, false, "F9"},
    {0x44, false, "F10"},

    {0x45, false, "NumLock"},
    {0x46, false, "ScrollLock"},

    {0x47, false, "Numpad7"}, {0x48, false, "Numpad8"}, {0x49, false, "Numpad9"},
    {0x4A, false, "NumpadSubtract"},
    {0x4B, false, "Numpad4"}, {0x4C, false, "Numpad5"}, {0x4D, false, "Numpad6"},
    {0x4E, false, "NumpadAdd"},
    {0x4F, false, "Numpad1"}, {0x50, false, "Numpad2"}, {0x51, false, "Numpad3"},
    {0x52, false, "Numpad0"}, {0x53, false, "NumpadDecimal"},

    // The extra key an ISO keyboard has and an ANSI one does not.
    {0x56, false, "IntlBackslash"},
    {0x57, false, "F11"}, {0x58, false, "F12"},

    // F13-F24. Rare, and present mostly on programmable boards; mapped
    // contiguously from 0x64, which is what the sets that do emit them use.
    {0x64, false, "F13"}, {0x65, false, "F14"}, {0x66, false, "F15"},
    {0x67, false, "F16"}, {0x68, false, "F17"}, {0x69, false, "F18"},
    {0x6A, false, "F19"}, {0x6B, false, "F20"}, {0x6C, false, "F21"},
    {0x6D, false, "F22"}, {0x6E, false, "F23"}, {0x6F, false, "F24"},

    // -- extended (E0-prefixed) -------------------------------------------
    //
    // This is where the flag earns its keep: without it NumpadEnter is Enter,
    // ControlRight is ControlLeft, and every arrow is a numpad digit.
    {0x1C, true, "NumpadEnter"},
    {0x1D, true, "ControlRight"},
    {0x35, true, "NumpadDivide"},
    {0x37, true, "PrintScreen"},
    {0x38, true, "AltRight"},

    {0x47, true, "Home"},
    {0x48, true, "ArrowUp"},
    {0x49, true, "PageUp"},
    {0x4B, true, "ArrowLeft"},
    {0x4D, true, "ArrowRight"},
    {0x4F, true, "End"},
    {0x50, true, "ArrowDown"},
    {0x51, true, "PageDown"},
    {0x52, true, "Insert"},
    {0x53, true, "Delete"},

    {0x5B, true, "MetaLeft"},
    {0x5C, true, "MetaRight"},
    {0x5D, true, "ContextMenu"},
};

// VK_PAUSE. Pause is the one key the scancode space cannot resolve on its own:
// it arrives as the E1-prefixed sequence whose second scancode is 0x45, which
// is NumLock's. The virtual key is the only thing that separates them, so it
// is consulted for this one case rather than pretending the collision is not
// there.
constexpr std::uint32_t kVkPause = 0x13;

}  // namespace

ScancodeKeymap::ScancodeKeymap() {
    for (const Entry& entry : kTable) {
        const KeyCode code = KeyCode::fromString(entry.name);
        forward_[index(entry.scanCode, entry.extended)] = code;
        reverse_.emplace(code.id(), std::make_pair(entry.scanCode, entry.extended));
    }
    pause_ = KeyCode::fromString("Pause");
    numLock_ = KeyCode::fromString("NumLock");
    // Pause has no unambiguous scancode of its own, so synthesising it is not
    // offered rather than offered wrongly (P6). It is absent from reverse_.
}

KeyCode ScancodeKeymap::toKeyCode(std::uint32_t scanCode, bool extended,
                                  std::uint32_t vkCode) const {
    if (vkCode == kVkPause) return pause_;
    return forward_[index(scanCode, extended)];
}

bool ScancodeKeymap::toScanCode(KeyCode code, std::uint32_t& scanCode,
                                bool& extended) const {
    const auto it = reverse_.find(code.id());
    if (it == reverse_.end()) return false;
    scanCode = it->second.first;
    extended = it->second.second;
    return true;
}

}  // namespace kgn::win

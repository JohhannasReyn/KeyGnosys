#include "sendinput_output.hpp"

#include "kgn/layer_engine.hpp"   // kKeyIdSpace

namespace kgn::win {
namespace {

DWORD buttonFlag(MouseButton button, bool down) {
    switch (button) {
        case MouseButton::Left:
            return down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
        case MouseButton::Right:
            return down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
        case MouseButton::Middle:
            return down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
    }
    return 0;
}

void send(INPUT& input) { ::SendInput(1, &input, sizeof(INPUT)); }

}  // namespace

SendInputOutput::SendInputOutput() : heldKeys_(kKeyIdSpace, false) {}

void SendInputOutput::moveCursorBy(int dx, int dy) {
    if (dx == 0 && dy == 0) return;
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dx = dx;
    input.mi.dy = dy;
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    send(input);
}

void SendInputOutput::moveCursorTo(int x, int y) {
    // Normalised across the VIRTUAL DESKTOP, not the primary monitor. SPEC
    // section 8.3 names normalising against the primary alone as the classic
    // bug that makes warp land on the wrong screen -- and it is worse than it
    // sounds, because a monitor left of or above the primary has negative
    // coordinates that the primary-only form cannot express at all.
    const int left = ::GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int top = ::GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int width = ::GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int height = ::GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (width <= 1 || height <= 1) return;

    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags =
        MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    input.mi.dx = static_cast<LONG>(
        (static_cast<std::int64_t>(x - left) * 65535) / (width - 1));
    input.mi.dy = static_cast<LONG>(
        (static_cast<std::int64_t>(y - top) * 65535) / (height - 1));
    send(input);
}

Point SendInputOutput::cursorPosition() {
    POINT point{};
    if (::GetCursorPos(&point) == 0) return {};
    return {static_cast<int>(point.x), static_cast<int>(point.y)};
}

void SendInputOutput::button(MouseButton button, bool down) {
    const std::size_t index = static_cast<std::size_t>(button);
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = buttonFlag(button, down);
    send(input);
    heldButtons_[index] = down;
}

void SendInputOutput::scroll(int dx, int dy) {
    if (dy != 0) {
        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = MOUSEEVENTF_WHEEL;
        input.mi.mouseData = static_cast<DWORD>(dy * WHEEL_DELTA);
        send(input);
    }
    if (dx != 0) {
        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = MOUSEEVENTF_HWHEEL;
        input.mi.mouseData = static_cast<DWORD>(dx * WHEEL_DELTA);
        send(input);
    }
}

void SendInputOutput::sendKey(KeyCode code, bool down) {
    std::uint32_t scan = 0;
    bool extended = false;
    // A key with no scancode is not synthesised at all rather than
    // approximated with a virtual key that means something else (P6).
    if (!keymap_.toScanCode(code, scan, extended)) return;

    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wScan = static_cast<WORD>(scan);
    input.ki.dwFlags = KEYEVENTF_SCANCODE |
                       (extended ? KEYEVENTF_EXTENDEDKEY : 0u) |
                       (down ? 0u : KEYEVENTF_KEYUP);
    send(input);
    heldKeys_[code.id()] = down;
}

void SendInputOutput::releaseAll() {
    // P7's last line of defence, and the only thing that can lift a key this
    // process put down that no physical key will ever release.
    for (std::size_t id = 0; id < heldKeys_.size(); ++id) {
        if (!heldKeys_[id]) continue;
        sendKey(KeyCode(static_cast<std::uint16_t>(id)), false);
    }
    for (std::size_t i = 0; i < heldButtons_.size(); ++i) {
        if (!heldButtons_[i]) continue;
        button(static_cast<MouseButton>(i), false);
    }
}

std::chrono::milliseconds SendInputOutput::doubleClickInterval() const {
    return std::chrono::milliseconds(static_cast<int>(::GetDoubleClickTime()));
}

Capabilities SendInputOutput::capabilities() const {
    Capabilities capabilities;
    capabilities.canWarpAbsolute = true;
    return capabilities;
}

}  // namespace kgn::win

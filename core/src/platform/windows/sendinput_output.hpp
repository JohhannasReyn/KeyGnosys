// Pointer, button, scroll and key synthesis through SendInput.
//
// It tracks everything it puts down. That is not bookkeeping for its own sake:
// a key this backend synthesised may have no physical key behind it -- a
// release_all can put one down that nothing will ever lift -- and
// releaseAll() is the only thing in the process that can discharge it. It is
// the third of the three obligation sets the shutdown fallback relies on.
//
// See docs/SPEC.md section 8.3.

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <array>
#include <vector>

#include "kgn/backends.hpp"
#include "scancode_keymap.hpp"

namespace kgn::win {

class SendInputOutput final : public OutputBackend {
public:
    SendInputOutput();

    void moveCursorBy(int dx, int dy) override;
    void moveCursorTo(int x, int y) override;
    [[nodiscard]] Point cursorPosition() override;
    void button(MouseButton button, bool down) override;
    void scroll(int dx, int dy) override;
    void sendKey(KeyCode code, bool down) override;
    void releaseAll() override;

    [[nodiscard]] std::chrono::milliseconds doubleClickInterval() const override;
    [[nodiscard]] Capabilities capabilities() const override;
    [[nodiscard]] std::string_view name() const override { return "windows-sendinput"; }

private:
    ScancodeKeymap keymap_;
    // One flag per key id: what this backend believes it is holding down.
    std::vector<bool> heldKeys_;
    std::array<bool, 3> heldButtons_{};
};

}  // namespace kgn::win

// Pointer, button, scroll and key synthesis through SendInput.
//
// It tracks everything it puts down. That is not bookkeeping for its own sake:
// a key this backend synthesised may have no physical key behind it -- a
// release_all can put one down that nothing will ever lift -- and
// releaseAll() is the only thing in the process that can discharge it. It is
// the third of the three obligation sets the shutdown fallback relies on.
//
// The tracking invariant:
//
//     heldKeys_ and heldButtons_ reflect SUCCESSFUL OS-visible transitions
//     only, never attempted ones.
//
// SendInput can fail -- UIPI blocks injection into a higher-integrity
// foreground window, and the input desktop can be switched out from under it.
// Recording an attempt rather than a result gets both directions wrong, and
// the second is the dangerous one:
//
//   - a failed Down recorded as held makes releaseAll() emit a release for a
//     key the OS never saw pressed;
//   - a failed Up recorded as released DISCARDS the only record that the key
//     is still down, so releaseAll() can no longer retry it. That is a key
//     held forever with nothing left in the process that knows.
//
// So a failed Down does not mark, and a failed Up does not clear.
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
#include "kgn/diagnostics.hpp"
#include "scancode_keymap.hpp"

namespace kgn::win {

// Not `final`: the dispatch point below is the seam a test overrides to make
// a refused SendInput deterministic.
class SendInputOutput : public OutputBackend {
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

    void drainDiagnostics(Diagnostics& out) override;

protected:
    // The single point where an INPUT reaches the OS. Virtual so a test can
    // make it fail on demand: provoking a real SendInput failure needs a
    // higher-integrity foreground window, which is not something a test suite
    // may arrange. Returns true only when the OS accepted the event.
    virtual bool dispatch(const INPUT& input);

private:
    ScancodeKeymap keymap_;
    std::uint64_t failedPresses_ = 0;
    std::uint64_t failedReleases_ = 0;
    // One flag per key id: what this backend believes it is holding down.
    std::vector<bool> heldKeys_;
    std::array<bool, 3> heldButtons_{};
};

}  // namespace kgn::win

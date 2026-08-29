// The input backend's model of what the HARDWARE is doing.
//
// Distinct from anything the layer engine holds, and the distinction is
// load-bearing. The engine tracks OBLIGATIONS -- what it owes the OS, what it
// has forwarded, what it is holding. Those can be reset: release_all discharges
// them, a config reload can drop them. This tracks PHYSICS -- which keys the
// user's fingers are currently on. Nothing the software does can change that,
// so no control operation may clear it.
//
// It exists for three jobs:
//   - deriving KeyState::Repeat, because KBDLLHOOKSTRUCT carries no repeat
//     count and a second Down for a key already down is an autorepeat;
//   - publishing modifier state, which must describe the keyboard rather than
//     the engine;
//   - distinguishing a duplicate press from a new one.
//
// Pure, allocation-free after construction, and every KeyCode it compares
// against is resolved once here rather than per event -- KeyCode::fromString
// takes the intern table's lock and builds a std::string, and this runs inside
// WH_KEYBOARD_LL.

#pragma once

#include <array>
#include <vector>

#include "kgn/hookchannel.hpp"
#include "kgn/keycode.hpp"

namespace kgn {

class PhysicalKeyState {
public:
    PhysicalKeyState();

    // Record one physical event and return the state to feed the engine.
    // A Down for a key already down is a Repeat.
    KeyState observe(KeyCode code, bool up);

    [[nodiscard]] bool down(KeyCode code) const;

    // Fill the four modifier group flags from physical state.
    void fillModifiers(PublishedState& out) const;

    // Forget everything.
    //
    // ONLY for an owner that has stopped seeing input -- after the hook is
    // uninstalled, when the bitmap describes a keyboard nobody is watching.
    // Calling it while events are still arriving asserts that physically held
    // keys are up, which turns the next autorepeat into a fresh press and
    // publishes a modifier as released while the user is holding it.
    void forgetAll();

private:
    std::vector<bool> down_;
    std::array<KeyCode, 2> shift_{};
    std::array<KeyCode, 2> control_{};
    std::array<KeyCode, 2> alt_{};
    std::array<KeyCode, 2> meta_{};
};

}  // namespace kgn

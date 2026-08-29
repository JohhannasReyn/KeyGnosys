#include "kgn/physical.hpp"

#include <algorithm>

#include "kgn/layer_engine.hpp"   // kKeyIdSpace

namespace kgn {

PhysicalKeyState::PhysicalKeyState() : down_(kKeyIdSpace, false) {
    // Resolved once. Doing this per event would take the intern table's mutex
    // eight times inside a hook that must not block on a lock any other thread
    // can hold (SPEC section 8.2).
    shift_ = {KeyCode::fromString("ShiftLeft"), KeyCode::fromString("ShiftRight")};
    control_ = {KeyCode::fromString("ControlLeft"),
                KeyCode::fromString("ControlRight")};
    alt_ = {KeyCode::fromString("AltLeft"), KeyCode::fromString("AltRight")};
    meta_ = {KeyCode::fromString("MetaLeft"), KeyCode::fromString("MetaRight")};
}

KeyState PhysicalKeyState::observe(KeyCode code, bool up) {
    if (!code.valid()) return up ? KeyState::Up : KeyState::Down;
    const std::size_t slot = code.id();
    const KeyState state = up            ? KeyState::Up
                           : down_[slot] ? KeyState::Repeat
                                         : KeyState::Down;
    down_[slot] = !up;
    return state;
}

bool PhysicalKeyState::down(KeyCode code) const {
    return code.valid() && down_[code.id()];
}

void PhysicalKeyState::fillModifiers(PublishedState& out) const {
    auto either = [this](const std::array<KeyCode, 2>& pair) {
        return down(pair[0]) || down(pair[1]);
    };
    out.shift = either(shift_);
    out.control = either(control_);
    out.alt = either(alt_);
    out.meta = either(meta_);
}

void PhysicalKeyState::forgetAll() {
    std::fill(down_.begin(), down_.end(), false);
}

}  // namespace kgn

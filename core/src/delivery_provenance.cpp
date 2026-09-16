#include "kgn/delivery_provenance.hpp"

#include <algorithm>

#include "kgn/layer_engine.hpp"   // kKeyIdSpace

namespace kgn {

DeliveryProvenance::DeliveryProvenance()
    : state_(kKeyIdSpace, static_cast<std::uint8_t>(State::None)) {}

Route DeliveryProvenance::route(KeyCode code, KeyState state, bool enabled) {
    // An unnamed key never reaches the engine anyway; it is passed through
    // untouched, and nothing about it can be recorded against a KeyCode.
    if (!code.valid()) return Route::Native;

    std::uint8_t& slot = state_[code.id()];
    const State current = static_cast<State>(slot);

    if (!enabled) {
        // Interception is off: the event goes to Windows whatever else is true,
        // so from here on this press is Windows' -- including a press whose
        // earlier events went to the engine. Its release must reach Windows.
        slot = static_cast<std::uint8_t>(state == KeyState::Up ? State::None : State::Native);
        return Route::Native;
    }

    if (state == KeyState::Up) {
        // The one decision this class exists for.
        const Route route = current == State::Engine ? Route::Engine : Route::Native;
        slot = static_cast<std::uint8_t>(State::None);
        return route;
    }

    // A press or a repeat. Once any event of this press has gone native, the
    // rest of it does too: the engine has no obligation it could discharge, and
    // Windows is already holding the key.
    if (current == State::Native) return Route::Native;
    slot = static_cast<std::uint8_t>(State::Engine);
    return Route::Engine;
}

DeliveryProvenance::State DeliveryProvenance::stateFor(KeyCode code) const {
    if (!code.valid()) return State::None;
    return static_cast<State>(state_[code.id()]);
}

void DeliveryProvenance::forgetAll() {
    std::fill(state_.begin(), state_.end(), static_cast<std::uint8_t>(State::None));
}

}  // namespace kgn

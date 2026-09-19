#include "kgn/delivery_channel.hpp"

#include <algorithm>

#include "kgn/layer_engine.hpp"   // kKeyIdSpace

namespace kgn {

DeliveryChannel::DeliveryChannel()
    : state_(kKeyIdSpace, static_cast<std::uint8_t>(Channel::None)) {}

bool DeliveryChannel::nativeAllowed(KeyCode code) const {
    // An unnamed key is passed through untouched and never synthesised, so
    // there is no synthetic press it could overtake.
    if (!code.valid()) return true;
    return static_cast<Channel>(state_[code.id()]) != Channel::Synthetic;
}

void DeliveryChannel::commitNative(KeyCode code) {
    if (!code.valid()) return;
    state_[code.id()] = static_cast<std::uint8_t>(Channel::Native);
}

void DeliveryChannel::commitSynthetic(KeyCode code) {
    if (!code.valid()) return;
    state_[code.id()] = static_cast<std::uint8_t>(Channel::Synthetic);
}

void DeliveryChannel::release(KeyCode code) {
    if (!code.valid()) return;
    state_[code.id()] = static_cast<std::uint8_t>(Channel::None);
}

DeliveryChannel::Channel DeliveryChannel::stateFor(KeyCode code) const {
    if (!code.valid()) return Channel::None;
    return static_cast<Channel>(state_[code.id()]);
}

void DeliveryChannel::forgetAll() {
    std::fill(state_.begin(), state_.end(), static_cast<std::uint8_t>(Channel::None));
}

}  // namespace kgn

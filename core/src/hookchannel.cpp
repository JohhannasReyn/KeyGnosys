#include "kgn/hookchannel.hpp"

namespace kgn {

// ---------------------------------------------------------------------------
// Published state

std::uint32_t PublishedState::pack() const {
    std::uint32_t bits = 0;
    if (mode == Mode::Cursor) bits |= 1u << 0;
    if (latched) bits |= 1u << 1;
    bits |= (static_cast<std::uint32_t>(activation) & 0x3u) << 2;
    if (shift) bits |= 1u << 4;
    if (control) bits |= 1u << 5;
    if (alt) bits |= 1u << 6;
    if (meta) bits |= 1u << 7;
    return bits;
}

PublishedState PublishedState::unpack(std::uint32_t bits) {
    PublishedState state;
    state.mode = (bits & (1u << 0)) != 0 ? Mode::Cursor : Mode::Normal;
    state.latched = (bits & (1u << 1)) != 0;
    state.activation = static_cast<ActivationMode>((bits >> 2) & 0x3u);
    state.shift = (bits & (1u << 4)) != 0;
    state.control = (bits & (1u << 5)) != 0;
    state.alt = (bits & (1u << 6)) != 0;
    state.meta = (bits & (1u << 7)) != 0;
    return state;
}

void StatePublisher::publish(const PublishedState& state) {
    const std::uint64_t current = value_.load(std::memory_order_relaxed);
    const std::uint32_t bits = state.pack();
    const std::uint32_t version = static_cast<std::uint32_t>(current >> 32);

    // Version zero means nothing has been published yet, so the first publish
    // counts even when it happens to match the default-constructed state.
    if (version != 0 && static_cast<std::uint32_t>(current) == bits) return;

    const std::uint64_t next = static_cast<std::uint64_t>(version + 1) << 32;
    value_.store(next | bits, std::memory_order_release);
}

PublishedState StatePublisher::state() const {
    return PublishedState::unpack(
        static_cast<std::uint32_t>(value_.load(std::memory_order_acquire)));
}

std::uint32_t StatePublisher::version() const {
    return static_cast<std::uint32_t>(value_.load(std::memory_order_acquire) >> 32);
}

// ---------------------------------------------------------------------------
// Decision translation

bool translateDecisions(const DecisionBuffer& decisions, KeyCode code,
                        KeyState state, WorkRing& out) {
    // Native passthrough: the engine asked for exactly this event and nothing
    // else, so the cheapest way to make it true is to let the OS deliver it
    // itself. Anything more than one decision means something has to be
    // ordered around it -- the grace replay's synthetic Down being the case
    // that matters -- and then the physical event must be suppressed and
    // re-synthesised so the order is ours to control.
    //
    // The code and state must both match. A Forward for a different state is
    // the engine saying the OS should see a repeat rather than the second
    // press it was handed, and passing the press through would deliver the
    // wrong thing.
    const bool native = code.valid()
                        && decisions.size() == 1
                        && decisions[0].kind == Decision::Kind::Forward
                        && decisions[0].code == code
                        && decisions[0].state == state;
    if (native) return true;

    for (const Decision& decision : decisions) {
        switch (decision.kind) {
            case Decision::Kind::Forward:
                out.push(WorkItem{WorkItem::Kind::SendKey,
                                  decision.state != KeyState::Up,
                                  decision.code.id()});
                break;
            case Decision::Kind::RunAction:
                out.push(WorkItem{WorkItem::Kind::RunAction, false,
                                  decision.code.id()});
                break;
            case Decision::Kind::ReleaseAction:
                out.push(WorkItem{WorkItem::Kind::ReleaseAction, false,
                                  decision.code.id()});
                break;
            case Decision::Kind::Suppress:
            case Decision::Kind::Buffer:
                // Nothing for the core to do, and -- load-bearing -- no ring
                // capacity spent. The release-capacity proof in the header
                // depends on this: an Up for a key with no obligation must
                // cost nothing, because a user can produce those without end.
                break;
        }
    }
    return false;
}

}  // namespace kgn

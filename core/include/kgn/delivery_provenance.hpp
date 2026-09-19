#pragma once

// Where the events of the physical press currently under a finger have been
// allowed to go -- and therefore whether its release may be withheld.
//
// The invariant it exists to enforce (M3 finding O-1):
//
//     KeyGnosys may suppress a physical key release only when it has positive
//     provenance that forwarding that release would violate the press/release
//     delivery obligation for the corresponding physical press. Without that
//     provenance, the release is forwarded natively.
//
// The rule it replaces assumed the opposite: that "no record of forwarding the
// press" meant "Windows never received the press". Live measurement disproved
// it. A key held across a disable/enable cycle reaches Windows natively while
// interception is off, so Windows has it down while the engine knows nothing
// about it; suppressing the release then left Shift stuck down in Windows until
// the user pressed it again. Presses that predate the hook, and presses made
// while an elevated window had focus, are the same shape.
//
// THREE DOMAINS, DELIBERATELY SEPARATE:
//
//   PhysicalKeyState      which keys the fingers are on. Physics; no control
//                         operation may clear it.
//   LayerEngine slots     what the software owes: forwarded, held, buffered.
//                         Resettable, and reset by release_all and reloads.
//   DeliveryProvenance    where this press's events were ROUTED. It does not
//                         know, and must not duplicate, what the engine did
//                         with a press once routed there -- only that it was
//                         routed there, and that nothing has since gone native.
//
// Routing, not policy. `Engine` means "ask the engine"; the engine remains the
// only thing that decides forward-or-suppress for those events, exactly as
// before. This class only keeps `Engine` from being asked about a press it
// never saw, or about one Windows has received behind its back.
//
// Hook-thread only: no locks, no allocation after construction.

#include <cstdint>
#include <vector>

#include "kgn/keycode.hpp"

namespace kgn {

// What the hook should do with one physical event.
enum class Route : std::uint8_t {
    // Pass it to the OS untouched, without consulting the engine.
    Native,
    // Hand it to the engine, whose decision stands.
    Engine,
};

class DeliveryProvenance {
public:
    // The provenance of the press currently under the finger.
    enum class State : std::uint8_t {
        // No press of this key has been observed, or its release is done.
        // An arriving release therefore has NO positive provenance: KeyGnosys
        // cannot show that it withheld the press, so the release goes native.
        None,
        // The press was admitted to the enabled engine path, and no event of it
        // has since been allowed through natively.
        Engine,
        // At least one event of this press was deliberately allowed through to
        // Windows without the engine -- today, because interception was off.
        // Windows may hold the key down, so its release must reach Windows.
        Native,
    };

    DeliveryProvenance();

    // Route one physical event and update the provenance it implies.
    // `state` is the classification the physical bitmap already produced
    // (Down, Repeat or Up); `enabled` is the interception master switch.
    Route route(KeyCode code, KeyState state, bool enabled);

    // Diagnostics and tests. Never used to make a routing decision.
    [[nodiscard]] State stateFor(KeyCode code) const;

    // Only after the hook is uninstalled: from then on no routing happens, so
    // the record describes a keyboard nobody is observing. Clearing it earlier
    // would open a window in which a release loses its provenance while the
    // hook can still swallow it.
    void forgetAll();

private:
    std::vector<std::uint8_t> state_;
};

}  // namespace kgn

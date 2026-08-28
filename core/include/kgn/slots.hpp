// Stable window slot indices.
//
// SPEC 6.5: `windows` slot ordering MUST be stable across emissions, or the
// number-key bindings become unusable. A window keeps its slot for as long as
// it exists, a new window takes the lowest free index, indices are 1-based,
// and at most nine are carried.
//
// Reordering by recency would be more "useful" and is explicitly rejected: a
// map that changes under the user's fingers cannot be learned, which defeats
// the entire purpose of the overlay.
//
// Pure. No OS API -- it is handed a window list and keeps the bookkeeping.

#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <vector>

#include "kgn/backends.hpp"

namespace kgn {

// The event carries at most nine slots, mapped to Digit1..Digit9.
inline constexpr std::size_t kMaxSlots = 9;

class SlotRegistry {
public:
    // Reconcile against the current window list. Windows that are gone free
    // their slots; windows that remain keep theirs; new ones take the lowest
    // free index, in the order the backend reported them.
    void update(const std::vector<WindowInfo>& windows);

    // 1-based, as the bindings are.
    [[nodiscard]] std::optional<WindowId> at(int index) const;

    // The slot for a window, or nullopt when it has none -- which is what a
    // tenth window gets, and is reported rather than invented.
    [[nodiscard]] std::optional<int> indexOf(WindowId id) const;

    // In slot order, skipping empties. This is the order window.cycle walks.
    [[nodiscard]] std::vector<WindowId> occupied() const;

private:
    std::array<WindowId, kMaxSlots> slots_{};   // 0 means free
};

}  // namespace kgn

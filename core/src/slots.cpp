#include "kgn/slots.hpp"

#include <algorithm>

namespace kgn {

void SlotRegistry::update(const std::vector<WindowInfo>& windows) {
    // Free the slots of windows that have gone. Survivors are untouched, which
    // is the whole point: a window that still exists must not move.
    for (WindowId& slot : slots_) {
        if (slot == 0) continue;
        const bool present =
            std::any_of(windows.begin(), windows.end(),
                        [&](const WindowInfo& info) { return info.id == slot; });
        if (!present) slot = 0;
    }

    // New windows take the lowest free index, in the order they were reported.
    for (const WindowInfo& info : windows) {
        const bool known = std::any_of(slots_.begin(), slots_.end(),
                                       [&](WindowId id) { return id == info.id; });
        if (known) continue;
        for (WindowId& slot : slots_) {
            if (slot != 0) continue;
            slot = info.id;
            break;
        }
        // A window that finds no free slot simply has none. Reported without
        // an index rather than displacing one that a user has learned.
    }
}

std::optional<WindowId> SlotRegistry::at(int index) const {
    if (index < 1 || static_cast<std::size_t>(index) > kMaxSlots) return std::nullopt;
    const WindowId id = slots_[static_cast<std::size_t>(index - 1)];
    if (id == 0) return std::nullopt;
    return id;
}

std::optional<int> SlotRegistry::indexOf(WindowId id) const {
    if (id == 0) return std::nullopt;
    for (std::size_t i = 0; i < kMaxSlots; ++i) {
        if (slots_[i] == id) return static_cast<int>(i + 1);
    }
    return std::nullopt;
}

std::vector<WindowId> SlotRegistry::occupied() const {
    std::vector<WindowId> ids;
    for (WindowId id : slots_) {
        if (id != 0) ids.push_back(id);
    }
    return ids;
}

}  // namespace kgn

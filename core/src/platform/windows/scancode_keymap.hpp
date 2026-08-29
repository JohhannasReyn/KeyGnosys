// The one translation table between Windows scancodes and the key vocabulary.
//
// Principle P2: each backend owns exactly ONE table, at the edge. No other
// module may hold a key translation.
//
// Every KeyCode is resolved once, at construction. KeyCode::fromString takes
// the intern table's mutex and builds a std::string, so calling it per event
// would put both an allocation and a lock another thread can hold inside
// WH_KEYBOARD_LL -- the two things SPEC section 8.2 forbids by name.
//
// See docs/SPEC.md sections 2.2 and 8.2.

#pragma once

#include <array>
#include <cstdint>
#include <unordered_map>

#include "kgn/keycode.hpp"

namespace kgn::win {

// Scancodes run 0x00-0xFF; the extended flag doubles the space.
inline constexpr std::size_t kScanSpace = 512;

class ScancodeKeymap {
public:
    ScancodeKeymap();

    // Positional lookup. `vkCode` disambiguates the one collision the
    // scancode space genuinely has (see the note on Pause in the .cpp); pass 0
    // when it is not available and the scancode path is used alone.
    [[nodiscard]] KeyCode toKeyCode(std::uint32_t scanCode, bool extended,
                                    std::uint32_t vkCode = 0) const;

    // The reverse, for synthesising a key through SendInput.
    [[nodiscard]] bool toScanCode(KeyCode code, std::uint32_t& scanCode,
                                  bool& extended) const;

private:
    static std::size_t index(std::uint32_t scanCode, bool extended) {
        return (scanCode & 0xFFu) | (extended ? 0x100u : 0u);
    }

    std::array<KeyCode, kScanSpace> forward_{};
    std::unordered_map<std::uint16_t, std::pair<std::uint32_t, bool>> reverse_;
    KeyCode pause_;
    KeyCode numLock_;
};

}  // namespace kgn::win

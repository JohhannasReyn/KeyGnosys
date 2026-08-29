// The Windows scancode table.
//
// SPEC section 13: round-trip every code through each backend's table, assert
// bijection and no unmapped entries. Windows-only, because the table is.

#if defined(_WIN32)

#include "../src/platform/windows/scancode_keymap.hpp"

#include "kgn_test.hpp"

#include <set>
#include <string>

using kgn::KeyCode;
using kgn::win::ScancodeKeymap;

KGN_TEST(every_mapped_scancode_round_trips) {
    const ScancodeKeymap map;
    int mapped = 0;

    for (std::uint32_t scan = 0; scan < 0x100; ++scan) {
        for (bool extended : {false, true}) {
            const KeyCode code = map.toKeyCode(scan, extended);
            if (!code.valid()) continue;
            ++mapped;

            std::uint32_t back = 0;
            bool backExtended = false;
            KGN_CHECK(map.toScanCode(code, back, backExtended));
            KGN_CHECK_EQ(back, scan);
            KGN_CHECK(backExtended == extended);
        }
    }
    // A table that quietly stopped mapping anything would pass every
    // assertion above.
    KGN_CHECK(mapped >= 100);
}

KGN_TEST(no_two_scancodes_claim_the_same_key) {
    const ScancodeKeymap map;
    std::set<std::uint16_t> seen;
    for (std::uint32_t scan = 0; scan < 0x100; ++scan) {
        for (bool extended : {false, true}) {
            const KeyCode code = map.toKeyCode(scan, extended);
            if (!code.valid()) continue;
            KGN_CHECK(seen.insert(code.id()).second);
        }
    }
}

KGN_TEST(the_extended_flag_separates_the_keys_that_share_a_scancode) {
    // Without this flag NumpadEnter is Enter, ControlRight is ControlLeft, and
    // every arrow key is a numpad digit.
    const ScancodeKeymap map;
    KGN_CHECK(map.toKeyCode(0x1C, false) == KeyCode::fromString("Enter"));
    KGN_CHECK(map.toKeyCode(0x1C, true) == KeyCode::fromString("NumpadEnter"));
    KGN_CHECK(map.toKeyCode(0x1D, false) == KeyCode::fromString("ControlLeft"));
    KGN_CHECK(map.toKeyCode(0x1D, true) == KeyCode::fromString("ControlRight"));
    KGN_CHECK(map.toKeyCode(0x48, false) == KeyCode::fromString("Numpad8"));
    KGN_CHECK(map.toKeyCode(0x48, true) == KeyCode::fromString("ArrowUp"));
    KGN_CHECK(map.toKeyCode(0x35, false) == KeyCode::fromString("Slash"));
    KGN_CHECK(map.toKeyCode(0x35, true) == KeyCode::fromString("NumpadDivide"));
    KGN_CHECK(map.toKeyCode(0x37, false) == KeyCode::fromString("NumpadMultiply"));
    KGN_CHECK(map.toKeyCode(0x37, true) == KeyCode::fromString("PrintScreen"));
}

KGN_TEST(the_letters_digits_and_function_keys_are_all_present) {
    const ScancodeKeymap map;
    std::uint32_t scan = 0;
    bool extended = false;
    for (char c = 'A'; c <= 'Z'; ++c) {
        KGN_CHECK(map.toScanCode(KeyCode::fromString(std::string("Key") + c),
                                 scan, extended));
    }
    for (char c = '0'; c <= '9'; ++c) {
        KGN_CHECK(map.toScanCode(KeyCode::fromString(std::string("Digit") + c),
                                 scan, extended));
    }
    for (int i = 1; i <= 24; ++i) {
        KGN_CHECK(map.toScanCode(KeyCode::fromString("F" + std::to_string(i)),
                                 scan, extended));
    }
    for (const char* name : {"ShiftLeft", "ShiftRight", "ControlLeft",
                             "ControlRight", "AltLeft", "AltRight", "MetaLeft",
                             "MetaRight", "CapsLock", "Space", "Tab", "Enter",
                             "Backspace", "Escape", "IntlBackslash",
                             "ContextMenu", "NumLock", "ScrollLock"}) {
        KGN_CHECK(map.toScanCode(KeyCode::fromString(name), scan, extended));
    }
}

KGN_TEST(pause_is_resolved_by_virtual_key_because_its_scancode_collides) {
    // Pause arrives as an E1-prefixed sequence whose second scancode is 0x45,
    // which is NumLock's. The virtual key is the only thing that separates
    // them.
    const ScancodeKeymap map;
    KGN_CHECK(map.toKeyCode(0x45, false, 0x13) == KeyCode::fromString("Pause"));
    KGN_CHECK(map.toKeyCode(0x45, false, 0) == KeyCode::fromString("NumLock"));

    // And because it has no scancode of its own, synthesising it is refused
    // rather than offered wrongly (P6).
    std::uint32_t scan = 0;
    bool extended = false;
    KGN_CHECK(!map.toScanCode(KeyCode::fromString("Pause"), scan, extended));
}

KGN_TEST(fn_is_never_mapped_because_the_os_never_sees_it) {
    // SPEC section 8.4: on most laptops Fn is handled in keyboard firmware and
    // produces no scancode. The layouts draw it; the core must not promise it.
    const ScancodeKeymap map;
    std::uint32_t scan = 0;
    bool extended = false;
    KGN_CHECK(!map.toScanCode(KeyCode::fromString("Fn"), scan, extended));
    KGN_CHECK(!map.toScanCode(KeyCode::fromString("FnLock"), scan, extended));
}

KGN_TEST(an_unmapped_scancode_is_invalid_rather_than_guessed) {
    const ScancodeKeymap map;
    KGN_CHECK(!map.toKeyCode(0x00, false).valid());
    KGN_CHECK(!map.toKeyCode(0xFE, false).valid());
    KGN_CHECK(!map.toKeyCode(0x02, true).valid());
}

int main() { return kgn::test::runAll(); }

#else

int main() { return 0; }

#endif

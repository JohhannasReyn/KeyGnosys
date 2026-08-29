// The key vocabulary is principle P2: one identifier set shared by layouts,
// bindings, both OS backends and the UI. These assert the properties everything
// downstream relies on.

#include "kgn/keycode.hpp"
#include "kgn_test.hpp"

#include <string>
#include <string_view>
#include <unordered_set>

using kgn::KeyCode;

KGN_TEST(round_trips_through_string) {
    for (const char* name : {"KeyA", "KeyZ", "Digit0", "Digit9", "F1", "F24",
                             "ShiftLeft", "CapsLock", "Numpad7", "IntlBackslash",
                             "ContextMenu", "ArrowLeft"}) {
        const KeyCode code = KeyCode::fromString(name);
        KGN_CHECK(code.valid());
        KGN_CHECK_EQ(std::string(code.toString()), std::string(name));
    }
}

KGN_TEST(same_name_yields_same_id) {
    KGN_CHECK(KeyCode::fromString("KeyQ") == KeyCode::fromString("KeyQ"));
    KGN_CHECK(KeyCode::fromString("KeyQ") != KeyCode::fromString("KeyW"));
}

KGN_TEST(ids_are_unique_across_the_whole_vocabulary) {
    std::unordered_set<std::uint16_t> seen;
    std::vector<std::string> names;
    for (char c = 'A'; c <= 'Z'; ++c) names.push_back(std::string("Key") + c);
    for (char c = '0'; c <= '9'; ++c) names.push_back(std::string("Digit") + c);
    for (int i = 1; i <= 24; ++i) names.push_back("F" + std::to_string(i));
    for (const char* n : {"ShiftLeft", "ShiftRight", "ControlLeft",
                          "ControlRight", "AltLeft", "AltRight", "MetaLeft",
                          "MetaRight", "CapsLock", "Space", "Tab", "Enter",
                          "Backspace", "NumpadEnter", "NumLock", "Escape"}) {
        names.emplace_back(n);
    }
    for (const auto& name : names) {
        const KeyCode code = KeyCode::fromString(name);
        KGN_CHECK(code.valid());
        KGN_CHECK(seen.insert(code.id()).second);
    }
}

KGN_TEST(empty_name_is_invalid) {
    KGN_CHECK(!KeyCode::fromString("").valid());
    KGN_CHECK(!KeyCode{}.valid());
    KGN_CHECK(KeyCode{}.toString().empty());
}

KGN_TEST(unknown_codes_are_interned_not_rejected) {
    // A layout may reference a key this build has never heard of. It must still
    // load and render; it simply never highlights. SPEC section 2.1.
    const KeyCode odd = KeyCode::fromString("VendorSpecificThing");
    KGN_CHECK(odd.valid());
    KGN_CHECK_EQ(std::string(odd.toString()), std::string("VendorSpecificThing"));
    KGN_CHECK(odd == KeyCode::fromString("VendorSpecificThing"));
}

KGN_TEST(code_lookup_is_case_sensitive) {
    // The vocabulary is a fixed set of exact spellings. Accepting "keya" would
    // mean two ids for one physical key.
    KGN_CHECK(KeyCode::fromString("KeyA") != KeyCode::fromString("keya"));
}

KGN_TEST(modifiers_are_recognised) {
    for (const char* name : {"ShiftLeft", "ShiftRight", "ControlLeft",
                             "ControlRight", "AltLeft", "AltRight",
                             "MetaLeft", "MetaRight"}) {
        KGN_CHECK(kgn::isModifier(KeyCode::fromString(name)));
    }
}

KGN_TEST(non_modifiers_are_not_modifiers) {
    // CapsLock is deliberately excluded: it is the layer gesture, not a
    // modifier whose held state selects a legend layer.
    for (const char* name : {"KeyA", "Space", "CapsLock", "Escape", "Fn",
                             "NumLock", "F1"}) {
        KGN_CHECK(!kgn::isModifier(KeyCode::fromString(name)));
    }
}

KGN_TEST(left_and_right_modifiers_share_a_group) {
    const auto group = [](const char* n) {
        return std::string(kgn::modifierGroup(KeyCode::fromString(n)));
    };
    KGN_CHECK_EQ(group("ShiftLeft"), group("ShiftRight"));
    KGN_CHECK_EQ(group("ControlLeft"), std::string("Control"));
    KGN_CHECK_EQ(group("AltRight"), std::string("Alt"));
    KGN_CHECK_EQ(group("MetaLeft"), std::string("Meta"));
    KGN_CHECK_EQ(group("KeyA"), std::string(""));
}

KGN_TEST(a_returned_name_survives_interning_many_more_keys) {
    // toString() hands back a view into the intern table's storage and then
    // releases its lock. If that storage can move, the view dangles -- which
    // becomes reachable the moment a second thread interns anything, and M3
    // introduces exactly that thread.
    //
    // Short-string optimisation keeping the data inside the std::string is an
    // implementation accident, not a lifetime guarantee, so this asserts the
    // guarantee directly.
    const KeyCode first = KeyCode::fromString("KeyA");
    const std::string_view view = first.toString();
    const char* const data = view.data();

    for (int i = 0; i < 4096; ++i) {
        KeyCode::fromString("Synthetic" + std::to_string(i));
    }

    KGN_CHECK_EQ(std::string(view), std::string("KeyA"));
    KGN_CHECK(view.data() == data);
}

int main() {
    return kgn::test::runAll();
}

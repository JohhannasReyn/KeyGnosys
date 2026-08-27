// The key vocabulary is principle P2: one identifier set shared by layouts,
// bindings, both OS backends and the UI. These assert the properties everything
// downstream relies on.

#include "mtk/keycode.hpp"
#include "mtk_test.hpp"

#include <string>
#include <unordered_set>

using mtk::KeyCode;

MTK_TEST(round_trips_through_string) {
    for (const char* name : {"KeyA", "KeyZ", "Digit0", "Digit9", "F1", "F24",
                             "ShiftLeft", "CapsLock", "Numpad7", "IntlBackslash",
                             "ContextMenu", "ArrowLeft"}) {
        const KeyCode code = KeyCode::fromString(name);
        MTK_CHECK(code.valid());
        MTK_CHECK_EQ(std::string(code.toString()), std::string(name));
    }
}

MTK_TEST(same_name_yields_same_id) {
    MTK_CHECK(KeyCode::fromString("KeyQ") == KeyCode::fromString("KeyQ"));
    MTK_CHECK(KeyCode::fromString("KeyQ") != KeyCode::fromString("KeyW"));
}

MTK_TEST(ids_are_unique_across_the_whole_vocabulary) {
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
        MTK_CHECK(code.valid());
        MTK_CHECK(seen.insert(code.id()).second);
    }
}

MTK_TEST(empty_name_is_invalid) {
    MTK_CHECK(!KeyCode::fromString("").valid());
    MTK_CHECK(!KeyCode{}.valid());
    MTK_CHECK(KeyCode{}.toString().empty());
}

MTK_TEST(unknown_codes_are_interned_not_rejected) {
    // A layout may reference a key this build has never heard of. It must still
    // load and render; it simply never highlights. SPEC section 2.1.
    const KeyCode odd = KeyCode::fromString("VendorSpecificThing");
    MTK_CHECK(odd.valid());
    MTK_CHECK_EQ(std::string(odd.toString()), std::string("VendorSpecificThing"));
    MTK_CHECK(odd == KeyCode::fromString("VendorSpecificThing"));
}

MTK_TEST(code_lookup_is_case_sensitive) {
    // The vocabulary is a fixed set of exact spellings. Accepting "keya" would
    // mean two ids for one physical key.
    MTK_CHECK(KeyCode::fromString("KeyA") != KeyCode::fromString("keya"));
}

MTK_TEST(modifiers_are_recognised) {
    for (const char* name : {"ShiftLeft", "ShiftRight", "ControlLeft",
                             "ControlRight", "AltLeft", "AltRight",
                             "MetaLeft", "MetaRight"}) {
        MTK_CHECK(mtk::isModifier(KeyCode::fromString(name)));
    }
}

MTK_TEST(non_modifiers_are_not_modifiers) {
    // CapsLock is deliberately excluded: it is the layer gesture, not a
    // modifier whose held state selects a legend layer.
    for (const char* name : {"KeyA", "Space", "CapsLock", "Escape", "Fn",
                             "NumLock", "F1"}) {
        MTK_CHECK(!mtk::isModifier(KeyCode::fromString(name)));
    }
}

MTK_TEST(left_and_right_modifiers_share_a_group) {
    const auto group = [](const char* n) {
        return std::string(mtk::modifierGroup(KeyCode::fromString(n)));
    };
    MTK_CHECK_EQ(group("ShiftLeft"), group("ShiftRight"));
    MTK_CHECK_EQ(group("ControlLeft"), std::string("Control"));
    MTK_CHECK_EQ(group("AltRight"), std::string("Alt"));
    MTK_CHECK_EQ(group("MetaLeft"), std::string("Meta"));
    MTK_CHECK_EQ(group("KeyA"), std::string(""));
}

int main() {
    return mtk::test::runAll();
}

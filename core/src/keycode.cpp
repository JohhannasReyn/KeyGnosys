#include "kgn/keycode.hpp"

#include <algorithm>
#include <array>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace kgn {
namespace {

// The vocabulary, in the order of docs/SPEC.md section 2.1. Index into this
// table (plus one, so that zero stays reserved for "invalid") is the KeyCode id.
//
// Codes not listed here are still usable: fromString() interns them, so a layout
// may reference a key this build has never heard of and still render it. Only
// the well-known ones get a stable, build-independent id.
const std::vector<std::string>& builtinNames() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> v;
        v.reserve(160);

        for (char c = 'A'; c <= 'Z'; ++c) v.push_back(std::string("Key") + c);
        for (char c = '0'; c <= '9'; ++c) v.push_back(std::string("Digit") + c);
        for (int i = 1; i <= 24; ++i) v.push_back("F" + std::to_string(i));

        for (const char* n : {
            "ShiftLeft", "ShiftRight", "ControlLeft", "ControlRight",
            "AltLeft", "AltRight", "MetaLeft", "MetaRight", "CapsLock",

            "Space", "Tab", "Enter", "Backspace",

            "Minus", "Equal", "BracketLeft", "BracketRight", "Backslash",
            "Semicolon", "Quote", "Backquote", "Comma", "Period", "Slash",
            "IntlBackslash",

            "Insert", "Delete", "Home", "End", "PageUp", "PageDown",
            "ArrowUp", "ArrowDown", "ArrowLeft", "ArrowRight",

            "Numpad0", "Numpad1", "Numpad2", "Numpad3", "Numpad4",
            "Numpad5", "Numpad6", "Numpad7", "Numpad8", "Numpad9",
            "NumpadAdd", "NumpadSubtract", "NumpadMultiply", "NumpadDivide",
            "NumpadDecimal", "NumpadEnter", "NumLock",

            "Escape", "PrintScreen", "ScrollLock", "Pause", "ContextMenu",

            // Present on laptop keyboards, usually invisible to software.
            // Rendered for fidelity; never promised as bindable (SPEC 8.4).
            "Fn", "FnLock",
        }) {
            v.emplace_back(n);
        }
        return v;
    }();
    return names;
}

// Interning table. Reads vastly outnumber writes -- the table is fully populated
// during config load and then only read on the input hot path -- but writes can
// happen at any time via a live config reload, so it is guarded.
struct Intern {
    // A deque, not a vector: toString() returns a string_view into this storage
    // and then releases the lock, so the storage must never move. A deque's
    // push_back never invalidates references to existing elements, and entries
    // are never erased, so a view stays valid for the process lifetime.
    //
    // A vector happens to be safe only while every name is short enough for the
    // small-string optimisation to keep its data inside the std::string object
    // -- and that object IS moved on reallocation. That is an implementation
    // accident, not a lifetime guarantee, and M3 introduces the second thread
    // that makes the difference reachable.
    std::deque<std::string> names;                  // id - 1 -> name
    std::unordered_map<std::string, std::uint16_t> ids;
    std::mutex mutex;

    Intern() {
        const auto& builtin = builtinNames();
        names.assign(builtin.begin(), builtin.end());
        ids.reserve(builtin.size() * 2);
        for (std::size_t i = 0; i < builtin.size(); ++i) {
            ids.emplace(builtin[i], static_cast<std::uint16_t>(i + 1));
        }
    }
};

Intern& intern() {
    static Intern table;
    return table;
}

const std::array<const char*, 8> kModifierNames = {
    "ShiftLeft", "ShiftRight", "ControlLeft", "ControlRight",
    "AltLeft", "AltRight", "MetaLeft", "MetaRight",
};

}  // namespace

KeyCode KeyCode::fromString(std::string_view name) {
    if (name.empty()) return KeyCode{};

    auto& table = intern();
    std::lock_guard<std::mutex> lock(table.mutex);

    const std::string key(name);
    if (auto it = table.ids.find(key); it != table.ids.end()) {
        return KeyCode{it->second};
    }

    // Unknown code: intern rather than reject. A layout referencing a key this
    // build does not know must still load and render (SPEC 2.1); it simply
    // never highlights, because no backend will emit it.
    if (table.names.size() >= KeyCode::kMaxId) return KeyCode{};  // id space full

    table.names.push_back(key);
    const auto id = static_cast<std::uint16_t>(table.names.size());
    table.ids.emplace(key, id);
    return KeyCode{id};
}

std::string_view KeyCode::toString() const {
    if (!valid()) return {};
    auto& table = intern();
    std::lock_guard<std::mutex> lock(table.mutex);
    if (id_ > table.names.size()) return {};
    return table.names[id_ - 1];
}

bool isModifier(KeyCode code) {
    return !modifierGroup(code).empty();
}

std::string_view modifierGroup(KeyCode code) {
    const std::string_view name = code.toString();
    if (name.empty()) return {};
    if (std::find(kModifierNames.begin(), kModifierNames.end(), name)
        == kModifierNames.end()) {
        return {};
    }
    // Left and right collapse to one group: a shortcut does not care which
    // hand pressed it (SPEC 4.4).
    if (name.rfind("Shift", 0) == 0) return "Shift";
    if (name.rfind("Control", 0) == 0) return "Control";
    if (name.rfind("Alt", 0) == 0) return "Alt";
    return "Meta";
}

}  // namespace kgn

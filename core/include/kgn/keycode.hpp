// The key vocabulary shared by every part of MouseTrapKeys.
//
// Keys are identified by W3C UI Events `code` values -- "KeyA", "Digit1",
// "ShiftLeft", "CapsLock", "Numpad7". They are positional rather than
// character-based, so they survive the user's OS keyboard language, and they
// are readable in a JSON file a person is expected to hand-edit.
//
// Principle P2: this is the ONLY vocabulary. Each backend owns exactly one
// translation table between this and its native representation, at the edge.
// No other module may hold a key translation table.
//
// See docs/SPEC.md section 2.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace kgn {

// A key identity. Interned to an integer id so the hot path compares ints
// rather than strings; the string form is recovered for IPC and config.
class KeyCode {
public:
    static constexpr std::uint16_t kInvalid = 0;
    // The largest id a KeyCode can hold. Every non-zero std::uint16_t is a
    // valid id, so this is the full width of the type -- consumers that index
    // state by id size their storage from it and can then accept any KeyCode
    // that valid() admits, with no boundary left to police.
    static constexpr std::uint16_t kMaxId = 0xFFFF;

    KeyCode() = default;
    explicit KeyCode(std::uint16_t id) : id_(id) {}

    // Look up (or intern) a vocabulary string. Unknown strings are interned
    // rather than rejected: a layout may legitimately reference a key this
    // build has never heard of, and it must still render.
    static KeyCode fromString(std::string_view name);

    // The canonical vocabulary string, or "" for an invalid code.
    [[nodiscard]] std::string_view toString() const;

    [[nodiscard]] bool valid() const { return id_ != kInvalid; }
    [[nodiscard]] std::uint16_t id() const { return id_; }

    bool operator==(const KeyCode& other) const { return id_ == other.id_; }
    bool operator!=(const KeyCode& other) const { return id_ != other.id_; }

private:
    std::uint16_t id_ = kInvalid;
};

enum class KeyState : std::uint8_t {
    Up = 0,
    Down = 1,
    Repeat = 2,
};

enum class MouseButton : std::uint8_t {
    Left,
    Right,
    Middle,
};

// True for the eight modifier codes, which the layer engine tracks separately.
bool isModifier(KeyCode code);

// Canonical modifier group name ("Control", "Alt", "Shift", "Meta") for a
// modifier code, or "" if it is not one. Left and right collapse to the same
// group, because a shortcut does not care which hand pressed it.
std::string_view modifierGroup(KeyCode code);

}  // namespace kgn

template <>
struct std::hash<kgn::KeyCode> {
    std::size_t operator()(const kgn::KeyCode& code) const noexcept {
        return std::hash<std::uint16_t>{}(code.id());
    }
};

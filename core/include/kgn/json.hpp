// A minimal JSON value, parser and serializer.
//
// Deliberately hand-written rather than vendored. The core takes no
// third-party dependency it would have to fetch at configure time (the same
// reasoning that kept Catch2 out of tests/kgn_test.hpp), and the IPC protocol
// needs a strict subset of JSON: no comments, no NaN, one message per line.
//
// Object members keep INSERTION order rather than sorting. Field order is not
// semantically meaningful in JSON, but golden-file tests of serialised
// messages are specified (SPEC section 13), and a message whose field order
// matches the specification's own examples is far easier to read in a failure
// diff. Lookup is linear, which is free: protocol objects have a handful of
// members.
//
// See docs/SPEC.md section 5.

#pragma once

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kgn {

class Json {
public:
    enum class Type : std::uint8_t {
        Null,
        Bool,
        Number,
        String,
        Array,
        Object,
    };

    using Array = std::vector<Json>;
    using Member = std::pair<std::string, Json>;
    using Object = std::vector<Member>;

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool value) : type_(Type::Bool), bool_(value) {}
    Json(int value) : type_(Type::Number), number_(value) {}
    Json(std::int64_t value)
        : type_(Type::Number), number_(static_cast<double>(value)) {}
    Json(std::uint64_t value)
        : type_(Type::Number), number_(static_cast<double>(value)) {}
    Json(double value) : type_(Type::Number), number_(value) {}
    Json(const char* value) : type_(Type::String), string_(value) {}
    Json(std::string value) : type_(Type::String), string_(std::move(value)) {}
    Json(std::string_view value) : type_(Type::String), string_(value) {}
    Json(Array value) : type_(Type::Array), array_(std::move(value)) {}
    Json(Object value) : type_(Type::Object), object_(std::move(value)) {}

    static Json array() { return Json(Array{}); }
    static Json object() { return Json(Object{}); }
    static Json object(std::initializer_list<Member> members) {
        return Json(Object(members));
    }

    [[nodiscard]] Type type() const { return type_; }
    [[nodiscard]] bool isNull() const { return type_ == Type::Null; }
    [[nodiscard]] bool isBool() const { return type_ == Type::Bool; }
    [[nodiscard]] bool isNumber() const { return type_ == Type::Number; }
    [[nodiscard]] bool isString() const { return type_ == Type::String; }
    [[nodiscard]] bool isArray() const { return type_ == Type::Array; }
    [[nodiscard]] bool isObject() const { return type_ == Type::Object; }

    // Typed accessors. Each returns the fallback when this value is not of the
    // requested type, so a malformed message degrades to a default rather than
    // throwing across the IPC read loop.
    [[nodiscard]] bool asBool(bool fallback = false) const {
        return type_ == Type::Bool ? bool_ : fallback;
    }
    [[nodiscard]] double asNumber(double fallback = 0.0) const {
        return type_ == Type::Number ? number_ : fallback;
    }
    [[nodiscard]] std::int64_t asInt(std::int64_t fallback = 0) const;
    [[nodiscard]] const std::string& asString() const;
    [[nodiscard]] std::string asString(std::string fallback) const {
        return type_ == Type::String ? string_ : std::move(fallback);
    }

    [[nodiscard]] const Array& items() const { return array_; }
    [[nodiscard]] const Object& members() const { return object_; }

    // Object member access. Returns a null Json when absent or when this is
    // not an object; `contains` distinguishes absent from present-and-null.
    [[nodiscard]] const Json& operator[](std::string_view key) const;
    [[nodiscard]] bool contains(std::string_view key) const;

    // Array element access. Returns a null Json when out of range.
    [[nodiscard]] const Json& at(std::size_t index) const;
    [[nodiscard]] std::size_t size() const;

    // Builders. `set` replaces an existing member of the same name in place,
    // preserving its position, so rebuilding a message cannot silently produce
    // two members with one name.
    Json& set(std::string key, Json value);
    Json& push(Json value);

    // Serialise to compact single-line JSON. Never emits a raw newline, so the
    // result is always one JSON Lines record.
    [[nodiscard]] std::string dump() const;
    void dumpTo(std::string& out) const;

    // Parse one complete JSON value. Returns false on any malformed input,
    // including trailing non-whitespace. `error` receives a human-readable
    // reason; it is never keystroke content, only structural detail.
    static bool parse(std::string_view text, Json& out, std::string& error);
    static bool parse(std::string_view text, Json& out);

private:
    Type type_ = Type::Null;
    bool bool_ = false;
    double number_ = 0.0;
    std::string string_;
    Array array_;
    Object object_;
};

}  // namespace kgn

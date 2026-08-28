#include "kgn/json.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace kgn {
namespace {

const Json& nullValue() {
    static const Json instance;
    return instance;
}

const std::string& emptyString() {
    static const std::string instance;
    return instance;
}

// ---------------------------------------------------------------------------
// Serialisation

void appendEscaped(const std::string& text, std::string& out) {
    out.push_back('"');
    for (const char raw : text) {
        const auto byte = static_cast<unsigned char>(raw);
        switch (raw) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            default:
                if (byte < 0x20) {
                    // Control characters must be escaped; a raw one would also
                    // risk breaking the one-message-per-line framing.
                    char buffer[7];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x",
                                  static_cast<unsigned>(byte));
                    out += buffer;
                } else {
                    // UTF-8 continuation bytes pass through untouched: the
                    // protocol is UTF-8, so escaping them would only bloat it.
                    out.push_back(raw);
                }
                break;
        }
    }
    out.push_back('"');
}

void appendNumber(double value, std::string& out) {
    if (!std::isfinite(value)) {
        // JSON has no representation for these. Emitting `null` keeps the
        // message parseable rather than producing a line no client can read.
        out += "null";
        return;
    }
    // Integral values print without a decimal point, so `seq` and counts read
    // as integers rather than as 1042.0.
    if (value == std::floor(value) && std::fabs(value) < 1e15) {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%lld",
                      static_cast<long long>(value));
        out += buffer;
        return;
    }
    char buffer[40];
    // 17 significant digits round-trips an IEEE-754 double exactly.
    std::snprintf(buffer, sizeof(buffer), "%.17g", value);
    out += buffer;
}

// ---------------------------------------------------------------------------
// Parsing

class Parser {
public:
    Parser(std::string_view text, std::string& error)
        : text_(text), error_(error) {}

    bool run(Json& out) {
        skipSpace();
        if (!parseValue(out, 0)) return false;
        skipSpace();
        if (pos_ != text_.size()) return fail("trailing content after value");
        return true;
    }

private:
    // Bounded so a hostile or corrupt line cannot exhaust the stack through
    // nesting alone. Protocol messages are two or three levels deep.
    static constexpr int kMaxDepth = 64;

    bool fail(const char* reason) {
        if (error_.empty()) {
            error_ = reason;
            error_ += " at offset ";
            error_ += std::to_string(pos_);
        }
        return false;
    }

    [[nodiscard]] bool done() const { return pos_ >= text_.size(); }
    [[nodiscard]] char peek() const { return text_[pos_]; }

    void skipSpace() {
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    bool literal(std::string_view word) {
        if (text_.size() - pos_ < word.size()) return false;
        if (text_.compare(pos_, word.size(), word) != 0) return false;
        pos_ += word.size();
        return true;
    }

    bool parseValue(Json& out, int depth) {
        if (depth > kMaxDepth) return fail("nesting too deep");
        if (done()) return fail("unexpected end of input");
        switch (peek()) {
            case 'n':
                if (!literal("null")) return fail("invalid literal");
                out = Json();
                return true;
            case 't':
                if (!literal("true")) return fail("invalid literal");
                out = Json(true);
                return true;
            case 'f':
                if (!literal("false")) return fail("invalid literal");
                out = Json(false);
                return true;
            case '"': {
                std::string value;
                if (!parseString(value)) return false;
                out = Json(std::move(value));
                return true;
            }
            case '[': return parseArray(out, depth);
            case '{': return parseObject(out, depth);
            default:  return parseNumber(out);
        }
    }

    bool parseArray(Json& out, int depth) {
        ++pos_;  // '['
        Json result = Json::array();
        skipSpace();
        if (!done() && peek() == ']') {
            ++pos_;
            out = std::move(result);
            return true;
        }
        for (;;) {
            skipSpace();
            Json element;
            if (!parseValue(element, depth + 1)) return false;
            result.push(std::move(element));
            skipSpace();
            if (done()) return fail("unterminated array");
            if (peek() == ',') {
                ++pos_;
                continue;
            }
            if (peek() == ']') {
                ++pos_;
                out = std::move(result);
                return true;
            }
            return fail("expected ',' or ']'");
        }
    }

    bool parseObject(Json& out, int depth) {
        ++pos_;  // '{'
        Json result = Json::object();
        skipSpace();
        if (!done() && peek() == '}') {
            ++pos_;
            out = std::move(result);
            return true;
        }
        for (;;) {
            skipSpace();
            if (done() || peek() != '"') return fail("expected member name");
            std::string key;
            if (!parseString(key)) return false;
            skipSpace();
            if (done() || peek() != ':') return fail("expected ':'");
            ++pos_;
            skipSpace();
            Json value;
            if (!parseValue(value, depth + 1)) return false;
            result.set(std::move(key), std::move(value));
            skipSpace();
            if (done()) return fail("unterminated object");
            if (peek() == ',') {
                ++pos_;
                continue;
            }
            if (peek() == '}') {
                ++pos_;
                out = std::move(result);
                return true;
            }
            return fail("expected ',' or '}'");
        }
    }

    bool parseHex4(unsigned& value) {
        if (text_.size() - pos_ < 4) return fail("truncated \\u escape");
        value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[pos_++];
            unsigned digit = 0;
            if (c >= '0' && c <= '9') {
                digit = static_cast<unsigned>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                digit = static_cast<unsigned>(c - 'a') + 10u;
            } else if (c >= 'A' && c <= 'F') {
                digit = static_cast<unsigned>(c - 'A') + 10u;
            } else {
                return fail("invalid hex digit in \\u escape");
            }
            value = (value << 4) | digit;
        }
        return true;
    }

    static void appendUtf8(unsigned cp, std::string& out) {
        if (cp < 0x80u) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800u) {
            out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
            out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        } else if (cp < 0x10000u) {
            out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
            out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        } else {
            out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
            out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        }
    }

    bool parseString(std::string& out) {
        ++pos_;  // opening quote
        out.clear();
        for (;;) {
            if (done()) return fail("unterminated string");
            const char c = text_[pos_++];
            if (c == '"') return true;
            if (c != '\\') {
                if (static_cast<unsigned char>(c) < 0x20) {
                    return fail("unescaped control character in string");
                }
                out.push_back(c);
                continue;
            }
            if (done()) return fail("unterminated escape");
            switch (text_[pos_++]) {
                case '"':  out.push_back('"');  break;
                case '\\': out.push_back('\\'); break;
                case '/':  out.push_back('/');  break;
                case 'b':  out.push_back('\b'); break;
                case 'f':  out.push_back('\f'); break;
                case 'n':  out.push_back('\n'); break;
                case 'r':  out.push_back('\r'); break;
                case 't':  out.push_back('\t'); break;
                case 'u': {
                    unsigned cp = 0;
                    if (!parseHex4(cp)) return false;
                    if (cp >= 0xD800u && cp <= 0xDBFFu) {
                        // High surrogate: a low surrogate must follow, or the
                        // text does not describe a character at all.
                        if (text_.size() - pos_ < 2 || text_[pos_] != '\\' ||
                            text_[pos_ + 1] != 'u') {
                            return fail("lone high surrogate");
                        }
                        pos_ += 2;
                        unsigned low = 0;
                        if (!parseHex4(low)) return false;
                        if (low < 0xDC00u || low > 0xDFFFu) {
                            return fail("invalid low surrogate");
                        }
                        cp = 0x10000u + ((cp - 0xD800u) << 10) + (low - 0xDC00u);
                    } else if (cp >= 0xDC00u && cp <= 0xDFFFu) {
                        return fail("lone low surrogate");
                    }
                    appendUtf8(cp, out);
                    break;
                }
                default:
                    return fail("unknown escape");
            }
        }
    }

    bool parseNumber(Json& out) {
        const std::size_t start = pos_;
        if (!done() && peek() == '-') ++pos_;
        if (done() || peek() < '0' || peek() > '9') {
            return fail("invalid number");
        }
        if (peek() == '0') {
            ++pos_;  // a leading zero admits no further integer digits
        } else {
            while (!done() && peek() >= '0' && peek() <= '9') ++pos_;
        }
        if (!done() && peek() == '.') {
            ++pos_;
            if (done() || peek() < '0' || peek() > '9') {
                return fail("expected digit after '.'");
            }
            while (!done() && peek() >= '0' && peek() <= '9') ++pos_;
        }
        if (!done() && (peek() == 'e' || peek() == 'E')) {
            ++pos_;
            if (!done() && (peek() == '+' || peek() == '-')) ++pos_;
            if (done() || peek() < '0' || peek() > '9') {
                return fail("expected digit in exponent");
            }
            while (!done() && peek() >= '0' && peek() <= '9') ++pos_;
        }
        const std::string token(text_.substr(start, pos_ - start));
        out = Json(std::strtod(token.c_str(), nullptr));
        return true;
    }

    std::string_view text_;
    std::string& error_;
    std::size_t pos_ = 0;
};

}  // namespace

// ---------------------------------------------------------------------------

std::int64_t Json::asInt(std::int64_t fallback) const {
    if (type_ != Type::Number) return fallback;
    if (!std::isfinite(number_)) return fallback;
    constexpr double kMax = 9223372036854775000.0;   // safely inside int64
    if (number_ > kMax || number_ < -kMax) return fallback;
    return static_cast<std::int64_t>(number_);
}

const std::string& Json::asString() const {
    return type_ == Type::String ? string_ : emptyString();
}

const Json& Json::operator[](std::string_view key) const {
    if (type_ != Type::Object) return nullValue();
    for (const auto& member : object_) {
        if (member.first == key) return member.second;
    }
    return nullValue();
}

bool Json::contains(std::string_view key) const {
    if (type_ != Type::Object) return false;
    for (const auto& member : object_) {
        if (member.first == key) return true;
    }
    return false;
}

const Json& Json::at(std::size_t index) const {
    if (type_ != Type::Array || index >= array_.size()) return nullValue();
    return array_[index];
}

std::size_t Json::size() const {
    if (type_ == Type::Array) return array_.size();
    if (type_ == Type::Object) return object_.size();
    return 0;
}

Json& Json::set(std::string key, Json value) {
    if (type_ != Type::Object) {
        type_ = Type::Object;
        object_.clear();
    }
    for (auto& member : object_) {
        if (member.first == key) {
            member.second = std::move(value);
            return *this;
        }
    }
    object_.emplace_back(std::move(key), std::move(value));
    return *this;
}

Json& Json::push(Json value) {
    if (type_ != Type::Array) {
        type_ = Type::Array;
        array_.clear();
    }
    array_.push_back(std::move(value));
    return *this;
}

void Json::dumpTo(std::string& out) const {
    switch (type_) {
        case Type::Null:   out += "null"; break;
        case Type::Bool:   out += bool_ ? "true" : "false"; break;
        case Type::Number: appendNumber(number_, out); break;
        case Type::String: appendEscaped(string_, out); break;
        case Type::Array: {
            out.push_back('[');
            bool first = true;
            for (const auto& element : array_) {
                if (!first) out.push_back(',');
                first = false;
                element.dumpTo(out);
            }
            out.push_back(']');
            break;
        }
        case Type::Object: {
            out.push_back('{');
            bool first = true;
            for (const auto& member : object_) {
                if (!first) out.push_back(',');
                first = false;
                appendEscaped(member.first, out);
                out.push_back(':');
                member.second.dumpTo(out);
            }
            out.push_back('}');
            break;
        }
    }
}

std::string Json::dump() const {
    std::string out;
    dumpTo(out);
    return out;
}

bool Json::parse(std::string_view text, Json& out, std::string& error) {
    error.clear();
    Json parsed;
    Parser parser(text, error);
    if (!parser.run(parsed)) {
        out = Json();
        return false;
    }
    out = std::move(parsed);
    return true;
}

bool Json::parse(std::string_view text, Json& out) {
    std::string ignored;
    return parse(text, out, ignored);
}

}  // namespace kgn

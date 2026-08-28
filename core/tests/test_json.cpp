// JSON value, parser and serializer.
//
// The IPC protocol is the only consumer, so the emphasis is on what a wire
// format has to survive: malformed input from a client, control characters,
// UTF-8, and round-tripping without gaining or losing precision.

#include "kgn/json.hpp"
#include "kgn_test.hpp"

using kgn::Json;

namespace {

Json parsed(std::string_view text) {
    Json value;
    KGN_CHECK(Json::parse(text, value));
    return value;
}

bool rejects(std::string_view text) {
    Json value;
    return !Json::parse(text, value);
}

}  // namespace

// ---------------------------------------------------------------------------
// Parsing

KGN_TEST(parses_the_scalar_types) {
    KGN_CHECK(parsed("null").isNull());
    KGN_CHECK(parsed("true").asBool());
    KGN_CHECK(!parsed("false").asBool(true));
    KGN_CHECK_EQ(parsed("42").asInt(), std::int64_t{42});
    KGN_CHECK_EQ(parsed("-7").asInt(), std::int64_t{-7});
    KGN_CHECK_EQ(parsed("\"hi\"").asString(), std::string("hi"));
}

KGN_TEST(parses_nested_objects_and_arrays) {
    const Json value = parsed(R"({"a":[1,2,{"b":true}],"c":null})");
    KGN_CHECK(value.isObject());
    KGN_CHECK_EQ(value["a"].size(), std::size_t{3});
    KGN_CHECK_EQ(value["a"].at(1).asInt(), std::int64_t{2});
    KGN_CHECK(value["a"].at(2)["b"].asBool());
    KGN_CHECK(value["c"].isNull());
}

KGN_TEST(distinguishes_absent_from_present_and_null) {
    const Json value = parsed(R"({"present":null})");
    KGN_CHECK(value.contains("present"));
    KGN_CHECK(!value.contains("absent"));
    // Both read as null, which is precisely why `contains` has to exist.
    KGN_CHECK(value["present"].isNull());
    KGN_CHECK(value["absent"].isNull());
}

KGN_TEST(decodes_string_escapes) {
    // Raw content: q=\" b=\\ n=\n t=\t
    const Json value = parsed(R"("q=\" b=\\ n=\n t=\t")");
    KGN_CHECK_EQ(value.asString(), std::string("q=\" b=\\ n=\n t=\t"));
}

KGN_TEST(decodes_surrogate_pairs_to_utf8) {
    // U+1F600. Outside the BMP, so JSON can only carry it as a pair.
    const Json value = parsed(R"("\ud83d\ude00")");
    KGN_CHECK_EQ(value.asString(), std::string("\xF0\x9F\x98\x80"));
}

KGN_TEST(passes_raw_utf8_through_unchanged) {
    // U+00E9 as its two UTF-8 bytes; the parser must not touch them.
    const Json value = parsed("\"h\xC3\xA9llo\"");
    KGN_CHECK_EQ(value.asString(), std::string("h\xC3\xA9llo"));
}

// ---------------------------------------------------------------------------
// Malformed input
//
// A client can send anything. None of it may be accepted as if it meant
// something, and none of it may take the read loop down.

KGN_TEST(rejects_malformed_documents) {
    KGN_CHECK(rejects(""));
    KGN_CHECK(rejects("   "));
    KGN_CHECK(rejects("{"));
    KGN_CHECK(rejects("}"));
    KGN_CHECK(rejects("["));
    KGN_CHECK(rejects("[1,]"));
    KGN_CHECK(rejects(R"({"a"})"));
    KGN_CHECK(rejects(R"({"a":})"));
    KGN_CHECK(rejects("{a:1}"));
    KGN_CHECK(rejects("tru"));
    KGN_CHECK(rejects("nul"));
    KGN_CHECK(rejects("\"unterminated"));
    KGN_CHECK(rejects("01"));
    KGN_CHECK(rejects("1."));
    KGN_CHECK(rejects(".1"));
    KGN_CHECK(rejects("1e"));
    KGN_CHECK(rejects("+1"));
}

KGN_TEST(rejects_trailing_content) {
    // Two values on one line is two messages, not one, and accepting it would
    // silently drop the second.
    KGN_CHECK(rejects("{} {}"));
    KGN_CHECK(rejects("1 2"));
    KGN_CHECK(rejects("null x"));
}

KGN_TEST(rejects_unescaped_control_characters_in_strings) {
    KGN_CHECK(rejects("\"a\nb\""));
    KGN_CHECK(rejects(std::string_view("\"a\0b\"", 5)));
}

KGN_TEST(rejects_lone_surrogates) {
    KGN_CHECK(rejects(R"("\ud83d")"));
    KGN_CHECK(rejects(R"("\ude00")"));
    KGN_CHECK(rejects(R"("\ud83dx")"));
}

KGN_TEST(rejects_nesting_deep_enough_to_threaten_the_stack) {
    std::string deep(200, '[');
    deep.append(200, ']');
    KGN_CHECK(rejects(deep));
}

KGN_TEST(a_failed_parse_leaves_the_output_null) {
    Json value(42);
    KGN_CHECK(!Json::parse("{oops", value));
    KGN_CHECK(value.isNull());
}

KGN_TEST(a_failed_parse_reports_a_reason) {
    Json value;
    std::string error;
    KGN_CHECK(!Json::parse("{oops", value, error));
    KGN_CHECK(!error.empty());
}

// ---------------------------------------------------------------------------
// Serialisation

KGN_TEST(serialises_compactly_and_in_insertion_order) {
    Json message = Json::object();
    message.set("v", Json(1));
    message.set("t", Json("event"));
    message.set("n", Json("key"));
    message.set("seq", Json(1042));
    KGN_CHECK_EQ(message.dump(),
                 std::string(R"({"v":1,"t":"event","n":"key","seq":1042})"));
}

KGN_TEST(set_replaces_in_place_rather_than_duplicating) {
    Json value = Json::object();
    value.set("a", Json(1));
    value.set("b", Json(2));
    value.set("a", Json(3));
    KGN_CHECK_EQ(value.size(), std::size_t{2});
    KGN_CHECK_EQ(value.dump(), std::string(R"({"a":3,"b":2})"));
}

KGN_TEST(integers_serialise_without_a_decimal_point) {
    // `seq` reading as 1042.0 would be legal JSON and would still look wrong
    // in every log and every golden file.
    KGN_CHECK_EQ(Json(1042).dump(), std::string("1042"));
    KGN_CHECK_EQ(Json(-3).dump(), std::string("-3"));
    KGN_CHECK_EQ(Json(0.5).dump(), std::string("0.5"));
}

KGN_TEST(serialisation_never_emits_a_raw_newline) {
    // One message per line is the framing. A string carrying a newline must
    // not be able to split one message into two.
    const Json value = Json(std::string("a\nb\r\tc"));
    const std::string text = value.dump();
    KGN_CHECK(text.find('\n') == std::string::npos);
    KGN_CHECK(text.find('\r') == std::string::npos);
    KGN_CHECK_EQ(text, std::string(R"("a\nb\r\tc")"));
}

KGN_TEST(escapes_control_characters_as_hex) {
    const Json value = Json(std::string("\x01\x1f"));
    KGN_CHECK_EQ(value.dump(), std::string(R"("\u0001\u001f")"));
}

KGN_TEST(round_trips_through_parse_and_dump) {
    const std::string text =
        R"({"v":1,"t":"reply","id":"c17","ok":true,"d":{"pong":true,"uptime_ms":1234}})";
    Json value;
    KGN_CHECK(Json::parse(text, value));
    KGN_CHECK_EQ(value.dump(), text);
}

KGN_TEST(round_trips_a_string_with_every_escape) {
    const Json original = Json(std::string("\"\\/\b\f\n\r\t and \xC3\xA9"));
    Json reparsed;
    KGN_CHECK(Json::parse(original.dump(), reparsed));
    KGN_CHECK_EQ(reparsed.asString(), original.asString());
}

// ---------------------------------------------------------------------------
// Accessors

KGN_TEST(typed_accessors_fall_back_rather_than_throwing) {
    const Json value = parsed(R"({"n":"text"})");
    KGN_CHECK_EQ(value["n"].asInt(-1), std::int64_t{-1});
    KGN_CHECK(value["n"].asBool(true));
    KGN_CHECK_EQ(value["missing"].asString(std::string("fallback")),
                 std::string("fallback"));
    // Indexing a non-object, and a non-array, are both survivable.
    KGN_CHECK(value["n"]["deeper"].isNull());
    KGN_CHECK(value.at(3).isNull());
}

KGN_TEST(asInt_refuses_values_that_would_not_survive_the_conversion) {
    KGN_CHECK_EQ(parsed("1e300").asInt(-1), std::int64_t{-1});
    KGN_CHECK_EQ(parsed("-1e300").asInt(-1), std::int64_t{-1});
    KGN_CHECK_EQ(parsed("9007199254740992").asInt(),
                 std::int64_t{9007199254740992});
}

int main() { return kgn::test::runAll(); }

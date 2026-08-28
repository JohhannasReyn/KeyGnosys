// Loading a bindings document.
//
// The governing rule is that a bad binding is a BINDING-level failure and
// never a document-level one (SPEC sections 4.2 and 7). Most of these tests
// are variations on that: something in the file is wrong, exactly one binding
// is lost, a diagnostic names it, and everything else still loads.

#include <string>

#include "kgn/config.hpp"
#include "kgn_test.hpp"

using kgn::ActionId;
using kgn::BindingsDocument;
using kgn::Diagnostics;
using kgn::Direction;
using kgn::KeyCode;
using kgn::MouseButton;

namespace {

KeyCode key(const char* name) { return KeyCode::fromString(name); }

bool load(std::string_view text, BindingsDocument& doc, Diagnostics& diags) {
    return kgn::parseBindingsDocument(text, "test.json", doc, diags);
}

bool hasCode(const Diagnostics& diags, std::string_view code) {
    for (const auto& diag : diags) {
        if (diag.code == code) return true;
    }
    return false;
}

constexpr std::string_view kMinimal = R"({
  "schema": "keygnosys/bindings/2",
  "id": "default",
  "name": "Default cursor layer",
  "bindings": {
    "KeyH": { "action": "pointer.move", "params": { "dir": "left" } },
    "KeyL": { "action": "pointer.move", "params": { "dir": "right" } },
    "Space": { "action": "button.click", "params": { "button": "left" } }
  }
})";

}  // namespace

// ---------------------------------------------------------------------------
// Schema

KGN_TEST(schema_version_is_parsed_from_the_schema_string) {
    KGN_CHECK_EQ(kgn::schemaVersionOf("keygnosys/bindings/2", "bindings"), 2);
    KGN_CHECK_EQ(kgn::schemaVersionOf("keygnosys/bindings/1", "bindings"), 1);
    KGN_CHECK_EQ(kgn::schemaVersionOf("keygnosys/layouts/2", "bindings"), 0);
    KGN_CHECK_EQ(kgn::schemaVersionOf("keygnosys/bindings/", "bindings"), 0);
    KGN_CHECK_EQ(kgn::schemaVersionOf("keygnosys/bindings/x", "bindings"), 0);
    KGN_CHECK_EQ(kgn::schemaVersionOf("", "bindings"), 0);
    // The prefix must match exactly. A document from some other project, or
    // from this one under a former name, is not silently accepted.
    KGN_CHECK_EQ(kgn::schemaVersionOf("someoneelse/bindings/2", "bindings"), 0);
    KGN_CHECK_EQ(kgn::schemaVersionOf("keygnosys/bindings", "bindings"), 0);
}

// ---------------------------------------------------------------------------
// Document-level failures
//
// These are the only cases where the whole file is refused.

KGN_TEST(a_document_that_is_not_json_is_refused) {
    BindingsDocument doc;
    Diagnostics diags;
    KGN_CHECK(!load("{ not json", doc, diags));
    KGN_CHECK(hasCode(diags, "binding.invalid"));
}

KGN_TEST(a_document_that_is_not_an_object_is_refused) {
    BindingsDocument doc;
    Diagnostics diags;
    KGN_CHECK(!load("[1,2,3]", doc, diags));
    KGN_CHECK(hasCode(diags, "binding.invalid"));
}

KGN_TEST(a_missing_or_foreign_schema_is_refused) {
    BindingsDocument doc;
    Diagnostics diags;
    KGN_CHECK(!load(R"({"id":"x"})", doc, diags));
    KGN_CHECK(!load(R"({"schema":"keygnosys/themes/1","id":"x"})", doc, diags));
}

KGN_TEST(a_schema_newer_than_this_build_is_refused_rather_than_guessed_at) {
    BindingsDocument doc;
    Diagnostics diags;
    KGN_CHECK(!load(R"({"schema":"keygnosys/bindings/99","id":"x"})", doc, diags));
    KGN_CHECK(hasCode(diags, "binding.invalid"));
}

KGN_TEST(a_document_with_no_id_is_refused) {
    BindingsDocument doc;
    Diagnostics diags;
    KGN_CHECK(!load(R"({"schema":"keygnosys/bindings/2"})", doc, diags));
}

// ---------------------------------------------------------------------------
// Loading

KGN_TEST(loads_a_minimal_document) {
    BindingsDocument doc;
    Diagnostics diags;
    KGN_CHECK(load(kMinimal, doc, diags));
    KGN_CHECK_EQ(doc.id, std::string("default"));
    KGN_CHECK_EQ(doc.name, std::string("Default cursor layer"));
    KGN_CHECK_EQ(doc.schemaVersion, 2);
    KGN_CHECK_EQ(doc.bindings.size(), std::size_t{3});
    KGN_CHECK(doc.bindings.at(key("KeyH")).dir == Direction::Left);
    KGN_CHECK(doc.bindings.at(key("Space")).button == MouseButton::Left);
    KGN_CHECK(diags.empty());
}

KGN_TEST(a_schema_1_document_still_loads) {
    // Schema 1 had no metadata and no unassigned; both simply default to
    // empty, and the bindings themselves are unchanged.
    BindingsDocument doc;
    Diagnostics diags;
    KGN_CHECK(load(R"({
        "schema": "keygnosys/bindings/1",
        "id": "old",
        "bindings": { "KeyJ": { "action": "pointer.move", "params": {"dir":"down"} } }
    })", doc, diags));
    KGN_CHECK_EQ(doc.schemaVersion, 1);
    KGN_CHECK_EQ(doc.bindings.size(), std::size_t{1});
}

KGN_TEST(a_document_with_no_bindings_block_loads_empty) {
    BindingsDocument doc;
    Diagnostics diags;
    KGN_CHECK(load(R"({"schema":"keygnosys/bindings/2","id":"empty"})", doc, diags));
    KGN_CHECK(doc.bindings.empty());
}

KGN_TEST(the_engine_map_carries_only_the_classification) {
    BindingsDocument doc;
    Diagnostics diags;
    KGN_CHECK(load(R"({
        "schema": "keygnosys/bindings/2",
        "id": "x",
        "bindings": {
            "KeyL": { "action": "pointer.move", "params": {"dir":"right"} },
            "Enter": { "action": "key.passthrough", "params": {"code":"Enter"} }
        }
    })", doc, diags));

    const kgn::BindingMap map = doc.toBindingMap();
    KGN_CHECK_EQ(map.size(), std::size_t{2});
    KGN_CHECK(map.at(key("KeyL")) == kgn::BindingKind::Action);
    KGN_CHECK(map.at(key("Enter")) == kgn::BindingKind::Passthrough);
}

// ---------------------------------------------------------------------------
// Binding-level failures

KGN_TEST(an_unknown_action_costs_exactly_that_binding) {
    BindingsDocument doc;
    Diagnostics diags;
    KGN_CHECK(load(R"({
        "schema": "keygnosys/bindings/2",
        "id": "x",
        "bindings": {
            "KeyH": { "action": "pointer.move", "params": {"dir":"left"} },
            "KeyQ": { "action": "pointer.teleport", "params": {} },
            "KeyL": { "action": "pointer.move", "params": {"dir":"right"} }
        }
    })", doc, diags));
    KGN_CHECK_EQ(doc.bindings.size(), std::size_t{2});
    KGN_CHECK(doc.bindings.count(key("KeyQ")) == 0);
    KGN_CHECK(hasCode(diags, "binding.unknown_action"));
}

KGN_TEST(an_invalid_parameter_set_costs_exactly_that_binding) {
    BindingsDocument doc;
    Diagnostics diags;
    KGN_CHECK(load(R"({
        "schema": "keygnosys/bindings/2",
        "id": "x",
        "bindings": {
            "KeyH": { "action": "pointer.move", "params": {"dir":"sideways"} },
            "KeyL": { "action": "pointer.move", "params": {"dir":"right"} }
        }
    })", doc, diags));
    KGN_CHECK_EQ(doc.bindings.size(), std::size_t{1});
    KGN_CHECK(doc.bindings.count(key("KeyL")) == 1);
    KGN_CHECK(hasCode(diags, "binding.unknown_action"));
}

KGN_TEST(a_binding_that_is_not_an_object_costs_exactly_that_binding) {
    BindingsDocument doc;
    Diagnostics diags;
    KGN_CHECK(load(R"({
        "schema": "keygnosys/bindings/2",
        "id": "x",
        "bindings": {
            "KeyH": "pointer.move",
            "KeyL": { "action": "pointer.move", "params": {"dir":"right"} }
        }
    })", doc, diags));
    KGN_CHECK_EQ(doc.bindings.size(), std::size_t{1});
    KGN_CHECK(hasCode(diags, "binding.unknown_action"));
}

KGN_TEST(an_unusable_key_name_costs_exactly_that_binding) {
    BindingsDocument doc;
    Diagnostics diags;
    KGN_CHECK(load(R"({
        "schema": "keygnosys/bindings/2",
        "id": "x",
        "bindings": {
            "": { "action": "pointer.move", "params": {"dir":"left"} },
            "KeyL": { "action": "pointer.move", "params": {"dir":"right"} }
        }
    })", doc, diags));
    KGN_CHECK_EQ(doc.bindings.size(), std::size_t{1});
    KGN_CHECK(hasCode(diags, "binding.unknown_key"));
}

KGN_TEST(a_key_this_build_has_never_heard_of_is_still_bindable) {
    // Interning rather than rejecting is deliberate: a layout may reference a
    // key this build does not know, and it must still work.
    BindingsDocument doc;
    Diagnostics diags;
    KGN_CHECK(load(R"({
        "schema": "keygnosys/bindings/2",
        "id": "x",
        "bindings": {
            "LaunchCoffee": { "action": "overlay.toggle", "params": {} }
        }
    })", doc, diags));
    KGN_CHECK_EQ(doc.bindings.size(), std::size_t{1});
    KGN_CHECK(!hasCode(diags, "binding.unknown_key"));
}

KGN_TEST(a_command_in_both_bindings_and_unassigned_keeps_the_binding) {
    BindingsDocument doc;
    Diagnostics diags;
    KGN_CHECK(load(R"({
        "schema": "keygnosys/bindings/2",
        "id": "x",
        "bindings": {
            "KeyM": { "action": "overlay.toggle", "params": {} }
        },
        "unassigned": [
            { "action": "overlay.toggle", "params": {} }
        ]
    })", doc, diags));
    KGN_CHECK_EQ(doc.bindings.size(), std::size_t{1});
    KGN_CHECK(doc.bindings.at(key("KeyM")).id == ActionId::OverlayToggle);
    KGN_CHECK(hasCode(diags, "binding.unassigned_conflict"));
}

KGN_TEST(an_unassigned_entry_with_no_conflict_is_silent) {
    BindingsDocument doc;
    Diagnostics diags;
    KGN_CHECK(load(R"({
        "schema": "keygnosys/bindings/2",
        "id": "x",
        "bindings": { "KeyM": { "action": "overlay.toggle", "params": {} } },
        "unassigned": [ { "action": "system.reload", "params": {} } ]
    })", doc, diags));
    KGN_CHECK(!hasCode(diags, "binding.unassigned_conflict"));
}

// ---------------------------------------------------------------------------
// Settings

KGN_TEST(settings_are_read_and_defaults_survive_their_absence) {
    BindingsDocument doc;
    Diagnostics diags;
    KGN_CHECK(load(R"({
        "schema": "keygnosys/bindings/2",
        "id": "x",
        "settings": {
            "pointer_base_speed": 3,
            "pointer_max_speed": 30,
            "pointer_ramp_ms": 400,
            "precision_factor": 0.5,
            "scroll_base_speed": 2,
            "scroll_max_speed": 8,
            "scroll_ramp_ms": 600
        }
    })", doc, diags));
    KGN_CHECK(doc.pointer.baseSpeed == 3.0);
    KGN_CHECK(doc.pointer.maxSpeed == 30.0);
    KGN_CHECK(doc.pointer.rampMs == std::chrono::milliseconds(400));
    KGN_CHECK(doc.pointer.precisionFactor == 0.5);
    KGN_CHECK(doc.scroll.baseSpeed == 2.0);
    KGN_CHECK(doc.scroll.maxSpeed == 8.0);
    KGN_CHECK(doc.scroll.rampMs == std::chrono::milliseconds(600));
    KGN_CHECK(doc.scroll.precisionFactor == 0.5);
    KGN_CHECK(diags.empty());
}

KGN_TEST(scroll_falls_back_to_its_own_documented_defaults_not_the_pointers) {
    BindingsDocument doc;
    Diagnostics diags;
    KGN_CHECK(load(R"({"schema":"keygnosys/bindings/2","id":"x"})", doc, diags));
    KGN_CHECK(doc.pointer.baseSpeed == 2.0);
    KGN_CHECK(doc.pointer.maxSpeed == 28.0);
    KGN_CHECK(doc.pointer.rampMs == std::chrono::milliseconds(420));
    KGN_CHECK(doc.scroll.baseSpeed == 1.0);
    KGN_CHECK(doc.scroll.maxSpeed == 6.0);
    KGN_CHECK(doc.scroll.rampMs == std::chrono::milliseconds(500));
}

KGN_TEST(out_of_range_settings_are_clamped_and_reported) {
    BindingsDocument doc;
    Diagnostics diags;
    KGN_CHECK(load(R"({
        "schema": "keygnosys/bindings/2",
        "id": "x",
        "settings": { "pointer_base_speed": -5, "precision_factor": 12 }
    })", doc, diags));
    KGN_CHECK(doc.pointer.baseSpeed > 0.0);
    KGN_CHECK(doc.pointer.precisionFactor <= 1.0);
    KGN_CHECK(hasCode(diags, "config.clamped"));
}

KGN_TEST(a_ceiling_below_the_floor_is_levelled_rather_than_run_backwards) {
    BindingsDocument doc;
    Diagnostics diags;
    KGN_CHECK(load(R"({
        "schema": "keygnosys/bindings/2",
        "id": "x",
        "settings": { "pointer_base_speed": 20, "pointer_max_speed": 5 }
    })", doc, diags));
    KGN_CHECK(doc.pointer.maxSpeed == doc.pointer.baseSpeed);
    KGN_CHECK(hasCode(diags, "config.clamped"));
}

KGN_TEST(settings_of_the_wrong_type_leave_the_default_in_place) {
    BindingsDocument doc;
    Diagnostics diags;
    KGN_CHECK(load(R"({
        "schema": "keygnosys/bindings/2",
        "id": "x",
        "settings": { "pointer_base_speed": "fast", "pointer_ramp_ms": null }
    })", doc, diags));
    KGN_CHECK(doc.pointer.baseSpeed == 2.0);
    KGN_CHECK(doc.pointer.rampMs == std::chrono::milliseconds(420));
}

int main() { return kgn::test::runAll(); }

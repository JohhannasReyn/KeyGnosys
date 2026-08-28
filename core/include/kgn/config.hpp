// Loading a bindings document into the shape the engine and dispatcher need.
//
// Parsing is pure: it takes the document's text and returns a value. Reading
// files is the caller's business, which keeps this testable against literals
// and keeps kgn_engine free of the filesystem.
//
// The governing rule is SPEC section 7's last paragraph, and section 4.2: a
// bad binding is a BINDING-level failure, never a document-level one. One
// unknown action or one invalid parameter set costs exactly that binding; a
// diagnostic names it, and every other binding in the file still loads.
//
// See docs/SPEC.md section 4.2.

#pragma once

#include <string>
#include <string_view>
#include <unordered_map>

#include "kgn/actions.hpp"
#include "kgn/diagnostics.hpp"
#include "kgn/keycode.hpp"
#include "kgn/motion.hpp"

namespace kgn {

struct BindingsDocument {
    std::string id;
    std::string name;
    int schemaVersion = 0;

    MotionSettings pointer;
    MotionSettings scroll;

    std::unordered_map<KeyCode, Action> bindings;

    // The engine takes only the Action/Passthrough classification; it has no
    // business knowing what an action does.
    [[nodiscard]] BindingMap toBindingMap() const;
};

// Parse a `keygnosys/bindings/<n>` document.
//
// Returns false only for a DOCUMENT-level failure: text that is not JSON, a
// root that is not an object, or a schema string this build does not
// recognise. Everything survivable is reported through `diags` and leaves the
// rest of the document intact.
bool parseBindingsDocument(std::string_view text, std::string_view sourceName,
                           BindingsDocument& out, Diagnostics& diags);

// Parse the `schema` field's trailing version number, e.g.
// "keygnosys/bindings/2" -> 2. Returns 0 when the string does not name this
// kind of document at all.
[[nodiscard]] int schemaVersionOf(std::string_view schema,
                                  std::string_view kind);

}  // namespace kgn

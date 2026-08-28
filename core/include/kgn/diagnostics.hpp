// Diagnostics: the core's one way of saying that something is wrong.
//
// Every diagnostic carries a stable machine-readable `code` from the table in
// SPEC section 11, so a client can react to a condition without parsing
// English. The message is for a human and its wording is not a contract.
//
// P6 lives here: one bad file never stops the rest from loading, and an
// unavailable capability is reported and disabled rather than emulated with
// something that merely looks similar. Every path that degrades emits one of
// these.
//
// Nothing in a diagnostic may ever contain keystroke content (SPEC section
// 12.1). Key *codes* are positional and may appear; characters may not.

#pragma once

#include <string>
#include <utility>
#include <vector>

namespace kgn {

enum class DiagLevel : std::uint8_t { Info, Warn, Error };

[[nodiscard]] inline const char* diagLevelName(DiagLevel level) {
    switch (level) {
        case DiagLevel::Info: return "info";
        case DiagLevel::Warn: return "warn";
        case DiagLevel::Error: return "error";
    }
    return "info";
}

struct Diagnostic {
    DiagLevel level = DiagLevel::Info;
    std::string code;      // e.g. "binding.unknown_action"
    std::string message;
    std::string file;      // optional; the document the problem was found in

    Diagnostic() = default;
    Diagnostic(DiagLevel l, std::string c, std::string m, std::string f = {})
        : level(l), code(std::move(c)), message(std::move(m)), file(std::move(f)) {}
};

using Diagnostics = std::vector<Diagnostic>;

}  // namespace kgn

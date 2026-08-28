#include "kgn/config.hpp"

#include <algorithm>

namespace kgn {
namespace {

constexpr int kHighestSupportedBindingsSchema = 2;

// Defensive bounds for the motion settings.
//
// SPEC section 4.2 gives the settings and their defaults but no ranges, unlike
// section 4.5 which bounds the appearance values explicitly. These are chosen
// to exclude only the values that would make the integrator useless -- a
// non-positive speed that never moves, a precision factor above 1 that made
// the precision key faster than normal -- rather than to express taste. Every
// clamp is reported as `config.clamped`, as section 4.5 requires of clamping
// generally, so a user who wrote something out of range is told.
constexpr double kMinSpeed = 0.01;
constexpr double kMaxSpeed = 10000.0;
constexpr double kMinPrecision = 0.001;
constexpr double kMaxPrecision = 1.0;
constexpr long long kMaxRampMs = 60000;

double clampNumber(const Json& value, double fallback, double low, double high,
                   const char* field, std::string_view source,
                   Diagnostics& diags) {
    if (!value.isNumber()) return fallback;
    const double raw = value.asNumber(fallback);
    const double clamped = std::clamp(raw, low, high);
    if (clamped != raw) {
        diags.emplace_back(DiagLevel::Info, "config.clamped",
                           std::string(field) + " was out of range and was clamped",
                           std::string(source));
    }
    return clamped;
}

void readMotionSettings(const Json& settings, std::string_view prefix,
                        MotionSettings& out, std::string_view source,
                        Diagnostics& diags) {
    const std::string base = std::string(prefix) + "_base_speed";
    const std::string max = std::string(prefix) + "_max_speed";
    const std::string ramp = std::string(prefix) + "_ramp_ms";

    out.baseSpeed = clampNumber(settings[base], out.baseSpeed, kMinSpeed,
                                kMaxSpeed, base.c_str(), source, diags);
    out.maxSpeed = clampNumber(settings[max], out.maxSpeed, kMinSpeed, kMaxSpeed,
                               max.c_str(), source, diags);
    const double rampMs =
        clampNumber(settings[ramp], static_cast<double>(out.rampMs.count()), 0.0,
                    static_cast<double>(kMaxRampMs), ramp.c_str(), source, diags);
    out.rampMs = std::chrono::milliseconds(static_cast<long long>(rampMs));

    // A ceiling below the floor is not a taste; it is a contradiction, and it
    // would make the ramp run backwards. Report and level them.
    if (out.maxSpeed < out.baseSpeed) {
        diags.emplace_back(DiagLevel::Info, "config.clamped",
                           max + " was below " + base + "; raised to match",
                           std::string(source));
        out.maxSpeed = out.baseSpeed;
    }
}

}  // namespace

int schemaVersionOf(std::string_view schema, std::string_view kind) {
    // "keygnosys/<kind>/<n>"
    std::string prefix = "keygnosys/";
    prefix.append(kind);
    prefix += "/";
    if (schema.size() <= prefix.size()) return 0;
    if (schema.compare(0, prefix.size(), prefix) != 0) return 0;

    int version = 0;
    for (std::size_t i = prefix.size(); i < schema.size(); ++i) {
        const char c = schema[i];
        if (c < '0' || c > '9') return 0;
        version = version * 10 + (c - '0');
        if (version > 9999) return 0;
    }
    return version;
}

BindingMap BindingsDocument::toBindingMap() const {
    BindingMap map;
    map.reserve(bindings.size());
    for (const auto& [code, action] : bindings) {
        map.emplace(code, bindingKindFor(action.id));
    }
    return map;
}

bool parseBindingsDocument(std::string_view text, std::string_view sourceName,
                           BindingsDocument& out, Diagnostics& diags) {
    Json root;
    std::string error;
    if (!Json::parse(text, root, error)) {
        diags.emplace_back(DiagLevel::Warn, "binding.invalid",
                           "document is not valid JSON: " + error,
                           std::string(sourceName));
        return false;
    }
    if (!root.isObject()) {
        diags.emplace_back(DiagLevel::Warn, "binding.invalid",
                           "document root is not an object",
                           std::string(sourceName));
        return false;
    }

    const int version = schemaVersionOf(root["schema"].asString(), "bindings");
    if (version == 0) {
        diags.emplace_back(DiagLevel::Warn, "binding.invalid",
                           "schema is missing or is not a bindings document",
                           std::string(sourceName));
        return false;
    }
    if (version > kHighestSupportedBindingsSchema) {
        diags.emplace_back(DiagLevel::Warn, "binding.invalid",
                           "schema version " + std::to_string(version) +
                               " is newer than this build understands",
                           std::string(sourceName));
        return false;
    }

    BindingsDocument doc;
    doc.schemaVersion = version;
    doc.id = root["id"].asString();
    doc.name = root["name"].asString();
    if (doc.id.empty()) {
        diags.emplace_back(DiagLevel::Warn, "binding.invalid",
                           "document has no id", std::string(sourceName));
        return false;
    }

    const Json& settings = root["settings"];
    if (settings.isObject()) {
        readMotionSettings(settings, "pointer", doc.pointer, sourceName, diags);
        readMotionSettings(settings, "scroll", doc.scroll, sourceName, diags);
        const double precision =
            clampNumber(settings["precision_factor"], doc.pointer.precisionFactor,
                        kMinPrecision, kMaxPrecision, "precision_factor",
                        sourceName, diags);
        doc.pointer.precisionFactor = precision;
        doc.scroll.precisionFactor = precision;
    }
    // Scroll defaults differ from pointer defaults, and MotionSettings carries
    // the pointer ones. Where the document said nothing, use section 4.2's
    // documented scroll defaults rather than the pointer's.
    if (!settings.isObject() || !settings["scroll_base_speed"].isNumber()) {
        doc.scroll.baseSpeed = 1.0;
    }
    if (!settings.isObject() || !settings["scroll_max_speed"].isNumber()) {
        doc.scroll.maxSpeed = 6.0;
    }
    if (!settings.isObject() || !settings["scroll_ramp_ms"].isNumber()) {
        doc.scroll.rampMs = std::chrono::milliseconds(500);
    }

    const Json& bindings = root["bindings"];
    if (bindings.isObject()) {
        for (const auto& [keyName, entry] : bindings.members()) {
            const KeyCode code = KeyCode::fromString(keyName);
            if (!code.valid()) {
                diags.emplace_back(DiagLevel::Warn, "binding.unknown_key",
                                   "binding names key '" + keyName +
                                       "', which is not a usable key code",
                                   std::string(sourceName));
                continue;
            }
            if (!entry.isObject()) {
                diags.emplace_back(DiagLevel::Warn, "binding.unknown_action",
                                   "binding for '" + keyName +
                                       "' is not an object; skipped",
                                   std::string(sourceName));
                continue;
            }
            Action action;
            std::string reason;
            if (!parseAction(entry["action"].asString(), entry["params"], action,
                             reason)) {
                diags.emplace_back(DiagLevel::Warn, "binding.unknown_action",
                                   "binding for '" + keyName + "' skipped: " + reason,
                                   std::string(sourceName));
                continue;
            }
            doc.bindings[code] = action;
        }
    }

    // `unassigned` holds commands bound to no key. The core has nothing to do
    // with them -- they exist for the editor's benefit (SPEC section 10.2.3) --
    // but the conflict rule is a loader obligation, so it is honoured here.
    const Json& unassigned = root["unassigned"];
    if (unassigned.isArray()) {
        for (const auto& entry : unassigned.items()) {
            if (!entry.isObject()) continue;
            Action action;
            std::string reason;
            if (!parseAction(entry["action"].asString(), entry["params"], action,
                             reason)) {
                continue;   // dropped with no diagnostic; the editor owns this list
            }
            for (const auto& [code, bound] : doc.bindings) {
                if (bound.id != action.id) continue;
                diags.emplace_back(
                    DiagLevel::Warn, "binding.unassigned_conflict",
                    "'" + std::string(actionName(action.id)) +
                        "' appears in both bindings and unassigned; the binding wins",
                    std::string(sourceName));
                break;
            }
        }
    }

    out = std::move(doc);
    return true;
}

}  // namespace kgn

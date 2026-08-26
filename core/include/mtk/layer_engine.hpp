// The layer engine: the state machine that decides what every key means.
//
// This class is deliberately PURE. It takes events and a clock and returns
// decisions; it touches no OS API, opens no device, and starts no thread. That
// is what makes the concurrency-sensitive logic inherited from the original
// prototype testable at all -- see docs/SPEC.md section 12.
//
// It is also where principle P7 lives: every forwarded key press is tracked so
// its release is guaranteed to be forwarded too, whatever the mode has become
// in the meantime.
//
// See docs/SPEC.md section 6.3.

#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "mtk/keycode.hpp"

namespace mtk {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

enum class Mode : std::uint8_t {
    Normal,
    Cursor,
};

enum class ActivationMode : std::uint8_t {
    Toggle,   // tap CapsLock to latch on and off
    Hold,     // engaged only while CapsLock is physically down
    Hybrid,   // tap latches, hold is momentary -- the default
};

struct EngineConfig {
    ActivationMode activation = ActivationMode::Hybrid;
    std::chrono::milliseconds hybridTap{200};
    // How long an ambiguous press is buffered waiting to see whether CapsLock
    // follows it. Inherited from the prototype's GRACE directive.
    std::chrono::milliseconds grace{50};
    // When true, CapsLock pressed while Shift is held produces a real
    // CapsLock instead of engaging the layer.
    bool shiftCapsIsRealCapsLock = true;
};

// What the engine decided to do about an event. The caller performs these;
// the engine performs nothing itself.
struct Decision {
    enum class Kind : std::uint8_t {
        Suppress,        // consume it; nothing reaches the OS
        Forward,         // pass it through unchanged
        RunAction,       // it is bound in the cursor layer
        ReleaseAction,   // a held cursor-layer binding was released
        Buffer,          // ambiguous; held pending the grace window
    };

    Kind kind = Kind::Forward;
    KeyCode code;
    KeyState state = KeyState::Down;
};

class LayerEngine {
public:
    explicit LayerEngine(EngineConfig config = {});

    void setConfig(const EngineConfig& config);
    void setBoundKeys(std::unordered_set<KeyCode> bound);

    // Feed one physical key event. Returns every decision it produced -- one
    // event can produce several, because resolving a buffered press emits both
    // its press and its release.
    std::vector<Decision> onKey(KeyCode code, KeyState state, TimePoint now);

    // Must be called regularly. Resolves buffered presses whose grace window
    // has lapsed without CapsLock arriving.
    std::vector<Decision> tick(TimePoint now);

    // Release everything, unconditionally. Every exit path calls this (P7).
    std::vector<Decision> releaseAll();

    [[nodiscard]] Mode mode() const { return mode_; }
    [[nodiscard]] bool latched() const { return latched_; }
    [[nodiscard]] std::unordered_set<KeyCode> heldActions() const {
        return heldActions_;
    }

private:
    struct Pending {
        TimePoint pressedAt;
    };

    std::vector<Decision> onCapsLock(KeyState state, TimePoint now);
    void enterCursorMode(bool latched);
    std::vector<Decision> leaveCursorMode();

    EngineConfig config_;
    Mode mode_ = Mode::Normal;
    bool latched_ = false;
    std::optional<TimePoint> capsPressedAt_;

    std::unordered_set<KeyCode> bound_;
    std::unordered_set<KeyCode> heldActions_;
    std::unordered_map<KeyCode, Pending> pending_;

    // Keys whose PRESS we forwarded to the OS. Their release MUST also be
    // forwarded, regardless of the mode by then. This set is the whole of
    // principle P7, and nothing may remove an entry except the matching
    // release or releaseAll().
    std::unordered_set<KeyCode> passthrough_;

    std::unordered_set<KeyCode> heldModifiers_;
};

}  // namespace mtk

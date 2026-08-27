// The layer engine: the state machine that decides what every key means.
//
// This class is deliberately PURE. It takes events and a clock and returns
// decisions; it touches no OS API, opens no device, and starts no thread. That
// is what makes the concurrency-sensitive logic inherited from the original
// prototype testable at all -- see docs/SPEC.md section 13.
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

    // The subset of bound keys whose binding is `key.passthrough` (SPEC 7.6).
    // These reach the OS even inside the layer. They must be an explicit
    // binding: a key with no binding at all stays suppressed.
    void setPassthroughKeys(std::unordered_set<KeyCode> keys);

    // Feed one physical key event, appending to `out`.
    //
    // One event can produce several decisions: resolving a buffered press
    // emits both its press and its release, and a CapsLock press can promote
    // several buffered keys at once.
    //
    // Appends rather than returns because on Windows this runs inside the
    // low-level hook, which must decide suppression synchronously and return
    // fast. The caller keeps one buffer and clears it between events, so the
    // hot path does not allocate.
    void onKey(KeyCode code, KeyState state, TimePoint now,
               std::vector<Decision>& out);

    // Must be called regularly. Resolves buffered presses whose grace window
    // has lapsed without CapsLock arriving.
    void tick(TimePoint now, std::vector<Decision>& out);

    // Release everything, unconditionally. Every exit path calls this (P7).
    void releaseAll(std::vector<Decision>& out);

    // Allocating conveniences. For tests and for callers not on the hot path.
    std::vector<Decision> onKey(KeyCode code, KeyState state, TimePoint now);
    std::vector<Decision> tick(TimePoint now);
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

    void onCapsLock(KeyState state, TimePoint now, std::vector<Decision>& out);
    void promoteBuffered(TimePoint now, std::vector<Decision>& out);
    void leaveCursorMode(std::vector<Decision>& out);

    // Emit a Forward for a press, recording it so its release is guaranteed to
    // be forwarded too. Every forwarded press goes through here; that is what
    // makes principle P7 provable rather than merely intended.
    void forwardPress(KeyCode code, KeyState state, std::vector<Decision>& out);

    EngineConfig config_;
    Mode mode_ = Mode::Normal;
    bool latched_ = false;
    std::optional<TimePoint> capsPressedAt_;
    // True when the current CapsLock press was forwarded to the OS as a real
    // CapsLock. Its release must be forwarded too.
    bool capsForwarded_ = false;
    // Whether the layer was already latched when the current CapsLock press
    // began. In hybrid mode this is what separates a tap that latches on from
    // a tap that latches off -- the press itself clears `latched_`, so the
    // release has no other way to tell the two apart.
    bool capsWasLatched_ = false;

    std::unordered_set<KeyCode> bound_;
    std::unordered_set<KeyCode> passthroughKeys_;
    std::unordered_set<KeyCode> heldActions_;
    std::unordered_map<KeyCode, Pending> pending_;

    // Keys whose PRESS we forwarded to the OS. Their release MUST also be
    // forwarded, regardless of the mode by then. This set is the whole of
    // principle P7, and nothing may remove an entry except the matching
    // release or releaseAll().
    //
    // Everything that reaches the OS lands here -- ordinary typing, modifiers
    // forwarded inside the layer, keys replayed when the grace window lapses,
    // explicit `key.passthrough` bindings, and a real CapsLock. There is no
    // second path to the OS, which is what makes the invariant checkable.
    std::unordered_set<KeyCode> forwardedPresses_;

    std::unordered_set<KeyCode> heldModifiers_;
};

}  // namespace mtk

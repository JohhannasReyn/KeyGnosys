// The layer engine: the state machine that decides what every key means.
//
// This class is deliberately PURE. It takes events and a clock and returns
// decisions; it touches no OS API, opens no device, and starts no thread. That
// is what makes the concurrency-sensitive logic inherited from the original
// prototype testable at all -- see docs/SPEC.md section 13.
//
// It is also where principle P7 lives: every forwarded key press is tracked so
// its release is guaranteed to be forwarded too, whatever the mode has become
// in the meantime -- and its mirror, that no release is ever forwarded without
// a matching press.
//
// See docs/SPEC.md section 6.3.

#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
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

// How a key behaves inside the cursor layer.
//
// One value per key, in one map. A key cannot be passthrough without being
// bound, because having an entry is what being bound *means* -- there is no
// second set that could disagree with the first. See setBindings().
enum class BindingKind : std::uint8_t {
    Action,        // runs a cursor-layer action; suppressed
    Passthrough,   // `key.passthrough` -- reaches the OS even inside the layer
};

using BindingMap = std::unordered_map<KeyCode, BindingKind>;

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

// ---------------------------------------------------------------------------
// Capacities
//
// The engine processes events without allocating, which means every container
// it touches on the event path has a fixed size. These are the bounds. All sit
// far above what a physical keyboard can produce -- a human has ten fingers --
// and each has a defined, safe behaviour on overflow.
// ---------------------------------------------------------------------------

// KeyCode ids the engine tracks state for. The built-in vocabulary is under
// 200 entries and backends only ever emit vocabulary codes, so this is not
// reachable in practice. A code beyond it is handled on a stateless path that
// forwards symmetrically (see onKey), which keeps both P7 and its mirror
// intact but means such a key can never be bound.
inline constexpr std::size_t kMaxTrackedKeyId = 512;

// Simultaneously buffered presses. Overflow forwards the press immediately,
// degrading to no-grace-window rather than dropping the keystroke.
inline constexpr std::size_t kMaxPending = 32;

// Simultaneously held keys, in each of the two ordered lists.
inline constexpr std::size_t kMaxHeld = 64;

// Decisions from one call. The worst case is releaseAll() unwinding every held
// action and every forwarded press at once.
inline constexpr std::size_t kDecisionCapacity = 192;

// A fixed-capacity decision sink.
//
// Not std::vector: a caller cannot be relied on to have reserved, and a
// reallocation inside a Windows low-level hook is exactly the kind of
// unbounded work that gets the hook silently unregistered.
class DecisionBuffer {
public:
    void clear() {
        size_ = 0;
        overflowed_ = false;
    }

    void push(const Decision& decision) {
        if (size_ < kDecisionCapacity) {
            items_[size_++] = decision;
            return;
        }
        // Cannot happen with a physical keyboard. Recorded rather than ignored
        // so that if it ever does, it surfaces as a diagnostic instead of as
        // silently lost input.
        overflowed_ = true;
    }

    [[nodiscard]] std::size_t size() const { return size_; }
    [[nodiscard]] bool empty() const { return size_ == 0; }
    [[nodiscard]] bool overflowed() const { return overflowed_; }

    [[nodiscard]] const Decision& operator[](std::size_t i) const {
        return items_[i];
    }
    [[nodiscard]] const Decision* begin() const { return items_.data(); }
    [[nodiscard]] const Decision* end() const { return items_.data() + size_; }

private:
    std::array<Decision, kDecisionCapacity> items_{};
    std::size_t size_ = 0;
    bool overflowed_ = false;
};

class LayerEngine {
public:
    explicit LayerEngine(EngineConfig config = {});

    void setConfig(const EngineConfig& config);

    // Replace the entire binding classification in one call.
    //
    // Atomic by construction: there is no way to update which keys are bound
    // without simultaneously updating what they are bound to, so no caller can
    // produce a state where a key is passthrough but not bound, and a reload
    // that removes a binding cannot leave stale behaviour behind.
    //
    // Emits decisions, because keys may be held or buffered when it is called:
    // an action whose binding disappeared is released, and a buffered press
    // whose key is no longer action-bound is resolved. Forwarded presses are
    // untouched -- P7 outranks a config reload.
    void setBindings(const BindingMap& bindings, DecisionBuffer& out);

    // -- the event path, allocation-free ----------------------------------

    // Feed one physical key event.
    //
    // One event can produce several decisions: resolving a buffered press
    // emits both its press and its release, and a CapsLock press can promote
    // several buffered keys at once.
    //
    // Performs no dynamic allocation. On Windows this runs inside
    // WH_KEYBOARD_LL, which must decide suppression synchronously and return
    // within LowLevelHooksTimeout or Windows silently unhooks it.
    void onKey(KeyCode code, KeyState state, TimePoint now, DecisionBuffer& out);

    // Must be called regularly. Resolves buffered presses whose grace window
    // has lapsed without CapsLock arriving. Allocation-free.
    void tick(TimePoint now, DecisionBuffer& out);

    // Release everything, unconditionally. Every exit path calls this (P7).
    // Allocation-free.
    void releaseAll(DecisionBuffer& out);

    // -- allocating conveniences, for tests and cold paths ----------------

    std::vector<Decision> setBindings(const BindingMap& bindings);
    std::vector<Decision> onKey(KeyCode code, KeyState state, TimePoint now);
    std::vector<Decision> tick(TimePoint now);
    std::vector<Decision> releaseAll();

    [[nodiscard]] Mode mode() const { return mode_; }
    [[nodiscard]] bool latched() const { return latched_; }
    // In press order.
    [[nodiscard]] std::vector<KeyCode> heldActions() const;

private:
    // Per-key state, indexed by KeyCode id. A flat array rather than a hash
    // set: membership tests are the hot path, and node-based containers
    // allocate on insertion.
    struct Slot {
        bool bound = false;          // has a binding at all
        BindingKind kind = BindingKind::Action;
        bool forwarded = false;      // press went to the OS; P7 owes a release
        bool heldAction = false;
        bool modifierHeld = false;
        bool isModifier = false;     // precomputed; avoids a lookup per event
        bool pending = false;
    };

    struct Pending {
        KeyCode code;
        TimePoint pressedAt;
    };

    void onCapsLock(KeyState state, TimePoint now, DecisionBuffer& out);
    void promoteBuffered(TimePoint now, DecisionBuffer& out);
    void leaveCursorMode(DecisionBuffer& out);

    // The single path by which a press reaches the OS. Records the key so its
    // release is guaranteed to follow. Nothing else may emit a Forward for a
    // Down; that is what makes P7 checkable rather than merely intended.
    void forwardPress(KeyCode code, KeyState state, DecisionBuffer& out);
    void forgetForwarded(KeyCode code);

    [[nodiscard]] bool trackable(KeyCode code) const {
        return code.valid() && code.id() < kMaxTrackedKeyId;
    }
    Slot& slot(KeyCode code) { return slots_[code.id()]; }
    [[nodiscard]] const Slot& slot(KeyCode code) const {
        return slots_[code.id()];
    }

    void addHeldAction(KeyCode code);
    void removeHeldAction(KeyCode code);
    bool addPending(KeyCode code, TimePoint now);
    [[nodiscard]] bool isPending(KeyCode code) const;
    std::optional<TimePoint> takePending(KeyCode code);

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

    // Resolved once at construction. Looking these up per event would take
    // the intern table's lock and build a std::string from the name -- an
    // allocation, on the path that must not have one.
    KeyCode capsLock_;
    KeyCode shiftLeft_;
    KeyCode shiftRight_;

    std::array<Slot, kMaxTrackedKeyId> slots_{};

    // Ordered companions to the flags above. Membership is O(1) via `slots_`;
    // these exist so that iteration happens in press order rather than in
    // whatever order a hash table happens to hold. Input ordering is
    // observable -- it affects chords, shortcuts and reproducibility -- so it
    // must not be left to chance.
    std::array<Pending, kMaxPending> pending_{};
    std::size_t pendingCount_ = 0;

    std::array<KeyCode, kMaxHeld> heldOrder_{};
    std::size_t heldCount_ = 0;

    std::array<KeyCode, kMaxHeld> forwardedOrder_{};
    std::size_t forwardedCount_ = 0;
};

}  // namespace mtk

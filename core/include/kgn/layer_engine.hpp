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

#include "kgn/clock.hpp"
#include "kgn/keycode.hpp"

namespace kgn {

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
// The event path performs no dynamic allocation, so every container it touches
// has a fixed size. There is deliberately no "untracked" key domain: per-key
// state covers the whole KeyCode id space, so every valid code carries the
// state its invariants require.
//
// The remaining bounds are on *simultaneity*, and each one fails safe: when an
// obligation cannot be recorded, the decision that would create it is not
// emitted. A press that cannot be tracked is suppressed rather than forwarded
// untracked, because a suppressed key is a missing keystroke while an
// untracked forwarded press is a key held down forever.
// ---------------------------------------------------------------------------

// Per-key state is indexed directly by KeyCode::id(). Sized from KeyCode's own
// bound, so every id valid() admits has a slot -- including the boundary one.
inline constexpr std::size_t kKeyIdSpace =
    static_cast<std::size_t>(KeyCode::kMaxId) + 1;
static_assert(kKeyIdSpace == 0x10000,
              "per-key state must cover the whole KeyCode id domain");

// Simultaneously buffered presses.
inline constexpr std::size_t kMaxPending = 64;

// Simultaneously held keys, in each of the two ordered lists. Comfortably
// above the key count of any physical keyboard.
inline constexpr std::size_t kMaxHeld = 256;

// Decisions from one call. Derived from the true worst case rather than
// guessed: releaseAll() unwinding every held action and every forwarded press
// at once, plus a full pending sweep, plus slack.
inline constexpr std::size_t kDecisionCapacity = 2 * kMaxHeld + kMaxPending + 8;

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
        // Unreachable: the capacity is derived from the maximum unwind. Kept
        // as a recorded condition rather than an assert so that if the
        // derivation ever drifts, it surfaces instead of corrupting memory.
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
    // Emits decisions, because keys may be held or buffered when it is called.
    // A reload never discards physical input the user has already committed:
    // an action whose binding disappeared is released, and a buffered press
    // whose key is no longer action-bound is resolved as the ordinary
    // keystroke it turned out to be. Forwarded presses are untouched -- P7
    // outranks a config reload.
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

    // Diagnostics. Non-zero means input was dropped because an obligation
    // could not be recorded -- unreachable from a physical keyboard, and worth
    // surfacing if it ever happens.
    [[nodiscard]] std::uint64_t capacityDrops() const { return capacityDrops_; }
    [[nodiscard]] std::uint64_t invalidEvents() const { return invalidEvents_; }

private:
    // Per-key state, indexed by KeyCode id. A flat array rather than a hash
    // set: membership tests are the hot path, and node-based containers
    // allocate on insertion. Packed into one byte per key so that covering the
    // whole id space costs 64 KiB, allocated once at construction.
    struct Slot {
        enum : std::uint8_t {
            kBound = 1u << 0,
            kPassthrough = 1u << 1,   // meaningful only when kBound
            kForwarded = 1u << 2,     // press went to the OS; P7 owes a release
            kHeldAction = 1u << 3,
            kModifierHeld = 1u << 4,
            kIsModifier = 1u << 5,
            kPending = 1u << 6,
        };

        [[nodiscard]] bool has(std::uint8_t mask) const {
            return (bits & mask) != 0;
        }
        void set(std::uint8_t mask) {
            bits = static_cast<std::uint8_t>(bits | mask);
        }
        void clear(std::uint8_t mask) {
            bits = static_cast<std::uint8_t>(bits & static_cast<std::uint8_t>(~mask));
        }
        void assign(std::uint8_t mask, bool on) {
            on ? set(mask) : clear(mask);
        }

        std::uint8_t bits = 0;
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
    //
    // Returns false, and emits a Suppress instead, when the obligation cannot
    // be recorded. Creating an obligation that cannot be discharged would be
    // strictly worse than dropping the keystroke.
    //
    // `suppressAs` is the state that Suppress reports. It is almost always the
    // press being forwarded, but the grace-window tap replay forwards a Down
    // while handling an Up -- and a Suppress must describe the event it
    // suppressed, not the one it was trying to synthesise.
    bool forwardPress(KeyCode code, KeyState state, DecisionBuffer& out,
                      KeyState suppressAs = KeyState::Down);
    void forgetForwarded(KeyCode code);

    Slot& slot(KeyCode code) { return slots_[code.id()]; }
    [[nodiscard]] const Slot& slot(KeyCode code) const {
        return slots_[code.id()];
    }

    // Returns false when the action could not be recorded, in which case the
    // caller must not emit RunAction -- an action start with no guaranteed
    // release is the P7 failure in a different currency.
    bool addHeldAction(KeyCode code);
    void removeHeldAction(KeyCode code);
    bool addPending(KeyCode code, TimePoint now);
    [[nodiscard]] bool isPending(KeyCode code) const;
    bool takePending(KeyCode code);

    EngineConfig config_;
    Mode mode_ = Mode::Normal;
    bool latched_ = false;

    // Physical CapsLock state, tracked explicitly so that duplicate and
    // orphaned CapsLock events follow the same policy as every other key
    // instead of bypassing it (SPEC 6.3.2).
    bool capsPhysicallyDown_ = false;
    std::optional<TimePoint> capsPressedAt_;
    // True when the current CapsLock press was classified as the Shift+CapsLock
    // escape gesture. Tracked separately from whether forwarding it actually
    // succeeded: a gesture whose obligation could not be recorded is still a
    // gesture, and its release must not fall through into layer handling.
    bool capsEscapeGesture_ = false;
    // True when that press was also successfully forwarded to the OS. Its
    // release must then be forwarded too.
    bool capsForwarded_ = false;
    // Whether the layer was already latched when the current CapsLock press
    // began. In hybrid mode this is what separates a tap that latches on from
    // a tap that latches off -- the press itself clears `latched_`, so the
    // release has no other way to tell the two apart.
    bool capsWasLatched_ = false;

    // Resolved once at construction. Looking these up per event would take the
    // intern table's lock and build a std::string from the name -- an
    // allocation, on the path that must not have one.
    KeyCode capsLock_;
    KeyCode shiftLeft_;
    KeyCode shiftRight_;
    std::array<KeyCode, 8> modifierCodes_{};

    // One byte per key over the whole id space; a single allocation at
    // construction, never resized, never touched by the event path.
    std::vector<Slot> slots_;

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

    std::uint64_t capacityDrops_ = 0;
    std::uint64_t invalidEvents_ = 0;
};

}  // namespace kgn

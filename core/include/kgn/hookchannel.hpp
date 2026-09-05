// The two streams that cross from the input-owning thread to the core loop.
//
// An engine DECISION is not a physical KEY EVENT, and conflating them is a bug
// waiting for a backend to exist:
//   - one physical event can produce several decisions (a grace replay, a
//     CapsLock press promoting buffered keys);
//   - grace expiry, release_all and config changes produce decisions with NO
//     physical event at all;
//   - a decision satisfied by native passthrough needs publication but no
//     synthesis, and re-synthesising it would deliver the key twice.
//
// So there are two streams:
//
//   Stream A -- WorkItem. Things the core must DO. Ordered, and it must never
//   overflow, because a dropped release is a key or a mouse button held down
//   forever (P7). The capacity argument that makes overflow structurally
//   impossible is below.
//
//   Stream B -- PhysicalRecord. Exactly one per physical hook callback, and
//   none for timer or control work. This is what produces the `key` IPC event.
//   Losing one costs an overlay highlight and nothing else, so it may drop.
//
// Everything here is pure: no OS API, no thread, no allocation after
// construction. See docs/superpowers/specs/2026-08-28-m3-threading-design.md.

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>

#include "kgn/keycode.hpp"
#include "kgn/layer_engine.hpp"

namespace kgn {

// ---------------------------------------------------------------------------
// Stream A -- work items

struct WorkItem {
    enum class Kind : std::uint8_t {
        SendKey,        // OutputBackend::sendKey(code, down)
        RunAction,      // Dispatcher: start the binding on `code`
        ReleaseAction,  // Dispatcher: stop the binding on `code`
        ReleaseToggles, // Dispatcher: the layer ended; lift its drag locks
    };

    Kind kind = Kind::SendKey;
    bool down = false;          // SendKey only
    std::uint16_t code = 0;     // unset for ReleaseToggles; it names no key
};

// ---------------------------------------------------------------------------
// Stream B -- physical resolution records

struct PhysicalRecord {
    std::uint16_t code = 0;
    KeyState state = KeyState::Down;   // the PHYSICAL state, as the OS gave it
    bool suppressed = false;           // true iff the hook returned non-zero
    std::uint32_t version = 0;         // published-state version after this callback
};

// ---------------------------------------------------------------------------
// Capacities
//
// The theorem the work ring's size comes from:
//
// Let U := heldActions + 2 * pendingPresses. Every Up path emits at most -dU
// work items -- releasing a held action costs 1 item and drops U by 1;
// resolving a buffered press costs at most 2 and drops U by 2 -- with ONE
// exception: a CapsLock release that leaves the layer costs H + 1 items while
// dropping U by only H. Every path that emits NO work item consumes no
// capacity either, because Suppress and Buffer decisions never become work
// items.
//
// That extra item is the layer-exit signal (ReleaseToggles), and there is at
// most one of it in any drain. It is emitted only when the layer was actually
// engaged, and the layer can only be engaged again by a CapsLock DOWN -- an
// obligation-creating operation, so it is admission-gated and outside the
// drain by construction. Hence the reserve carries exactly one, not one per
// exit attempt.
//
// So once no new obligation is admitted, ALL remaining Up events together emit
// at most U + kMaxLayerExitWork <= kMaxReleaseWork items.
//
// Admit an obligation-creating operation (a Down, a Repeat, tick(), any
// control) only while free >= kWorkAdmissionGate. It then emits at most
// kDecisionCapacity items, leaving free >= kMaxReleaseWork >= U. Up events are
// never refused, and each one preserves free - U >= 0. Therefore the ring
// never overflows and no release is ever dropped.
//
// That rows emitting no work item also consume no capacity is exactly why mode
// and modifier publication is NOT in this ring: an Up for a key with no
// obligation at all would otherwise spend reserved capacity while discharging
// nothing, and a user can produce those without limit. The bound would not
// exist.

// The layer-exit signal. ONE item covers the whole drain, per the argument
// above: leaving the layer emits a single ReleaseToggles regardless of how
// many toggles are set, because expanding it into per-button releases happens
// on the core's side of the ring, where no capacity is at stake.
inline constexpr std::size_t kMaxLayerExitWork = 1;

// The worst case for the whole release drain.
inline constexpr std::size_t kMaxReleaseWork =
    kMaxHeld + 2 * kMaxPending + kMaxLayerExitWork;
static_assert(kMaxReleaseWork == 385, "the capacity proof is stated for these bounds");

// Free slots required before an obligation-creating operation may run.
inline constexpr std::size_t kWorkAdmissionGate = kDecisionCapacity + kMaxReleaseWork;

// Power of two so the ring masks instead of dividing. The proof needs only
// kWorkAdmissionGate; the rest is slack so the gate is unreachable in practice.
inline constexpr std::size_t kWorkRingCapacity = 2048;
static_assert(kWorkRingCapacity >= kWorkAdmissionGate,
              "the ring must be able to satisfy its own admission gate");

// Publication may drop; sized for comfort, not for a proof.
inline constexpr std::size_t kPublicationRingCapacity = 1024;

// ---------------------------------------------------------------------------
// A single-producer / single-consumer ring.
//
// Two atomics and a fixed array. Deliberately the smallest thing that
// establishes the ownership rather than the cleverest: exactly one thread
// pushes and exactly one pops, which is a property of the architecture rather
// than something this class enforces.
//
// One slot is always left free so that a full ring is distinguishable from an
// empty one without a third variable.
template <typename T, std::size_t N>
class SpscRing {
    static_assert(N >= 2, "a ring needs room for one item plus its sentinel");
    static_assert((N & (N - 1)) == 0, "capacity must be a power of two");

public:
    // -- producer side ----------------------------------------------------

    [[nodiscard]] std::size_t free() const {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        return N - 1 - (head - tail);
    }

    // Returns false when full. Anything that must not be refused checks free()
    // first, so a false here is a programming error on the work ring and an
    // accepted drop on the publication ring.
    bool push(const T& item) {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        if (head - tail >= N - 1) return false;
        items_[head & (N - 1)] = item;
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    // -- consumer side ----------------------------------------------------

    bool pop(T& out) {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) return false;
        out = items_[tail & (N - 1)];
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    // -- either side ------------------------------------------------------

    [[nodiscard]] std::size_t size() const {
        const std::size_t head = head_.load(std::memory_order_acquire);
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        return head - tail;
    }
    [[nodiscard]] bool empty() const { return size() == 0; }

private:
    std::array<T, N> items_{};
    std::atomic<std::size_t> head_{0};
    std::atomic<std::size_t> tail_{0};
};

using WorkRing = SpscRing<WorkItem, kWorkRingCapacity>;
using PublicationRing = SpscRing<PhysicalRecord, kPublicationRingCapacity>;

// ---------------------------------------------------------------------------
// Published engine state
//
// Mode, latch, activation and the four modifier groups, plus a version, in one
// 64-bit atomic. The core reads it freely; ordering against `key` events comes
// from the version stamped into each PhysicalRecord, so no ordered queue entry
// is needed and none of the work ring's reserved capacity is spent on the
// overlay's benefit.
struct PublishedState {
    Mode mode = Mode::Normal;
    bool latched = false;
    ActivationMode activation = ActivationMode::Hybrid;
    bool shift = false;
    bool control = false;
    bool alt = false;
    bool meta = false;

    [[nodiscard]] std::uint32_t pack() const;
    [[nodiscard]] static PublishedState unpack(std::uint32_t bits);

    bool operator==(const PublishedState& other) const {
        return pack() == other.pack();
    }
};

class StatePublisher {
public:
    // Producer. Stores only when something actually changed, and bumps the
    // version when it does -- otherwise the core would emit a `mode` event on
    // every tick forever.
    void publish(const PublishedState& state);

    // Consumer. Version and state come from one load, so a reader can never
    // pair a version from one store with the state from another.
    [[nodiscard]] PublishedState state() const;
    [[nodiscard]] std::uint32_t version() const;

private:
    // High 32 bits version, low 32 bits packed state.
    std::atomic<std::uint64_t> value_{0};
};

// ---------------------------------------------------------------------------
// Core -> input control channel

struct Control {
    enum class Kind : std::uint8_t {
        SetConfig,     // payload: const EngineConfig*
        SetBindings,   // payload: const BindingMap*
        ReleaseAll,
        SetEnabled,    // flag
        Stop,
    };

    Kind kind = Kind::ReleaseAll;
    bool flag = false;
    std::uint32_t seq = 0;
    // Owned by the core, which must not free it before the change is
    // acknowledged. LayerEngine::setBindings reads its argument and retains
    // nothing, so ack-then-free is sufficient and no ownership crosses.
    const void* payload = nullptr;
};

using ControlRing = SpscRing<Control, 64>;

// Whoever owns the LayerEngine on this build. With an input backend that is
// the input thread; with none, the core owns it directly and applies control
// inline. Either way the core only ever SUBMITS -- it never calls a mutating
// engine method itself, which is what makes the ownership checkable rather
// than merely intended.
class EngineOwner {
public:
    virtual ~EngineOwner() = default;

    // Never blocks. Returns false when the control ring is full.
    virtual bool submit(const Control& control) = 0;

    // Blocks the CALLER -- always the core thread, never the input thread --
    // until the owner reports `seq` applied, or the timeout lapses. Waiting in
    // this direction is safe: it stalls the core loop, never a hook callback,
    // and Windows only measures the latter.
    virtual bool awaitApplied(std::uint32_t seq,
                              std::chrono::milliseconds timeout) = 0;
};

// ---------------------------------------------------------------------------
// Decision translation
//
// Turns one DecisionBuffer into work items, applying the native-passthrough
// rule. Returns TRUE when the current physical event should reach the OS
// untouched -- exactly when the engine emitted one decision and it is Forward
// for this very code and state. In that case NOTHING is pushed, because the OS
// is about to receive the event itself and synthesising it too would deliver
// it twice (INV-SYN).
//
// `code` and `state` describe the physical event in hand. The timer and
// control paths have none: pass an invalid KeyCode and the result is always
// false.
bool translateDecisions(const DecisionBuffer& decisions, KeyCode code,
                        KeyState state, WorkRing& out);

}  // namespace kgn

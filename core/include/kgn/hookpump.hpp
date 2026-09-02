// The two decisions the hook thread's message loop has to make, extracted from
// Win32 so they can be tested without a desktop, a message queue or a human.
//
// This file exists because of a defect found during live M3 validation. The
// hook thread had been parking in MsgWaitForMultipleObjectsEx, whose timeout
// doubled as the grace-window timer. That is a WAIT, not a message-RETRIEVAL
// call, and WH_KEYBOARD_LL callbacks are only dispatched while the installing
// thread is inside a retrieval call. The hook installed, reported healthy, and
// never received a single callback. The automated suite did not catch it
// because nothing in it depended on the thread retrieving messages.
//
// The repair makes GetMessageW the blocking primitive. That fixes dispatch but
// costs the timeout, so the two jobs it used to carry are now explicit:
//
//   Doorbell        -- core -> hook wakeups, coalesced, carrying no payload.
//   GraceTimerPlan  -- when the thread timer must be armed or killed.
//
// Both are pure. The Win32 calls they imply (PostThreadMessageW, SetTimer,
// KillTimer) stay in the backend; the reasoning lives here where a test can
// reach it.
//
// See docs/superpowers/specs/2026-08-28-m3-threading-design.md section 4.

#pragma once

#include <atomic>
#include <cstdint>
#include <optional>

#include "kgn/clock.hpp"

namespace kgn {

// ---------------------------------------------------------------------------
// Doorbell
//
// A coalescing wake signal. The control ring stays authoritative for payload
// and ordering; this only answers "does someone need to post a wakeup?", so a
// burst of controls cannot approach the Windows 10 000-message queue limit.
//
// ORDER MATTERS ON THE CONSUMER SIDE. disarm() must be called BEFORE draining,
// never after. The proof, with P the producer and C the consumer:
//
//   Producer:  push(item)            [release]
//              if (arm()) post()     [acq_rel exchange]
//   Consumer:  on wake: disarm()     [release]
//              drain until empty
//
//   Case 1 -- arm() returns true. P owns the post, C receives it, disarms and
//   drains. push happens-before the exchange happens-before the post, so the
//   drain observes the item.
//
//   Case 2 -- arm() returns false, because a post is already outstanding. C
//   has not yet consumed it. C will disarm and then drain to empty, and a
//   drain-to-empty observes every item pushed before it began -- including
//   this one.
//
//   Case 3 -- arm() returns false because C is between disarm() and the end of
//   its drain. C's drain is still running and will observe the item, since the
//   push completed before the exchange that read the cleared flag... and if it
//   did not, the flag was already false, so arm() would have returned true and
//   we are in case 1 instead.
//
// The broken ordering is drain-then-disarm: C drains to empty, P pushes and
// finds the flag still set so posts nothing, C then clears the flag, and the
// item waits forever. That is the lost wakeup this class exists to prevent,
// and test_hookpump drives exactly that interleaving.
class Doorbell {
public:
    // Producer side. Returns true when THIS caller must send the wake signal.
    // Exactly one caller gets true per un-consumed episode.
    bool arm() { return !pending_.exchange(true, std::memory_order_acq_rel); }

    // Consumer side. Call before draining, never after.
    void disarm() { pending_.store(false, std::memory_order_release); }

    [[nodiscard]] bool pending() const {
        return pending_.load(std::memory_order_acquire);
    }

private:
    std::atomic<bool> pending_{false};
};

// ---------------------------------------------------------------------------
// GraceTimerPlan
//
// The engine's only timed work is expiring buffered presses, so the thread
// needs a timer exactly when LayerEngine::nextDeadline() has a value. The
// timer is treated as ONE-SHOT: the loop kills it when it fires and re-arms
// from the deadline that remains. A periodic timer would be a poll, which is
// the thing this design refuses to become.
//
// Because a fired timer is killed before the engine is consulted, a stale or
// coalesced WM_TIMER is harmless by construction: the loop re-reads the clock,
// finds nothing due, does nothing, and re-arms only if a deadline still
// exists.
//
// pending_ is in press order and the grace window is uniform, so the head
// entry is always the earliest deadline and an armed timer never needs to be
// replaced with an earlier one. When the head is consumed the next deadline is
// strictly later, so the armed timer merely fires early -- which the
// not-yet-due path absorbs.
class GraceTimerPlan {
public:
    enum class Action {
        None,   // leave the timer as it is
        Arm,    // SetTimer for `delayMs`
        Kill,   // KillTimer; nothing is pending
    };

    struct Decision {
        Action action = Action::None;
        std::uint32_t delayMs = 0;

        friend bool operator==(const Decision& a, const Decision& b) {
            return a.action == b.action && a.delayMs == b.delayMs;
        }
    };

    // `armed` is whether a timer is live right now. A timer that has just
    // fired must be reported as NOT armed, because the loop kills it first.
    //
    // A deadline already in the past yields Arm with the floor delay rather
    // than a zero-delay timer: the loop will run the expiry on the next turn,
    // and asking Windows for a 0 ms timer is asking for a spin.
    static Decision decide(bool armed, const std::optional<TimePoint>& deadline,
                           TimePoint now) {
        if (!deadline.has_value()) {
            return armed ? Decision{Action::Kill, 0} : Decision{Action::None, 0};
        }
        if (armed) return Decision{Action::None, 0};

        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(*deadline - now)
                .count();
        // USER_TIMER_MINIMUM is 10 ms and Windows clamps below it anyway, so
        // the floor is honest about what the OS will actually do.
        const std::int64_t clamped =
            remaining < kMinDelayMs ? kMinDelayMs
                                    : (remaining > kMaxDelayMs ? kMaxDelayMs
                                                               : remaining);
        return Decision{Action::Arm, static_cast<std::uint32_t>(clamped)};
    }

    static constexpr std::int64_t kMinDelayMs = 10;
    // A ceiling so a nonsensical deadline cannot arm a timer years out; the
    // loop re-evaluates on every wake regardless.
    static constexpr std::int64_t kMaxDelayMs = 1000;
};

}  // namespace kgn

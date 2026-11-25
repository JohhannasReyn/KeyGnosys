// The hook thread's core -> hook wakeup, extracted from Win32 so it can be
// tested without a desktop, a message queue or a human.
//
// The thread blocks in GetMessageW, because WH_KEYBOARD_LL callbacks are only
// dispatched while the installing thread is inside a message-RETRIEVAL call. A
// wait such as MsgWaitForMultipleObjectsEx is not one: the hook installed,
// reported healthy, and never received a callback.
//
//   Doorbell -- core -> hook wakeups, coalesced, carrying no payload.
//
// It is pure. The Win32 call it implies (PostThreadMessageW) stays in the
// backend; the reasoning lives here where a test can reach it.
//
// See docs/superpowers/specs/2026-08-28-m3-threading-design.md section 4.

#pragma once

#include <atomic>
#include <cstdint>

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

}  // namespace kgn

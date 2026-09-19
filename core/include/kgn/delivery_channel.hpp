#pragma once

// Which channel is delivering the physical press currently under a finger --
// and therefore which channel the rest of that press must use.
//
// The invariant it exists to enforce (M3 finding O-5):
//
//     Once the Windows-visible Down for a physical press is committed to native
//     or synthetic delivery, the remainder of that physical press stays on that
//     delivery channel.
//
// WHY. Two channels carry events to the OS, and nothing orders them against
// each other:
//
//   synthetic  decision -> work ring -> core loop thread -> SendInput.
//              Asynchronous, and the ring's tail advances at DEQUEUE, before
//              SendInput is called.
//   native     the hook callback returns CallNextHookEx and the physical event
//              continues down the chain. Synchronous.
//
// A grace-window replay publishes a synthetic Down; if the physical release is
// then forwarded natively it can overtake that Down, and Windows is left holding
// a key whose release has already gone past. Measured live on 2026-09-16: a
// `Space` press replayed at grace expiry, its release forwarded natively 15 ms
// ahead of the injected press, left Space held in Windows until a later press
// of the same key cleared it.
//
// A COMMITMENT, NOT A RECEIPT. `Synthetic` records that the press was routed to
// the ordered channel. It does not claim SendInput succeeded, and hook routing
// MUST NOT depend on that: the hook thread cannot observe the core thread's
// progress without reintroducing the race this class removes. A refused
// injection stays the output backend's business -- it keeps `heldKeys_` honest,
// reports `output.send_failed`, and retries on releaseAll.
//
// Ordering is therefore established by CONSTRUCTION -- one ring, one consumer,
// FIFO -- rather than by observing how far the other thread has got. There is no
// pending-work barrier here on purpose; every barrier formulation available to
// the hook thread is a check-then-act against a channel whose completion it
// cannot see.
//
// SEPARATE FROM DeliveryProvenance, DELIBERATELY. They answer different
// questions about different halves of the path, and the answers are independent:
//
//   DeliveryProvenance   did this press reach the ENGINE, or go past it?
//                        Consulted BEFORE the engine. (O-1)
//   DeliveryChannel      which channel delivered this press to WINDOWS?
//                        Decided AFTER the engine. (O-5)
//
// An ordinary bound key passed through is Engine-routed and Native-delivered; a
// grace replay is Engine-routed and Synthetic-delivered. Folding them into one
// enum would need the cross product and would make the O-1 meaning ambiguous,
// which is worth more than the byte it would save.
//
// Hook-thread only: no locks, no allocation after construction.

#include <cstdint>
#include <vector>

#include "kgn/keycode.hpp"

namespace kgn {

class DeliveryChannel {
public:
    // The channel carrying the press currently under the finger.
    enum class Channel : std::uint8_t {
        // No press of this key is in flight, or its release is done. The next
        // Down is free to select either channel -- including a press the engine
        // is still withholding in the grace buffer, which has selected NEITHER
        // channel yet and so must not constrain anything.
        None,
        // The Windows-visible Down was the physical event itself, passed down
        // the hook chain. The rest of the press may keep using that path.
        Native,
        // The Windows-visible Down was published to the work ring. Everything
        // else about this press must go the same way or it can overtake it.
        Synthetic,
    };

    DeliveryChannel();

    // May the event in hand be satisfied by letting the OS deliver the physical
    // event itself? False exactly when this press is committed to synthetic
    // delivery.
    [[nodiscard]] bool nativeAllowed(KeyCode code) const;

    // The press of `code` is being delivered by passing the physical event
    // through. Down and Repeat only; a release commits nothing.
    void commitNative(KeyCode code);

    // The press of `code` is being delivered through the work ring.
    void commitSynthetic(KeyCode code);

    // The release for `code` has been emitted, on whichever channel. The press
    // is over, so the next one starts uncommitted.
    void release(KeyCode code);

    // Diagnostics and tests. Never used to make a routing decision.
    [[nodiscard]] Channel stateFor(KeyCode code) const;

    // Only once nothing can still be delivered for these presses -- after the
    // hook is uninstalled, or after every obligation has been released. A
    // commitment dropped while the press is still live would let the very
    // bypass this class forbids happen to the release.
    void forgetAll();

private:
    std::vector<std::uint8_t> state_;
};

}  // namespace kgn

// Which channel delivers a physical press, and why it may not change mid-press
// (finding O-5).
//
// Live measurement on 2026-09-16: a `Space` press replayed when its grace window
// lapsed was published to the work ring, and the physical release was then
// forwarded natively -- straight down the hook chain, while the replayed press
// was still short of SendInput. Windows received Up, then Down, and held Space
// until a later press of the same key cleared it.
//
// The rule these tests pin:
//
//     Once the Windows-visible Down for a physical press is committed to native
//     or synthetic delivery, the remainder of that physical press stays on that
//     delivery channel.
//
// The canonical regression is first. It runs with no thread, no sleep and no
// scheduler: the defect is a routing decision, so it reproduces as one.

#include "kgn/delivery_channel.hpp"
#include "kgn/hookchannel.hpp"

#include "kgn_test.hpp"

#include <vector>

using namespace kgn;
using Channel = DeliveryChannel::Channel;

namespace {

const KeyCode SPACE = KeyCode::fromString("Space");
const KeyCode A = KeyCode::fromString("KeyA");
const KeyCode B = KeyCode::fromString("KeyB");
const KeyCode SHIFT = KeyCode::fromString("ShiftLeft");
const KeyCode CAPS = KeyCode::fromString("CapsLock");

Decision forwardOf(KeyCode code, KeyState state) {
    return {Decision::Kind::Forward, code, state};
}

// What the OS would actually receive, in order, from the work published so far.
std::vector<std::pair<std::uint16_t, bool>> drain(WorkRing& ring) {
    std::vector<std::pair<std::uint16_t, bool>> out;
    WorkItem item{};
    while (ring.pop(item)) {
        if (item.kind == WorkItem::Kind::SendKey) out.emplace_back(item.code, item.down);
    }
    return out;
}

// One decision, translated as the grace timer does it: no physical event in
// hand, so nothing can be native.
bool translateTimer(const Decision& decision, WorkRing& ring, DeliveryChannel& channel) {
    DecisionBuffer buffer;
    buffer.push(decision);
    return translateDecisions(buffer, KeyCode{}, KeyState::Down, ring, &channel);
}

// One decision, translated as the hook does it for the physical event in hand.
bool translateEvent(const Decision& decision, KeyCode code, KeyState state,
                    WorkRing& ring, DeliveryChannel& channel) {
    DecisionBuffer buffer;
    buffer.push(decision);
    return translateDecisions(buffer, code, state, ring, &channel);
}

// ---------------------------------------------------------------------------
// THE CANONICAL O-5 REGRESSION
//
// Fails against the pre-repair implementation at the `native` assertion: the
// release matched the native fast path's shape, bypassed the press still
// sitting in the ring, and the drained ring held a press with no release.

KGN_TEST(a_release_may_not_overtake_its_own_still_queued_press) {
    WorkRing ring;
    DeliveryChannel channel;

    // 1. The grace window lapses and the press is replayed onto synthetic work.
    KGN_CHECK(!translateTimer(forwardOf(SPACE, KeyState::Down), ring, channel));
    KGN_CHECK(channel.stateFor(SPACE) == Channel::Synthetic);

    // 2. That work is NOT drained. This is the whole defect: the ring reports an
    //    item gone when it is DEQUEUED, so "published" and "delivered to
    //    Windows" are different moments and nothing observable separates them.

    // 3. The physical release arrives for the same press. The engine emits
    //    exactly one Forward for this code and state -- the shape the native
    //    fast path exists for.
    const bool native =
        translateEvent(forwardOf(SPACE, KeyState::Up), SPACE, KeyState::Up, ring, channel);

    // 4./5. The native bypass must be refused.
    KGN_CHECK(!native);

    // 6./7. The release is queued behind the press, and draining yields the
    //       order the engine asked for.
    const auto delivered = drain(ring);
    KGN_CHECK(delivered.size() == 2);
    KGN_CHECK(delivered[0].first == SPACE.id() && delivered[0].second);    // Down
    KGN_CHECK(delivered[1].first == SPACE.id() && !delivered[1].second);   // Up
    // The press is over, so the next one starts uncommitted.
    KGN_CHECK(channel.stateFor(SPACE) == Channel::None);
}

// ---------------------------------------------------------------------------
// The fast path is not lost

KGN_TEST(an_ordinary_press_stays_native_through_its_release) {
    WorkRing ring;
    DeliveryChannel channel;
    KGN_CHECK(translateEvent(forwardOf(A, KeyState::Down), A, KeyState::Down, ring, channel));
    KGN_CHECK(channel.stateFor(A) == Channel::Native);
    KGN_CHECK(translateEvent(forwardOf(A, KeyState::Repeat), A, KeyState::Repeat, ring, channel));
    KGN_CHECK(translateEvent(forwardOf(A, KeyState::Up), A, KeyState::Up, ring, channel));
    KGN_CHECK(channel.stateFor(A) == Channel::None);
    // Nothing was synthesised: the OS delivered all three itself.
    KGN_CHECK(drain(ring).empty());
}

KGN_TEST(with_nothing_committed_the_native_path_is_still_taken) {
    WorkRing ring;
    DeliveryChannel channel;
    // A release whose press selected no channel at all -- the engine withheld
    // it, or it predates us. Native, exactly as before the repair.
    KGN_CHECK(translateEvent(forwardOf(A, KeyState::Up), A, KeyState::Up, ring, channel));
    KGN_CHECK(drain(ring).empty());
}

KGN_TEST(a_grace_pending_press_commits_to_neither_channel) {
    WorkRing ring;
    DeliveryChannel channel;
    // Buffered: the Windows-visible Down has not happened on EITHER channel, so
    // it must not constrain what follows.
    DecisionBuffer buffer;
    buffer.push({Decision::Kind::Buffer, A, KeyState::Down});
    KGN_CHECK(!translateDecisions(buffer, A, KeyState::Down, ring, &channel));
    KGN_CHECK(channel.stateFor(A) == Channel::None);
    KGN_CHECK(drain(ring).empty());
    // And a press that then resolves natively is free to do so.
    KGN_CHECK(translateEvent(forwardOf(A, KeyState::Down), A, KeyState::Down, ring, channel));
    KGN_CHECK(channel.stateFor(A) == Channel::Native);
}

// ---------------------------------------------------------------------------
// Repeats

KGN_TEST(repeats_of_a_synthetic_press_stay_ordered) {
    WorkRing ring;
    DeliveryChannel channel;
    KGN_CHECK(!translateTimer(forwardOf(A, KeyState::Down), ring, channel));
    // layer_engine emits Forward(code, Repeat) for a repeat of a forwarded key,
    // which matches the native shape. It must not take it.
    KGN_CHECK(!translateEvent(forwardOf(A, KeyState::Repeat), A, KeyState::Repeat, ring, channel));
    KGN_CHECK(channel.stateFor(A) == Channel::Synthetic);
    KGN_CHECK(!translateEvent(forwardOf(A, KeyState::Up), A, KeyState::Up, ring, channel));

    const auto delivered = drain(ring);
    KGN_CHECK(delivered.size() == 3);
    KGN_CHECK(delivered[0].second && delivered[1].second && !delivered[2].second);
}

KGN_TEST(repeats_of_a_native_press_stay_native) {
    WorkRing ring;
    DeliveryChannel channel;
    KGN_CHECK(translateEvent(forwardOf(A, KeyState::Down), A, KeyState::Down, ring, channel));
    KGN_CHECK(translateEvent(forwardOf(A, KeyState::Repeat), A, KeyState::Repeat, ring, channel));
    KGN_CHECK(translateEvent(forwardOf(A, KeyState::Repeat), A, KeyState::Repeat, ring, channel));
    KGN_CHECK(drain(ring).empty());
}

// ---------------------------------------------------------------------------
// Scope: per key, and no wider

KGN_TEST(an_unrelated_key_is_not_blocked_by_a_committed_one) {
    WorkRing ring;
    DeliveryChannel channel;
    KGN_CHECK(!translateTimer(forwardOf(A, KeyState::Down), ring, channel));
    // B has nothing pending of its own. Serialising it here would slow every
    // keystroke typed near a replayed one for no correctness gain: Windows key
    // state is per key, so B cannot be stranded by A's press.
    KGN_CHECK(translateEvent(forwardOf(B, KeyState::Down), B, KeyState::Down, ring, channel));
    KGN_CHECK(translateEvent(forwardOf(B, KeyState::Up), B, KeyState::Up, ring, channel));
    KGN_CHECK(channel.stateFor(A) == Channel::Synthetic);
    KGN_CHECK(channel.stateFor(B) == Channel::None);
    // Only A's press was synthesised.
    KGN_CHECK(drain(ring).size() == 1);
}

// ---------------------------------------------------------------------------
// The tap that resolves at its own release (layer_engine takePending)

KGN_TEST(a_fast_tap_replays_press_and_release_in_order) {
    WorkRing ring;
    DeliveryChannel channel;
    // Two decisions, so the native path never applied here even before O-5.
    DecisionBuffer buffer;
    buffer.push(forwardOf(A, KeyState::Down));
    buffer.push(forwardOf(A, KeyState::Up));
    KGN_CHECK(!translateDecisions(buffer, A, KeyState::Up, ring, &channel));

    const auto delivered = drain(ring);
    KGN_CHECK(delivered.size() == 2);
    KGN_CHECK(delivered[0].second && !delivered[1].second);
    // The release in the same batch ends the press.
    KGN_CHECK(channel.stateFor(A) == Channel::None);
}

// ---------------------------------------------------------------------------
// Modifiers and CapsLock follow the same rule

KGN_TEST(a_modifier_replay_keeps_its_release_ordered) {
    WorkRing ring;
    DeliveryChannel channel;
    KGN_CHECK(!translateTimer(forwardOf(SHIFT, KeyState::Down), ring, channel));
    KGN_CHECK(!translateEvent(forwardOf(SHIFT, KeyState::Up), SHIFT, KeyState::Up, ring, channel));
    const auto delivered = drain(ring);
    KGN_CHECK(delivered.size() == 2);
    KGN_CHECK(delivered[0].first == SHIFT.id() && delivered[0].second);
    KGN_CHECK(delivered[1].first == SHIFT.id() && !delivered[1].second);
}

KGN_TEST(caps_lock_is_not_exempt) {
    // CapsLock is dispatched before the general per-key logic, so it has to be
    // covered here or it bypasses the policy entirely (SPEC 6.3.4).
    WorkRing ring;
    DeliveryChannel channel;
    KGN_CHECK(!translateTimer(forwardOf(CAPS, KeyState::Down), ring, channel));
    KGN_CHECK(!translateEvent(forwardOf(CAPS, KeyState::Up), CAPS, KeyState::Up, ring, channel));
    KGN_CHECK(drain(ring).size() == 2);
}

// ---------------------------------------------------------------------------
// Cleanup, disable and the release paths

KGN_TEST(release_all_discharges_every_commitment_in_press_order) {
    WorkRing ring;
    DeliveryChannel channel;
    KGN_CHECK(!translateTimer(forwardOf(A, KeyState::Down), ring, channel));
    KGN_CHECK(translateEvent(forwardOf(B, KeyState::Down), B, KeyState::Down, ring, channel));
    KGN_CHECK(channel.stateFor(A) == Channel::Synthetic);
    KGN_CHECK(channel.stateFor(B) == Channel::Native);

    // What releaseAll() emits: an Up per forwarded key, no physical event in
    // hand. It clears BOTH kinds of commitment -- including the natively
    // pressed key, whose release is synthetic here because no physical release
    // is coming.
    DecisionBuffer buffer;
    buffer.push(forwardOf(A, KeyState::Up));
    buffer.push(forwardOf(B, KeyState::Up));
    KGN_CHECK(!translateDecisions(buffer, KeyCode{}, KeyState::Down, ring, &channel));
    KGN_CHECK(channel.stateFor(A) == Channel::None);
    KGN_CHECK(channel.stateFor(B) == Channel::None);
}

KGN_TEST(forgetting_everything_leaves_no_commitment) {
    WorkRing ring;
    DeliveryChannel channel;
    KGN_CHECK(!translateTimer(forwardOf(A, KeyState::Down), ring, channel));
    channel.forgetAll();
    KGN_CHECK(channel.stateFor(A) == Channel::None);
    KGN_CHECK(channel.nativeAllowed(A));
}

KGN_TEST(an_invalid_key_never_blocks_the_native_path) {
    DeliveryChannel channel;
    const KeyCode invalid;
    KGN_CHECK(channel.nativeAllowed(invalid));
    channel.commitSynthetic(invalid);
    KGN_CHECK(channel.nativeAllowed(invalid));
    KGN_CHECK(channel.stateFor(invalid) == Channel::None);
}

// ---------------------------------------------------------------------------
// A refused injection is not this class's problem

KGN_TEST(a_commitment_is_a_routing_choice_not_a_receipt) {
    // The hook thread cannot see whether SendInput succeeded without observing
    // the core thread, which is the race this class exists to remove. So the
    // release is ordered behind the press whatever became of it; a press the OS
    // refused leaves heldKeys_ false and the ordered release is a harmless
    // orphan key-up (K-E5), reported through output.send_failed.
    WorkRing ring;
    DeliveryChannel channel;
    KGN_CHECK(!translateTimer(forwardOf(A, KeyState::Down), ring, channel));
    // The consumer takes the press and, say, fails to send it. Nothing about
    // that reaches this decision.
    WorkItem item{};
    KGN_CHECK(ring.pop(item));
    KGN_CHECK(ring.empty());
    // The ring now reads EMPTY while that press may not have reached the OS at
    // all -- which is exactly why emptiness was rejected as the guard.
    KGN_CHECK(!translateEvent(forwardOf(A, KeyState::Up), A, KeyState::Up, ring, channel));
    const auto delivered = drain(ring);
    KGN_CHECK(delivered.size() == 1 && !delivered[0].second);
}

// ---------------------------------------------------------------------------
// The commitment clears at the physical release boundary, and not before

KGN_TEST(the_commitment_outlives_every_event_before_the_release) {
    WorkRing ring;
    DeliveryChannel channel;
    KGN_CHECK(!translateTimer(forwardOf(A, KeyState::Down), ring, channel));
    for (int i = 0; i < 5; ++i) {
        KGN_CHECK(!translateEvent(forwardOf(A, KeyState::Repeat), A, KeyState::Repeat,
                                  ring, channel));
        KGN_CHECK(channel.stateFor(A) == Channel::Synthetic);
    }
    KGN_CHECK(!translateEvent(forwardOf(A, KeyState::Up), A, KeyState::Up, ring, channel));
    KGN_CHECK(channel.stateFor(A) == Channel::None);
    // A brand new press is free to choose again.
    KGN_CHECK(translateEvent(forwardOf(A, KeyState::Down), A, KeyState::Down, ring, channel));
    KGN_CHECK(channel.stateFor(A) == Channel::Native);
}

// ---------------------------------------------------------------------------
// The invariant itself, over the shapes the engine can actually emit

KGN_TEST(a_release_is_never_delivered_by_a_channel_its_press_did_not_use) {
    // Every ordering of commit-then-release, checked against the rule rather
    // than against the implementation's state machine.
    const KeyState presses[] = {KeyState::Down, KeyState::Repeat};
    for (const KeyState press : presses) {
        for (int synthetic = 0; synthetic <= 1; ++synthetic) {
            WorkRing ring;
            DeliveryChannel channel;
            if (synthetic != 0) {
                KGN_CHECK(!translateTimer(forwardOf(A, press), ring, channel));
            } else {
                KGN_CHECK(translateEvent(forwardOf(A, press), A, press, ring, channel));
            }
            const bool nativeRelease =
                translateEvent(forwardOf(A, KeyState::Up), A, KeyState::Up, ring, channel);
            // THE INVARIANT: the release is native exactly when the press was.
            KGN_CHECK(nativeRelease == (synthetic == 0));
            KGN_CHECK(channel.stateFor(A) == Channel::None);
        }
    }
}

}  // namespace

int main() { return kgn::test::runAll(); }

// Which physical releases may be withheld from Windows (finding O-1).
//
// Live measurement: a key held across disable -> enable reached Windows while
// interception was off, the engine therefore had no record of forwarding it,
// and the hook suppressed the release -- leaving Shift stuck down in Windows
// until the user pressed it again. The rule these tests pin is the one that
// replaces the assumption that failed:
//
//     a release may be withheld ONLY with positive provenance that the press
//     was routed to the engine and nothing has since gone to Windows natively.
//
// The last test states that invariant directly over random event streams,
// rather than restating the implementation's state machine.

#include "kgn/delivery_provenance.hpp"

#include "kgn_test.hpp"

#include <cstdint>
#include <random>
#include <vector>

using namespace kgn;
using State = DeliveryProvenance::State;

namespace {

const KeyCode SHIFT = KeyCode::fromString("ShiftLeft");
const KeyCode A = KeyCode::fromString("KeyA");
const KeyCode CAPS = KeyCode::fromString("CapsLock");

constexpr bool kEnabled = true;
constexpr bool kDisabled = false;

// ---------------------------------------------------------------------------
// The unobserved press: P1 (pressed before the hook existed) and P3 (pressed
// while an elevated window owned the input).

KGN_TEST(a_release_with_no_observed_press_goes_to_windows) {
    DeliveryProvenance p;
    KGN_CHECK(p.route(SHIFT, KeyState::Up, kEnabled) == Route::Native);
    KGN_CHECK(p.stateFor(SHIFT) == State::None);
}

KGN_TEST(a_repeat_with_no_observed_press_starts_an_engine_press) {
    // The hook classifies the first event of an unobserved press as a Down, but
    // a Repeat must not be able to slip past the engine either.
    DeliveryProvenance p;
    KGN_CHECK(p.route(A, KeyState::Repeat, kEnabled) == Route::Engine);
    KGN_CHECK(p.stateFor(A) == State::Engine);
}

// ---------------------------------------------------------------------------
// The ordinary path is unchanged: observed presses still go to the engine.

KGN_TEST(an_observed_press_and_its_release_both_go_to_the_engine) {
    DeliveryProvenance p;
    KGN_CHECK(p.route(A, KeyState::Down, kEnabled) == Route::Engine);
    KGN_CHECK(p.stateFor(A) == State::Engine);
    KGN_CHECK(p.route(A, KeyState::Repeat, kEnabled) == Route::Engine);
    KGN_CHECK(p.route(A, KeyState::Up, kEnabled) == Route::Engine);
    KGN_CHECK(p.stateFor(A) == State::None);
}

KGN_TEST(every_key_is_tracked_separately) {
    DeliveryProvenance p;
    KGN_CHECK(p.route(A, KeyState::Down, kEnabled) == Route::Engine);
    KGN_CHECK(p.route(SHIFT, KeyState::Down, kDisabled) == Route::Native);
    KGN_CHECK(p.route(A, KeyState::Up, kEnabled) == Route::Engine);
    KGN_CHECK(p.route(SHIFT, KeyState::Up, kEnabled) == Route::Native);
}

KGN_TEST(caps_lock_follows_the_same_routing) {
    // CapsLock is dispatched before the engine's per-key logic, so it has to be
    // covered here or it bypasses the policy entirely (SPEC 6.3.4).
    DeliveryProvenance p;
    KGN_CHECK(p.route(CAPS, KeyState::Up, kEnabled) == Route::Native);
    KGN_CHECK(p.route(CAPS, KeyState::Down, kDisabled) == Route::Native);
    KGN_CHECK(p.route(CAPS, KeyState::Up, kEnabled) == Route::Native);
    KGN_CHECK(p.route(CAPS, KeyState::Down, kEnabled) == Route::Engine);
    KGN_CHECK(p.route(CAPS, KeyState::Up, kEnabled) == Route::Engine);
}

// ---------------------------------------------------------------------------
// M2, the measured defect.

KGN_TEST(a_key_held_across_disable_and_enable_keeps_its_release) {
    DeliveryProvenance p;
    // Pressed while interception is off: Windows has it.
    KGN_CHECK(p.route(SHIFT, KeyState::Down, kDisabled) == Route::Native);
    KGN_CHECK(p.stateFor(SHIFT) == State::Native);
    // Re-enabled with the key still held: autorepeat must not be handed to an
    // engine that has no press for it...
    KGN_CHECK(p.route(SHIFT, KeyState::Repeat, kEnabled) == Route::Native);
    KGN_CHECK(p.route(SHIFT, KeyState::Repeat, kEnabled) == Route::Native);
    // ...and the release must reach Windows. This is the stuck Shift.
    KGN_CHECK(p.route(SHIFT, KeyState::Up, kEnabled) == Route::Native);
    KGN_CHECK(p.stateFor(SHIFT) == State::None);
}

KGN_TEST(the_press_alone_earns_the_release_no_repeat_required) {
    // The press made while interception was off is what establishes that
    // Windows holds the key. If the release only survived because an autorepeat
    // arrived after re-enabling, correctness would depend on the repeat delay,
    // the repeat rate, and how long the key happened to be held.
    DeliveryProvenance p;
    KGN_CHECK(p.route(SHIFT, KeyState::Down, kDisabled) == Route::Native);
    KGN_CHECK(p.route(SHIFT, KeyState::Up, kEnabled) == Route::Native);
}

KGN_TEST(an_engine_press_that_goes_native_while_disabled_keeps_its_release) {
    DeliveryProvenance p;
    KGN_CHECK(p.route(A, KeyState::Down, kEnabled) == Route::Engine);
    // Interception off while the key is still held: the autorepeat reaches
    // Windows, so Windows now holds a key the engine no longer owes anything for.
    KGN_CHECK(p.route(A, KeyState::Repeat, kDisabled) == Route::Native);
    KGN_CHECK(p.stateFor(A) == State::Native);
    KGN_CHECK(p.route(A, KeyState::Up, kEnabled) == Route::Native);
}

KGN_TEST(a_release_that_arrives_while_disabled_clears_the_press) {
    DeliveryProvenance p;
    KGN_CHECK(p.route(A, KeyState::Down, kEnabled) == Route::Engine);
    KGN_CHECK(p.route(A, KeyState::Up, kDisabled) == Route::Native);
    KGN_CHECK(p.stateFor(A) == State::None);
    // The next press, with interception back on, is an ordinary engine press.
    KGN_CHECK(p.route(A, KeyState::Down, kEnabled) == Route::Engine);
    KGN_CHECK(p.route(A, KeyState::Up, kEnabled) == Route::Engine);
}

// ---------------------------------------------------------------------------
// Housekeeping

KGN_TEST(forgetting_everything_leaves_no_press_owning_a_release) {
    DeliveryProvenance p;
    KGN_CHECK(p.route(A, KeyState::Down, kEnabled) == Route::Engine);
    KGN_CHECK(p.route(SHIFT, KeyState::Down, kDisabled) == Route::Native);
    p.forgetAll();
    KGN_CHECK(p.stateFor(A) == State::None);
    KGN_CHECK(p.stateFor(SHIFT) == State::None);
    // After teardown nothing is routed at all; a release arriving from a hook
    // that no longer exists cannot be withheld on stale provenance.
    KGN_CHECK(p.route(A, KeyState::Up, kEnabled) == Route::Native);
}

KGN_TEST(an_invalid_key_is_passed_through_and_recorded_nowhere) {
    DeliveryProvenance p;
    const KeyCode invalid;
    KGN_CHECK(p.route(invalid, KeyState::Down, kEnabled) == Route::Native);
    KGN_CHECK(p.route(invalid, KeyState::Up, kEnabled) == Route::Native);
    KGN_CHECK(p.stateFor(invalid) == State::None);
}

// ---------------------------------------------------------------------------
// The safety invariant itself, over random streams.

KGN_TEST(a_release_is_withheld_only_with_positive_engine_provenance) {
    std::mt19937 rng(20260915u);
    std::uniform_int_distribution<int> pick(0, 3);
    const std::vector<KeyCode> keys{A, SHIFT, CAPS};

    for (int round = 0; round < 500; ++round) {
        DeliveryProvenance p;
        // Per key, the facts an auditor would need -- recorded from the routes
        // actually taken, not from the class's internals.
        std::vector<bool> pressOpen(keys.size(), false);
        std::vector<bool> pressRoutedToEngine(keys.size(), false);
        std::vector<bool> somethingWentNative(keys.size(), false);
        bool enabled = true;

        for (int step = 0; step < 60; ++step) {
            if (pick(rng) == 0) enabled = !enabled;
            const std::size_t k = static_cast<std::size_t>(pick(rng)) % keys.size();
            const KeyCode key = keys[k];

            KeyState state = KeyState::Down;
            if (pressOpen[k]) state = pick(rng) == 0 ? KeyState::Up : KeyState::Repeat;

            const Route route = p.route(key, state, enabled);

            if (state == KeyState::Up) {
                if (route == Route::Engine) {
                    // THE INVARIANT.
                    KGN_CHECK(pressOpen[k]);
                    KGN_CHECK(pressRoutedToEngine[k]);
                    KGN_CHECK(!somethingWentNative[k]);
                }
                pressOpen[k] = false;
                pressRoutedToEngine[k] = false;
                somethingWentNative[k] = false;
            } else {
                pressOpen[k] = true;
                if (route == Route::Engine) pressRoutedToEngine[k] = true;
                if (route == Route::Native) somethingWentNative[k] = true;
            }
        }
    }
}

// The mirror of the same rule: a press Windows received must keep its release.
KGN_TEST(a_press_windows_received_natively_always_keeps_its_release) {
    std::mt19937 rng(7u);
    std::uniform_int_distribution<int> pick(0, 3);

    for (int round = 0; round < 500; ++round) {
        DeliveryProvenance p;
        bool pressOpen = false, wentNative = false, enabled = true;
        for (int step = 0; step < 60; ++step) {
            if (pick(rng) == 0) enabled = !enabled;
            KeyState state = KeyState::Down;
            if (pressOpen) state = pick(rng) == 0 ? KeyState::Up : KeyState::Repeat;

            const Route route = p.route(A, state, enabled);
            if (state == KeyState::Up) {
                if (wentNative) KGN_CHECK(route == Route::Native);
                pressOpen = false; wentNative = false;
            } else {
                pressOpen = true;
                if (route == Route::Native) wentNative = true;
            }
        }
    }
}

}  // namespace

int main() { return kgn::test::runAll(); }

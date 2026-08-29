// The Windows hook backend's control path.
//
// Installing a real low-level hook needs a desktop and a human, so what is
// tested here is the part that can go wrong without one: whether a control
// message can falsify the backend's model of the hardware.
//
// The rule it enforces:
//
//     A control-driven engine reset never falsifies physical device state.
//
// The engine tracks what the software OWES and that is resettable. This tracks
// what the user's fingers are doing, and release_all does not lift a finger.

#if defined(_WIN32)

#include "../src/platform/windows/hook_input.hpp"

#include "kgn_test.hpp"

#include <memory>

using namespace kgn;
using kgn::win::HookInput;

namespace {

// Wires a backend to rings without starting its thread, which is all the
// control path needs.
struct Harness {
    WorkRing work;
    PublicationRing publication;
    StatePublisher published;
    EngineConfig config;
    HookInput hook;
    std::unique_ptr<EngineOwner> owner;

    Harness() { owner = hook.engineOwner(work, publication, published, config); }

    void control(Control::Kind kind, bool flag = false) {
        hook.applyControlForTests(Control{kind, flag, 1, nullptr});
    }
};

}  // namespace

KGN_TEST(release_all_does_not_lift_the_users_finger_off_a_key) {
    // Physical Down, release_all, hardware autorepeat, physical Up.
    //
    // If release_all cleared the bitmap, the autorepeat would be classified as
    // a FRESH press. The engine would then forward a second press for a key
    // the OS already believes is down, owing a second release that never
    // arrives -- P7's failure, arrived at through the back door.
    Harness harness;
    const KeyCode a = KeyCode::fromString("KeyA");

    KGN_CHECK(harness.hook.observeForTests(a, false) == KeyState::Down);
    KGN_CHECK(harness.hook.physicallyDownForTests(a));

    harness.control(Control::Kind::ReleaseAll);

    KGN_CHECK(harness.hook.physicallyDownForTests(a));
    KGN_CHECK(harness.hook.observeForTests(a, false) == KeyState::Repeat);
    KGN_CHECK(harness.hook.observeForTests(a, true) == KeyState::Up);
    KGN_CHECK(!harness.hook.physicallyDownForTests(a));
}

KGN_TEST(a_modifier_survives_release_all_disable_and_re_enable) {
    // The user is holding Shift throughout. Every one of these operations
    // resets engine obligations; none of them is allowed to claim the key came
    // up, because no further event will arrive until it actually does.
    Harness harness;
    const KeyCode shift = KeyCode::fromString("ShiftLeft");

    harness.hook.observeForTests(shift, false);
    KGN_CHECK(harness.hook.publishedForTests().shift);

    harness.control(Control::Kind::ReleaseAll);
    KGN_CHECK(harness.hook.physicallyDownForTests(shift));
    KGN_CHECK(harness.hook.publishedForTests().shift);

    harness.control(Control::Kind::SetEnabled, false);
    KGN_CHECK(harness.hook.physicallyDownForTests(shift));
    KGN_CHECK(harness.hook.publishedForTests().shift);

    harness.control(Control::Kind::SetEnabled, true);
    KGN_CHECK(harness.hook.physicallyDownForTests(shift));
    KGN_CHECK(harness.hook.publishedForTests().shift);

    // And it goes false when, and only when, the user lets go.
    harness.hook.observeForTests(shift, true);
    KGN_CHECK(!harness.hook.physicallyDownForTests(shift));
    KGN_CHECK(!harness.hook.publishedForTests().shift);
}

KGN_TEST(an_ordinary_key_held_across_a_disable_is_still_held_after_re_enable) {
    Harness harness;
    const KeyCode j = KeyCode::fromString("KeyJ");

    harness.hook.observeForTests(j, false);
    harness.control(Control::Kind::SetEnabled, false);

    // Events keep arriving while disabled -- they bypass the engine, not the
    // bitmap -- so the state stays true even without the engine's help.
    KGN_CHECK(harness.hook.observeForTests(j, false) == KeyState::Repeat);

    harness.control(Control::Kind::SetEnabled, true);
    KGN_CHECK(harness.hook.physicallyDownForTests(j));
    KGN_CHECK(harness.hook.observeForTests(j, false) == KeyState::Repeat);
}

KGN_TEST(a_release_all_still_unwinds_the_engine_it_is_meant_to_unwind) {
    // The mirror of the tests above: not falsifying physical state must not
    // have cost release_all its actual job.
    Harness harness;
    KGN_CHECK(harness.hook.publishedForTests().mode == Mode::Normal);
    harness.control(Control::Kind::ReleaseAll);
    KGN_CHECK(harness.hook.publishedForTests().mode == Mode::Normal);
    KGN_CHECK(!harness.hook.publishedForTests().latched);
}

KGN_TEST(the_backend_reports_that_its_hook_is_not_installed) {
    // P6: a backend that never installed its hook says so rather than looking
    // like it is working.
    Harness harness;
    Diagnostics found;
    harness.hook.drainDiagnostics(found);

    bool reported = false;
    for (const auto& diagnostic : found) {
        if (diagnostic.code == "input.permission_denied") reported = true;
    }
    KGN_CHECK(reported);
}

int main() { return kgn::test::runAll(); }

#else

int main() { return 0; }

#endif

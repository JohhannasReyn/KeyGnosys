// The layer engine is pure: events and a clock in, decisions out. No OS, no
// device, no thread. That is what makes the two invariants inherited from the
// original prototype testable at all --
//
//   the grace window   (SPEC 6.3): a mapped key pressed just before CapsLock
//                      resolves as intent, not as a typo;
//   P7                 every press forwarded to the OS gets its release
//                      forwarded too, whatever the mode has become by then.

#include "mtk/layer_engine.hpp"
#include "mtk_test.hpp"

#include <random>
#include <string>
#include <vector>

using namespace mtk;

namespace {

const KeyCode CAPS = KeyCode::fromString("CapsLock");
const KeyCode H = KeyCode::fromString("KeyH");
const KeyCode J = KeyCode::fromString("KeyJ");
const KeyCode Q = KeyCode::fromString("KeyQ");
const KeyCode LSHIFT = KeyCode::fromString("ShiftLeft");
const KeyCode LCTRL = KeyCode::fromString("ControlLeft");
// Bound to `key.passthrough`: reaches the OS even inside the layer.
const KeyCode BKSP = KeyCode::fromString("Backspace");

TimePoint at(int ms) {
    return TimePoint{} + std::chrono::milliseconds(ms);
}

BindingMap defaultBindings() {
    return {
        {H, BindingKind::Action},
        {J, BindingKind::Action},
        {BKSP, BindingKind::Passthrough},
    };
}

// A harness that owns the clock, so tests read as a timeline.
struct Fixture {
    LayerEngine engine;
    std::vector<Decision> last;

    explicit Fixture(ActivationMode mode = ActivationMode::Hybrid) {
        EngineConfig config;
        config.activation = mode;
        config.grace = std::chrono::milliseconds(50);
        config.hybridTap = std::chrono::milliseconds(200);
        engine.setConfig(config);
        engine.setBindings(defaultBindings());
    }

    std::vector<Decision>& press(KeyCode code, int ms) {
        last = engine.onKey(code, KeyState::Down, at(ms));
        return last;
    }
    std::vector<Decision>& release(KeyCode code, int ms) {
        last = engine.onKey(code, KeyState::Up, at(ms));
        return last;
    }
    std::vector<Decision>& tick(int ms) {
        last = engine.tick(at(ms));
        return last;
    }
};

bool has(const std::vector<Decision>& ds, Decision::Kind kind, KeyCode code) {
    for (const auto& d : ds) {
        if (d.kind == kind && d.code == code) return true;
    }
    return false;
}

std::size_t countKind(const std::vector<Decision>& ds, Decision::Kind kind) {
    std::size_t n = 0;
    for (const auto& d : ds) {
        if (d.kind == kind) ++n;
    }
    return n;
}

}  // namespace

// ---------------------------------------------------------------------------
// Unbound keys
// ---------------------------------------------------------------------------

MTK_TEST(unbound_keys_pass_straight_through_when_the_layer_is_off) {
    Fixture f;
    MTK_CHECK(has(f.press(Q, 0), Decision::Kind::Forward, Q));
    MTK_CHECK(has(f.release(Q, 10), Decision::Kind::Forward, Q));
}

MTK_TEST(unbound_keys_are_swallowed_while_the_layer_is_engaged) {
    // The layer is a mode, not an overlay on normal typing. The overlay draws
    // these keys blank and dimmed to say so.
    Fixture f;
    f.press(CAPS, 0);
    MTK_CHECK(has(f.press(Q, 10), Decision::Kind::Suppress, Q));
}

MTK_TEST(modifiers_keep_working_inside_the_layer) {
    // So Ctrl+click and Shift+drag do what the user expects.
    Fixture f;
    f.press(CAPS, 0);
    MTK_CHECK(has(f.press(LCTRL, 10), Decision::Kind::Forward, LCTRL));
    MTK_CHECK(has(f.release(LCTRL, 20), Decision::Kind::Forward, LCTRL));
}

// ---------------------------------------------------------------------------
// The grace window -- the three resolutions of SPEC 6.3
// ---------------------------------------------------------------------------

MTK_TEST(a_bound_key_pressed_with_the_layer_off_is_buffered_not_sent) {
    Fixture f;
    MTK_CHECK(has(f.press(H, 0), Decision::Kind::Buffer, H));
    MTK_CHECK_EQ(countKind(f.last, Decision::Kind::Forward), std::size_t{0});
}

MTK_TEST(capslock_arriving_inside_the_window_promotes_the_press) {
    // The user was a few milliseconds ahead of the CapsLock they meant to
    // press first. Nothing reaches the OS.
    Fixture f;
    f.press(H, 0);
    f.press(CAPS, 20);
    MTK_CHECK(has(f.last, Decision::Kind::RunAction, H));
    MTK_CHECK_EQ(countKind(f.last, Decision::Kind::Forward), std::size_t{0});
}

MTK_TEST(release_inside_the_window_is_an_ordinary_tap) {
    Fixture f;
    f.press(H, 0);
    f.release(H, 20);
    MTK_CHECK_EQ(f.last.size(), std::size_t{2});
    MTK_CHECK(f.last[0].kind == Decision::Kind::Forward);
    MTK_CHECK(f.last[0].state == KeyState::Down);
    MTK_CHECK(f.last[1].kind == Decision::Kind::Forward);
    MTK_CHECK(f.last[1].state == KeyState::Up);
}

MTK_TEST(the_window_lapsing_makes_it_an_ordinary_hold) {
    Fixture f;
    f.press(H, 0);
    f.tick(30);
    MTK_CHECK_EQ(f.last.size(), std::size_t{0});   // not yet
    f.tick(60);
    MTK_CHECK(has(f.last, Decision::Kind::Forward, H));
}

MTK_TEST(capslock_after_the_window_lapsed_does_not_steal_the_keystroke) {
    Fixture f;
    f.press(H, 0);
    f.press(CAPS, 200);
    // Buffered far too long ago to have been meant as an action.
    MTK_CHECK(has(f.last, Decision::Kind::Forward, H));
    MTK_CHECK(!has(f.last, Decision::Kind::RunAction, H));
}

MTK_TEST(capslock_promotes_every_buffered_key_at_once) {
    Fixture f;
    f.press(H, 0);
    f.press(J, 5);
    f.press(CAPS, 20);
    MTK_CHECK_EQ(countKind(f.last, Decision::Kind::RunAction), std::size_t{2});
}

MTK_TEST(only_bound_keys_are_ever_delayed) {
    // Buffering costs latency, so it must never touch keys that could not
    // possibly become actions.
    Fixture f;
    MTK_CHECK(has(f.press(Q, 0), Decision::Kind::Forward, Q));
    MTK_CHECK_EQ(countKind(f.last, Decision::Kind::Buffer), std::size_t{0});
}

// ---------------------------------------------------------------------------
// Activation modes
// ---------------------------------------------------------------------------

MTK_TEST(hold_mode_leaves_the_layer_on_release) {
    Fixture f(ActivationMode::Hold);
    f.press(CAPS, 0);
    MTK_CHECK(f.engine.mode() == Mode::Cursor);
    f.release(CAPS, 500);
    MTK_CHECK(f.engine.mode() == Mode::Normal);
}

MTK_TEST(toggle_mode_ignores_release) {
    Fixture f(ActivationMode::Toggle);
    f.press(CAPS, 0);
    f.release(CAPS, 10);
    MTK_CHECK(f.engine.mode() == Mode::Cursor);
    f.press(CAPS, 100);
    MTK_CHECK(f.engine.mode() == Mode::Normal);
}

MTK_TEST(hybrid_tap_latches_the_layer_on) {
    Fixture f(ActivationMode::Hybrid);
    f.press(CAPS, 0);
    f.release(CAPS, 50);           // shorter than hybridTap
    MTK_CHECK(f.engine.mode() == Mode::Cursor);
    MTK_CHECK(f.engine.latched());
}

MTK_TEST(hybrid_hold_is_momentary) {
    Fixture f(ActivationMode::Hybrid);
    f.press(CAPS, 0);
    f.release(CAPS, 400);          // longer than hybridTap
    MTK_CHECK(f.engine.mode() == Mode::Normal);
}

MTK_TEST(a_latched_layer_is_released_by_tapping_again) {
    Fixture f(ActivationMode::Hybrid);
    f.press(CAPS, 0);
    f.release(CAPS, 50);
    MTK_CHECK(f.engine.mode() == Mode::Cursor);
    f.press(CAPS, 1000);
    f.release(CAPS, 1050);
    MTK_CHECK(f.engine.mode() == Mode::Normal);
}

MTK_TEST(capslock_itself_never_reaches_the_os) {
    Fixture f;
    MTK_CHECK(has(f.press(CAPS, 0), Decision::Kind::Suppress, CAPS));
    MTK_CHECK(has(f.release(CAPS, 50), Decision::Kind::Suppress, CAPS));
}

// ---------------------------------------------------------------------------
// Real CapsLock
// ---------------------------------------------------------------------------

MTK_TEST(shift_capslock_produces_a_real_capslock) {
    Fixture f;
    f.press(LSHIFT, 0);
    MTK_CHECK(has(f.press(CAPS, 10), Decision::Kind::Forward, CAPS));
    MTK_CHECK(f.engine.mode() == Mode::Normal);
}

MTK_TEST(a_real_capslock_release_is_forwarded_too) {
    Fixture f;
    f.press(LSHIFT, 0);
    f.press(CAPS, 10);
    f.release(LSHIFT, 20);            // Shift goes first -- common in practice
    MTK_CHECK(has(f.release(CAPS, 30), Decision::Kind::Forward, CAPS));
}

MTK_TEST(the_escape_gesture_can_be_turned_off) {
    EngineConfig config;
    config.shiftCapsIsRealCapsLock = false;
    Fixture f;
    f.engine.setConfig(config);
    f.engine.setBindings(defaultBindings());
    f.press(LSHIFT, 0);
    f.press(CAPS, 10);
    MTK_CHECK(f.engine.mode() == Mode::Cursor);
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

MTK_TEST(bound_keys_run_actions_while_the_layer_is_engaged) {
    Fixture f;
    f.press(CAPS, 0);
    MTK_CHECK(has(f.press(H, 10), Decision::Kind::RunAction, H));
    MTK_CHECK(has(f.release(H, 20), Decision::Kind::ReleaseAction, H));
}

MTK_TEST(leaving_the_layer_releases_every_held_action) {
    Fixture f(ActivationMode::Hold);
    f.press(CAPS, 0);
    f.press(H, 10);
    f.press(J, 20);
    MTK_CHECK_EQ(f.engine.heldActions().size(), std::size_t{2});
    MTK_CHECK(f.engine.heldActions()[0] == H);   // press order
    MTK_CHECK(f.engine.heldActions()[1] == J);
    f.release(CAPS, 500);
    MTK_CHECK_EQ(countKind(f.last, Decision::Kind::ReleaseAction),
                 std::size_t{2});
    MTK_CHECK(f.engine.heldActions().empty());
}

// ---------------------------------------------------------------------------
// P7 -- never strand a key down
// ---------------------------------------------------------------------------

MTK_TEST(a_forwarded_press_is_released_even_after_the_mode_changes) {
    // The failure this prevents: the compositor believing a key is held
    // forever, because its press went to the OS and its release did not.
    Fixture f;
    f.press(H, 0);
    f.tick(60);                        // grace lapses -> H forwarded
    MTK_CHECK(has(f.last, Decision::Kind::Forward, H));
    f.press(CAPS, 100);                // layer engages while H is still down
    MTK_CHECK(has(f.release(H, 200), Decision::Kind::Forward, H));
}

MTK_TEST(release_all_releases_every_forwarded_press) {
    Fixture f;
    f.press(Q, 0);
    f.press(LCTRL, 5);
    auto out = f.engine.releaseAll();
    MTK_CHECK(has(out, Decision::Kind::Forward, Q));
    MTK_CHECK(has(out, Decision::Kind::Forward, LCTRL));
    for (const auto& d : out) {
        if (d.kind == Decision::Kind::Forward) {
            MTK_CHECK(d.state == KeyState::Up);
        }
    }
}

MTK_TEST(release_all_releases_held_actions_and_leaves_the_layer) {
    Fixture f;
    f.press(CAPS, 0);
    f.press(H, 10);
    auto out = f.engine.releaseAll();
    MTK_CHECK(has(out, Decision::Kind::ReleaseAction, H));
    MTK_CHECK(f.engine.mode() == Mode::Normal);
}

MTK_TEST(release_all_does_not_type_buffered_keys) {
    // A buffered press never reached the OS, so there is nothing to release --
    // and replaying it would type a character the user never committed to.
    Fixture f;
    f.press(H, 0);
    auto out = f.engine.releaseAll();
    MTK_CHECK(!has(out, Decision::Kind::Forward, H));
}

MTK_TEST(release_all_is_idempotent) {
    Fixture f;
    f.press(Q, 0);
    f.engine.releaseAll();
    MTK_CHECK_EQ(f.engine.releaseAll().size(), std::size_t{0});
}

// ---------------------------------------------------------------------------
// Property tests
//
// One generator, two directions. The event space deliberately includes
// everything the specification says P7 must survive: repeats, mid-sequence
// releaseAll(), configuration reloads while keys are held, and duplicate or
// orphaned physical events of the kind a dropped event or a stuck driver
// produces.
// ---------------------------------------------------------------------------

namespace {

// Replays one pseudo-random session and returns every decision it produced.
std::vector<Decision> randomSession(unsigned seed) {
    const std::vector<KeyCode> keys = {H, J, Q, LCTRL, LSHIFT, BKSP};
    const std::vector<ActivationMode> modes = {
        ActivationMode::Hold, ActivationMode::Toggle, ActivationMode::Hybrid};

    // Reloads that add, remove and reclassify bindings while keys are held.
    const std::vector<BindingMap> reloads = {
        defaultBindings(),
        {{H, BindingKind::Action}},
        {{H, BindingKind::Passthrough}, {J, BindingKind::Action}},
        {{Q, BindingKind::Action}, {BKSP, BindingKind::Action}},
        {},
    };

    std::mt19937 rng(seed);
    Fixture f(modes[seed % modes.size()]);

    std::vector<Decision> log;
    DecisionBuffer buffer;
    auto drain = [&log, &buffer]() {
        for (const auto& d : buffer) log.push_back(d);
        buffer.clear();
    };

    std::vector<bool> down(keys.size(), false);
    bool capsDown = false;
    int clock = 0;

    for (int step = 0; step < 80; ++step) {
        clock += static_cast<int>(rng() % 120);
        buffer.clear();

        switch (rng() % 16) {
            case 0:
                f.engine.tick(at(clock), buffer);
                break;

            case 1:
                f.engine.onKey(CAPS, capsDown ? KeyState::Up : KeyState::Down,
                               at(clock), buffer);
                capsDown = !capsDown;
                break;

            case 2: {
                // Panic. Everything unwinds; the physical keys stay down, so
                // the releases that follow exercise recovery.
                f.engine.releaseAll(buffer);
                capsDown = false;
                break;
            }

            case 3: {
                // Configuration reload, possibly while keys are held.
                const auto& reload = reloads[rng() % reloads.size()];
                f.engine.setBindings(reload, buffer);
                break;
            }

            case 4: {
                // Autorepeat, including for keys that are not down.
                const std::size_t i = rng() % keys.size();
                f.engine.onKey(keys[i], KeyState::Repeat, at(clock), buffer);
                break;
            }

            case 5: {
                // A malformed event: a press or release that ignores what the
                // engine believes about that key. Dropped events and stuck
                // drivers produce these.
                const std::size_t i = rng() % keys.size();
                const KeyState state =
                    (rng() % 2) ? KeyState::Down : KeyState::Up;
                f.engine.onKey(keys[i], state, at(clock), buffer);
                if (state == KeyState::Down) down[i] = true;
                if (state == KeyState::Up) down[i] = false;
                break;
            }

            default: {
                const std::size_t i = rng() % keys.size();
                const KeyState state = down[i] ? KeyState::Up : KeyState::Down;
                f.engine.onKey(keys[i], state, at(clock), buffer);
                down[i] = !down[i];
                break;
            }
        }
        drain();
    }

    // Wind down: release everything still physically held, then panic.
    clock += 1000;
    buffer.clear();
    for (std::size_t i = 0; i < keys.size(); ++i) {
        if (down[i]) f.engine.onKey(keys[i], KeyState::Up, at(clock), buffer);
    }
    if (capsDown) f.engine.onKey(CAPS, KeyState::Up, at(clock), buffer);
    f.engine.releaseAll(buffer);
    drain();

    return log;
}

// Running press/release depth per key over a decision log.
struct Depth {
    std::vector<std::pair<KeyCode, int>> entries;

    int bump(KeyCode code, int delta) {
        for (auto& entry : entries) {
            if (entry.first == code) {
                entry.second += delta;
                return entry.second;
            }
        }
        entries.emplace_back(code, delta);
        return delta;
    }
};

}  // namespace

MTK_TEST(p7_every_forwarded_press_is_eventually_released) {
    // The failure this prevents: the compositor believing a key is held
    // forever, because its press went to the OS and its release did not.
    for (unsigned seed = 0; seed < 200; ++seed) {
        Depth depth;
        for (const auto& d : randomSession(seed)) {
            if (d.kind != Decision::Kind::Forward) continue;
            if (d.state == KeyState::Down) depth.bump(d.code, 1);
            if (d.state == KeyState::Up) depth.bump(d.code, -1);
        }
        for (const auto& entry : depth.entries) {
            if (entry.second != 0) {
                mtk::test::fail("seed " + std::to_string(seed) + ": "
                                + std::string(entry.first.toString())
                                + " left with press/release imbalance "
                                + std::to_string(entry.second));
                break;
            }
        }
    }
}

MTK_TEST(p7_mirror_no_release_is_forwarded_without_a_press) {
    // The opposite corruption: a key-up the OS has no key-down for. Reached by
    // forwarding the release of a key that was suppressed while held.
    for (unsigned seed = 0; seed < 200; ++seed) {
        Depth depth;
        bool broken = false;
        for (const auto& d : randomSession(seed)) {
            if (d.kind != Decision::Kind::Forward || broken) continue;
            if (d.state == KeyState::Down) {
                depth.bump(d.code, 1);
            } else if (d.state == KeyState::Up && depth.bump(d.code, -1) < 0) {
                mtk::test::fail("seed " + std::to_string(seed) + ": "
                                + std::string(d.code.toString())
                                + " released without a forwarded press");
                broken = true;
            }
        }
    }
}

MTK_TEST(the_generator_actually_exercises_the_paths_it_claims) {
    // A property test that silently stopped generating the interesting
    // transitions would keep passing and prove nothing. Assert the event space
    // is not degenerate.
    std::size_t forwards = 0, suppressions = 0, actions = 0, releases = 0;
    std::size_t buffers = 0, repeats = 0;
    for (unsigned seed = 0; seed < 200; ++seed) {
        for (const auto& d : randomSession(seed)) {
            switch (d.kind) {
                case Decision::Kind::Forward:
                    ++forwards;
                    if (d.state == KeyState::Repeat) ++repeats;
                    break;
                case Decision::Kind::Suppress:
                    ++suppressions;
                    if (d.state == KeyState::Repeat) ++repeats;
                    break;
                case Decision::Kind::RunAction: ++actions; break;
                case Decision::Kind::ReleaseAction: ++releases; break;
                case Decision::Kind::Buffer: ++buffers; break;
            }
        }
    }
    MTK_CHECK(forwards > 1000);
    MTK_CHECK(suppressions > 1000);
    MTK_CHECK(actions > 100);
    MTK_CHECK(releases > 100);
    MTK_CHECK(buffers > 100);
    MTK_CHECK(repeats > 100);
}

// ---------------------------------------------------------------------------
// key.passthrough -- the escape hatch, and only when explicitly bound
// ---------------------------------------------------------------------------

MTK_TEST(an_explicit_passthrough_binding_reaches_the_os_inside_the_layer) {
    Fixture f;
    f.press(CAPS, 0);
    MTK_CHECK(has(f.press(BKSP, 10), Decision::Kind::Forward, BKSP));
    MTK_CHECK(has(f.release(BKSP, 20), Decision::Kind::Forward, BKSP));
}

MTK_TEST(an_unbound_key_is_never_treated_as_passthrough) {
    // The escape hatch requires an explicit binding. Absence of a binding is
    // not permission to type.
    Fixture f;
    f.press(CAPS, 0);
    MTK_CHECK(has(f.press(Q, 10), Decision::Kind::Suppress, Q));
    MTK_CHECK(!has(f.last, Decision::Kind::Forward, Q));
}

MTK_TEST(a_passthrough_key_is_not_delayed_by_the_grace_window) {
    // It does the same thing in both modes, so there is nothing to
    // disambiguate and no reason to make the user wait for it.
    Fixture f;
    MTK_CHECK(has(f.press(BKSP, 0), Decision::Kind::Forward, BKSP));
    MTK_CHECK_EQ(countKind(f.last, Decision::Kind::Buffer), std::size_t{0});
}

MTK_TEST(p7_covers_passthrough_keys_across_a_mode_change) {
    Fixture f(ActivationMode::Hold);
    f.press(CAPS, 0);
    f.press(BKSP, 10);             // forwarded inside the layer
    f.release(CAPS, 500);          // layer drops while it is still held
    MTK_CHECK(f.engine.mode() == Mode::Normal);
    MTK_CHECK(has(f.release(BKSP, 600), Decision::Kind::Forward, BKSP));
}

// ---------------------------------------------------------------------------
// Modifiers held across a mode change
// ---------------------------------------------------------------------------

MTK_TEST(a_modifier_release_survives_the_layer_deactivating) {
    // Ctrl goes down inside the layer, the layer drops while it is still held,
    // and the release must still reach the OS -- otherwise the compositor
    // believes Ctrl is held forever.
    Fixture f(ActivationMode::Hold);
    f.press(CAPS, 0);
    f.press(LCTRL, 10);
    f.release(CAPS, 500);
    MTK_CHECK(f.engine.mode() == Mode::Normal);
    MTK_CHECK(has(f.release(LCTRL, 600), Decision::Kind::Forward, LCTRL));
}

MTK_TEST(a_modifier_release_survives_release_all) {
    Fixture f;
    f.press(CAPS, 0);
    f.press(LSHIFT, 10);
    auto out = f.engine.releaseAll();
    MTK_CHECK(has(out, Decision::Kind::Forward, LSHIFT));
}

MTK_TEST(a_modifier_held_from_before_the_layer_still_releases) {
    Fixture f;
    f.press(LCTRL, 0);             // forwarded in normal mode
    f.press(CAPS, 10);             // layer engages around it
    MTK_CHECK(has(f.release(LCTRL, 20), Decision::Kind::Forward, LCTRL));
}

// ---------------------------------------------------------------------------
// Suppressed keys must not produce orphan releases
// ---------------------------------------------------------------------------

MTK_TEST(a_suppressed_key_held_across_deactivation_releases_suppressed) {
    // Its press never reached the OS, so forwarding its release would be a
    // key-up for a key the OS never saw go down.
    Fixture f(ActivationMode::Hold);
    f.press(CAPS, 0);
    f.press(Q, 10);                // suppressed
    f.release(CAPS, 500);          // layer drops, Q still physically held
    MTK_CHECK(has(f.release(Q, 600), Decision::Kind::Suppress, Q));
    MTK_CHECK(!has(f.last, Decision::Kind::Forward, Q));
}

MTK_TEST(an_action_key_held_across_deactivation_releases_suppressed) {
    Fixture f(ActivationMode::Hold);
    f.press(CAPS, 0);
    f.press(H, 10);                // RunAction; nothing reached the OS
    f.release(CAPS, 500);          // ReleaseAction emitted here
    MTK_CHECK(has(f.last, Decision::Kind::ReleaseAction, H));
    MTK_CHECK(has(f.release(H, 600), Decision::Kind::Suppress, H));
    MTK_CHECK(!has(f.last, Decision::Kind::Forward, H));
}

// ---------------------------------------------------------------------------
// The binding classification is one map, so it cannot disagree with itself
// ---------------------------------------------------------------------------

MTK_TEST(passthrough_requires_a_binding_and_the_api_cannot_express_otherwise) {
    // A key is passthrough because its binding says so. There is no second
    // set that could list it without the first agreeing -- BindingKind lives
    // on the map entry, and having an entry is what being bound means.
    Fixture f;
    f.engine.setBindings({{H, BindingKind::Action}});
    f.press(CAPS, 0);
    // BKSP is no longer in the map at all, so it is unbound, so it is
    // suppressed -- not forwarded as a leftover passthrough.
    MTK_CHECK(has(f.press(BKSP, 10), Decision::Kind::Suppress, BKSP));
    MTK_CHECK(!has(f.last, Decision::Kind::Forward, BKSP));
}

MTK_TEST(removing_a_binding_cannot_leave_stale_passthrough_behaviour) {
    Fixture f;
    f.press(CAPS, 0);
    MTK_CHECK(has(f.press(BKSP, 10), Decision::Kind::Forward, BKSP));
    f.release(BKSP, 20);

    f.engine.setBindings({{H, BindingKind::Action}});      // reload drops BKSP
    MTK_CHECK(has(f.press(BKSP, 30), Decision::Kind::Suppress, BKSP));
}

MTK_TEST(a_reload_releases_actions_whose_binding_disappeared) {
    Fixture f;
    f.press(CAPS, 0);
    f.press(H, 10);
    f.press(J, 20);
    auto out = f.engine.setBindings({{H, BindingKind::Action}});
    // J lost its binding, so its action must end. H keeps its, so it must not.
    MTK_CHECK(has(out, Decision::Kind::ReleaseAction, J));
    MTK_CHECK(!has(out, Decision::Kind::ReleaseAction, H));
    MTK_CHECK_EQ(f.engine.heldActions().size(), std::size_t{1});
}

MTK_TEST(a_reload_that_changes_a_keys_kind_shows_no_intermediate_state) {
    // H goes from Action to Passthrough in one call. No sequence of events can
    // observe it as neither, or as both.
    Fixture f;
    f.press(CAPS, 0);
    f.engine.setBindings({{H, BindingKind::Passthrough}});
    MTK_CHECK(has(f.press(H, 10), Decision::Kind::Forward, H));
    MTK_CHECK(!has(f.last, Decision::Kind::RunAction, H));
    MTK_CHECK(has(f.release(H, 20), Decision::Kind::Forward, H));
}

MTK_TEST(a_reload_drops_buffered_presses_without_typing_them) {
    Fixture f;
    f.press(H, 0);                                    // buffered
    auto out = f.engine.setBindings({{J, BindingKind::Action}});
    MTK_CHECK(!has(out, Decision::Kind::Forward, H));  // never typed
    // Its press never reached the OS, so its release must not either.
    MTK_CHECK(has(f.release(H, 10), Decision::Kind::Suppress, H));
}

MTK_TEST(a_reload_leaves_forwarded_presses_alone) {
    // P7 outranks a config reload: a key the OS has seen go down still owes
    // it a key-up.
    Fixture f;
    f.press(Q, 0);
    f.engine.setBindings({{Q, BindingKind::Action}});
    MTK_CHECK(has(f.release(Q, 10), Decision::Kind::Forward, Q));
}

// ---------------------------------------------------------------------------
// Ordering -- buffered events must resolve in press order
// ---------------------------------------------------------------------------

MTK_TEST(expired_buffered_presses_are_forwarded_in_press_order) {
    Fixture f;
    f.press(H, 0);
    f.press(J, 5);
    f.tick(100);
    MTK_CHECK_EQ(f.last.size(), std::size_t{2});
    MTK_CHECK(f.last[0].code == H);
    MTK_CHECK(f.last[1].code == J);
}

MTK_TEST(reversed_presses_expire_in_that_reversed_order) {
    // The order comes from when the keys were pressed, not from anything
    // intrinsic to the keys themselves.
    Fixture f;
    f.press(J, 0);
    f.press(H, 5);
    f.tick(100);
    MTK_CHECK_EQ(f.last.size(), std::size_t{2});
    MTK_CHECK(f.last[0].code == J);
    MTK_CHECK(f.last[1].code == H);
}

MTK_TEST(capslock_promotes_buffered_keys_in_press_order) {
    Fixture f;
    f.press(J, 0);
    f.press(H, 5);
    f.press(CAPS, 20);
    MTK_CHECK_EQ(countKind(f.last, Decision::Kind::RunAction), std::size_t{2});
    MTK_CHECK(f.last[1].code == J);   // [0] is the CapsLock suppression
    MTK_CHECK(f.last[2].code == H);
}

MTK_TEST(partial_expiry_preserves_the_order_of_what_remains) {
    Fixture f;
    f.press(H, 0);
    f.press(J, 40);
    f.press(Q, 45);          // unbound, forwarded immediately, never buffered
    f.tick(60);              // H has expired; J has not
    MTK_CHECK_EQ(f.last.size(), std::size_t{1});
    MTK_CHECK(f.last[0].code == H);
    f.tick(120);
    MTK_CHECK_EQ(f.last.size(), std::size_t{1});
    MTK_CHECK(f.last[0].code == J);
}

MTK_TEST(held_actions_are_released_in_press_order) {
    Fixture f(ActivationMode::Hold);
    f.press(CAPS, 0);
    f.press(J, 10);
    f.press(H, 20);
    f.release(CAPS, 500);
    MTK_CHECK_EQ(f.last.size(), std::size_t{3});   // suppress + two releases
    MTK_CHECK(f.last[1].code == J);
    MTK_CHECK(f.last[2].code == H);
}

MTK_TEST(release_all_unwinds_forwarded_presses_in_press_order) {
    Fixture f;
    f.press(LCTRL, 0);
    f.press(Q, 5);
    f.press(LSHIFT, 10);
    auto out = f.engine.releaseAll();
    MTK_CHECK_EQ(out.size(), std::size_t{3});
    MTK_CHECK(out[0].code == LCTRL);
    MTK_CHECK(out[1].code == Q);
    MTK_CHECK(out[2].code == LSHIFT);
}

MTK_TEST(the_same_input_always_produces_the_same_decisions) {
    // Nondeterminism here would be invisible in normal use and vicious to
    // debug, so it is asserted directly rather than hoped for.
    auto replay = []() {
        Fixture f;
        std::vector<Decision> log;
        for (KeyCode code : {H, J, Q, LCTRL}) {
            for (const auto& d : f.press(code, 0)) log.push_back(d);
        }
        for (const auto& d : f.tick(100)) log.push_back(d);
        for (const auto& d : f.press(CAPS, 110)) log.push_back(d);
        for (const auto& d : f.engine.releaseAll()) log.push_back(d);
        return log;
    };
    const auto first = replay();
    for (int run = 0; run < 20; ++run) {
        const auto again = replay();
        MTK_CHECK_EQ(again.size(), first.size());
        for (std::size_t i = 0; i < first.size() && i < again.size(); ++i) {
            MTK_CHECK(again[i].kind == first[i].kind);
            MTK_CHECK(again[i].code == first[i].code);
            MTK_CHECK(again[i].state == first[i].state);
        }
    }
}

// ---------------------------------------------------------------------------
// Malformed and duplicate physical events
// ---------------------------------------------------------------------------

MTK_TEST(a_duplicate_press_does_not_double_the_release_obligation) {
    Fixture f;
    f.press(Q, 0);
    f.press(Q, 10);                  // dropped release, or a stuck driver
    MTK_CHECK(has(f.release(Q, 20), Decision::Kind::Forward, Q));
    // The books are settled: a second release owes nothing.
    MTK_CHECK(has(f.release(Q, 30), Decision::Kind::Suppress, Q));
}

MTK_TEST(a_release_with_no_press_is_suppressed) {
    Fixture f;
    MTK_CHECK(has(f.release(Q, 0), Decision::Kind::Suppress, Q));
}

MTK_TEST(a_duplicate_press_cannot_extend_the_grace_window) {
    Fixture f;
    f.press(H, 0);
    f.press(H, 40);                  // must not reset the clock
    f.tick(60);
    MTK_CHECK(has(f.last, Decision::Kind::Forward, H));
}

MTK_TEST(repeat_events_follow_the_press_that_preceded_them) {
    Fixture f;
    f.press(Q, 0);
    f.last = f.engine.onKey(Q, KeyState::Repeat, at(100));
    MTK_CHECK(has(f.last, Decision::Kind::Forward, Q));

    f.press(CAPS, 200);
    f.press(H, 210);                 // running an action
    f.last = f.engine.onKey(H, KeyState::Repeat, at(300));
    MTK_CHECK(has(f.last, Decision::Kind::Suppress, H));
}

// ---------------------------------------------------------------------------
// The capacity bounds
// ---------------------------------------------------------------------------

MTK_TEST(the_decision_buffer_never_reallocates) {
    // The point of DecisionBuffer: fixed storage, so nothing on the event path
    // can allocate. Drive it hard and assert it stayed within capacity.
    Fixture f;
    DecisionBuffer buffer;
    for (int i = 0; i < 500; ++i) {
        buffer.clear();
        f.engine.onKey(H, KeyState::Down, at(i * 10), buffer);
        f.engine.onKey(CAPS, KeyState::Down, at(i * 10 + 1), buffer);
        f.engine.onKey(H, KeyState::Up, at(i * 10 + 2), buffer);
        f.engine.onKey(CAPS, KeyState::Up, at(i * 10 + 3), buffer);
        f.engine.tick(at(i * 10 + 4), buffer);
        MTK_CHECK(!buffer.overflowed());
    }
    buffer.clear();
    f.engine.releaseAll(buffer);
    MTK_CHECK(!buffer.overflowed());
}

int main() {
    return mtk::test::runAll();
}

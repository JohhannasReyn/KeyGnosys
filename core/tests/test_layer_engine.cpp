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
        engine.setBoundKeys({H, J, BKSP});
        engine.setPassthroughKeys({BKSP});
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
    f.engine.setBoundKeys({H, J, BKSP});
    f.engine.setPassthroughKeys({BKSP});
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

MTK_TEST(p7_holds_over_random_event_sequences) {
    // Property test. For any interleaving of presses, releases, ticks and mode
    // changes, every Forward(Down) must be matched by a Forward(Up) once the
    // sequence is wound down.
    const std::vector<KeyCode> keys = {H, J, Q, LCTRL, LSHIFT, BKSP};
    const std::vector<ActivationMode> modes = {
        ActivationMode::Hold, ActivationMode::Toggle, ActivationMode::Hybrid};

    for (unsigned seed = 0; seed < 200; ++seed) {
        std::mt19937 rng(seed);
        Fixture f(modes[seed % modes.size()]);

        std::vector<bool> down(keys.size(), false);
        bool capsDown = false;
        std::vector<Decision> log;
        int clock = 0;

        for (int step = 0; step < 60; ++step) {
            clock += static_cast<int>(rng() % 120);
            const unsigned choice = rng() % 10;

            if (choice == 0) {
                f.engine.tick(at(clock), log);
            } else if (choice == 1) {
                if (capsDown) {
                    f.engine.onKey(CAPS, KeyState::Up, at(clock), log);
                } else {
                    f.engine.onKey(CAPS, KeyState::Down, at(clock), log);
                }
                capsDown = !capsDown;
            } else {
                const std::size_t i = rng() % keys.size();
                const KeyState state = down[i] ? KeyState::Up : KeyState::Down;
                f.engine.onKey(keys[i], state, at(clock), log);
                down[i] = !down[i];
            }
        }

        // Wind down: release everything still physically held, then panic.
        clock += 1000;
        for (std::size_t i = 0; i < keys.size(); ++i) {
            if (down[i]) f.engine.onKey(keys[i], KeyState::Up, at(clock), log);
        }
        if (capsDown) f.engine.onKey(CAPS, KeyState::Up, at(clock), log);
        f.engine.releaseAll(log);

        // Now count. A key must never be left down.
        std::vector<std::pair<KeyCode, int>> depth;
        auto bump = [&depth](KeyCode code, int delta) {
            for (auto& entry : depth) {
                if (entry.first == code) { entry.second += delta; return; }
            }
            depth.emplace_back(code, delta);
        };
        for (const auto& d : log) {
            if (d.kind != Decision::Kind::Forward) continue;
            if (d.state == KeyState::Down) bump(d.code, 1);
            if (d.state == KeyState::Up) bump(d.code, -1);
        }
        for (const auto& entry : depth) {
            if (entry.second != 0) {
                std::string name(entry.first.toString());
                mtk::test::fail("seed " + std::to_string(seed) + ": " + name
                     + " left with press/release imbalance "
                     + std::to_string(entry.second));
                break;
            }
        }
    }
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

MTK_TEST(every_forwarded_release_has_a_matching_forwarded_press) {
    // The mirror of P7, and the property that stops the engine emitting a
    // key-up the OS has no key-down for. Same random sweep, opposite
    // direction: the depth counter must never go negative.
    const std::vector<KeyCode> keys = {H, J, Q, LCTRL, LSHIFT, BKSP};
    const std::vector<ActivationMode> modes = {
        ActivationMode::Hold, ActivationMode::Toggle, ActivationMode::Hybrid};

    for (unsigned seed = 0; seed < 200; ++seed) {
        std::mt19937 rng(seed);
        Fixture f(modes[seed % modes.size()]);

        std::vector<bool> down(keys.size(), false);
        bool capsDown = false;
        std::vector<Decision> log;
        int clock = 0;

        for (int step = 0; step < 60; ++step) {
            clock += static_cast<int>(rng() % 120);
            const unsigned choice = rng() % 10;
            if (choice == 0) {
                f.engine.tick(at(clock), log);
            } else if (choice == 1) {
                f.engine.onKey(CAPS, capsDown ? KeyState::Up : KeyState::Down,
                               at(clock), log);
                capsDown = !capsDown;
            } else {
                const std::size_t i = rng() % keys.size();
                f.engine.onKey(keys[i], down[i] ? KeyState::Up : KeyState::Down,
                               at(clock), log);
                down[i] = !down[i];
            }
        }

        std::vector<std::pair<KeyCode, int>> depth;
        auto bump = [&depth](KeyCode code, int delta) -> int {
            for (auto& entry : depth) {
                if (entry.first == code) {
                    entry.second += delta;
                    return entry.second;
                }
            }
            depth.emplace_back(code, delta);
            return delta;
        };

        bool broken = false;
        for (const auto& d : log) {
            if (d.kind != Decision::Kind::Forward || broken) continue;
            if (d.state == KeyState::Down) {
                bump(d.code, 1);
            } else if (d.state == KeyState::Up && bump(d.code, -1) < 0) {
                mtk::test::fail("seed " + std::to_string(seed) + ": "
                                + std::string(d.code.toString())
                                + " released without a forwarded press");
                broken = true;
            }
        }
    }
}

int main() {
    return mtk::test::runAll();
}

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

            case 6: {
                // Malformed CapsLock specifically. It is dispatched before the
                // general duplicate-state logic, so it needs exercising in its
                // own right rather than only as one key among many.
                const unsigned which = rng() % 3;
                const KeyState state = which == 0   ? KeyState::Down
                                       : which == 1 ? KeyState::Up
                                                    : KeyState::Repeat;
                f.engine.onKey(CAPS, state, at(clock), buffer);
                if (state == KeyState::Down) capsDown = true;
                if (state == KeyState::Up) capsDown = false;
                break;
            }

            case 7: {
                // An invalid code, which no backend emits but which the engine
                // must nonetheless not mishandle.
                f.engine.onKey(KeyCode{},
                               (rng() % 2) ? KeyState::Down : KeyState::Up,
                               at(clock), buffer);
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
// The key domain
//
// Per-key state covers the whole KeyCode id space, so there is no "untracked"
// class of key whose invariants cannot be maintained. The only code without
// state is the invalid one, which no backend can emit.
// ---------------------------------------------------------------------------

MTK_TEST(a_high_id_key_is_tracked_like_any_other) {
    // Codes interned beyond the built-in vocabulary used to fall off a
    // stateless path that forwarded everything, which no invariant survives.
    Fixture f;
    const KeyCode odd = KeyCode::fromString("VendorSpecificThing");
    MTK_CHECK(odd.valid());
    MTK_CHECK(has(f.press(odd, 0), Decision::Kind::Forward, odd));

    auto out = f.engine.releaseAll();
    MTK_CHECK(has(out, Decision::Kind::Forward, odd));   // release is owed
    for (const auto& d : out) {
        if (d.kind == Decision::Kind::Forward) {
            MTK_CHECK(d.state == KeyState::Up);
        }
    }
}

MTK_TEST(an_invalid_code_is_suppressed_in_every_direction) {
    // Never forwards a press, so it can never owe a release. Both invariants
    // hold trivially rather than by argument.
    Fixture f;
    const KeyCode bad{};
    MTK_CHECK(!bad.valid());
    MTK_CHECK(has(f.press(bad, 0), Decision::Kind::Suppress, bad));
    MTK_CHECK(has(f.release(bad, 10), Decision::Kind::Suppress, bad));
    MTK_CHECK(has(f.release(bad, 20), Decision::Kind::Suppress, bad));  // orphan
    MTK_CHECK_EQ(f.engine.releaseAll().size(), std::size_t{0});
    MTK_CHECK(f.engine.invalidEvents() > 0);
}

MTK_TEST(an_orphan_release_of_a_high_id_key_is_suppressed) {
    Fixture f;
    const KeyCode odd = KeyCode::fromString("AnotherVendorKey");
    MTK_CHECK(has(f.release(odd, 0), Decision::Kind::Suppress, odd));
}

// ---------------------------------------------------------------------------
// Forced capacity overflow
//
// "A human has ten fingers" is not a safety argument: malformed drivers,
// injected events and lost releases are in the threat model. These drive the
// bounded lists past capacity on purpose.
// ---------------------------------------------------------------------------

namespace {

std::vector<KeyCode> syntheticKeys(const char* prefix, std::size_t count) {
    std::vector<KeyCode> keys;
    keys.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        keys.push_back(KeyCode::fromString(std::string(prefix)
                                           + std::to_string(i)));
    }
    return keys;
}

}  // namespace

MTK_TEST(overflowing_the_forwarded_list_suppresses_rather_than_stranding) {
    // Past capacity the engine must refuse to create an obligation it cannot
    // record. A dropped keystroke is recoverable; a key the OS believes is
    // held forever is not.
    Fixture f;
    const auto keys = syntheticKeys("OverflowFwd", kMaxHeld + 40);

    std::vector<Decision> log;
    for (KeyCode code : keys) {
        for (const auto& d : f.press(code, 0)) log.push_back(d);
    }
    for (const auto& d : f.engine.releaseAll()) log.push_back(d);

    MTK_CHECK(f.engine.capacityDrops() > 0);   // the overflow really happened

    Depth depth;
    bool orphan = false;
    for (const auto& d : log) {
        if (d.kind != Decision::Kind::Forward) continue;
        if (d.state == KeyState::Down) depth.bump(d.code, 1);
        if (d.state == KeyState::Up && depth.bump(d.code, -1) < 0) orphan = true;
    }
    MTK_CHECK(!orphan);
    for (const auto& entry : depth.entries) {
        MTK_CHECK_EQ(entry.second, 0);
    }
}

MTK_TEST(overflowing_the_held_action_list_emits_no_unreleasable_action) {
    // The action-side twin: RunAction without a guaranteed ReleaseAction is
    // P7's failure in a different currency.
    const auto keys = syntheticKeys("OverflowAct", kMaxHeld + 40);

    BindingMap bindings;
    for (KeyCode code : keys) bindings[code] = BindingKind::Action;

    Fixture f;
    f.engine.setBindings(bindings);
    f.press(CAPS, 0);

    std::vector<Decision> log;
    for (KeyCode code : keys) {
        for (const auto& d : f.press(code, 10)) log.push_back(d);
    }
    for (const auto& d : f.engine.releaseAll()) log.push_back(d);

    MTK_CHECK(f.engine.capacityDrops() > 0);

    Depth depth;
    for (const auto& d : log) {
        if (d.kind == Decision::Kind::RunAction) depth.bump(d.code, 1);
        if (d.kind == Decision::Kind::ReleaseAction) depth.bump(d.code, -1);
    }
    for (const auto& entry : depth.entries) {
        MTK_CHECK_EQ(entry.second, 0);
    }
}

MTK_TEST(overflowing_the_pending_list_degrades_to_no_grace_window) {
    // Buffering is the optional part; delivering the keystroke is not.
    const auto keys = syntheticKeys("OverflowPend", kMaxPending + 10);

    BindingMap bindings;
    for (KeyCode code : keys) bindings[code] = BindingKind::Action;

    Fixture f;
    f.engine.setBindings(bindings);

    std::size_t forwarded = 0;
    for (KeyCode code : keys) {
        for (const auto& d : f.press(code, 0)) {
            if (d.kind == Decision::Kind::Forward) ++forwarded;
        }
    }
    MTK_CHECK(forwarded > 0);                  // the surplus was not dropped
    MTK_CHECK(f.engine.capacityDrops() > 0);
}

MTK_TEST(the_decision_buffer_holds_a_full_worst_case_unwind) {
    // The capacity is derived from this case, so assert the derivation.
    const auto keys = syntheticKeys("Unwind", kMaxHeld);
    BindingMap bindings;
    for (KeyCode code : keys) bindings[code] = BindingKind::Action;

    Fixture f;
    f.engine.setBindings(bindings);
    f.press(CAPS, 0);
    for (KeyCode code : keys) f.press(code, 10);

    DecisionBuffer buffer;
    f.engine.releaseAll(buffer);
    MTK_CHECK(!buffer.overflowed());
}

// ---------------------------------------------------------------------------
// Malformed CapsLock events
//
// CapsLock is dispatched before the general duplicate-state logic, so it needs
// its own physical-state tracking or it bypasses that policy entirely.
// ---------------------------------------------------------------------------

MTK_TEST(a_duplicate_capslock_press_does_not_toggle_the_layer_twice) {
    Fixture f(ActivationMode::Toggle);
    f.press(CAPS, 0);
    MTK_CHECK(f.engine.mode() == Mode::Cursor);
    f.press(CAPS, 10);                 // duplicate Down, no release between
    MTK_CHECK(f.engine.mode() == Mode::Cursor);
}

MTK_TEST(a_duplicate_capslock_press_is_suppressed_not_re_run) {
    Fixture f(ActivationMode::Hybrid);
    f.press(CAPS, 0);
    MTK_CHECK(has(f.press(CAPS, 10), Decision::Kind::Suppress, CAPS));
}

MTK_TEST(a_duplicate_real_capslock_press_forwards_as_a_repeat) {
    Fixture f;
    f.press(LSHIFT, 0);
    f.press(CAPS, 10);                 // real CapsLock, forwarded
    f.press(CAPS, 20);                 // duplicate
    MTK_CHECK_EQ(f.last.size(), std::size_t{1});
    MTK_CHECK(f.last[0].kind == Decision::Kind::Forward);
    MTK_CHECK(f.last[0].state == KeyState::Repeat);
}

MTK_TEST(an_orphan_capslock_release_cannot_leave_the_layer) {
    Fixture f(ActivationMode::Toggle);
    f.press(CAPS, 0);
    f.release(CAPS, 10);
    MTK_CHECK(f.engine.mode() == Mode::Cursor);
    MTK_CHECK(has(f.release(CAPS, 20), Decision::Kind::Suppress, CAPS));
    MTK_CHECK(f.engine.mode() == Mode::Cursor);   // orphan changed nothing
}

MTK_TEST(an_orphan_capslock_release_cannot_latch_in_hybrid_mode) {
    Fixture f(ActivationMode::Hybrid);
    MTK_CHECK(f.engine.mode() == Mode::Normal);
    f.release(CAPS, 0);                // never pressed
    MTK_CHECK(f.engine.mode() == Mode::Normal);
    MTK_CHECK(!f.engine.latched());
}

MTK_TEST(a_stale_press_time_cannot_influence_a_later_release) {
    // capsPressedAt_ left set after a release used to remain available to the
    // next malformed event, which could latch the layer from a stale clock.
    Fixture f(ActivationMode::Hybrid);
    f.press(CAPS, 0);
    f.release(CAPS, 50);               // tap: latches on
    MTK_CHECK(f.engine.latched());
    f.release(CAPS, 10000);            // orphan, long after
    MTK_CHECK(f.engine.mode() == Mode::Cursor);
    MTK_CHECK(f.engine.latched());
}

MTK_TEST(release_all_clears_physical_capslock_state) {
    Fixture f(ActivationMode::Hold);
    f.press(CAPS, 0);
    f.engine.releaseAll();
    MTK_CHECK(f.engine.mode() == Mode::Normal);
    // The physical release that follows must not re-enter or alter anything.
    MTK_CHECK(has(f.release(CAPS, 10), Decision::Kind::Suppress, CAPS));
    MTK_CHECK(f.engine.mode() == Mode::Normal);
}

// ---------------------------------------------------------------------------
// Modifier state after a panic
// ---------------------------------------------------------------------------

MTK_TEST(release_all_clears_modifier_state_for_an_action_bound_modifier) {
    // A modifier bound to an action lives in the held list, not the forwarded
    // list. Clearing only the latter left `modifierHeld` set, and the next
    // CapsLock was then misread as the Shift+CapsLock escape gesture -- so the
    // layer would silently stop engaging.
    Fixture f(ActivationMode::Toggle);
    f.engine.setBindings({{LSHIFT, BindingKind::Action}});
    f.press(CAPS, 0);                  // engage the layer
    f.press(LSHIFT, 10);               // runs an action; Shift held physically
    f.engine.releaseAll();

    f.press(CAPS, 20);
    MTK_CHECK(f.engine.mode() == Mode::Cursor);          // layer, not caps
    MTK_CHECK(!has(f.last, Decision::Kind::Forward, CAPS));
}

MTK_TEST(release_all_clears_modifier_state_for_a_forwarded_modifier) {
    Fixture f(ActivationMode::Toggle);
    f.press(LSHIFT, 0);
    f.engine.releaseAll();
    f.press(CAPS, 10);
    MTK_CHECK(f.engine.mode() == Mode::Cursor);
    MTK_CHECK(!has(f.last, Decision::Kind::Forward, CAPS));
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

MTK_TEST(a_reload_keeps_a_pending_press_that_is_still_action_bound) {
    // The press is physical input the user already committed to. It was
    // delayed only to resolve layer intent, so an unrelated reload must not
    // consume it.
    Fixture f;
    f.press(H, 0);
    auto out = f.engine.setBindings({{H, BindingKind::Action}});
    MTK_CHECK_EQ(out.size(), std::size_t{0});
    f.tick(100);                                   // still waiting; now expires
    MTK_CHECK(has(f.last, Decision::Kind::Forward, H));
    MTK_CHECK(has(f.release(H, 110), Decision::Kind::Forward, H));
}

MTK_TEST(a_reload_preserves_the_original_press_time_of_a_kept_pending_key) {
    // Keeping the press but restarting its clock would silently extend the
    // grace window, which is the same defect as a duplicate press extending it.
    Fixture f;
    f.press(H, 0);
    f.engine.setBindings({{H, BindingKind::Action}});
    f.tick(60);                                    // 60ms > 50ms grace
    MTK_CHECK(has(f.last, Decision::Kind::Forward, H));
}

MTK_TEST(a_reload_resolves_a_pending_press_that_lost_its_binding) {
    // No longer action-bound, so there is nothing left to disambiguate: it was
    // an ordinary keystroke all along. It must be delivered, not swallowed.
    Fixture f;
    f.press(H, 0);
    auto out = f.engine.setBindings({{J, BindingKind::Action}});
    MTK_CHECK(has(out, Decision::Kind::Forward, H));
    // And having been forwarded, it now owes a release.
    MTK_CHECK(has(f.release(H, 10), Decision::Kind::Forward, H));
}

MTK_TEST(a_reload_resolves_multiple_pending_keys_in_press_order) {
    Fixture f;
    f.press(J, 0);
    f.press(H, 5);
    auto out = f.engine.setBindings({});
    MTK_CHECK_EQ(out.size(), std::size_t{2});
    MTK_CHECK(out[0].code == J);
    MTK_CHECK(out[1].code == H);
}

MTK_TEST(a_reload_releases_actions_in_press_order) {
    // SPEC 6.3.1 applies here too: multi-key decisions are emitted in press
    // order, not in whatever order the held list happens to be walked.
    Fixture f;
    f.press(CAPS, 0);
    f.press(J, 10);
    f.press(H, 20);
    auto out = f.engine.setBindings({});
    MTK_CHECK_EQ(countKind(out, Decision::Kind::ReleaseAction), std::size_t{2});
    MTK_CHECK(out[0].code == J);
    MTK_CHECK(out[1].code == H);
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

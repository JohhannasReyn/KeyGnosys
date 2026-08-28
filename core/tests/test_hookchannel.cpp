// The two hook-to-core streams, and the capacity argument that keeps a release
// path from ever failing for want of queue space.
//
// Everything here runs with no thread and no operating system, which is the
// point: the rules that decide whether a key can be stranded are testable
// exactly because they are pure.

#include "kgn/hookchannel.hpp"

#include "kgn_test.hpp"

#include <string>
#include <vector>

using namespace kgn;

namespace {

TimePoint at(int ms) {
    return TimePoint{} + std::chrono::milliseconds(ms);
}

Decision forwardOf(const char* name, KeyState state) {
    return {Decision::Kind::Forward, KeyCode::fromString(name), state};
}
Decision suppressOf(const char* name, KeyState state) {
    return {Decision::Kind::Suppress, KeyCode::fromString(name), state};
}
Decision runActionOf(const char* name) {
    return {Decision::Kind::RunAction, KeyCode::fromString(name), KeyState::Down};
}

std::size_t drain(WorkRing& ring) {
    WorkItem item{};
    std::size_t count = 0;
    while (ring.pop(item)) ++count;
    return count;
}

}  // namespace

// ---------------------------------------------------------------------------
// The native passthrough rule

KGN_TEST(a_lone_matching_forward_passes_through_and_synthesises_nothing) {
    DecisionBuffer decisions;
    decisions.push(forwardOf("KeyA", KeyState::Down));
    WorkRing ring;

    const bool native = translateDecisions(
        decisions, KeyCode::fromString("KeyA"), KeyState::Down, ring);

    KGN_CHECK(native);
    KGN_CHECK(ring.empty());
}

KGN_TEST(a_forward_for_a_different_state_is_synthesised_not_passed_through) {
    // The engine turns a duplicate Down on a forwarded key into a Repeat. The
    // OS must see a repeat, not a second press, so this cannot pass through.
    DecisionBuffer decisions;
    decisions.push(forwardOf("KeyA", KeyState::Repeat));
    WorkRing ring;

    const bool native = translateDecisions(
        decisions, KeyCode::fromString("KeyA"), KeyState::Down, ring);

    KGN_CHECK(!native);
    KGN_CHECK_EQ(ring.size(), std::size_t{1});
}

KGN_TEST(a_forward_for_a_different_key_is_synthesised_not_passed_through) {
    DecisionBuffer decisions;
    decisions.push(forwardOf("KeyB", KeyState::Down));
    WorkRing ring;

    const bool native = translateDecisions(
        decisions, KeyCode::fromString("KeyA"), KeyState::Down, ring);

    KGN_CHECK(!native);
    KGN_CHECK_EQ(ring.size(), std::size_t{1});
}

KGN_TEST(the_grace_replay_synthesises_both_halves_in_order) {
    // The physical event is the Up, but a synthetic Down must reach the OS
    // first. Passing the Up through natively would deliver up-then-down and
    // leave the key held forever.
    DecisionBuffer decisions;
    decisions.push(forwardOf("KeyD", KeyState::Down));
    decisions.push(forwardOf("KeyD", KeyState::Up));
    WorkRing ring;

    const bool native = translateDecisions(
        decisions, KeyCode::fromString("KeyD"), KeyState::Up, ring);

    KGN_CHECK(!native);
    WorkItem first{};
    WorkItem second{};
    KGN_CHECK(ring.pop(first));
    KGN_CHECK(ring.pop(second));
    KGN_CHECK(first.kind == WorkItem::Kind::SendKey);
    KGN_CHECK(first.down);
    KGN_CHECK(second.kind == WorkItem::Kind::SendKey);
    KGN_CHECK(!second.down);
    KGN_CHECK(ring.empty());
}

KGN_TEST(a_matching_forward_is_not_native_when_anything_follows_it) {
    // The "exactly one decision" half of the rule, pinned directly.
    //
    // Passing the event through natively while a later decision still had to
    // be synthesised would deliver them in the wrong order: the OS gets the
    // physical event the instant the hook returns, while the work item waits
    // for the next core tick. Today no engine path produces this shape, which
    // is precisely why it needs a test -- an engine change that introduced one
    // would otherwise silently reorder input.
    DecisionBuffer decisions;
    decisions.push(forwardOf("KeyA", KeyState::Down));
    decisions.push(forwardOf("KeyB", KeyState::Down));
    WorkRing ring;

    const bool native = translateDecisions(
        decisions, KeyCode::fromString("KeyA"), KeyState::Down, ring);

    KGN_CHECK(!native);
    KGN_CHECK_EQ(ring.size(), std::size_t{2});
}

KGN_TEST(the_timer_path_never_passes_anything_through) {
    // No physical event is in hand, so nothing can be satisfied natively even
    // when the decision looks like an ordinary forward.
    DecisionBuffer decisions;
    decisions.push(forwardOf("KeyD", KeyState::Down));
    WorkRing ring;

    const bool native = translateDecisions(decisions, KeyCode{}, KeyState::Down, ring);

    KGN_CHECK(!native);
    KGN_CHECK_EQ(ring.size(), std::size_t{1});
}

KGN_TEST(suppress_and_buffer_decisions_cost_no_capacity) {
    // This is what the release-capacity proof rests on. If either of these
    // spends a slot, an Up for a key with no obligation spends reserve while
    // discharging nothing, and no bound exists.
    DecisionBuffer decisions;
    decisions.push(suppressOf("KeyQ", KeyState::Up));
    decisions.push({Decision::Kind::Buffer, KeyCode::fromString("KeyW"),
                    KeyState::Down});
    WorkRing ring;

    translateDecisions(decisions, KeyCode::fromString("KeyQ"), KeyState::Up, ring);

    KGN_CHECK(ring.empty());
}

KGN_TEST(actions_become_work_items) {
    DecisionBuffer decisions;
    decisions.push(suppressOf("CapsLock", KeyState::Down));
    decisions.push(runActionOf("KeyJ"));
    WorkRing ring;

    const bool native = translateDecisions(
        decisions, KeyCode::fromString("CapsLock"), KeyState::Down, ring);

    KGN_CHECK(!native);
    WorkItem item{};
    KGN_CHECK(ring.pop(item));
    KGN_CHECK(item.kind == WorkItem::Kind::RunAction);
    KGN_CHECK_EQ(item.code, KeyCode::fromString("KeyJ").id());
    KGN_CHECK(ring.empty());
}

KGN_TEST(a_release_action_becomes_a_work_item) {
    DecisionBuffer decisions;
    decisions.push({Decision::Kind::ReleaseAction, KeyCode::fromString("KeyJ"),
                    KeyState::Up});
    WorkRing ring;

    translateDecisions(decisions, KeyCode::fromString("KeyJ"), KeyState::Up, ring);

    WorkItem item{};
    KGN_CHECK(ring.pop(item));
    KGN_CHECK(item.kind == WorkItem::Kind::ReleaseAction);
}

// ---------------------------------------------------------------------------
// The ring

KGN_TEST(the_ring_reports_its_free_capacity_and_refuses_when_full) {
    SpscRing<WorkItem, 4> ring;
    KGN_CHECK_EQ(ring.free(), std::size_t{3});
    KGN_CHECK(ring.push({}));
    KGN_CHECK(ring.push({}));
    KGN_CHECK(ring.push({}));
    KGN_CHECK_EQ(ring.free(), std::size_t{0});
    KGN_CHECK(!ring.push({}));

    WorkItem out{};
    KGN_CHECK(ring.pop(out));
    KGN_CHECK_EQ(ring.free(), std::size_t{1});
    KGN_CHECK(ring.push({}));
}

KGN_TEST(the_ring_preserves_order_across_wraparound) {
    // Ordering is not a nicety: synthetic key events must reach SendInput in
    // emission order or a press can follow its own release.
    SpscRing<WorkItem, 4> ring;
    WorkItem out{};
    for (std::uint16_t i = 0; i < 30; ++i) {
        KGN_CHECK(ring.push({WorkItem::Kind::SendKey, true, i}));
        KGN_CHECK(ring.pop(out));
        KGN_CHECK_EQ(out.code, i);
    }
}

// ---------------------------------------------------------------------------
// Published state

KGN_TEST(the_published_state_round_trips_and_versions_monotonically) {
    StatePublisher publisher;
    KGN_CHECK_EQ(publisher.version(), std::uint32_t{0});

    PublishedState state;
    state.mode = Mode::Cursor;
    state.latched = true;
    state.activation = ActivationMode::Toggle;
    state.shift = true;
    publisher.publish(state);

    KGN_CHECK_EQ(publisher.version(), std::uint32_t{1});
    KGN_CHECK(publisher.state().mode == Mode::Cursor);
    KGN_CHECK(publisher.state().latched);
    KGN_CHECK(publisher.state().activation == ActivationMode::Toggle);
    KGN_CHECK(publisher.state().shift);
    KGN_CHECK(!publisher.state().control);

    // Republishing the same state must not churn the version, or the core
    // emits a `mode` event on every tick for the life of the process.
    publisher.publish(state);
    KGN_CHECK_EQ(publisher.version(), std::uint32_t{1});

    state.control = true;
    publisher.publish(state);
    KGN_CHECK_EQ(publisher.version(), std::uint32_t{2});
}

KGN_TEST(every_modifier_survives_the_packing) {
    PublishedState state;
    state.shift = true;
    state.control = true;
    state.alt = true;
    state.meta = true;
    state.mode = Mode::Cursor;
    state.latched = true;
    state.activation = ActivationMode::Hold;

    const PublishedState back = PublishedState::unpack(state.pack());
    KGN_CHECK(back.shift && back.control && back.alt && back.meta);
    KGN_CHECK(back.mode == Mode::Cursor);
    KGN_CHECK(back.latched);
    KGN_CHECK(back.activation == ActivationMode::Hold);
}

// ---------------------------------------------------------------------------
// The capacity proof, made executable

KGN_TEST(the_whole_release_drain_fits_inside_the_reserved_capacity) {
    // Drive the engine to its worst case -- as many held actions as it can
    // hold -- then admit nothing further and drain it entirely through Up
    // events, counting the work items produced.
    //
    // The theorem says that total is at most kMaxReleaseWork. If it ever
    // exceeds it, the admission gate is too small and a release can be lost,
    // which is P7's failure.
    EngineConfig config;
    config.activation = ActivationMode::Hold;
    LayerEngine engine(config);

    BindingMap bindings;
    std::vector<KeyCode> actionKeys;
    for (int i = 0; i < 300; ++i) {
        const KeyCode code = KeyCode::fromString("CapTest" + std::to_string(i));
        bindings[code] = BindingKind::Action;
        actionKeys.push_back(code);
    }
    engine.setBindings(bindings);

    WorkRing ring;
    DecisionBuffer buffer;
    auto feed = [&](KeyCode code, KeyState state, TimePoint now) {
        buffer.clear();
        engine.onKey(code, state, now, buffer);
        translateDecisions(buffer, code, state, ring);
    };

    const KeyCode caps = KeyCode::fromString("CapsLock");
    feed(caps, KeyState::Down, at(0));
    for (std::size_t i = 0; i < actionKeys.size(); ++i) {
        feed(actionKeys[i], KeyState::Down, at(1));
    }
    // Discard what filling produced; the release is what is being measured.
    drain(ring);
    KGN_CHECK_EQ(engine.heldActions().size(), kMaxHeld);

    // From here nothing new is admitted. Releasing CapsLock in hold mode
    // unwinds every held action in one call -- the largest single Up the
    // engine has, and the case the earlier "an Up emits at most two decisions"
    // reasoning missed entirely.
    feed(caps, KeyState::Up, at(2));
    for (std::size_t i = 0; i < actionKeys.size(); ++i) {
        feed(actionKeys[i], KeyState::Up, at(3));
    }

    const std::size_t produced = drain(ring);
    KGN_CHECK(produced <= kMaxReleaseWork);
    KGN_CHECK(produced > 0);   // the generator must not be degenerate
}

KGN_TEST(a_capslock_release_alone_can_emit_a_work_item_per_held_action) {
    // The specific case the reserve has to cover: one Up, many obligations.
    EngineConfig config;
    config.activation = ActivationMode::Hold;
    LayerEngine engine(config);

    BindingMap bindings;
    std::vector<KeyCode> actionKeys;
    for (int i = 0; i < 40; ++i) {
        const KeyCode code = KeyCode::fromString("CapsUnwind" + std::to_string(i));
        bindings[code] = BindingKind::Action;
        actionKeys.push_back(code);
    }
    engine.setBindings(bindings);

    WorkRing ring;
    DecisionBuffer buffer;
    const KeyCode caps = KeyCode::fromString("CapsLock");

    buffer.clear();
    engine.onKey(caps, KeyState::Down, at(0), buffer);
    translateDecisions(buffer, caps, KeyState::Down, ring);
    for (KeyCode code : actionKeys) {
        buffer.clear();
        engine.onKey(code, KeyState::Down, at(1), buffer);
        translateDecisions(buffer, code, KeyState::Down, ring);
    }
    drain(ring);

    buffer.clear();
    engine.onKey(caps, KeyState::Up, at(2), buffer);
    translateDecisions(buffer, caps, KeyState::Up, ring);

    KGN_CHECK_EQ(ring.size(), actionKeys.size());
    KGN_CHECK(actionKeys.size() <= kMaxReleaseWork);
}

KGN_TEST(an_up_for_a_key_with_no_obligation_costs_no_capacity) {
    // The row that makes the bound possible. A user can produce these without
    // limit -- an orphan release, or the release of a press the admission gate
    // refused -- so if each one spent a slot the reserve could be exhausted by
    // ordinary use and a real release would then be lost.
    LayerEngine engine;
    engine.setBindings({});

    WorkRing ring;
    DecisionBuffer buffer;
    const KeyCode code = KeyCode::fromString("KeyZ");
    for (int i = 0; i < 4000; ++i) {
        buffer.clear();
        engine.onKey(code, KeyState::Up, at(i), buffer);
        translateDecisions(buffer, code, KeyState::Up, ring);
    }
    KGN_CHECK(ring.empty());
}

KGN_TEST(ordinary_typing_never_synthesises_and_never_fills_the_ring) {
    // The common path must stay native, or every keystroke in the machine
    // becomes an injected event with a tick of latency.
    LayerEngine engine;
    engine.setBindings({});

    WorkRing ring;
    DecisionBuffer buffer;
    const KeyCode code = KeyCode::fromString("KeyS");
    for (int i = 0; i < 2000; ++i) {
        buffer.clear();
        engine.onKey(code, KeyState::Down, at(i * 2), buffer);
        KGN_CHECK(translateDecisions(buffer, code, KeyState::Down, ring));
        buffer.clear();
        engine.onKey(code, KeyState::Up, at(i * 2 + 1), buffer);
        KGN_CHECK(translateDecisions(buffer, code, KeyState::Up, ring));
    }
    KGN_CHECK(ring.empty());
}

KGN_TEST(the_admission_gate_leaves_room_for_the_whole_release_drain) {
    // The arithmetic the proof turns on, asserted rather than assumed.
    KGN_CHECK_EQ(kWorkAdmissionGate, kDecisionCapacity + kMaxReleaseWork);
    KGN_CHECK(kWorkRingCapacity >= kWorkAdmissionGate);
    KGN_CHECK_EQ(kWorkRingCapacity - kWorkAdmissionGate + kMaxReleaseWork,
                 kWorkRingCapacity - kDecisionCapacity);
}

int main() { return kgn::test::runAll(); }

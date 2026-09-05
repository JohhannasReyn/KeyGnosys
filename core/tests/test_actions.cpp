// The action catalog and the dispatcher.
//
// Two things are being proven here. First, that a bindings document maps onto
// the catalog exactly, and that a binding which does not is refused rather
// than half-accepted. Second -- and this is the one that matters -- that the
// dispatcher never leaves an obligation half-created: no button goes down
// without a way to bring it back up, and every exit path brings everything up.

#include <string>
#include <unordered_map>

#include "kgn/actions.hpp"
#include "kgn_test.hpp"

using kgn::Action;
using kgn::ActionId;
using kgn::Corner;
using kgn::Cycle;
using kgn::Decision;
using kgn::Direction;
using kgn::Dispatcher;
using kgn::Effect;
using kgn::EffectBuffer;
using kgn::Json;
using kgn::KeyCode;
using kgn::KeyState;
using kgn::MonitorTarget;
using kgn::MotionSettings;
using kgn::MouseButton;
using kgn::TimePoint;

namespace {

TimePoint t0() { return TimePoint{} + std::chrono::seconds(1000); }
TimePoint at(long long ms) { return t0() + std::chrono::milliseconds(ms); }

KeyCode key(const char* name) { return KeyCode::fromString(name); }

Json params(std::string_view text) {
    Json value;
    KGN_CHECK(Json::parse(text, value));
    return value;
}

Action parseOk(std::string_view name, std::string_view paramText) {
    Action action;
    std::string error;
    KGN_CHECK(kgn::parseAction(name, params(paramText), action, error));
    return action;
}

bool parseFails(std::string_view name, std::string_view paramText) {
    Action action;
    std::string error;
    const bool ok = kgn::parseAction(name, params(paramText), action, error);
    if (!ok) KGN_CHECK(!error.empty());
    return !ok;
}

Action move(Direction dir) {
    Action action;
    action.id = ActionId::PointerMove;
    action.dir = dir;
    return action;
}

Action click(MouseButton button) {
    Action action;
    action.id = ActionId::ButtonClick;
    action.button = button;
    return action;
}

Action simple(ActionId id) {
    Action action;
    action.id = id;
    return action;
}

Action dragLock(MouseButton button) {
    Action action;
    action.id = ActionId::ButtonDragLock;
    action.button = button;
    return action;
}

// A flat, fast integrator so displacement assertions are exact.
MotionSettings flat(double speed) {
    MotionSettings settings;
    settings.baseSpeed = speed;
    settings.maxSpeed = speed;
    settings.rampMs = std::chrono::milliseconds(0);
    settings.precisionFactor = 0.5;
    return settings;
}

Decision run(KeyCode code) {
    return Decision{Decision::Kind::RunAction, code, KeyState::Down};
}

Decision stop(KeyCode code) {
    return Decision{Decision::Kind::ReleaseAction, code, KeyState::Up};
}

// Count the Button effects for one button, and report the last state seen.
int buttonEdges(const EffectBuffer& effects, MouseButton button, bool& lastDown) {
    int count = 0;
    for (const auto& effect : effects) {
        if (effect.kind != Effect::Kind::Button || effect.button != button) continue;
        ++count;
        lastDown = effect.down;
    }
    return count;
}

}  // namespace

// ---------------------------------------------------------------------------
// Catalog

KGN_TEST(every_catalog_name_round_trips) {
    const ActionId ids[] = {
        ActionId::PointerMove,        ActionId::PointerPrecision,
        ActionId::ButtonClick,        ActionId::ButtonDoubleClick,
        ActionId::ButtonDragLock,     ActionId::ScrollScroll,
        ActionId::ScrollPage,         ActionId::WarpGrid,
        ActionId::WarpCorner,         ActionId::WarpMonitor,
        ActionId::WindowCycle,        ActionId::WindowSlot,
        ActionId::WindowFocusMonitor, ActionId::WindowMoveToMonitor,
        ActionId::LayerRelease,       ActionId::OverlayToggle,
        ActionId::SystemReload,       ActionId::KeyPassthrough,
    };
    for (const ActionId id : ids) {
        const std::string_view name = kgn::actionName(id);
        KGN_CHECK(!name.empty());
        KGN_CHECK(kgn::actionFromName(name) == id);
    }
    KGN_CHECK(kgn::actionFromName("nope") == ActionId::Unknown);
    KGN_CHECK(kgn::actionName(ActionId::Unknown).empty());
}

KGN_TEST(only_key_passthrough_reaches_the_os_from_inside_the_layer) {
    // The escape hatch requires an explicit binding, and nothing else may
    // classify as passthrough (SPEC section 6.3).
    KGN_CHECK(kgn::bindingKindFor(ActionId::KeyPassthrough) ==
              kgn::BindingKind::Passthrough);
    KGN_CHECK(kgn::bindingKindFor(ActionId::PointerMove) == kgn::BindingKind::Action);
    KGN_CHECK(kgn::bindingKindFor(ActionId::LayerRelease) == kgn::BindingKind::Action);
    KGN_CHECK(kgn::bindingKindFor(ActionId::Unknown) == kgn::BindingKind::Action);
}

KGN_TEST(parses_every_parameterised_action) {
    KGN_CHECK(parseOk("pointer.move", R"({"dir":"left"})").dir == Direction::Left);
    KGN_CHECK(parseOk("scroll.scroll", R"({"dir":"up"})").dir == Direction::Up);
    KGN_CHECK(parseOk("scroll.page", R"({"dir":"down"})").dir == Direction::Down);
    KGN_CHECK(parseOk("button.click", R"({"button":"middle"})").button ==
              MouseButton::Middle);
    KGN_CHECK(parseOk("button.drag_lock", R"({"button":"right"})").button ==
              MouseButton::Right);
    KGN_CHECK_EQ(parseOk("warp.grid", R"({"cell":7})").index, 7);
    KGN_CHECK(parseOk("warp.corner", R"({"corner":"br"})").corner ==
              Corner::BottomRight);
    KGN_CHECK(parseOk("warp.monitor", R"({"target":"prev"})").target.kind ==
              MonitorTarget::Kind::Prev);
    KGN_CHECK_EQ(parseOk("window.move_to_monitor", R"({"target":2})").target.index, 2);
    KGN_CHECK(parseOk("window.cycle", R"({"dir":"prev"})").cycle == Cycle::Prev);
    KGN_CHECK_EQ(parseOk("window.slot", R"({"index":9})").index, 9);
    KGN_CHECK(parseOk("key.passthrough", R"({"code":"Escape"})").code ==
              key("Escape"));
}

KGN_TEST(parameterless_actions_ignore_whatever_params_they_are_given) {
    KGN_CHECK(parseOk("pointer.precision", "{}").id == ActionId::PointerPrecision);
    KGN_CHECK(parseOk("layer.release", R"({"stray":1})").id == ActionId::LayerRelease);
    KGN_CHECK(parseOk("overlay.toggle", "{}").id == ActionId::OverlayToggle);
    KGN_CHECK(parseOk("system.reload", "{}").id == ActionId::SystemReload);
}

KGN_TEST(an_invalid_parameter_set_fails_the_binding_not_the_document) {
    KGN_CHECK(parseFails("pointer.move", "{}"));
    KGN_CHECK(parseFails("pointer.move", R"({"dir":"sideways"})"));
    KGN_CHECK(parseFails("pointer.move", R"({"dir":3})"));
    KGN_CHECK(parseFails("button.click", R"({"button":"fourth"})"));
    KGN_CHECK(parseFails("warp.grid", R"({"cell":0})"));
    KGN_CHECK(parseFails("warp.grid", R"({"cell":10})"));
    KGN_CHECK(parseFails("window.slot", R"({"index":0})"));
    KGN_CHECK(parseFails("warp.corner", R"({"corner":"middle"})"));
    KGN_CHECK(parseFails("warp.monitor", R"({"target":"elsewhere"})"));
    KGN_CHECK(parseFails("warp.monitor", R"({"target":-1})"));
    KGN_CHECK(parseFails("window.cycle", R"({"dir":"up"})"));
    KGN_CHECK(parseFails("key.passthrough", "{}"));
    KGN_CHECK(parseFails("key.passthrough", R"({"code":""})"));
    KGN_CHECK(parseFails("nonsense.action", "{}"));
}

KGN_TEST(scroll_page_admits_only_the_vertical_directions) {
    // A horizontal page would be a different action, not a different parameter.
    KGN_CHECK(parseFails("scroll.page", R"({"dir":"left"})"));
    KGN_CHECK(parseFails("scroll.page", R"({"dir":"right"})"));
}

// ---------------------------------------------------------------------------
// Dispatch mapping

KGN_TEST(a_held_direction_drives_the_pointer_integrator) {
    Dispatcher dispatcher;
    dispatcher.setPointerSettings(flat(10.0));
    EffectBuffer effects;
    dispatcher.setBindings({{key("KeyL"), move(Direction::Right)}}, effects);

    effects.clear();
    dispatcher.onDecision(run(key("KeyL")), t0(), effects);
    KGN_CHECK(effects.empty());   // motion is a tick result, not an effect
    KGN_CHECK_EQ(dispatcher.tick(at(17)).pointer.x, 10);

    dispatcher.onDecision(stop(key("KeyL")), at(50), effects);
    KGN_CHECK(dispatcher.tick(at(67)).zero());
}

KGN_TEST(two_keys_on_one_direction_are_refcounted) {
    Dispatcher dispatcher;
    dispatcher.setPointerSettings(flat(10.0));
    EffectBuffer effects;
    dispatcher.setBindings(
        {{key("KeyL"), move(Direction::Right)}, {key("ArrowRight"), move(Direction::Right)}},
        effects);

    dispatcher.onDecision(run(key("KeyL")), t0(), effects);
    dispatcher.onDecision(run(key("ArrowRight")), t0(), effects);
    KGN_CHECK_EQ(dispatcher.tick(at(17)).pointer.x, 10);

    // Releasing one must not stop what the other still holds.
    dispatcher.onDecision(stop(key("KeyL")), at(20), effects);
    KGN_CHECK_EQ(dispatcher.tick(at(34)).pointer.x, 10);

    dispatcher.onDecision(stop(key("ArrowRight")), at(40), effects);
    KGN_CHECK(dispatcher.tick(at(51)).zero());
}

KGN_TEST(precision_slows_pointer_and_scroll_alike) {
    Dispatcher dispatcher;
    dispatcher.setPointerSettings(flat(10.0));
    dispatcher.setScrollSettings(flat(4.0));
    EffectBuffer effects;
    dispatcher.setBindings({{key("KeyF"), simple(ActionId::PointerPrecision)},
                            {key("KeyL"), move(Direction::Right)},
                            {key("KeyO"), [] {
                                 Action a;
                                 a.id = ActionId::ScrollScroll;
                                 a.dir = Direction::Right;
                                 return a;
                             }()}},
                           effects);

    dispatcher.onDecision(run(key("KeyL")), t0(), effects);
    dispatcher.onDecision(run(key("KeyO")), t0(), effects);
    KGN_CHECK_EQ(dispatcher.tick(at(17)).pointer.x, 10);

    dispatcher.onDecision(run(key("KeyF")), at(20), effects);
    const auto slowed = dispatcher.tick(at(34));
    KGN_CHECK_EQ(slowed.pointer.x, 5);
    KGN_CHECK_EQ(slowed.scroll.x, 2);

    dispatcher.onDecision(stop(key("KeyF")), at(40), effects);
    KGN_CHECK_EQ(dispatcher.tick(at(51)).pointer.x, 10);
}

KGN_TEST(a_key_repeat_does_not_re_trigger_an_action) {
    Dispatcher dispatcher;
    EffectBuffer effects;
    dispatcher.setBindings({{key("KeyD"), click(MouseButton::Left)}}, effects);

    effects.clear();
    dispatcher.onDecision(run(key("KeyD")), t0(), effects);
    dispatcher.onDecision(
        Decision{Decision::Kind::RunAction, key("KeyD"), KeyState::Repeat}, at(30),
        effects);
    bool down = false;
    KGN_CHECK_EQ(buttonEdges(effects, MouseButton::Left, down), 1);
    KGN_CHECK(down);
    KGN_CHECK_EQ(dispatcher.heldCount(), std::size_t{1});
}

KGN_TEST(one_shot_actions_produce_one_effect_each) {
    Dispatcher dispatcher;
    EffectBuffer effects;
    dispatcher.setBindings({{key("KeyX"), [] {
                                 Action a;
                                 a.id = ActionId::ButtonDoubleClick;
                                 a.button = MouseButton::Left;
                                 return a;
                             }()},
                            {key("KeyP"), [] {
                                 Action a;
                                 a.id = ActionId::ScrollPage;
                                 a.dir = Direction::Up;
                                 return a;
                             }()},
                            {key("Numpad7"), [] {
                                 Action a;
                                 a.id = ActionId::WarpGrid;
                                 a.index = 7;
                                 return a;
                             }()},
                            {key("Digit1"), [] {
                                 Action a;
                                 a.id = ActionId::WindowSlot;
                                 a.index = 1;
                                 return a;
                             }()},
                            {key("Escape"), simple(ActionId::LayerRelease)},
                            {key("KeyM"), simple(ActionId::OverlayToggle)},
                            {key("KeyR"), simple(ActionId::SystemReload)}},
                           effects);

    const std::pair<const char*, Effect::Kind> expected[] = {
        {"KeyX", Effect::Kind::DoubleClick},
        {"KeyP", Effect::Kind::Scroll},
        {"Numpad7", Effect::Kind::Warp},
        {"Digit1", Effect::Kind::Window},
        {"Escape", Effect::Kind::LayerRelease},
        {"KeyM", Effect::Kind::OverlayToggle},
        {"KeyR", Effect::Kind::ReloadConfig},
    };
    for (const auto& [name, kind] : expected) {
        effects.clear();
        dispatcher.onDecision(run(key(name)), t0(), effects);
        KGN_CHECK_EQ(effects.size(), std::size_t{1});
        KGN_CHECK(effects[0].kind == kind);
    }
    // A one-shot creates no obligation, so nothing is left held.
    KGN_CHECK_EQ(dispatcher.heldCount(), std::size_t{0});
}

KGN_TEST(page_scroll_carries_a_signed_notch_count) {
    Dispatcher dispatcher;
    EffectBuffer effects;
    Action up;
    up.id = ActionId::ScrollPage;
    up.dir = Direction::Up;
    Action down = up;
    down.dir = Direction::Down;
    dispatcher.setBindings({{key("KeyP"), up}, {key("Semicolon"), down}}, effects);

    effects.clear();
    dispatcher.onDecision(run(key("KeyP")), t0(), effects);
    dispatcher.onDecision(run(key("Semicolon")), t0(), effects);
    KGN_CHECK_EQ(effects.size(), std::size_t{2});
    KGN_CHECK_EQ(effects[0].dy, kgn::kPageScrollNotches);
    KGN_CHECK_EQ(effects[1].dy, -kgn::kPageScrollNotches);
}

KGN_TEST(an_unbound_key_is_counted_rather_than_acted_on) {
    Dispatcher dispatcher;
    EffectBuffer effects;
    dispatcher.setBindings({}, effects);
    effects.clear();
    dispatcher.onDecision(run(key("KeyZ")), t0(), effects);
    KGN_CHECK(effects.empty());
    KGN_CHECK_EQ(dispatcher.unknownActions(), std::uint64_t{1});
}

KGN_TEST(a_release_for_something_never_started_is_counted_not_acted_on) {
    Dispatcher dispatcher;
    EffectBuffer effects;
    dispatcher.setBindings({{key("KeyD"), click(MouseButton::Left)}}, effects);
    effects.clear();
    dispatcher.onDecision(stop(key("KeyD")), t0(), effects);
    KGN_CHECK(effects.empty());
    KGN_CHECK_EQ(dispatcher.unknownActions(), std::uint64_t{1});
    KGN_CHECK(!dispatcher.buttonDown(MouseButton::Left));
}

// ---------------------------------------------------------------------------
// Press/release symmetry

KGN_TEST(a_click_goes_down_on_press_and_up_on_release) {
    Dispatcher dispatcher;
    EffectBuffer effects;
    dispatcher.setBindings({{key("KeyD"), click(MouseButton::Left)}}, effects);

    effects.clear();
    dispatcher.onDecision(run(key("KeyD")), t0(), effects);
    KGN_CHECK_EQ(effects.size(), std::size_t{1});
    KGN_CHECK(effects[0].kind == Effect::Kind::Button);
    KGN_CHECK(effects[0].down);
    KGN_CHECK(dispatcher.buttonDown(MouseButton::Left));

    effects.clear();
    dispatcher.onDecision(stop(key("KeyD")), at(30), effects);
    KGN_CHECK_EQ(effects.size(), std::size_t{1});
    KGN_CHECK(!effects[0].down);
    KGN_CHECK(!dispatcher.buttonDown(MouseButton::Left));
}

KGN_TEST(two_keys_on_one_button_never_double_press_or_lose_the_release) {
    // The shipped defaults bind left-click to both KeyD and Space.
    Dispatcher dispatcher;
    EffectBuffer effects;
    dispatcher.setBindings(
        {{key("KeyD"), click(MouseButton::Left)}, {key("Space"), click(MouseButton::Left)}},
        effects);

    effects.clear();
    dispatcher.onDecision(run(key("KeyD")), t0(), effects);
    dispatcher.onDecision(run(key("Space")), at(10), effects);
    bool down = false;
    KGN_CHECK_EQ(buttonEdges(effects, MouseButton::Left, down), 1);
    KGN_CHECK(down);

    effects.clear();
    dispatcher.onDecision(stop(key("KeyD")), at(20), effects);
    KGN_CHECK(effects.empty());                       // still held by Space
    KGN_CHECK(dispatcher.buttonDown(MouseButton::Left));

    dispatcher.onDecision(stop(key("Space")), at(30), effects);
    KGN_CHECK_EQ(buttonEdges(effects, MouseButton::Left, down), 1);
    KGN_CHECK(!down);
    KGN_CHECK(!dispatcher.buttonDown(MouseButton::Left));
}

KGN_TEST(the_three_buttons_are_independent) {
    Dispatcher dispatcher;
    EffectBuffer effects;
    dispatcher.setBindings({{key("KeyD"), click(MouseButton::Left)},
                            {key("KeyS"), click(MouseButton::Right)},
                            {key("KeyA"), click(MouseButton::Middle)}},
                           effects);
    dispatcher.onDecision(run(key("KeyD")), t0(), effects);
    dispatcher.onDecision(run(key("KeyS")), t0(), effects);
    KGN_CHECK(dispatcher.buttonDown(MouseButton::Left));
    KGN_CHECK(dispatcher.buttonDown(MouseButton::Right));
    KGN_CHECK(!dispatcher.buttonDown(MouseButton::Middle));

    dispatcher.onDecision(stop(key("KeyD")), at(10), effects);
    KGN_CHECK(!dispatcher.buttonDown(MouseButton::Left));
    KGN_CHECK(dispatcher.buttonDown(MouseButton::Right));
}

// ---------------------------------------------------------------------------
// Drag lock

KGN_TEST(drag_lock_toggles_and_reports_before_the_button_moves) {
    Dispatcher dispatcher;
    EffectBuffer effects;
    dispatcher.setBindings({{key("KeyG"), dragLock(MouseButton::Left)}}, effects);

    effects.clear();
    dispatcher.onDecision(run(key("KeyG")), t0(), effects);
    KGN_CHECK_EQ(effects.size(), std::size_t{2});
    // A client must never see a button go down for a drag it has not been told
    // about, so the lock notification comes first.
    KGN_CHECK(effects[0].kind == Effect::Kind::DragLock);
    KGN_CHECK(effects[0].down);
    KGN_CHECK(effects[1].kind == Effect::Kind::Button);
    KGN_CHECK(effects[1].down);
    KGN_CHECK(dispatcher.dragLockActive(MouseButton::Left));

    // The key coming up does nothing: a lock is a toggle, not a hold.
    effects.clear();
    dispatcher.onDecision(stop(key("KeyG")), at(30), effects);
    KGN_CHECK(effects.empty());
    KGN_CHECK(dispatcher.buttonDown(MouseButton::Left));

    effects.clear();
    dispatcher.onDecision(run(key("KeyG")), at(60), effects);
    KGN_CHECK_EQ(effects.size(), std::size_t{2});
    KGN_CHECK(!effects[0].down);
    KGN_CHECK(!effects[1].down);
    KGN_CHECK(!dispatcher.dragLockActive(MouseButton::Left));
    KGN_CHECK(!dispatcher.buttonDown(MouseButton::Left));
}

KGN_TEST(a_click_over_a_drag_lock_does_not_release_the_drag) {
    // The button is down because of the lock; a click on the same button that
    // comes and goes must not lift it.
    Dispatcher dispatcher;
    EffectBuffer effects;
    dispatcher.setBindings(
        {{key("KeyG"), dragLock(MouseButton::Left)}, {key("KeyD"), click(MouseButton::Left)}},
        effects);

    dispatcher.onDecision(run(key("KeyG")), t0(), effects);
    effects.clear();
    dispatcher.onDecision(run(key("KeyD")), at(10), effects);
    KGN_CHECK(effects.empty());                   // already down; no second press
    dispatcher.onDecision(stop(key("KeyD")), at(20), effects);
    KGN_CHECK(effects.empty());                   // the lock still holds it
    KGN_CHECK(dispatcher.buttonDown(MouseButton::Left));
    KGN_CHECK(dispatcher.dragLockActive(MouseButton::Left));
}

// ---------------------------------------------------------------------------
// Fail-safe and P7

KGN_TEST(release_all_lifts_every_obligation) {
    Dispatcher dispatcher;
    dispatcher.setPointerSettings(flat(10.0));
    EffectBuffer effects;
    dispatcher.setBindings({{key("KeyL"), move(Direction::Right)},
                            {key("KeyF"), simple(ActionId::PointerPrecision)},
                            {key("KeyD"), click(MouseButton::Left)},
                            {key("KeyG"), dragLock(MouseButton::Right)}},
                           effects);

    dispatcher.onDecision(run(key("KeyL")), t0(), effects);
    dispatcher.onDecision(run(key("KeyF")), t0(), effects);
    dispatcher.onDecision(run(key("KeyD")), t0(), effects);
    dispatcher.onDecision(run(key("KeyG")), t0(), effects);
    KGN_CHECK(dispatcher.buttonDown(MouseButton::Left));
    KGN_CHECK(dispatcher.buttonDown(MouseButton::Right));

    effects.clear();
    dispatcher.releaseAll(effects);

    bool leftDown = true;
    bool rightDown = true;
    KGN_CHECK_EQ(buttonEdges(effects, MouseButton::Left, leftDown), 1);
    KGN_CHECK_EQ(buttonEdges(effects, MouseButton::Right, rightDown), 1);
    KGN_CHECK(!leftDown);
    KGN_CHECK(!rightDown);

    bool sawDragRelease = false;
    for (const auto& effect : effects) {
        if (effect.kind == Effect::Kind::DragLock && effect.button == MouseButton::Right) {
            sawDragRelease = !effect.down;
        }
    }
    KGN_CHECK(sawDragRelease);

    KGN_CHECK(!dispatcher.buttonDown(MouseButton::Left));
    KGN_CHECK(!dispatcher.buttonDown(MouseButton::Right));
    KGN_CHECK(!dispatcher.dragLockActive(MouseButton::Right));
    KGN_CHECK_EQ(dispatcher.heldCount(), std::size_t{0});
    KGN_CHECK(!dispatcher.pointer().moving());
    KGN_CHECK(!dispatcher.pointer().precision());
    KGN_CHECK(dispatcher.tick(at(100)).zero());
}

KGN_TEST(leaving_the_layer_lifts_the_drag_lock_but_not_the_integrators) {
    // The narrow release the layer-exit path uses. Leaving the layer is not
    // the panic path: the held actions come back as ReleaseAction decisions of
    // their own, and reset()ing the integrators here would discard a sub-pixel
    // remainder the user never asked to lose. Only the toggle is stranded by
    // an exit, because only the toggle outlives the key that set it.
    Dispatcher dispatcher;
    dispatcher.setPointerSettings(flat(10.0));
    EffectBuffer effects;
    dispatcher.setBindings({{key("KeyL"), move(Direction::Right)},
                            {key("KeyG"), dragLock(MouseButton::Left)}},
                           effects);

    dispatcher.onDecision(run(key("KeyL")), t0(), effects);
    dispatcher.onDecision(run(key("KeyG")), t0(), effects);
    KGN_CHECK(dispatcher.buttonDown(MouseButton::Left));

    effects.clear();
    dispatcher.releaseDragLocks(effects);

    bool down = true;
    KGN_CHECK_EQ(buttonEdges(effects, MouseButton::Left, down), 1);
    KGN_CHECK(!down);
    KGN_CHECK(!dispatcher.dragLockActive(MouseButton::Left));
    KGN_CHECK(!dispatcher.buttonDown(MouseButton::Left));
    // Untouched: the engine, not this, owns the held action's release.
    KGN_CHECK_EQ(dispatcher.heldCount(), std::size_t{1});
    KGN_CHECK(dispatcher.pointer().moving());
}

KGN_TEST(a_click_still_holding_a_button_survives_the_layer_exit) {
    // The refcount rule, on the exit path. Clearing the lock must not lift a
    // button a click is still holding down -- that would be a release with no
    // press behind it, P7's mirror.
    Dispatcher dispatcher;
    EffectBuffer effects;
    dispatcher.setBindings({{key("KeyD"), click(MouseButton::Left)},
                            {key("KeyG"), dragLock(MouseButton::Left)}},
                           effects);

    dispatcher.onDecision(run(key("KeyD")), t0(), effects);
    dispatcher.onDecision(run(key("KeyG")), t0(), effects);

    effects.clear();
    dispatcher.releaseDragLocks(effects);

    bool down = true;
    KGN_CHECK_EQ(buttonEdges(effects, MouseButton::Left, down), 0);
    KGN_CHECK(dispatcher.buttonDown(MouseButton::Left));
    KGN_CHECK(!dispatcher.dragLockActive(MouseButton::Left));
}

KGN_TEST(release_all_is_idempotent) {
    Dispatcher dispatcher;
    EffectBuffer effects;
    dispatcher.setBindings({{key("KeyD"), click(MouseButton::Left)}}, effects);
    dispatcher.onDecision(run(key("KeyD")), t0(), effects);
    dispatcher.releaseAll(effects);
    effects.clear();
    dispatcher.releaseAll(effects);
    KGN_CHECK(effects.empty());
}

KGN_TEST(changing_the_bindings_releases_what_the_old_ones_held) {
    // A binding that has gone cannot be asked to release itself afterwards, so
    // the release has to happen before the table changes (P7).
    Dispatcher dispatcher;
    EffectBuffer effects;
    dispatcher.setBindings({{key("KeyD"), click(MouseButton::Left)}}, effects);
    dispatcher.onDecision(run(key("KeyD")), t0(), effects);
    KGN_CHECK(dispatcher.buttonDown(MouseButton::Left));

    effects.clear();
    dispatcher.setBindings({{key("KeyS"), click(MouseButton::Right)}}, effects);
    bool down = true;
    KGN_CHECK_EQ(buttonEdges(effects, MouseButton::Left, down), 1);
    KGN_CHECK(!down);
    KGN_CHECK(!dispatcher.buttonDown(MouseButton::Left));
    KGN_CHECK_EQ(dispatcher.heldCount(), std::size_t{0});
}

KGN_TEST(an_action_whose_obligation_cannot_be_recorded_is_not_started) {
    // Fill the held list, then ask for one more. The refused action must emit
    // nothing at all -- a button down with no way back up is strictly worse
    // than an action that visibly did nothing.
    Dispatcher dispatcher;
    EffectBuffer effects;
    std::unordered_map<KeyCode, Action> bindings;
    for (std::size_t i = 0; i < kgn::kMaxHeld; ++i) {
        bindings[KeyCode(static_cast<std::uint16_t>(4000 + i))] = move(Direction::Right);
    }
    bindings[key("KeyD")] = click(MouseButton::Left);
    dispatcher.setBindings(bindings, effects);

    for (std::size_t i = 0; i < kgn::kMaxHeld; ++i) {
        dispatcher.onDecision(run(KeyCode(static_cast<std::uint16_t>(4000 + i))), t0(),
                              effects);
    }
    KGN_CHECK_EQ(dispatcher.heldCount(), kgn::kMaxHeld);

    effects.clear();
    dispatcher.onDecision(run(key("KeyD")), t0(), effects);
    KGN_CHECK(effects.empty());
    KGN_CHECK(!dispatcher.buttonDown(MouseButton::Left));
    KGN_CHECK_EQ(dispatcher.capacityDrops(), std::uint64_t{1});

    // And the refusal leaves nothing to unwind for that key.
    effects.clear();
    dispatcher.onDecision(stop(key("KeyD")), t0(), effects);
    KGN_CHECK(effects.empty());
}

KGN_TEST(held_actions_unwind_in_press_order) {
    Dispatcher dispatcher;
    EffectBuffer effects;
    dispatcher.setBindings({{key("KeyD"), click(MouseButton::Left)},
                            {key("KeyS"), click(MouseButton::Right)},
                            {key("KeyA"), click(MouseButton::Middle)}},
                           effects);
    dispatcher.onDecision(run(key("KeyA")), t0(), effects);
    dispatcher.onDecision(run(key("KeyD")), at(10), effects);
    dispatcher.onDecision(run(key("KeyS")), at(20), effects);

    effects.clear();
    dispatcher.releaseAll(effects);
    // Middle pressed first, so middle comes up first.
    KGN_CHECK(effects.size() >= 3);
    KGN_CHECK(effects[0].button == MouseButton::Middle);
    KGN_CHECK(effects[1].button == MouseButton::Left);
    KGN_CHECK(effects[2].button == MouseButton::Right);
}

KGN_TEST(a_decision_that_is_not_a_dispatch_is_ignored) {
    Dispatcher dispatcher;
    EffectBuffer effects;
    dispatcher.setBindings({{key("KeyD"), click(MouseButton::Left)}}, effects);
    effects.clear();
    for (const auto kind : {Decision::Kind::Suppress, Decision::Kind::Forward,
                            Decision::Kind::Buffer}) {
        dispatcher.onDecision(Decision{kind, key("KeyD"), KeyState::Down}, t0(),
                              effects);
    }
    KGN_CHECK(effects.empty());
    KGN_CHECK(!dispatcher.buttonDown(MouseButton::Left));
    KGN_CHECK_EQ(dispatcher.unknownActions(), std::uint64_t{0});
}

// ---------------------------------------------------------------------------
// button.double_click scheduling
//
// Manual matrix row 5.3 failed because the second pair was scheduled at exactly
// the OS interval. That value is the largest gap the OS accepts, and the 60 Hz
// service loop then added up to a tick on top, so the pair landed outside the
// window and Windows saw two single clicks. The rule these tests enforce is the
// one the old code broke: delay + one tick of jitter must stay strictly inside
// the interval.

namespace {

constexpr std::chrono::milliseconds jitter() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(kgn::kTickInterval)
           + std::chrono::milliseconds{1};
}

}  // namespace

KGN_TEST(a_double_click_is_scheduled_inside_the_os_interval_not_at_it) {
    // The exact value measured on the machine where row 5.3 failed.
    const auto interval = std::chrono::milliseconds{410};
    const auto delay = kgn::doubleClickDelay(interval);
    KGN_CHECK(delay < interval);
    KGN_CHECK(delay + jitter() < interval);   // survives a late loop tick
}

KGN_TEST(the_windows_default_interval_leaves_ample_headroom) {
    const auto interval = std::chrono::milliseconds{500};
    const auto delay = kgn::doubleClickDelay(interval);
    KGN_CHECK(delay + jitter() < interval);
    KGN_CHECK(delay == kgn::kDoubleClickDelayCap);   // capped, not a quarter
}

KGN_TEST(the_delay_stays_inside_the_window_across_every_plausible_setting) {
    // Windows exposes roughly 200..900 ms through the mouse control panel.
    for (int ms = 100; ms <= 1200; ++ms) {
        const auto interval = std::chrono::milliseconds{ms};
        const auto delay = kgn::doubleClickDelay(interval);
        KGN_CHECK(delay + jitter() < interval);
        KGN_CHECK(delay > std::chrono::milliseconds{0});
    }
}

KGN_TEST(a_generous_interval_does_not_make_the_gesture_sluggish) {
    // A quarter of 900 ms would be 225 ms, which is a long time to wait for a
    // click the user already made. The cap exists for this.
    const auto delay = kgn::doubleClickDelay(std::chrono::milliseconds{900});
    KGN_CHECK(delay <= kgn::kDoubleClickDelayCap);
}

KGN_TEST(a_tight_interval_scales_down_rather_than_clamping_to_the_cap) {
    const auto delay = kgn::doubleClickDelay(std::chrono::milliseconds{200});
    KGN_CHECK(delay == std::chrono::milliseconds{50});   // a quarter, under the cap
}

KGN_TEST(an_interval_below_one_tick_still_returns_something_usable) {
    // Nothing can both wait and land inside a sub-tick window on a 60 Hz loop.
    // Degrading to half the interval is honest; returning the interval itself
    // would be the original bug in miniature.
    const auto interval = std::chrono::milliseconds{10};
    const auto delay = kgn::doubleClickDelay(interval);
    KGN_CHECK(delay < interval);
    KGN_CHECK(delay > std::chrono::milliseconds{0});
}

KGN_TEST(a_backend_that_reports_no_interval_does_not_schedule_at_zero) {
    const auto delay = kgn::doubleClickDelay(std::chrono::milliseconds{0});
    KGN_CHECK(delay > std::chrono::milliseconds{0});
}

KGN_TEST(the_delay_is_never_the_interval_itself) {
    // The regression, stated directly: this is what the code used to do.
    for (int ms : {100, 200, 410, 500, 900}) {
        const auto interval = std::chrono::milliseconds{ms};
        KGN_CHECK(kgn::doubleClickDelay(interval) != interval);
    }
}

int main() { return kgn::test::runAll(); }

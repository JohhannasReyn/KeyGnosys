#include "kgn/actions.hpp"

#include <algorithm>

namespace kgn {
namespace {

struct CatalogEntry {
    ActionId id;
    std::string_view name;
};

// The catalog, in SPEC section 7 order.
constexpr CatalogEntry kCatalog[] = {
    {ActionId::PointerMove,         "pointer.move"},
    {ActionId::PointerPrecision,    "pointer.precision"},
    {ActionId::ButtonClick,         "button.click"},
    {ActionId::ButtonDoubleClick,   "button.double_click"},
    {ActionId::ButtonDragLock,      "button.drag_lock"},
    {ActionId::ScrollScroll,        "scroll.scroll"},
    {ActionId::ScrollPage,          "scroll.page"},
    {ActionId::WarpGrid,            "warp.grid"},
    {ActionId::WarpCorner,          "warp.corner"},
    {ActionId::WarpMonitor,         "warp.monitor"},
    {ActionId::WindowCycle,         "window.cycle"},
    {ActionId::WindowSlot,          "window.slot"},
    {ActionId::WindowFocusMonitor,  "window.focus_monitor"},
    {ActionId::WindowMoveToMonitor, "window.move_to_monitor"},
    {ActionId::LayerRelease,        "layer.release"},
    {ActionId::OverlayToggle,       "overlay.toggle"},
    {ActionId::SystemReload,        "system.reload"},
    {ActionId::KeyPassthrough,      "key.passthrough"},
};

bool parseDirection(const Json& value, Direction& out) {
    if (!value.isString()) return false;
    const std::string& text = value.asString();
    if (text == "up") { out = Direction::Up; return true; }
    if (text == "down") { out = Direction::Down; return true; }
    if (text == "left") { out = Direction::Left; return true; }
    if (text == "right") { out = Direction::Right; return true; }
    return false;
}

bool parseButton(const Json& value, MouseButton& out) {
    if (!value.isString()) return false;
    const std::string& text = value.asString();
    if (text == "left") { out = MouseButton::Left; return true; }
    if (text == "right") { out = MouseButton::Right; return true; }
    if (text == "middle") { out = MouseButton::Middle; return true; }
    return false;
}

bool parseCorner(const Json& value, Corner& out) {
    if (!value.isString()) return false;
    const std::string& text = value.asString();
    if (text == "tl") { out = Corner::TopLeft; return true; }
    if (text == "tr") { out = Corner::TopRight; return true; }
    if (text == "bl") { out = Corner::BottomLeft; return true; }
    if (text == "br") { out = Corner::BottomRight; return true; }
    if (text == "center") { out = Corner::Center; return true; }
    return false;
}

bool parseTarget(const Json& value, MonitorTarget& out) {
    if (value.isString()) {
        const std::string& text = value.asString();
        if (text == "next") { out = {MonitorTarget::Kind::Next, 0}; return true; }
        if (text == "prev") { out = {MonitorTarget::Kind::Prev, 0}; return true; }
        return false;
    }
    if (value.isNumber()) {
        const std::int64_t index = value.asInt(-1);
        if (index < 0 || index > 63) return false;
        out = {MonitorTarget::Kind::Index, static_cast<int>(index)};
        return true;
    }
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// Catalog

std::string_view actionName(ActionId id) {
    for (const auto& entry : kCatalog) {
        if (entry.id == id) return entry.name;
    }
    return {};
}

ActionId actionFromName(std::string_view name) {
    for (const auto& entry : kCatalog) {
        if (entry.name == name) return entry.id;
    }
    return ActionId::Unknown;
}

BindingKind bindingKindFor(ActionId id) {
    // Exactly one action reaches the OS from inside the layer, and it does so
    // only because it was explicitly bound (SPEC section 6.3).
    return id == ActionId::KeyPassthrough ? BindingKind::Passthrough
                                          : BindingKind::Action;
}

bool isHeldAction(ActionId id) {
    switch (id) {
        case ActionId::PointerMove:
        case ActionId::PointerPrecision:
        case ActionId::ButtonClick:
        case ActionId::ScrollScroll:
            return true;
        default:
            return false;
    }
}

bool parseAction(std::string_view name, const Json& params, Action& out,
                 std::string& error) {
    const ActionId id = actionFromName(name);
    if (id == ActionId::Unknown) {
        error = "unknown action '";
        error.append(name);
        error += "'";
        return false;
    }

    Action action;
    action.id = id;

    const auto missing = [&error](const char* param) {
        error = "missing or invalid '";
        error += param;
        error += "'";
        return false;
    };

    switch (id) {
        case ActionId::PointerMove:
        case ActionId::ScrollScroll:
            if (!parseDirection(params["dir"], action.dir)) return missing("dir");
            break;

        case ActionId::ScrollPage:
            if (!parseDirection(params["dir"], action.dir)) return missing("dir");
            // The catalog admits only vertical paging; a horizontal one would
            // be a different action, not a different parameter.
            if (action.dir != Direction::Up && action.dir != Direction::Down) {
                error = "'dir' must be 'up' or 'down' for scroll.page";
                return false;
            }
            break;

        case ActionId::ButtonClick:
        case ActionId::ButtonDoubleClick:
        case ActionId::ButtonDragLock:
            if (!parseButton(params["button"], action.button)) {
                return missing("button");
            }
            break;

        case ActionId::WarpGrid:
        case ActionId::WindowSlot: {
            const Json& value = params[id == ActionId::WarpGrid ? "cell" : "index"];
            const std::int64_t index = value.isNumber() ? value.asInt(0) : 0;
            if (index < 1 || index > 9) {
                return missing(id == ActionId::WarpGrid ? "cell" : "index");
            }
            action.index = static_cast<int>(index);
            break;
        }

        case ActionId::WarpCorner:
            if (!parseCorner(params["corner"], action.corner)) {
                return missing("corner");
            }
            break;

        case ActionId::WarpMonitor:
        case ActionId::WindowFocusMonitor:
        case ActionId::WindowMoveToMonitor:
            if (!parseTarget(params["target"], action.target)) {
                return missing("target");
            }
            break;

        case ActionId::WindowCycle: {
            const Json& value = params["dir"];
            if (!value.isString()) return missing("dir");
            if (value.asString() == "next") {
                action.cycle = Cycle::Next;
            } else if (value.asString() == "prev") {
                action.cycle = Cycle::Prev;
            } else {
                return missing("dir");
            }
            break;
        }

        case ActionId::KeyPassthrough: {
            const Json& value = params["code"];
            if (!value.isString() || value.asString().empty()) {
                return missing("code");
            }
            action.code = KeyCode::fromString(value.asString());
            if (!action.code.valid()) return missing("code");
            break;
        }

        case ActionId::PointerPrecision:
        case ActionId::LayerRelease:
        case ActionId::OverlayToggle:
        case ActionId::SystemReload:
            break;   // no parameters

        case ActionId::Unknown:
            return false;
    }

    out = action;
    error.clear();
    return true;
}

// ---------------------------------------------------------------------------
// Dispatcher

Dispatcher::Dispatcher() = default;

void Dispatcher::setPointerSettings(const MotionSettings& settings) {
    pointer_.setSettings(settings);
}

void Dispatcher::setScrollSettings(const MotionSettings& settings) {
    // Precision applies to pointer and scroll alike, so the scroll integrator
    // carries the same factor rather than one of its own.
    scroll_.setSettings(settings);
}

void Dispatcher::setBindings(const std::unordered_map<KeyCode, Action>& bindings,
                             EffectBuffer& out) {
    // Everything held is released before the table changes. A binding that has
    // disappeared cannot be asked to release itself afterwards, so releasing
    // first is the only ordering that cannot strand an obligation (P7).
    releaseAll(out);
    bindings_ = bindings;
}

bool Dispatcher::dragLockActive(MouseButton button) const {
    return dragLock_[buttonIndex(button)];
}

bool Dispatcher::buttonDown(MouseButton button) const {
    return buttonDown_[buttonIndex(button)];
}

void Dispatcher::syncButton(MouseButton button, EffectBuffer& out) {
    const std::size_t i = buttonIndex(button);
    const bool wanted = clickHolders_[i] > 0 || dragLock_[i];
    if (wanted == buttonDown_[i]) return;
    buttonDown_[i] = wanted;
    Effect effect;
    effect.kind = Effect::Kind::Button;
    effect.button = button;
    effect.down = wanted;
    out.push(effect);
}

bool Dispatcher::beginHeld(KeyCode code, const Action& action) {
    if (heldCount_ >= kMaxHeld) {
        ++capacityDrops_;
        return false;
    }
    held_[heldCount_++] = Held{code, action};
    return true;
}

bool Dispatcher::endHeld(KeyCode code, Action& action) {
    for (std::size_t i = 0; i < heldCount_; ++i) {
        if (held_[i].code != code) continue;
        action = held_[i].action;
        // Preserve press order for the rest, so an unwind is reproducible.
        for (std::size_t j = i + 1; j < heldCount_; ++j) held_[j - 1] = held_[j];
        --heldCount_;
        return true;
    }
    return false;
}

void Dispatcher::startAction(const Action& action, EffectBuffer& out,
                             TimePoint now) {
    switch (action.id) {
        case ActionId::PointerMove: {
            const auto i = static_cast<std::size_t>(action.dir);
            if (++directionHolders_[i] == 1) pointer_.press(action.dir, now);
            break;
        }
        case ActionId::ScrollScroll: {
            const auto i = static_cast<std::size_t>(action.dir);
            if (++scrollHolders_[i] == 1) scroll_.press(action.dir, now);
            break;
        }
        case ActionId::PointerPrecision:
            if (++precisionHolders_ == 1) {
                pointer_.setPrecision(true);
                scroll_.setPrecision(true);
            }
            break;
        case ActionId::ButtonClick:
            ++clickHolders_[buttonIndex(action.button)];
            syncButton(action.button, out);
            break;
        default:
            break;   // not a held action
    }
}

void Dispatcher::stopAction(const Action& action, EffectBuffer& out) {
    switch (action.id) {
        case ActionId::PointerMove: {
            const auto i = static_cast<std::size_t>(action.dir);
            if (directionHolders_[i] > 0 && --directionHolders_[i] == 0) {
                pointer_.release(action.dir);
            }
            break;
        }
        case ActionId::ScrollScroll: {
            const auto i = static_cast<std::size_t>(action.dir);
            if (scrollHolders_[i] > 0 && --scrollHolders_[i] == 0) {
                scroll_.release(action.dir);
            }
            break;
        }
        case ActionId::PointerPrecision:
            if (precisionHolders_ > 0 && --precisionHolders_ == 0) {
                pointer_.setPrecision(false);
                scroll_.setPrecision(false);
            }
            break;
        case ActionId::ButtonClick: {
            const std::size_t i = buttonIndex(action.button);
            if (clickHolders_[i] > 0) --clickHolders_[i];
            syncButton(action.button, out);
            break;
        }
        default:
            break;
    }
}

void Dispatcher::onDecision(const Decision& decision, TimePoint now,
                            EffectBuffer& out) {
    if (decision.kind == Decision::Kind::ReleaseAction) {
        Action action;
        if (!endHeld(decision.code, action)) {
            // A release for something never started. Not an error the user can
            // see -- it happens when the press itself was refused -- but worth
            // counting so a systematic mismatch would be visible.
            ++unknownActions_;
            return;
        }
        stopAction(action, out);
        return;
    }

    if (decision.kind != Decision::Kind::RunAction) return;
    if (decision.state == KeyState::Repeat) return;   // holding is not re-triggering

    const auto found = bindings_.find(decision.code);
    if (found == bindings_.end()) {
        ++unknownActions_;
        return;
    }
    const Action& action = found->second;

    if (isHeldAction(action.id)) {
        // Record the obligation BEFORE emitting anything that creates it. If
        // it cannot be recorded, nothing is emitted at all -- a button that
        // went down with no way to bring it up is the failure this ordering
        // exists to prevent.
        if (!beginHeld(decision.code, action)) return;
        startAction(action, out, now);
        return;
    }

    // One-shot actions.
    Effect effect;
    effect.action = action;
    switch (action.id) {
        case ActionId::ButtonDoubleClick:
            effect.kind = Effect::Kind::DoubleClick;
            effect.button = action.button;
            out.push(effect);
            break;

        case ActionId::ButtonDragLock: {
            const std::size_t i = buttonIndex(action.button);
            dragLock_[i] = !dragLock_[i];
            effect.kind = Effect::Kind::DragLock;
            effect.button = action.button;
            effect.down = dragLock_[i];
            out.push(effect);
            // Order matters: the lock state change is reported first, then the
            // button follows it, so a client never sees a button go down for a
            // drag it has not been told about.
            syncButton(action.button, out);
            break;
        }

        case ActionId::ScrollPage:
            effect.kind = Effect::Kind::Scroll;
            effect.dy = action.dir == Direction::Up ? kPageScrollNotches
                                                    : -kPageScrollNotches;
            out.push(effect);
            break;

        case ActionId::WarpGrid:
        case ActionId::WarpCorner:
        case ActionId::WarpMonitor:
            effect.kind = Effect::Kind::Warp;
            out.push(effect);
            break;

        case ActionId::WindowCycle:
        case ActionId::WindowSlot:
        case ActionId::WindowFocusMonitor:
        case ActionId::WindowMoveToMonitor:
            effect.kind = Effect::Kind::Window;
            out.push(effect);
            break;

        case ActionId::LayerRelease:
            effect.kind = Effect::Kind::LayerRelease;
            out.push(effect);
            break;

        case ActionId::OverlayToggle:
            effect.kind = Effect::Kind::OverlayToggle;
            out.push(effect);
            break;

        case ActionId::SystemReload:
            effect.kind = Effect::Kind::ReloadConfig;
            out.push(effect);
            break;

        case ActionId::KeyPassthrough:
            // Never reaches here: the engine classifies it as Passthrough and
            // forwards it rather than emitting RunAction. Counted, not acted
            // on, so a classification bug would show up rather than silently
            // sending a key twice.
            ++unknownActions_;
            break;

        default:
            ++unknownActions_;
            break;
    }
}

TickResult Dispatcher::tick(TimePoint now) {
    TickResult result;
    result.pointer = pointer_.tick(now);
    result.scroll = scroll_.tick(now);
    return result;
}

void Dispatcher::releaseDragLocks(EffectBuffer& out) {
    // Drag locks are not held actions -- they are toggles that outlive the key
    // that set them -- so they need releasing explicitly. This is the P7 clause
    // in SPEC section 7.2: a drag lock MUST auto-release on layer exit.
    //
    // One copy, called from both the exit path and the panic path. The version
    // that lived only inside releaseAll() was the whole defect: layer exit
    // never reached it, so the button stayed down until some later key
    // happened to toggle the lock back off.
    for (std::size_t i = 0; i < kButtonCount; ++i) {
        if (!dragLock_[i]) continue;
        const auto button = static_cast<MouseButton>(i);
        dragLock_[i] = false;
        Effect effect;
        effect.kind = Effect::Kind::DragLock;
        effect.button = button;
        effect.down = false;
        out.push(effect);
        // Same order as setting the lock: the state change is reported first,
        // then the button follows it. A click still holding this button keeps
        // it down, and syncButton is what knows that.
        syncButton(button, out);
    }
}

void Dispatcher::releaseAll(EffectBuffer& out) {
    // Held actions first, in press order, so the unwind is reproducible.
    for (std::size_t i = 0; i < heldCount_; ++i) {
        stopAction(held_[i].action, out);
    }
    heldCount_ = 0;

    releaseDragLocks(out);

    // Anything still counted as holding a button is stale by definition now.
    for (std::size_t i = 0; i < kButtonCount; ++i) {
        clickHolders_[i] = 0;
        syncButton(static_cast<MouseButton>(i), out);
    }

    directionHolders_.fill(0);
    scrollHolders_.fill(0);
    precisionHolders_ = 0;

    // reset(), not releaseDirections(): this is the panic path, and a
    // sub-pixel remainder carried across it would be a surprise rather than a
    // continuation.
    pointer_.reset();
    scroll_.reset();
}

}  // namespace kgn

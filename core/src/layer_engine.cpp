#include "mtk/layer_engine.hpp"

namespace mtk {
namespace {

Decision suppress(KeyCode code, KeyState state) {
    return {Decision::Kind::Suppress, code, state};
}

Decision forward(KeyCode code, KeyState state) {
    return {Decision::Kind::Forward, code, state};
}

Decision runAction(KeyCode code) {
    return {Decision::Kind::RunAction, code, KeyState::Down};
}

Decision releaseAction(KeyCode code) {
    return {Decision::Kind::ReleaseAction, code, KeyState::Up};
}

Decision bufferPress(KeyCode code) {
    return {Decision::Kind::Buffer, code, KeyState::Down};
}

std::chrono::milliseconds since(TimePoint then, TimePoint now) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(now - then);
}

std::vector<Decision> collect(const DecisionBuffer& buffer) {
    return std::vector<Decision>(buffer.begin(), buffer.end());
}

constexpr const char* kModifierNames[] = {
    "ShiftLeft", "ShiftRight", "ControlLeft", "ControlRight",
    "AltLeft", "AltRight", "MetaLeft", "MetaRight",
};

}  // namespace

LayerEngine::LayerEngine(EngineConfig config) : config_(config) {
    capsLock_ = KeyCode::fromString("CapsLock");
    shiftLeft_ = KeyCode::fromString("ShiftLeft");
    shiftRight_ = KeyCode::fromString("ShiftRight");

    // Precomputed so the event path never has to ask what a key is. Resolving
    // this per event would take the intern table's lock and build a string.
    for (const char* name : kModifierNames) {
        const KeyCode code = KeyCode::fromString(name);
        if (trackable(code)) slot(code).isModifier = true;
    }
}

void LayerEngine::setConfig(const EngineConfig& config) { config_ = config; }

// ---------------------------------------------------------------------------
// Bindings
// ---------------------------------------------------------------------------

void LayerEngine::setBindings(const BindingMap& bindings, DecisionBuffer& out) {
    // Resolve outstanding state against the OLD classification first, so that
    // nothing is left half-applied. Held actions whose binding is going away
    // must be released, and buffered presses must not sit waiting on a grace
    // window whose premise no longer holds.
    for (std::size_t i = heldCount_; i > 0; --i) {
        const KeyCode code = heldOrder_[i - 1];
        const auto it = bindings.find(code);
        const bool stillAction =
            it != bindings.end() && it->second == BindingKind::Action;
        if (!stillAction) {
            out.push(releaseAction(code));
            removeHeldAction(code);
        }
    }

    // Buffered presses never reached the OS. Dropping them is right: replaying
    // them as keystrokes would type characters the user never committed to,
    // and their eventual physical release is suppressed because no press was
    // ever forwarded -- which is the mirror invariant working as intended.
    for (std::size_t i = 0; i < pendingCount_; ++i) {
        if (trackable(pending_[i].code)) slot(pending_[i].code).pending = false;
    }
    pendingCount_ = 0;

    // Now swap the classification wholesale. Clearing and repopulating leaves
    // no window in which a key could be passthrough but not bound: nothing
    // observes the engine between these two loops.
    for (Slot& s : slots_) {
        s.bound = false;
        s.kind = BindingKind::Action;
    }
    for (const auto& [code, kind] : bindings) {
        if (!trackable(code)) continue;   // can never be bound; see header
        slot(code).bound = true;
        slot(code).kind = kind;
    }
}

// ---------------------------------------------------------------------------
// P7: every forwarded press is recorded so its release is guaranteed to follow
// ---------------------------------------------------------------------------

void LayerEngine::forwardPress(KeyCode code, KeyState state,
                               DecisionBuffer& out) {
    if (trackable(code) && !slot(code).forwarded) {
        slot(code).forwarded = true;
        if (forwardedCount_ < kMaxHeld) {
            forwardedOrder_[forwardedCount_++] = code;
        }
    }
    out.push(forward(code, state));
}

void LayerEngine::forgetForwarded(KeyCode code) {
    if (!trackable(code) || !slot(code).forwarded) return;
    slot(code).forwarded = false;
    for (std::size_t i = 0; i < forwardedCount_; ++i) {
        if (forwardedOrder_[i] == code) {
            for (std::size_t j = i + 1; j < forwardedCount_; ++j) {
                forwardedOrder_[j - 1] = forwardedOrder_[j];
            }
            --forwardedCount_;
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// Ordered held-action and pending lists
// ---------------------------------------------------------------------------

void LayerEngine::addHeldAction(KeyCode code) {
    if (!trackable(code) || slot(code).heldAction) return;
    if (heldCount_ >= kMaxHeld) return;
    slot(code).heldAction = true;
    heldOrder_[heldCount_++] = code;
}

void LayerEngine::removeHeldAction(KeyCode code) {
    if (!trackable(code) || !slot(code).heldAction) return;
    slot(code).heldAction = false;
    for (std::size_t i = 0; i < heldCount_; ++i) {
        if (heldOrder_[i] == code) {
            for (std::size_t j = i + 1; j < heldCount_; ++j) {
                heldOrder_[j - 1] = heldOrder_[j];
            }
            --heldCount_;
            return;
        }
    }
}

bool LayerEngine::addPending(KeyCode code, TimePoint now) {
    if (!trackable(code) || pendingCount_ >= kMaxPending) return false;
    slot(code).pending = true;
    pending_[pendingCount_++] = Pending{code, now};
    return true;
}

bool LayerEngine::isPending(KeyCode code) const {
    return trackable(code) && slot(code).pending;
}

std::optional<TimePoint> LayerEngine::takePending(KeyCode code) {
    if (!isPending(code)) return std::nullopt;
    slot(code).pending = false;
    for (std::size_t i = 0; i < pendingCount_; ++i) {
        if (pending_[i].code == code) {
            const TimePoint at = pending_[i].pressedAt;
            for (std::size_t j = i + 1; j < pendingCount_; ++j) {
                pending_[j - 1] = pending_[j];
            }
            --pendingCount_;
            return at;
        }
    }
    return std::nullopt;
}

std::vector<KeyCode> LayerEngine::heldActions() const {
    return std::vector<KeyCode>(heldOrder_.data(),
                                heldOrder_.data() + heldCount_);
}

// ---------------------------------------------------------------------------
// Key events
// ---------------------------------------------------------------------------

void LayerEngine::onKey(KeyCode code, KeyState state, TimePoint now,
                        DecisionBuffer& out) {
    if (code == capsLock_) {
        onCapsLock(state, now, out);
        return;
    }

    // A code outside the tracked id space can hold no state, so it takes a
    // stateless path: always forwarded, in both directions. That keeps P7 and
    // its mirror intact -- every press it produces has a release and vice
    // versa -- at the cost of never being bindable. Unreachable from a real
    // backend, whose translation table only emits vocabulary codes.
    if (!trackable(code)) {
        out.push(forward(code, state));
        return;
    }

    Slot& s = slot(code);

    if (s.isModifier) {
        if (state == KeyState::Down) {
            s.modifierHeld = true;
        } else if (state == KeyState::Up) {
            s.modifierHeld = false;
        }
    }

    switch (state) {
        case KeyState::Down: {
            // Already forwarded and not yet released: a duplicate press,
            // which a dropped event or a stuck driver can produce. Forwarded
            // as a REPEAT, not as a second press.
            //
            // A second Down would owe a second Up, and only one physical
            // release is coming -- so the key would be left down forever.
            // Semantically this is what a duplicate Down for a held key
            // already is, so nothing is lost by saying so.
            if (s.forwarded) {
                out.push(forward(code, KeyState::Repeat));
                return;
            }
            // Same for a repeated press of a key already driving an action.
            if (s.heldAction) {
                out.push(suppress(code, state));
                return;
            }
            // And for one already buffered: keep the original press time, so a
            // duplicate cannot extend the grace window indefinitely.
            if (s.pending) {
                out.push(bufferPress(code));
                return;
            }

            if (mode_ == Mode::Cursor) {
                if (s.bound && s.kind == BindingKind::Passthrough) {
                    // The escape hatch. Requires a binding: `bound` and `kind`
                    // come from the same map entry, so this cannot fire for a
                    // key that has no binding at all.
                    forwardPress(code, state, out);
                    return;
                }
                if (s.bound) {
                    addHeldAction(code);
                    out.push(runAction(code));
                    return;
                }
                // Modifiers keep working in the cursor layer, so Ctrl+click
                // and Shift+drag do what the user expects.
                if (s.isModifier) {
                    forwardPress(code, state, out);
                    return;
                }
                // Everything else is swallowed: the layer is a mode, not an
                // overlay on normal typing. The overlay draws these keys blank
                // and dimmed to say so.
                out.push(suppress(code, state));
                return;
            }

            // A passthrough key does the same thing in both modes, so there is
            // nothing for the grace window to disambiguate. Buffering it would
            // cost latency and buy nothing.
            if (!s.bound || s.kind == BindingKind::Passthrough) {
                forwardPress(code, state, out);
                return;
            }

            // Action-bound, but the layer is off: ambiguous. The user may be
            // typing it, or may be a few milliseconds ahead of the CapsLock
            // that was meant to precede it. Hold it and see.
            if (!addPending(code, now)) {
                // More simultaneous buffered keys than a hand can produce.
                // Degrade to no grace window rather than drop the keystroke.
                forwardPress(code, state, out);
                return;
            }
            out.push(bufferPress(code));
            return;
        }

        case KeyState::Up: {
            // P7. Unconditional, whatever the mode has become in the meantime.
            if (s.forwarded) {
                forgetForwarded(code);
                out.push(forward(code, state));
                return;
            }

            if (takePending(code).has_value()) {
                // Released before the grace window resolved: an ordinary quick
                // tap. Send the whole press and release now, in order.
                out.push(forward(code, KeyState::Down));
                out.push(forward(code, KeyState::Up));
                return;
            }

            if (s.heldAction) {
                removeHeldAction(code);
                out.push(releaseAction(code));
                return;
            }

            // Never forwarded, never buffered, never held. Its press did not
            // reach the OS, so neither may its release: forwarding it would be
            // a key-up for a key the OS never saw go down.
            out.push(suppress(code, state));
            return;
        }

        case KeyState::Repeat: {
            if (s.forwarded) {
                out.push(forward(code, state));
                return;
            }
            // Buffered and held keys handle repetition themselves -- the grace
            // sweep resolves one, the motion integrator drives the other.
            out.push(suppress(code, state));
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// CapsLock
// ---------------------------------------------------------------------------

void LayerEngine::onCapsLock(KeyState state, TimePoint now,
                             DecisionBuffer& out) {
    if (state == KeyState::Repeat) {
        out.push(capsForwarded_ ? forward(capsLock_, state)
                                : suppress(capsLock_, state));
        return;
    }

    if (state == KeyState::Down) {
        // The escape gesture: CapsLock while Shift is physically held is a
        // request for real CapsLock, not for the layer.
        const bool shiftHeld =
            (trackable(shiftLeft_) && slot(shiftLeft_).modifierHeld)
            || (trackable(shiftRight_) && slot(shiftRight_).modifierHeld);
        if (config_.shiftCapsIsRealCapsLock && shiftHeld) {
            capsForwarded_ = true;
            forwardPress(capsLock_, state, out);
            return;
        }

        capsPressedAt_ = now;
        out.push(suppress(capsLock_, state));

        switch (config_.activation) {
            case ActivationMode::Toggle:
                if (mode_ == Mode::Cursor) {
                    leaveCursorMode(out);
                } else {
                    mode_ = Mode::Cursor;
                    latched_ = true;
                    promoteBuffered(now, out);
                }
                return;
            case ActivationMode::Hold:
            case ActivationMode::Hybrid:
                // Both engage on press. They differ only in what release does.
                capsWasLatched_ = latched_;
                mode_ = Mode::Cursor;
                latched_ = false;
                promoteBuffered(now, out);
                return;
        }
        return;
    }

    // Release.
    if (capsForwarded_) {
        capsForwarded_ = false;
        forgetForwarded(capsLock_);
        out.push(forward(capsLock_, state));
        return;
    }

    out.push(suppress(capsLock_, state));

    switch (config_.activation) {
        case ActivationMode::Toggle:
            return;                       // release means nothing
        case ActivationMode::Hold:
            leaveCursorMode(out);
            return;
        case ActivationMode::Hybrid: {
            const bool tapped =
                capsPressedAt_.has_value()
                && since(*capsPressedAt_, now) < config_.hybridTap;
            if (tapped && !capsWasLatched_) {
                // A tap from normal mode latches the layer on.
                latched_ = true;
            } else {
                // Either a hold, which was momentary, or a tap while already
                // latched, which is how the user turns the layer back off.
                leaveCursorMode(out);
            }
            capsWasLatched_ = false;
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// The grace window
// ---------------------------------------------------------------------------

void LayerEngine::promoteBuffered(TimePoint now, DecisionBuffer& out) {
    // In press order. The keys were pressed in a particular sequence and the
    // actions they become must start in that same sequence.
    const std::size_t count = pendingCount_;
    for (std::size_t i = 0; i < count; ++i) {
        const KeyCode code = pending_[i].code;
        const bool inWindow = since(pending_[i].pressedAt, now) <= config_.grace;
        if (trackable(code)) slot(code).pending = false;

        if (inWindow) {
            // Pressed just before CapsLock registered: the user meant the
            // action, not the character. Nothing reaches the OS.
            addHeldAction(code);
            out.push(runAction(code));
        } else {
            // The window had already lapsed when CapsLock arrived. Treat it as
            // the ordinary keystroke it turned out to be.
            forwardPress(code, KeyState::Down, out);
        }
    }
    pendingCount_ = 0;
}

void LayerEngine::tick(TimePoint now, DecisionBuffer& out) {
    // Entries are in press order, and expiry is monotonic in press time, so
    // walking forwards and compacting preserves that order for both the keys
    // that expire and the ones that stay.
    std::size_t kept = 0;
    for (std::size_t i = 0; i < pendingCount_; ++i) {
        const Pending entry = pending_[i];
        if (since(entry.pressedAt, now) < config_.grace) {
            pending_[kept++] = entry;
            continue;
        }
        // The window lapsed without CapsLock showing up: an ordinary hold.
        if (trackable(entry.code)) slot(entry.code).pending = false;
        forwardPress(entry.code, KeyState::Down, out);
    }
    pendingCount_ = kept;
}

// ---------------------------------------------------------------------------
// Leaving the layer
// ---------------------------------------------------------------------------

void LayerEngine::leaveCursorMode(DecisionBuffer& out) {
    for (std::size_t i = 0; i < heldCount_; ++i) {
        const KeyCode code = heldOrder_[i];
        if (trackable(code)) slot(code).heldAction = false;
        out.push(releaseAction(code));
    }
    heldCount_ = 0;
    mode_ = Mode::Normal;
    latched_ = false;
}

void LayerEngine::releaseAll(DecisionBuffer& out) {
    leaveCursorMode(out);

    // P7, the whole point of it: every press we forwarded gets its release,
    // even on a panic path. In press order, so the sequence is reproducible.
    for (std::size_t i = 0; i < forwardedCount_; ++i) {
        const KeyCode code = forwardedOrder_[i];
        if (trackable(code)) {
            slot(code).forwarded = false;
            slot(code).modifierHeld = false;
        }
        out.push(forward(code, KeyState::Up));
    }
    forwardedCount_ = 0;

    // Buffered presses never reached the OS, so there is nothing to release.
    // Dropping them is right for a panic path: replaying them as keystrokes
    // would type characters the user never committed to.
    for (std::size_t i = 0; i < pendingCount_; ++i) {
        if (trackable(pending_[i].code)) slot(pending_[i].code).pending = false;
    }
    pendingCount_ = 0;

    capsPressedAt_.reset();
    capsForwarded_ = false;
    capsWasLatched_ = false;
}

// ---------------------------------------------------------------------------
// Allocating conveniences
// ---------------------------------------------------------------------------

std::vector<Decision> LayerEngine::setBindings(const BindingMap& bindings) {
    DecisionBuffer buffer;
    setBindings(bindings, buffer);
    return collect(buffer);
}

std::vector<Decision> LayerEngine::onKey(KeyCode code, KeyState state,
                                         TimePoint now) {
    DecisionBuffer buffer;
    onKey(code, state, now, buffer);
    return collect(buffer);
}

std::vector<Decision> LayerEngine::tick(TimePoint now) {
    DecisionBuffer buffer;
    tick(now, buffer);
    return collect(buffer);
}

std::vector<Decision> LayerEngine::releaseAll() {
    DecisionBuffer buffer;
    releaseAll(buffer);
    return collect(buffer);
}

}  // namespace mtk

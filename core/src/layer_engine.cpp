#include "kgn/layer_engine.hpp"

namespace kgn {
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

bool isActionBound(const BindingMap& bindings, KeyCode code) {
    const auto it = bindings.find(code);
    return it != bindings.end() && it->second == BindingKind::Action;
}

}  // namespace

LayerEngine::LayerEngine(EngineConfig config)
    : config_(config), slots_(kKeyIdSpace) {
    capsLock_ = KeyCode::fromString("CapsLock");
    shiftLeft_ = KeyCode::fromString("ShiftLeft");
    shiftRight_ = KeyCode::fromString("ShiftRight");

    // Precomputed so the event path never has to ask what a key is. Resolving
    // this per event would take the intern table's lock and build a string.
    std::size_t i = 0;
    for (const char* name : kModifierNames) {
        const KeyCode code = KeyCode::fromString(name);
        modifierCodes_[i++] = code;
        if (code.valid()) slot(code).set(Slot::kIsModifier);
    }
}

void LayerEngine::setConfig(const EngineConfig& config) { config_ = config; }

// ---------------------------------------------------------------------------
// Bindings
// ---------------------------------------------------------------------------

void LayerEngine::setBindings(const BindingMap& bindings, DecisionBuffer& out) {
    // Held actions, in press order (SPEC 6.3.3). Compacting in place keeps the
    // survivors in order too.
    std::size_t keptHeld = 0;
    for (std::size_t i = 0; i < heldCount_; ++i) {
        const KeyCode code = heldOrder_[i];
        if (isActionBound(bindings, code)) {
            heldOrder_[keptHeld++] = code;
            continue;
        }
        slot(code).clear(Slot::kHeldAction);
        out.push(releaseAction(code));
    }
    heldCount_ = keptHeld;

    // Buffered presses are physical input the user already committed to; they
    // were delayed only to resolve layer intent, so a reload must not discard
    // them. A key that is still action-bound keeps waiting, with its original
    // press time. One that is not becomes the ordinary keystroke it turned out
    // to be, tracked so its release is owed.
    std::size_t keptPending = 0;
    for (std::size_t i = 0; i < pendingCount_; ++i) {
        const Pending entry = pending_[i];
        if (isActionBound(bindings, entry.code)) {
            pending_[keptPending++] = entry;
            continue;
        }
        slot(entry.code).clear(Slot::kPending);
        forwardPress(entry.code, KeyState::Down, out);
    }
    pendingCount_ = keptPending;

    // Swap the classification wholesale. Nothing observes the engine between
    // these two loops, so there is no window in which a key could be
    // passthrough but not bound.
    for (Slot& s : slots_) {
        s.clear(static_cast<std::uint8_t>(Slot::kBound | Slot::kPassthrough));
    }
    for (const auto& [code, kind] : bindings) {
        if (!code.valid()) continue;
        slot(code).set(Slot::kBound);
        if (kind == BindingKind::Passthrough) {
            slot(code).set(Slot::kPassthrough);
        }
    }
}

// ---------------------------------------------------------------------------
// P7: every forwarded press is recorded so its release is guaranteed to follow
// ---------------------------------------------------------------------------

bool LayerEngine::forwardPress(KeyCode code, KeyState state,
                               DecisionBuffer& out, KeyState suppressAs) {
    Slot& s = slot(code);
    if (!s.has(Slot::kForwarded)) {
        if (forwardedCount_ >= kMaxHeld) {
            // The obligation cannot be recorded, so it must not be created.
            // A dropped keystroke is recoverable; a key the OS believes is
            // held forever is not.
            ++capacityDrops_;
            out.push(suppress(code, suppressAs));
            return false;
        }
        s.set(Slot::kForwarded);
        forwardedOrder_[forwardedCount_++] = code;
    }
    out.push(forward(code, state));
    return true;
}

void LayerEngine::forgetForwarded(KeyCode code) {
    Slot& s = slot(code);
    if (!s.has(Slot::kForwarded)) return;
    s.clear(Slot::kForwarded);
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

bool LayerEngine::addHeldAction(KeyCode code) {
    Slot& s = slot(code);
    if (s.has(Slot::kHeldAction)) return true;
    if (heldCount_ >= kMaxHeld) {
        // Same reasoning as forwardPress: an action whose release cannot be
        // guaranteed is P7's failure in a different currency.
        ++capacityDrops_;
        return false;
    }
    s.set(Slot::kHeldAction);
    heldOrder_[heldCount_++] = code;
    return true;
}

void LayerEngine::removeHeldAction(KeyCode code) {
    Slot& s = slot(code);
    if (!s.has(Slot::kHeldAction)) return;
    s.clear(Slot::kHeldAction);
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
    if (pendingCount_ >= kMaxPending) return false;
    slot(code).set(Slot::kPending);
    pending_[pendingCount_++] = Pending{code, now};
    return true;
}

bool LayerEngine::isPending(KeyCode code) const {
    return slot(code).has(Slot::kPending);
}

bool LayerEngine::takePending(KeyCode code) {
    if (!isPending(code)) return false;
    slot(code).clear(Slot::kPending);
    for (std::size_t i = 0; i < pendingCount_; ++i) {
        if (pending_[i].code == code) {
            for (std::size_t j = i + 1; j < pendingCount_; ++j) {
                pending_[j - 1] = pending_[j];
            }
            --pendingCount_;
            return true;
        }
    }
    return false;
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
    // The only code with no state is the invalid one, which no backend can
    // emit. Suppressing it in every direction keeps both invariants trivially
    // true: it never produces a press, so it can never owe a release.
    if (!code.valid()) {
        ++invalidEvents_;
        out.push(suppress(code, state));
        return;
    }

    if (code == capsLock_) {
        onCapsLock(state, now, out);
        return;
    }

    Slot& s = slot(code);

    if (s.has(Slot::kIsModifier)) {
        if (state == KeyState::Down) {
            s.set(Slot::kModifierHeld);
        } else if (state == KeyState::Up) {
            s.clear(Slot::kModifierHeld);
        }
    }

    switch (state) {
        case KeyState::Down: {
            // Already forwarded and not yet released: a duplicate press, which
            // a dropped event or a stuck driver can produce. Forwarded as a
            // REPEAT, not as a second press.
            //
            // A second Down would owe a second Up, and only one physical
            // release is coming -- so the key would be left down forever.
            // Semantically this is what a duplicate Down for a held key
            // already is, so nothing is lost by saying so.
            if (s.has(Slot::kForwarded)) {
                out.push(forward(code, KeyState::Repeat));
                return;
            }
            // Same for a repeated press of a key already driving an action.
            if (s.has(Slot::kHeldAction)) {
                out.push(suppress(code, state));
                return;
            }
            // And for one already buffered: keep the original press time, so a
            // duplicate cannot extend the grace window indefinitely.
            if (s.has(Slot::kPending)) {
                out.push(bufferPress(code));
                return;
            }

            if (mode_ == Mode::Cursor) {
                if (s.has(Slot::kBound) && s.has(Slot::kPassthrough)) {
                    // The escape hatch. Requires a binding: both flags come
                    // from the same map entry, so this cannot fire for a key
                    // that has no binding at all.
                    forwardPress(code, state, out);
                    return;
                }
                if (s.has(Slot::kBound)) {
                    if (addHeldAction(code)) {
                        out.push(runAction(code));
                    } else {
                        out.push(suppress(code, state));
                    }
                    return;
                }
                // Modifiers keep working in the cursor layer, so Ctrl+click
                // and Shift+drag do what the user expects.
                if (s.has(Slot::kIsModifier)) {
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
            if (!s.has(Slot::kBound) || s.has(Slot::kPassthrough)) {
                forwardPress(code, state, out);
                return;
            }

            // Action-bound, but the layer is off: ambiguous. The user may be
            // typing it, or may be a few milliseconds ahead of the CapsLock
            // that was meant to precede it. Hold it and see.
            if (!addPending(code, now)) {
                // More simultaneous buffered keys than a hand can produce.
                // Degrade to no grace window rather than drop the keystroke.
                ++capacityDrops_;
                forwardPress(code, state, out);
                return;
            }
            out.push(bufferPress(code));
            return;
        }

        case KeyState::Up: {
            // P7. Unconditional, whatever the mode has become in the meantime.
            if (s.has(Slot::kForwarded)) {
                forgetForwarded(code);
                out.push(forward(code, state));
                return;
            }

            if (takePending(code)) {
                // Released before the grace window resolved: an ordinary quick
                // tap. Send the whole press and release now, in order -- and
                // only if the press can be tracked, so the pair stays matched.
                //
                // The event being handled is the release, so a suppression
                // here reports Up even though the press was what could not be
                // forwarded.
                if (forwardPress(code, KeyState::Down, out, KeyState::Up)) {
                    forgetForwarded(code);
                    out.push(forward(code, KeyState::Up));
                }
                return;
            }

            if (s.has(Slot::kHeldAction)) {
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
            if (s.has(Slot::kForwarded)) {
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
//
// Physical state is tracked explicitly so that duplicate and orphaned events
// follow the same policy as every other key rather than bypassing it. Without
// it, a duplicate Down toggles the layer twice and an orphan Up can change the
// mode (SPEC 6.3.2).
// ---------------------------------------------------------------------------

void LayerEngine::onCapsLock(KeyState state, TimePoint now,
                             DecisionBuffer& out) {
    if (state == KeyState::Repeat) {
        out.push(capsForwarded_ ? forward(capsLock_, state)
                                : suppress(capsLock_, state));
        return;
    }

    if (state == KeyState::Down) {
        if (capsPhysicallyDown_) {
            // Duplicate press. Same policy as any other key: a real CapsLock
            // repeats, a layer gesture is swallowed. Crucially, neither
            // re-runs the activation logic, so the layer cannot toggle twice.
            out.push(capsForwarded_ ? forward(capsLock_, KeyState::Repeat)
                                    : suppress(capsLock_, KeyState::Down));
            return;
        }
        capsPhysicallyDown_ = true;

        // The escape gesture: CapsLock while Shift is physically held is a
        // request for real CapsLock, not for the layer.
        const bool shiftHeld = slot(shiftLeft_).has(Slot::kModifierHeld)
                               || slot(shiftRight_).has(Slot::kModifierHeld);
        if (config_.shiftCapsIsRealCapsLock && shiftHeld) {
            // Classified as the escape gesture whether or not the forwarding
            // obligation can be recorded. If it cannot, the press is suppressed
            // -- but it stays a gesture, so its release is suppressed too and
            // the layer is left entirely alone.
            capsEscapeGesture_ = true;
            capsForwarded_ = forwardPress(capsLock_, state, out);
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
    if (!capsPhysicallyDown_) {
        // Orphan release: the OS never told us it went down, or releaseAll()
        // has already unwound it. Either way it must not change the layer.
        out.push(suppress(capsLock_, state));
        return;
    }
    capsPhysicallyDown_ = false;

    if (capsEscapeGesture_) {
        // A real CapsLock, released. Never touches the mode or the latch --
        // the press did not engage the layer, so the release cannot leave it.
        capsEscapeGesture_ = false;
        capsPressedAt_.reset();
        if (capsForwarded_) {
            capsForwarded_ = false;
            forgetForwarded(capsLock_);
            out.push(forward(capsLock_, state));
        } else {
            // The press was suppressed for want of capacity, so forwarding
            // this release would be an orphan key-up.
            out.push(suppress(capsLock_, state));
        }
        return;
    }

    out.push(suppress(capsLock_, state));

    const bool tapped = capsPressedAt_.has_value()
                        && since(*capsPressedAt_, now) < config_.hybridTap;
    capsPressedAt_.reset();

    switch (config_.activation) {
        case ActivationMode::Toggle:
            return;                       // release means nothing
        case ActivationMode::Hold:
            leaveCursorMode(out);
            return;
        case ActivationMode::Hybrid:
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

// ---------------------------------------------------------------------------
// The grace window
// ---------------------------------------------------------------------------

void LayerEngine::promoteBuffered(TimePoint now, DecisionBuffer& out) {
    // In press order. The keys were pressed in a particular sequence and the
    // actions they become must start in that same sequence.
    const std::size_t count = pendingCount_;
    pendingCount_ = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const KeyCode code = pending_[i].code;
        const bool inWindow = since(pending_[i].pressedAt, now) <= config_.grace;
        slot(code).clear(Slot::kPending);

        if (inWindow) {
            // Pressed just before CapsLock registered: the user meant the
            // action, not the character. Nothing reaches the OS.
            if (addHeldAction(code)) {
                out.push(runAction(code));
            } else {
                out.push(suppress(code, KeyState::Down));
            }
        } else {
            // The window had already lapsed when CapsLock arrived. Treat it as
            // the ordinary keystroke it turned out to be.
            forwardPress(code, KeyState::Down, out);
        }
    }
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
        slot(entry.code).clear(Slot::kPending);
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
        slot(code).clear(Slot::kHeldAction);
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
        slot(code).clear(Slot::kForwarded);
        out.push(forward(code, KeyState::Up));
    }
    forwardedCount_ = 0;

    // Buffered presses never reached the OS, so there is nothing to release.
    // Dropping them is right for a panic path: replaying them as keystrokes
    // would type characters the user never committed to, and the mirror
    // invariant then suppresses their eventual physical release.
    for (std::size_t i = 0; i < pendingCount_; ++i) {
        slot(pending_[i].code).clear(Slot::kPending);
    }
    pendingCount_ = 0;

    // Physical modifier state must be cleared for every modifier, not only the
    // ones that happened to be forwarded. A modifier bound to an action lives
    // in the held list instead, and leaving its flag set would make the next
    // CapsLock look like the Shift+CapsLock escape gesture.
    for (KeyCode code : modifierCodes_) {
        if (code.valid()) slot(code).clear(Slot::kModifierHeld);
    }

    capsPhysicallyDown_ = false;
    capsPressedAt_.reset();
    capsEscapeGesture_ = false;
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

}  // namespace kgn

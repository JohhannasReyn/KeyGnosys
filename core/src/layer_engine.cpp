#include "mtk/layer_engine.hpp"

#include <algorithm>

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

Decision buffer(KeyCode code) {
    return {Decision::Kind::Buffer, code, KeyState::Down};
}

std::chrono::milliseconds since(TimePoint then, TimePoint now) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(now - then);
}

}  // namespace

LayerEngine::LayerEngine(EngineConfig config) : config_(config) {}

void LayerEngine::setConfig(const EngineConfig& config) { config_ = config; }

void LayerEngine::setBoundKeys(std::unordered_set<KeyCode> bound) {
    bound_ = std::move(bound);
}

void LayerEngine::setPassthroughKeys(std::unordered_set<KeyCode> keys) {
    passthroughKeys_ = std::move(keys);
}

// ---------------------------------------------------------------------------
// P7: every forwarded press is recorded so its release is guaranteed to follow
// ---------------------------------------------------------------------------

void LayerEngine::forwardPress(KeyCode code, KeyState state,
                               std::vector<Decision>& out) {
    forwardedPresses_.insert(code);
    out.push_back(forward(code, state));
}

// ---------------------------------------------------------------------------
// Key events
// ---------------------------------------------------------------------------

void LayerEngine::onKey(KeyCode code, KeyState state, TimePoint now,
                        std::vector<Decision>& out) {
    if (code == KeyCode::fromString("CapsLock")) {
        onCapsLock(state, now, out);
        return;
    }

    const bool isMod = isModifier(code);
    if (isMod) {
        if (state == KeyState::Down) {
            heldModifiers_.insert(code);
        } else if (state == KeyState::Up) {
            heldModifiers_.erase(code);
        }
    }

    switch (state) {
        case KeyState::Down: {
            // A key already flagged as forwarded should not be pressed again
            // without an intervening release, but if the OS says so, staying
            // consistent beats asserting.
            if (forwardedPresses_.count(code)) {
                out.push_back(forward(code, state));
                return;
            }

            if (mode_ == Mode::Cursor) {
                // An explicit `key.passthrough` binding reaches the OS even
                // inside the layer, and is tracked like any other forwarded
                // press so P7 covers it.
                if (passthroughKeys_.count(code)) {
                    forwardPress(code, state, out);
                    return;
                }
                if (bound_.count(code)) {
                    heldActions_.insert(code);
                    out.push_back(runAction(code));
                    return;
                }
                // Modifiers keep working in the cursor layer, so Ctrl+click
                // and Shift+drag do what the user expects.
                if (isMod) {
                    forwardPress(code, state, out);
                    return;
                }
                // Everything else is swallowed: the layer is a mode, not an
                // overlay on normal typing. The overlay draws these keys blank
                // and dimmed to say so, and `key.passthrough` is the escape
                // hatch for a key that must still reach the OS.
                out.push_back(suppress(code, state));
                return;
            }

            // A passthrough key does the same thing in both modes, so there
            // is nothing for the grace window to disambiguate. Buffering it
            // would cost latency and buy nothing.
            if (!bound_.count(code) || passthroughKeys_.count(code)) {
                forwardPress(code, state, out);
                return;
            }

            // Bound, but the layer is off: ambiguous. The user may be typing
            // it, or may be a few milliseconds ahead of the CapsLock that was
            // meant to precede it. Hold it and see.
            pending_[code] = Pending{now};
            out.push_back(buffer(code));
            return;
        }

        case KeyState::Up: {
            // P7. Unconditional, whatever the mode has become in the meantime.
            if (forwardedPresses_.count(code)) {
                forwardedPresses_.erase(code);
                out.push_back(forward(code, state));
                return;
            }

            if (auto it = pending_.find(code); it != pending_.end()) {
                // Released before the grace window resolved: an ordinary quick
                // tap. Send the whole press and release now, in order.
                pending_.erase(it);
                out.push_back(forward(code, KeyState::Down));
                out.push_back(forward(code, KeyState::Up));
                return;
            }

            if (heldActions_.erase(code)) {
                out.push_back(releaseAction(code));
                return;
            }

            // Never forwarded, never buffered, never held: nothing to undo.
            out.push_back(suppress(code, state));
            return;
        }

        case KeyState::Repeat: {
            if (forwardedPresses_.count(code)) {
                out.push_back(forward(code, state));
                return;
            }
            // Buffered and held keys handle repetition themselves -- the grace
            // sweep resolves one, the motion integrator drives the other.
            out.push_back(suppress(code, state));
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// CapsLock
// ---------------------------------------------------------------------------

void LayerEngine::onCapsLock(KeyState state, TimePoint now,
                             std::vector<Decision>& out) {
    const KeyCode caps = KeyCode::fromString("CapsLock");

    if (state == KeyState::Repeat) {
        out.push_back(capsForwarded_ ? forward(caps, state)
                                     : suppress(caps, state));
        return;
    }

    if (state == KeyState::Down) {
        // The escape gesture: CapsLock while Shift is physically held is a
        // request for real CapsLock, not for the layer.
        const bool shiftHeld = std::any_of(
            heldModifiers_.begin(), heldModifiers_.end(),
            [](KeyCode m) { return modifierGroup(m) == "Shift"; });
        if (config_.shiftCapsIsRealCapsLock && shiftHeld) {
            capsForwarded_ = true;
            forwardPress(caps, state, out);
            return;
        }

        capsPressedAt_ = now;
        out.push_back(suppress(caps, state));

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
        forwardedPresses_.erase(caps);
        out.push_back(forward(caps, state));
        return;
    }

    out.push_back(suppress(caps, state));

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

void LayerEngine::promoteBuffered(TimePoint now, std::vector<Decision>& out) {
    for (auto it = pending_.begin(); it != pending_.end();) {
        const KeyCode code = it->first;
        const bool inWindow = since(it->second.pressedAt, now) <= config_.grace;
        it = pending_.erase(it);

        if (inWindow) {
            // Pressed just before CapsLock registered: the user meant the
            // action, not the character. Nothing reaches the OS.
            heldActions_.insert(code);
            out.push_back(runAction(code));
        } else {
            // The window had already lapsed when CapsLock arrived. Treat it as
            // the ordinary keystroke it turned out to be.
            forwardPress(code, KeyState::Down, out);
        }
    }
}

void LayerEngine::tick(TimePoint now, std::vector<Decision>& out) {
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (since(it->second.pressedAt, now) < config_.grace) {
            ++it;
            continue;
        }
        // The window lapsed without CapsLock showing up: an ordinary hold.
        const KeyCode code = it->first;
        it = pending_.erase(it);
        forwardPress(code, KeyState::Down, out);
    }
}

// ---------------------------------------------------------------------------
// Leaving the layer
// ---------------------------------------------------------------------------

void LayerEngine::leaveCursorMode(std::vector<Decision>& out) {
    for (KeyCode code : heldActions_) {
        out.push_back(releaseAction(code));
    }
    heldActions_.clear();
    mode_ = Mode::Normal;
    latched_ = false;
}

void LayerEngine::releaseAll(std::vector<Decision>& out) {
    leaveCursorMode(out);

    // P7, the whole point of it: every press we forwarded gets its release,
    // even on a panic path.
    for (KeyCode code : forwardedPresses_) {
        out.push_back(forward(code, KeyState::Up));
    }
    forwardedPresses_.clear();

    // Buffered presses never reached the OS, so there is nothing to release.
    // Dropping them is right for a panic path: replaying them as keystrokes
    // would type characters the user never committed to.
    pending_.clear();
    heldModifiers_.clear();
    capsPressedAt_.reset();
    capsForwarded_ = false;
    capsWasLatched_ = false;
}

// ---------------------------------------------------------------------------
// Allocating conveniences
// ---------------------------------------------------------------------------

std::vector<Decision> LayerEngine::onKey(KeyCode code, KeyState state,
                                         TimePoint now) {
    std::vector<Decision> out;
    onKey(code, state, now, out);
    return out;
}

std::vector<Decision> LayerEngine::tick(TimePoint now) {
    std::vector<Decision> out;
    tick(now, out);
    return out;
}

std::vector<Decision> LayerEngine::releaseAll() {
    std::vector<Decision> out;
    releaseAll(out);
    return out;
}

}  // namespace mtk

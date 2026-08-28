#include "kgn/core.hpp"

#include <atomic>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace kgn {
namespace {

std::string environment(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
}

std::string joinPath(const std::string& base, const std::string& leaf) {
    if (base.empty()) return leaf;
#if defined(_WIN32)
    const char separator = '\\';
#else
    const char separator = '/';
#endif
    if (base.back() == '/' || base.back() == '\\') return base + leaf;
    return base + separator + leaf;
}

std::string executableDirectory() {
#if defined(_WIN32)
    char buffer[MAX_PATH]{};
    const DWORD length = ::GetModuleFileNameA(nullptr, buffer, MAX_PATH);
    std::string path(buffer, length);
#else
    char buffer[4096]{};
    const ssize_t length = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    std::string path(buffer, length > 0 ? static_cast<std::size_t>(length) : 0);
#endif
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

bool readFile(const std::string& path, std::string& out) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return false;
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    out = buffer.str();
    return true;
}

std::string activationName(ActivationMode mode) {
    switch (mode) {
        case ActivationMode::Toggle: return "toggle";
        case ActivationMode::Hold:   return "hold";
        case ActivationMode::Hybrid: return "hybrid";
    }
    return "hybrid";
}

bool activationFromName(const std::string& name, ActivationMode& out) {
    if (name == "toggle") { out = ActivationMode::Toggle; return true; }
    if (name == "hold")   { out = ActivationMode::Hold;   return true; }
    if (name == "hybrid") { out = ActivationMode::Hybrid; return true; }
    return false;
}

const char* platformName() {
#if defined(_WIN32)
    return "windows";
#elif defined(__linux__)
    return "linux";
#else
    return "unsupported";
#endif
}

}  // namespace

// ---------------------------------------------------------------------------

std::string userConfigRoot() {
#if defined(_WIN32)
    const std::string appData = environment("APPDATA");
    if (!appData.empty()) return joinPath(appData, "KeyGnosys");
    return joinPath(joinPath(environment("USERPROFILE"), "AppData\\Roaming"),
                    "KeyGnosys");
#else
    const std::string xdg = environment("XDG_CONFIG_HOME");
    if (!xdg.empty()) return joinPath(xdg, "keygnosys");
    return joinPath(joinPath(environment("HOME"), ".config"), "keygnosys");
#endif
}

std::vector<std::string> bindingsSearchPaths(const CoreOptions& options) {
    if (!options.bindingsFile.empty()) return {options.bindingsFile};

    const std::string leaf = joinPath("bindings", options.bindingsId + ".json");
    std::vector<std::string> paths;

    // A user document shadows a bundled one by id (SPEC section 3.2), so the
    // user root is searched first.
    const std::string config =
        options.configDir.empty() ? userConfigRoot() : options.configDir;
    if (!config.empty()) paths.push_back(joinPath(config, leaf));

    if (!options.dataDir.empty()) {
        paths.push_back(joinPath(options.dataDir, leaf));
    } else {
        const std::string fromEnv = environment("KGN_DATA_DIR");
        if (!fromEnv.empty()) paths.push_back(joinPath(fromEnv, leaf));
        // Walk up from the executable. A build tree puts the binary several
        // levels below the repository root, and an installed layout puts it
        // one; trying both costs nothing and avoids a build-only assumption.
        std::string base = executableDirectory();
        for (int level = 0; level < 4; ++level) {
            paths.push_back(joinPath(joinPath(base, "data"), leaf));
            base = joinPath(base, "..");
        }
    }
    return paths;
}

// ---------------------------------------------------------------------------

struct Core::Impl {
    explicit Impl(CoreOptions opts) : options(std::move(opts)) {}

    CoreOptions options;
    EngineConfig engineConfig;
    LayerEngine engine{engineConfig};
    Dispatcher dispatcher;
    BindingsDocument bindings;
    Backends backends;
    EndpointOwner owner;
    std::unique_ptr<Server> server;
    HelloInfo hello;
    Diagnostics diagnostics;

    DecisionBuffer decisions;
    EffectBuffer effects;

    TimePoint startedAt{};
    std::atomic<bool> stopRequested{false};
    bool started = false;
    bool stopped = false;
    bool enabled = true;

    // Modifier state, tracked here rather than in the engine because the
    // engine has no reason to publish it and the `modifiers` event needs it.
    // With no input backend these never change, which is the honest answer.
    bool shift = false;
    bool control = false;
    bool alt = false;
    bool meta = false;

    void note(DiagLevel level, std::string code, std::string message,
              std::string file = {}) {
        Diagnostic diagnostic(level, std::move(code), std::move(message),
                              std::move(file));
        if (server) server->broadcastDiagnostic(diagnostic);
        diagnostics.push_back(std::move(diagnostic));
    }

    bool loadBindings(std::string& loadedFrom) {
        for (const auto& path : bindingsSearchPaths(options)) {
            std::string text;
            if (!readFile(path, text)) continue;
            BindingsDocument document;
            Diagnostics found;
            const bool ok = parseBindingsDocument(text, path, document, found);
            for (auto& diagnostic : found) {
                if (server) server->broadcastDiagnostic(diagnostic);
                diagnostics.push_back(std::move(diagnostic));
            }
            if (!ok) continue;
            bindings = std::move(document);
            loadedFrom = path;
            return true;
        }
        return false;
    }

    // Apply what was loaded to the engine and the dispatcher, atomically for
    // each. Both release what they were holding first, because a binding that
    // has gone cannot be asked to release itself afterwards (P7).
    void applyBindings() {
        dispatcher.setPointerSettings(bindings.pointer);
        dispatcher.setScrollSettings(bindings.scroll);

        effects.clear();
        dispatcher.setBindings(bindings.bindings, effects);
        applyEffects(Clock::now());

        decisions.clear();
        engine.setBindings(bindings.toBindingMap(), decisions);
        applyDecisions(Clock::now());
    }

    // Both of these take a snapshot before acting.
    //
    // An effect can re-enter: `layer.release` unwinds the engine back into
    // these same buffers, and `system.reload` releases everything through
    // them. Iterating the member buffer while something downstream clears it
    // is the kind of aliasing that works until the day a user binds
    // layer.release. Copying first costs one small allocation on a path that
    // runs at typing speed, never inside the engine's allocation-free event
    // path.
    void applyDecisions(TimePoint now) {
        const std::vector<Decision> batch(decisions.begin(), decisions.end());
        decisions.clear();
        for (const auto& decision : batch) {
            switch (decision.kind) {
                case Decision::Kind::Forward:
                    if (backends.output) {
                        backends.output->sendKey(decision.code,
                                                 decision.state != KeyState::Up);
                    }
                    break;
                case Decision::Kind::RunAction:
                case Decision::Kind::ReleaseAction:
                    effects.clear();
                    dispatcher.onDecision(decision, now, effects);
                    applyEffects(now);
                    break;
                case Decision::Kind::Suppress:
                case Decision::Kind::Buffer:
                    break;
            }
            if (server) {
                Json data = Json::object();
                data.set("code", Json(std::string(decision.code.toString())));
                data.set("state", Json(decision.state == KeyState::Up       ? "up"
                                       : decision.state == KeyState::Repeat ? "repeat"
                                                                            : "down"));
                data.set("suppressed",
                         Json(decision.kind != Decision::Kind::Forward));
                // Positional codes only. The core never resolves a keystroke
                // to a character, so this stream cannot reconstruct typed text
                // (SPEC section 12.4).
                server->broadcast("key", std::move(data));
            }
        }
    }

    void applyEffects([[maybe_unused]] TimePoint now) {
        const std::vector<Effect> batch(effects.begin(), effects.end());
        effects.clear();
        for (const auto& effect : batch) {
            switch (effect.kind) {
                case Effect::Kind::Button:
                    if (backends.output) {
                        backends.output->button(effect.button, effect.down);
                    } else {
                        reportNoOutput("button.click");
                    }
                    break;
                case Effect::Kind::DoubleClick:
                    if (!backends.output) reportNoOutput("button.double_click");
                    break;
                case Effect::Kind::Scroll:
                    if (backends.output) {
                        backends.output->scroll(effect.dx, effect.dy);
                    } else {
                        reportNoOutput("scroll.page");
                    }
                    break;
                case Effect::Kind::DragLock:
                    if (server) {
                        Json data = Json::object();
                        data.set("button",
                                 Json(effect.button == MouseButton::Left    ? "left"
                                      : effect.button == MouseButton::Right ? "right"
                                                                            : "middle"));
                        data.set("active", Json(effect.down));
                        server->broadcast("drag_lock", std::move(data));
                    }
                    break;
                case Effect::Kind::Warp:
                case Effect::Kind::Window:
                    // Both need a window backend to know where anything is.
                    // Reported as unavailable rather than approximated (P6).
                    if (!backends.window) {
                        note(DiagLevel::Warn, "window.unsupported",
                             std::string(actionName(effect.action.id)) +
                                 " needs a window backend, and this build has none");
                    }
                    break;
                case Effect::Kind::LayerRelease:
                    releaseEverything();
                    publishMode();
                    break;
                case Effect::Kind::OverlayToggle:
                    if (server) server->broadcast("overlay_toggle", Json::object());
                    break;
                case Effect::Kind::ReloadConfig:
                    reload();
                    break;
            }
        }
    }

    void reportNoOutput(const char* action) {
        note(DiagLevel::Warn, "window.unsupported",
             std::string(action) +
                 " needs an output backend, and this build has none");
    }

    void publishMode() {
        if (!server) return;
        Json data = Json::object();
        data.set("mode", Json(engine.mode() == Mode::Cursor ? "cursor" : "normal"));
        data.set("latched", Json(engine.latched()));
        data.set("activation", Json(activationName(engineConfig.activation)));
        server->broadcast("mode", std::move(data));
    }

    [[nodiscard]] Json stateSnapshot() const {
        Json modifiers = Json::object();
        modifiers.set("shift", Json(shift));
        modifiers.set("control", Json(control));
        modifiers.set("alt", Json(alt));
        modifiers.set("meta", Json(meta));
        modifiers.set("caps_layer", Json(engine.mode() == Mode::Cursor));

        Json snapshot = Json::object();
        snapshot.set("mode", Json(engine.mode() == Mode::Cursor ? "cursor" : "normal"));
        snapshot.set("latched", Json(engine.latched()));
        snapshot.set("activation", Json(activationName(engineConfig.activation)));
        snapshot.set("enabled", Json(enabled));
        snapshot.set("modifiers", std::move(modifiers));
        // Null and empty rather than invented: there is no window backend, so
        // there is nothing truthful to say about focus, windows or monitors.
        snapshot.set("focus", Json());
        snapshot.set("windows", Json::array());
        snapshot.set("monitors", Json::array());
        return snapshot;
    }

    void releaseEverything() {
        decisions.clear();
        engine.releaseAll(decisions);
        for (const auto& decision : decisions) {
            if (decision.kind == Decision::Kind::Forward && backends.output) {
                backends.output->sendKey(decision.code, false);
            }
        }
        decisions.clear();

        effects.clear();
        dispatcher.releaseAll(effects);
        for (const auto& effect : effects) {
            if (effect.kind == Effect::Kind::Button && backends.output) {
                backends.output->button(effect.button, effect.down);
            }
        }
        effects.clear();

        // The backend's own belt and braces: whatever it believes it is
        // holding goes up too.
        if (backends.output) backends.output->releaseAll();
    }

    Reply reload() {
        releaseEverything();
        std::string from;
        Json loaded = Json::object();
        if (loadBindings(from)) {
            applyBindings();
            loaded.set("bindings", Json(1));
        } else {
            note(DiagLevel::Warn, "binding.invalid",
                 "no bindings document could be loaded");
            loaded.set("bindings", Json(0));
        }
        // The core loads only bindings. Layouts and profiles are presentation
        // and belong to the overlay's registry, so the counts it can honestly
        // report for them are zero rather than a number it made up.
        loaded.set("layouts", Json(0));
        loaded.set("profiles", Json(0));
        if (server) {
            Json changed = Json::object();
            Json kinds = Json::array();
            kinds.push(Json("bindings"));
            changed.set("kinds", std::move(kinds));
            server->broadcast("config_changed", std::move(changed));
        }
        Json data = Json::object();
        data.set("loaded", std::move(loaded));
        return Reply::success(std::move(data));
    }

    Reply handle(const Command& command) {
        if (command.name == "ping") {
            Json data = Json::object();
            data.set("pong", Json(true));
            const auto uptime = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    Clock::now() - startedAt)
                                    .count();
            data.set("uptime_ms", Json(static_cast<std::int64_t>(uptime)));
            return Reply::success(std::move(data));
        }
        if (command.name == "get_state") {
            return Reply::success(stateSnapshot());
        }
        if (command.name == "set_activation_mode") {
            ActivationMode mode = engineConfig.activation;
            if (!activationFromName(command.data["mode"].asString(), mode)) {
                return Reply::failure("config.invalid",
                                      "mode must be toggle, hold or hybrid");
            }
            releaseEverything();
            engineConfig.activation = mode;
            engine.setConfig(engineConfig);
            publishMode();
            return Reply::success();
        }
        if (command.name == "set_enabled") {
            if (!command.data["enabled"].isBool()) {
                return Reply::failure("config.invalid", "enabled must be a boolean");
            }
            const bool wanted = command.data["enabled"].asBool();
            if (wanted != enabled) {
                // Disabling releases everything first: a master switch that
                // left a key or a button held would be the P7 failure in its
                // most obvious form.
                releaseEverything();
                enabled = wanted;
                publishMode();
            }
            return Reply::success();
        }
        if (command.name == "reload_config") return reload();
        if (command.name == "set_bindings") {
            const std::string id = command.data["id"].asString();
            if (id.empty()) return Reply::failure("config.invalid", "id is required");
            const std::string previous = options.bindingsId;
            options.bindingsId = id;
            std::string from;
            if (!loadBindings(from)) {
                options.bindingsId = previous;
                return Reply::failure("binding.invalid",
                                      "no bindings document with id '" + id + "'");
            }
            releaseEverything();
            applyBindings();
            return Reply::success();
        }
        if (command.name == "set_setting") {
            return setSetting(command.data["path"].asString(), command.data["value"]);
        }
        if (command.name == "release_all") {
            releaseEverything();
            publishMode();
            return Reply::success();
        }
        return Reply::failure("ipc.unsupported",
                              "unknown command '" + command.name + "'");
    }

    Reply setSetting(const std::string& path, const Json& value) {
        if (path == "behavior.activation_mode") {
            ActivationMode mode = engineConfig.activation;
            if (!activationFromName(value.asString(), mode)) {
                return Reply::failure("config.invalid",
                                      "mode must be toggle, hold or hybrid");
            }
            releaseEverything();
            engineConfig.activation = mode;
            engine.setConfig(engineConfig);
            publishMode();
            return Reply::success();
        }
        if (path == "behavior.hybrid_tap_ms" || path == "behavior.grace_ms") {
            if (!value.isNumber()) {
                return Reply::failure("config.invalid", "value must be a number");
            }
            std::int64_t milliseconds = value.asInt(0);
            const std::int64_t limit = 5000;
            const std::int64_t clamped = milliseconds < 0     ? 0
                                         : milliseconds > limit ? limit
                                                                : milliseconds;
            if (clamped != milliseconds) {
                note(DiagLevel::Info, "config.clamped",
                     path + " was out of range and was clamped");
                milliseconds = clamped;
            }
            if (path == "behavior.hybrid_tap_ms") {
                engineConfig.hybridTap = std::chrono::milliseconds(milliseconds);
            } else {
                engineConfig.grace = std::chrono::milliseconds(milliseconds);
            }
            engine.setConfig(engineConfig);
            return Reply::success();
        }
        if (path == "behavior.real_capslock_gesture") {
            // The catalog has one gesture today; the setting records whether
            // it is on. "none" turns it off, anything else names it on.
            engineConfig.shiftCapsIsRealCapsLock = value.asString() != "none";
            engine.setConfig(engineConfig);
            return Reply::success();
        }
        return Reply::failure("config.invalid",
                              "no setting at '" + path + "'");
    }
};

// ---------------------------------------------------------------------------

Core::Core(CoreOptions options) : impl_(std::make_unique<Impl>(std::move(options))) {}

Core::~Core() { stop("core destroyed"); }

OwnResult Core::start() {
    if (impl_->started) return {OwnStatus::Ok, "", "already started"};
    impl_->startedAt = Clock::now();

    const std::string address = impl_->options.endpointOverrideForTests.empty()
                                    ? resolveEndpoint()
                                    : impl_->options.endpointOverrideForTests;

    // The endpoint comes first. Nothing else the core does matters if another
    // core already owns it, and refusing before touching configuration keeps
    // the failure clean.
    const OwnResult owned = impl_->owner.acquire(address);
    if (!owned.ok()) return owned;

    impl_->backends = createBackends();

    impl_->hello.coreVersion = "0.1.0";
    impl_->hello.platform = platformName();
    if (impl_->backends.input) impl_->hello.inputBackend = "input";
    if (impl_->backends.output) impl_->hello.outputBackend = "output";
    if (impl_->backends.window) impl_->hello.windowBackend = "window";
    if (impl_->backends.input) {
        const Capabilities capabilities = impl_->backends.input->capabilities();
        if (capabilities.canSuppress) impl_->hello.capabilities.emplace_back("suppress");
        if (capabilities.canWarpAbsolute) {
            impl_->hello.capabilities.emplace_back("warp_absolute");
        }
        if (capabilities.canMoveWindows) {
            impl_->hello.capabilities.emplace_back("move_windows");
        }
        for (const auto& limitation : capabilities.limitations) {
            impl_->hello.limitations.push_back(limitation);
        }
    } else {
        // Said plainly, in the one place a client is guaranteed to read.
        impl_->hello.limitations.emplace_back(
            "No input backend on this build: keys are not intercepted and the "
            "pointer is not driven (milestones M3 and M4).");
    }

    impl_->server =
        std::make_unique<Server>(impl_->hello, impl_->owner.takeTransport());
    impl_->server->setCommandHandler(
        [impl = impl_.get()](const Command& command) { return impl->handle(command); });

    std::string from;
    if (impl_->loadBindings(from)) {
        impl_->applyBindings();
    } else {
        std::string looked;
        for (const auto& path : bindingsSearchPaths(impl_->options)) {
            if (!looked.empty()) looked += ", ";
            looked += path;
        }
        impl_->note(DiagLevel::Warn, "binding.invalid",
                    "no bindings document found; the cursor layer has no bindings. "
                    "Looked in: " + looked);
    }

    impl_->started = true;
    return owned;
}

void Core::step(TimePoint now) {
    if (!impl_->started || impl_->stopped) return;

    if (impl_->enabled) {
        impl_->decisions.clear();
        impl_->engine.tick(now, impl_->decisions);
        impl_->applyDecisions(now);

        const TickResult motion = impl_->dispatcher.tick(now);
        if (!motion.pointer.zero() && impl_->backends.output) {
            impl_->backends.output->moveCursorBy(motion.pointer.x, motion.pointer.y);
        }
        if (!motion.scroll.zero() && impl_->backends.output) {
            impl_->backends.output->scroll(motion.scroll.x, motion.scroll.y);
        }
    }

    impl_->server->poll();
}

void Core::run() {
    if (!impl_->started) return;
    TimePoint next = Clock::now();
    while (!impl_->stopRequested.load() && !impl_->stopped) {
        const TimePoint now = Clock::now();
        step(now);
        next += kTickInterval;
        // A loop that fell far behind must not then sprint to catch up; a
        // burst of ticks would move the pointer in one jump.
        if (next < now) next = now + kTickInterval;
        std::this_thread::sleep_until(next);
    }
    stop("core is exiting");
}

void Core::requestStop() { impl_->stopRequested.store(true); }

void Core::stop(const std::string& reason) {
    if (!impl_ || impl_->stopped) return;
    impl_->stopped = true;
    // P7 first, always: unwind before anything is torn down, because after
    // the backends are gone there is no way to release anything.
    if (impl_->started) impl_->releaseEverything();
    if (impl_->server) impl_->server->shutdown(reason);
    impl_->owner.release();
}

Server* Core::server() { return impl_->server.get(); }
const Diagnostics& Core::diagnostics() const { return impl_->diagnostics; }
const HelloInfo& Core::hello() const { return impl_->hello; }
bool Core::running() const { return impl_->started && !impl_->stopped; }

}  // namespace kgn

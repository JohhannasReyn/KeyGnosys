#include "kgn/core.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
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
    // is_open(), not operator bool. On MinGW/UCRT a freshly constructed
    // ifstream whose open FAILED still tests true, so `if (!stream)` never
    // fires and every missing path is read as an empty file -- which the
    // bindings search then reports as a document that is not valid JSON, once
    // per path it looked in. Verified on GCC 15.2 / UCRT64.
    if (!stream.is_open()) return false;
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

// The engine owner on a build with no input backend.
//
// It owns the LayerEngine outright and applies control the moment it is
// submitted, so a core with nothing feeding it behaves exactly as it did
// before the ownership split. The Windows hook thread is the other
// implementation: same interface, same rings, different thread.
//
// Having ONE abstraction rather than a conditional inside the core is what
// keeps the rule checkable -- the core never calls a mutating engine method,
// on any build.
class LocalEngineOwner : public EngineOwner {
public:
    LocalEngineOwner(WorkRing& work, StatePublisher& published, EngineConfig config)
        : work_(work), published_(published), engine_(config), config_(config) {
        republish();
    }

    bool submit(const Control& control) override {
        switch (control.kind) {
            case Control::Kind::SetConfig:
                config_ = *static_cast<const EngineConfig*>(control.payload);
                engine_.setConfig(config_);
                break;
            case Control::Kind::SetBindings:
                buffer_.clear();
                engine_.setBindings(*static_cast<const BindingMap*>(control.payload),
                                    buffer_);
                translateDecisions(buffer_, KeyCode{}, KeyState::Down, work_);
                break;
            case Control::Kind::ReleaseAll:
                buffer_.clear();
                engine_.releaseAll(buffer_);
                translateDecisions(buffer_, KeyCode{}, KeyState::Down, work_);
                break;
            case Control::Kind::SetEnabled:
                buffer_.clear();
                engine_.releaseAll(buffer_);
                translateDecisions(buffer_, KeyCode{}, KeyState::Down, work_);
                enabled_ = control.flag;
                break;
            case Control::Kind::Stop:
                break;
        }
        republish();
        applied_ = control.seq;
        return true;
    }

    bool awaitApplied(std::uint32_t seq, std::chrono::milliseconds) override {
        return applied_ >= seq;
    }

private:
    void republish() {
        PublishedState state;
        state.mode = engine_.mode();
        state.latched = engine_.latched();
        state.activation = config_.activation;
        published_.publish(state);
    }

    WorkRing& work_;
    StatePublisher& published_;
    LayerEngine engine_;
    EngineConfig config_;
    DecisionBuffer buffer_;
    std::uint32_t applied_ = 0;
    bool enabled_ = true;
};

struct Core::Impl {
    explicit Impl(CoreOptions opts) : options(std::move(opts)) {}

    CoreOptions options;
    EngineConfig engineConfig;
    Dispatcher dispatcher;
    BindingsDocument bindings;
    Backends backends;
    EndpointOwner owner;
    std::unique_ptr<Server> server;
    HelloInfo hello;
    Diagnostics diagnostics;

    // The two streams from whoever owns the engine. The work ring is
    // correctness-critical and provably never overflows; the publication ring
    // may coalesce, because a lost `key` event costs an overlay highlight.
    WorkRing work;
    PublicationRing publication;
    StatePublisher published;
    std::atomic<std::uint64_t> publicationDrops{0};
    std::uint32_t publishedVersion = 0;

    std::unique_ptr<EngineOwner> engineOwner;
    std::uint32_t controlSeq = 0;
    // Kept alive across a submit so the owner can read it before acknowledging.
    BindingMap pendingBindings;

    EffectBuffer effects;

    // Window and monitor state, and the slot registry that keeps the number
    // bindings pointing at the same window from one emission to the next.
    SlotRegistry slots;
    std::vector<WindowInfo> lastWindows;
    std::vector<MonitorInfo> lastMonitors;
    WindowId lastFocus = 0;
    TimePoint lastWindowsPublished{};
    TimePoint lastPointerPublished{};
    Point lastPointer{};

    // Scheduled second halves of a double click. Serviced from the loop rather
    // than slept through: the OS interval is tens of milliseconds and the loop
    // must keep moving the pointer while it elapses.
    struct PendingClick {
        MouseButton button = MouseButton::Left;
        TimePoint dueAt{};
    };
    std::vector<PendingClick> pendingDoubleClicks;

    TimePoint startedAt{};
    std::atomic<bool> stopRequested{false};
    bool started = false;
    bool stopped = false;
    bool enabled = true;

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

    // Apply what was loaded to the dispatcher and the engine's owner,
    // atomically for each. Both release what they were holding first, because
    // a binding that has gone cannot be asked to release itself afterwards (P7).
    void applyBindings() {
        dispatcher.setPointerSettings(bindings.pointer);
        dispatcher.setScrollSettings(bindings.scroll);

        effects.clear();
        dispatcher.setBindings(bindings.bindings, effects);
        applyEffects(Clock::now());

        pendingBindings = bindings.toBindingMap();
        submitAndWait(Control{Control::Kind::SetBindings, false, 0, &pendingBindings});
        drainWork();
    }

    // Stream B. ONE `key` event per PHYSICAL event -- never one per decision.
    //
    // A grace replay is two decisions and one physical event; a release_all is
    // many decisions and no physical event at all. Publishing per decision, as
    // this once did, reports keys the user never pressed.
    void drainPublication() {
        PhysicalRecord record{};
        while (publication.pop(record)) {
            if (server) {
                Json data = Json::object();
                data.set("code", Json(std::string(KeyCode(record.code).toString())));
                data.set("state", Json(record.state == KeyState::Up       ? "up"
                                       : record.state == KeyState::Repeat ? "repeat"
                                                                          : "down"));
                data.set("suppressed", Json(record.suppressed));
                // Positional codes only. The core never resolves a keystroke
                // to a character, so this stream cannot reconstruct typed text
                // (SPEC section 12.4).
                server->broadcast("key", std::move(data));
            }
            // Stamped with the state as of this callback, so the mode change a
            // key caused is published straight after that key rather than in
                // whatever order two queues happened to interleave.
            if (record.version != publishedVersion) publishState();
        }
        if (published.version() != publishedVersion) publishState();

        const std::uint64_t dropped = publicationDrops.exchange(0);
        if (dropped != 0) {
            note(DiagLevel::Info, "input.publication_dropped",
                 "key event publication fell behind by " + std::to_string(dropped) +
                     " event(s); overlay feedback may have missed a key");
        }
    }

    // Stream A. What the core must DO. Never dropped: the ring's capacity makes
    // overflow structurally impossible (hookchannel.hpp).
    void drainWork() {
        WorkItem item{};
        while (work.pop(item)) {
            const KeyCode code{item.code};
            switch (item.kind) {
                case WorkItem::Kind::SendKey:
                    if (backends.output) backends.output->sendKey(code, item.down);
                    break;
                case WorkItem::Kind::RunAction:
                case WorkItem::Kind::ReleaseAction: {
                    const bool run = item.kind == WorkItem::Kind::RunAction;
                    const Decision decision{run ? Decision::Kind::RunAction
                                                : Decision::Kind::ReleaseAction,
                                            code,
                                            run ? KeyState::Down : KeyState::Up};
                    effects.clear();
                    dispatcher.onDecision(decision, Clock::now(), effects);
                    applyEffects(Clock::now());
                    break;
                }
            }
        }
    }

    void applyEffects([[maybe_unused]] TimePoint now) {
        // A snapshot, because an effect can re-enter: `layer.release` unwinds
        // back into this same buffer and `system.reload` releases everything
        // through it. Iterating the member buffer while something downstream
        // clears it is the kind of aliasing that works until the day a user
        // binds layer.release.
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
                    if (backends.output) {
                        scheduleDoubleClick(effect.button);
                    } else {
                        reportNoOutput("button.double_click");
                    }
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
                    applyWarp(effect.action);
                    break;
                case Effect::Kind::Window:
                    applyWindow(effect.action);
                    break;
                case Effect::Kind::LayerRelease:
                    releaseEverything();
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

    // -- warp and window actions ------------------------------------------

    [[nodiscard]] std::optional<MonitorInfo> monitorAt(Point point) const {
        for (const MonitorInfo& monitor : lastMonitors) {
            if (point.x >= monitor.bounds.x &&
                point.x < monitor.bounds.x + monitor.bounds.w &&
                point.y >= monitor.bounds.y &&
                point.y < monitor.bounds.y + monitor.bounds.h) {
                return monitor;
            }
        }
        if (!lastMonitors.empty()) return lastMonitors.front();
        return std::nullopt;
    }

    [[nodiscard]] std::optional<MonitorInfo> resolveMonitor(
        const MonitorTarget& target, int from) const {
        if (lastMonitors.empty()) return std::nullopt;
        const int count = static_cast<int>(lastMonitors.size());
        int index = from;
        switch (target.kind) {
            case MonitorTarget::Kind::Next: index = (from + 1) % count; break;
            case MonitorTarget::Kind::Prev: index = (from - 1 + count) % count; break;
            case MonitorTarget::Kind::Index: index = target.index; break;
        }
        if (index < 0 || index >= count) return std::nullopt;
        return lastMonitors[static_cast<std::size_t>(index)];
    }

    void applyWarp(const Action& action) {
        if (!backends.output || !backends.window) {
            noteUnsupported(action.id);
            return;
        }
        refreshMonitors();
        const Point cursor = backends.output->cursorPosition();
        const std::optional<MonitorInfo> current = monitorAt(cursor);
        if (!current.has_value()) {
            noteUnsupported(action.id);
            return;
        }
        const Rect bounds = current->bounds;

        switch (action.id) {
            case ActionId::WarpGrid: {
                // A 3x3 grid over the CURRENT monitor, cell 1 top-left.
                if (action.index < 1 || action.index > 9) return;
                const int cell = action.index - 1;
                const int column = cell % 3;
                const int row = cell / 3;
                backends.output->moveCursorTo(
                    bounds.x + bounds.w * (2 * column + 1) / 6,
                    bounds.y + bounds.h * (2 * row + 1) / 6);
                return;
            }
            case ActionId::WarpCorner: {
                // Inset by one pixel so a corner lands ON the monitor rather
                // than on the exclusive edge that belongs to the next one.
                const int left = bounds.x;
                const int top = bounds.y;
                const int right = bounds.x + bounds.w - 1;
                const int bottom = bounds.y + bounds.h - 1;
                switch (action.corner) {
                    case Corner::TopLeft: backends.output->moveCursorTo(left, top); return;
                    case Corner::TopRight: backends.output->moveCursorTo(right, top); return;
                    case Corner::BottomLeft: backends.output->moveCursorTo(left, bottom); return;
                    case Corner::BottomRight: backends.output->moveCursorTo(right, bottom); return;
                    case Corner::Center:
                        backends.output->moveCursorTo(bounds.center().x, bounds.center().y);
                        return;
                }
                return;
            }
            case ActionId::WarpMonitor: {
                const std::optional<MonitorInfo> target =
                    resolveMonitor(action.target, current->index);
                if (!target.has_value()) {
                    noteUnsupported(action.id);
                    return;
                }
                backends.output->moveCursorTo(target->bounds.center().x,
                                              target->bounds.center().y);
                return;
            }
            default: return;
        }
    }

    void applyWindow(const Action& action) {
        if (!backends.window) {
            noteUnsupported(action.id);
            return;
        }
        refreshWindows();
        refreshMonitors();

        switch (action.id) {
            case ActionId::WindowSlot: {
                const std::optional<WindowId> id = slots.at(action.index);
                if (!id.has_value()) return;   // an empty slot does nothing
                if (!backends.window->focus(*id)) noteUnsupported(action.id);
                return;
            }
            case ActionId::WindowCycle: {
                // Slot order, never recency: the numbers the user has learned
                // are the order (SPEC 6.5).
                const std::vector<WindowId> ids = slots.occupied();
                if (ids.empty()) return;
                const std::optional<WindowInfo> focused = backends.window->focused();
                std::size_t at = 0;
                if (focused.has_value()) {
                    for (std::size_t i = 0; i < ids.size(); ++i) {
                        if (ids[i] == focused->id) {
                            at = i;
                            break;
                        }
                    }
                }
                const std::size_t count = ids.size();
                const std::size_t next = action.cycle == Cycle::Next
                                             ? (at + 1) % count
                                             : (at + count - 1) % count;
                if (!backends.window->focus(ids[next])) noteUnsupported(action.id);
                return;
            }
            case ActionId::WindowFocusMonitor: {
                const std::optional<WindowInfo> focused = backends.window->focused();
                const int from = focused.has_value() ? focused->monitor : 0;
                const std::optional<MonitorInfo> target =
                    resolveMonitor(action.target, from);
                if (!target.has_value()) {
                    noteUnsupported(action.id);
                    return;
                }
                // The topmost window on that monitor, in the backend's own
                // z-order, and the pointer follows so focus and cursor agree.
                for (const WindowInfo& info : lastWindows) {
                    if (info.monitor != target->index) continue;
                    backends.window->focus(info.id);
                    break;
                }
                if (backends.output) {
                    backends.output->moveCursorTo(target->bounds.center().x,
                                                  target->bounds.center().y);
                }
                return;
            }
            case ActionId::WindowMoveToMonitor: {
                const std::optional<WindowInfo> focused = backends.window->focused();
                if (!focused.has_value()) return;
                const std::optional<MonitorInfo> target =
                    resolveMonitor(action.target, focused->monitor);
                if (!target.has_value()) {
                    noteUnsupported(action.id);
                    return;
                }
                if (!backends.window->moveWindowToMonitor(focused->id, target->index)) {
                    noteUnsupported(action.id);
                }
                return;
            }
            default: return;
        }
    }

    void noteUnsupported(ActionId id) {
        note(DiagLevel::Warn, "window.unsupported",
             std::string(actionName(id)) +
                 " is unavailable on this build's backends");
    }

    // -- double click ------------------------------------------------------

    // The first pair goes out now; the second is scheduled. SPEC 7.2 wants the
    // OS interval between them, and only the backend knows it -- but the loop
    // must not sleep through it, or the pointer stops moving mid-gesture.
    void scheduleDoubleClick(MouseButton button) {
        if (!backends.output) return;
        backends.output->button(button, true);
        backends.output->button(button, false);
        pendingDoubleClicks.push_back(
            PendingClick{button, Clock::now() + backends.output->doubleClickInterval()});
    }

    void serviceDoubleClicks(TimePoint now) {
        if (pendingDoubleClicks.empty() || !backends.output) return;
        std::vector<PendingClick> remaining;
        for (const PendingClick& pending : pendingDoubleClicks) {
            if (pending.dueAt > now) {
                remaining.push_back(pending);
                continue;
            }
            backends.output->button(pending.button, true);
            backends.output->button(pending.button, false);
        }
        pendingDoubleClicks = std::move(remaining);
    }

    // -- window, monitor and pointer publication ---------------------------

    void refreshWindows() {
        if (!backends.window) return;
        lastWindows = backends.window->windows();
        slots.update(lastWindows);
    }

    void refreshMonitors() {
        if (!backends.window) return;
        lastMonitors = backends.window->monitors();
    }

    [[nodiscard]] Json focusJson() {
        if (!backends.window) return Json();
        const std::optional<WindowInfo> focused = backends.window->focused();
        if (!focused.has_value()) return Json();
        Json data = Json::object();
        data.set("app_id", Json(focused->process));
        data.set("process", Json(focused->process));
        data.set("wm_class", Json(focused->wmClass));
        data.set("title", Json(focused->title));
        data.set("window_id", Json(static_cast<std::int64_t>(focused->id)));
        return data;
    }

    [[nodiscard]] Json windowsJson() {
        Json list = Json::array();
        if (!backends.window) return list;
        refreshWindows();
        for (const WindowInfo& info : lastWindows) {
            const std::optional<int> index = slots.indexOf(info.id);
            Json slot = Json::object();
            // A tenth window is reported without an index rather than given
            // one that would displace a slot the user has learned (SPEC 6.5).
            slot.set("index", index.has_value() ? Json(*index) : Json());
            slot.set("window_id", Json(static_cast<std::int64_t>(info.id)));
            slot.set("process", Json(info.process));
            slot.set("title", Json(info.title));
            slot.set("monitor", Json(info.monitor));
            list.push(std::move(slot));
        }
        return list;
    }

    [[nodiscard]] Json monitorsJson() {
        Json list = Json::array();
        if (!backends.window) return list;
        refreshMonitors();
        for (const MonitorInfo& monitor : lastMonitors) {
            Json entry = Json::object();
            entry.set("index", Json(monitor.index));
            entry.set("x", Json(monitor.bounds.x));
            entry.set("y", Json(monitor.bounds.y));
            entry.set("w", Json(monitor.bounds.w));
            entry.set("h", Json(monitor.bounds.h));
            entry.set("primary", Json(monitor.primary));
            entry.set("name", Json(monitor.name));
            list.push(std::move(entry));
        }
        return list;
    }

    void publishWindowState(TimePoint now) {
        if (!backends.window || !server) return;

        // Debounced: SPEC 5.3 asks for at most one `windows` emission every
        // 250 ms, because enumeration is not free and a window list that
        // churns is a list nobody can read.
        const bool due = lastWindowsPublished == TimePoint{} ||
                         now - lastWindowsPublished >= std::chrono::milliseconds(250);

        const std::optional<WindowInfo> focused = backends.window->focused();
        const WindowId focusId = focused.has_value() ? focused->id : 0;
        if (focusId != lastFocus) {
            lastFocus = focusId;
            server->broadcast("focus", focusJson());
        }

        if (!due) return;
        lastWindowsPublished = now;

        const std::vector<WindowInfo> windows = backends.window->windows();
        const bool changed =
            windows.size() != lastWindows.size() ||
            !std::equal(windows.begin(), windows.end(), lastWindows.begin(),
                        [](const WindowInfo& a, const WindowInfo& b) {
                            return a.id == b.id && a.title == b.title &&
                                   a.monitor == b.monitor;
                        });
        if (changed) {
            lastWindows = windows;
            slots.update(lastWindows);
            Json data = Json::object();
            data.set("slots", windowsJson());
            server->broadcast("windows", std::move(data));
        }

        const std::vector<MonitorInfo> monitors = backends.window->monitors();
        const bool topologyChanged =
            monitors.size() != lastMonitors.size() ||
            !std::equal(monitors.begin(), monitors.end(), lastMonitors.begin(),
                        [](const MonitorInfo& a, const MonitorInfo& b) {
                            return a.index == b.index && a.bounds.x == b.bounds.x &&
                                   a.bounds.y == b.bounds.y && a.bounds.w == b.bounds.w &&
                                   a.bounds.h == b.bounds.h;
                        });
        if (topologyChanged) {
            lastMonitors = monitors;
            Json data = Json::object();
            data.set("monitors", monitorsJson());
            server->broadcast("monitors", std::move(data));
        }
    }

    void publishPointer(TimePoint now) {
        // Only while the layer is engaged, and at no more than 20 Hz: this is
        // the one event that would otherwise stream continuously (SPEC 5.3).
        if (!server || !backends.output) return;
        if (published.state().mode != Mode::Cursor) return;
        if (lastPointerPublished != TimePoint{} &&
            now - lastPointerPublished < std::chrono::milliseconds(50)) {
            return;
        }
        lastPointerPublished = now;

        const Point cursor = backends.output->cursorPosition();
        if (cursor.x == lastPointer.x && cursor.y == lastPointer.y) return;
        lastPointer = cursor;

        const std::optional<MonitorInfo> monitor = monitorAt(cursor);
        Json data = Json::object();
        data.set("x", Json(cursor.x));
        data.set("y", Json(cursor.y));
        data.set("monitor", monitor.has_value() ? Json(monitor->index) : Json());
        server->broadcast("pointer", std::move(data));
    }

    // Whatever the input backend noticed on its own thread. It cannot log or
    // touch IPC there, so it counts and this reports.
    void drainBackendDiagnostics() {
        if (!backends.input) return;
        Diagnostics found;
        backends.input->drainDiagnostics(found);
        for (auto& diagnostic : found) {
            if (server) server->broadcastDiagnostic(diagnostic);
            diagnostics.push_back(std::move(diagnostic));
        }
    }

    void reportNoOutput(const char* action) {
        note(DiagLevel::Warn, "window.unsupported",
             std::string(action) +
                 " needs an output backend, and this build has none");
    }

    void publishState() {
        const PublishedState state = published.state();
        publishedVersion = published.version();
        if (!server) return;

        Json mode = Json::object();
        mode.set("mode", Json(state.mode == Mode::Cursor ? "cursor" : "normal"));
        mode.set("latched", Json(state.latched));
        mode.set("activation", Json(activationName(state.activation)));
        server->broadcast("mode", std::move(mode));

        Json modifiers = Json::object();
        modifiers.set("shift", Json(state.shift));
        modifiers.set("control", Json(state.control));
        modifiers.set("alt", Json(state.alt));
        modifiers.set("meta", Json(state.meta));
        modifiers.set("caps_layer", Json(state.mode == Mode::Cursor));
        server->broadcast("modifiers", std::move(modifiers));
    }

    [[nodiscard]] Json stateSnapshot() {
        const PublishedState state = published.state();

        Json modifiers = Json::object();
        modifiers.set("shift", Json(state.shift));
        modifiers.set("control", Json(state.control));
        modifiers.set("alt", Json(state.alt));
        modifiers.set("meta", Json(state.meta));
        modifiers.set("caps_layer", Json(state.mode == Mode::Cursor));

        Json snapshot = Json::object();
        snapshot.set("mode", Json(state.mode == Mode::Cursor ? "cursor" : "normal"));
        snapshot.set("latched", Json(state.latched));
        snapshot.set("activation", Json(activationName(state.activation)));
        snapshot.set("enabled", Json(enabled));
        snapshot.set("modifiers", std::move(modifiers));
        snapshot.set("focus", focusJson());
        snapshot.set("windows", windowsJson());
        snapshot.set("monitors", monitorsJson());
        return snapshot;
    }

    // Submit a control message and wait for the owner to report it applied.
    //
    // Waiting in this direction is safe: it stalls the core loop, never a hook
    // callback, and Windows only measures the latter. Bounded, because the
    // owner drains control the moment it wakes and never blocks.
    bool submitAndWait(Control control) {
        if (!engineOwner) return true;
        control.seq = ++controlSeq;
        if (!engineOwner->submit(control)) return false;
        return engineOwner->awaitApplied(control.seq, options.controlTimeout);
    }

    // Every obligation the core can discharge without the engine's help.
    //
    // The engine's own unwind is asked for first, but nothing here depends on
    // it arriving: the dispatcher's buttons and drag locks are core-owned, and
    // the output backend tracks whatever it synthesised. A natively forwarded
    // press is discharged by the user's own physical release once the hook is
    // gone. Those three sets are exhaustive, which is why shutdown stays
    // correct even when the engine's owner never answers.
    void releaseEverything() {
        submitAndWait(Control{Control::Kind::ReleaseAll, false, 0, nullptr});
        drainWork();

        pendingDoubleClicks.clear();

        effects.clear();
        dispatcher.releaseAll(effects);
        for (const auto& effect : effects) {
            if (effect.kind == Effect::Kind::Button && backends.output) {
                backends.output->button(effect.button, effect.down);
            }
        }
        effects.clear();

        // The backend's own belt and braces: whatever it believes it is
        // holding goes up too, including anything it synthesised that no
        // physical key will ever release.
        if (backends.output) backends.output->releaseAll();
        publishState();
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
            return applyActivation(mode);
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
                if (!submitAndWait(Control{Control::Kind::SetEnabled, wanted, 0,
                                           nullptr})) {
                    return notApplied();
                }
                enabled = wanted;
                publishState();
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
            return applyActivation(mode);
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
            return applyEngineConfig();
        }
        if (path == "behavior.real_capslock_gesture") {
            // The catalog has one gesture today; the setting records whether
            // it is on. "none" turns it off, anything else names it on.
            engineConfig.shiftCapsIsRealCapsLock = value.asString() != "none";
            return applyEngineConfig();
        }
        return Reply::failure("config.invalid",
                              "no setting at '" + path + "'");
    }

    // P6: say the change did not happen rather than report a success the
    // engine never saw.
    static Reply notApplied() {
        return Reply::failure("input.queue_overflow",
                              "the input thread did not apply the change in time");
    }

    Reply applyEngineConfig() {
        if (!submitAndWait(Control{Control::Kind::SetConfig, false, 0,
                                   &engineConfig})) {
            return notApplied();
        }
        return Reply::success();
    }

    Reply applyActivation(ActivationMode mode) {
        releaseEverything();
        engineConfig.activation = mode;
        const Reply reply = applyEngineConfig();
        publishState();
        return reply;
    }
};

// ---------------------------------------------------------------------------

Core::Core(CoreOptions options, Backends backends)
    : impl_(std::make_unique<Impl>(std::move(options))) {
    impl_->backends = std::move(backends);
}

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

    impl_->hello.coreVersion = "0.1.0";
    impl_->hello.platform = platformName();

    // Each backend reports its OWN capabilities. Reading a pointer-warp
    // capability off the input backend, as this once did, is how a build ends
    // up announcing something nothing implements.
    auto absorb = [&](const Capabilities& capabilities) {
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
    };

    if (impl_->backends.input) {
        impl_->hello.inputBackend = std::string(impl_->backends.input->name());
        absorb(impl_->backends.input->capabilities());
    }
    if (impl_->backends.output) {
        impl_->hello.outputBackend = std::string(impl_->backends.output->name());
        absorb(impl_->backends.output->capabilities());
    }
    if (impl_->backends.window) {
        impl_->hello.windowBackend = std::string(impl_->backends.window->name());
        absorb(impl_->backends.window->capabilities());
    }
    if (!impl_->backends.input) {
        // Said plainly, in the one place a client is guaranteed to read.
        impl_->hello.limitations.emplace_back(
            "No input backend on this build: keys are not intercepted and the "
            "pointer is not driven (milestones M3 and M4).");
    }

    // The engine owner. With an input backend the backend IS the owner --
    // it runs the engine on its own thread and answers Windows synchronously.
    // With none, a local owner applies control inline, which is what keeps a
    // backend-less build behaving exactly as it did before the split.
    if (!impl_->engineOwner) {
        if (impl_->backends.input) {
            impl_->engineOwner = impl_->backends.input->engineOwner(
                impl_->work, impl_->publication, impl_->published,
                impl_->engineConfig);
        }
        if (!impl_->engineOwner) {
            impl_->engineOwner = std::make_unique<LocalEngineOwner>(
                impl_->work, impl_->published, impl_->engineConfig);
        }
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

    if (impl_->backends.input) {
        // The handler is a formality on the hook path: the backend owns the
        // engine and answers Windows itself. It is here so a future backend
        // that does NOT own an engine still has the documented seam.
        if (!impl_->backends.input->start(nullptr)) {
            impl_->note(DiagLevel::Error, "input.permission_denied",
                        "the input backend could not start; keys are not "
                        "intercepted and the pointer is not driven");
        }
    }

    impl_->refreshWindows();
    impl_->refreshMonitors();

    impl_->started = true;
    return owned;
}

void Core::step(TimePoint now) {
    if (!impl_->started || impl_->stopped) return;

    // The layer engine is no longer ticked here. It is owned by whoever feeds
    // it, and it wakes on its own grace deadline rather than at 60 Hz --
    // expiring buffered presses is its only timed work. What remains on this
    // beat is motion, which SPEC 6.4 does specify at 60 Hz.
    impl_->drainPublication();
    impl_->drainWork();
    impl_->serviceDoubleClicks(now);

    if (impl_->enabled) {
        const TickResult motion = impl_->dispatcher.tick(now);
        if (!motion.pointer.zero() && impl_->backends.output) {
            impl_->backends.output->moveCursorBy(motion.pointer.x, motion.pointer.y);
        }
        if (!motion.scroll.zero() && impl_->backends.output) {
            impl_->backends.output->scroll(motion.scroll.x, motion.scroll.y);
        }
    }

    impl_->drainBackendDiagnostics();
    impl_->publishWindowState(now);
    impl_->publishPointer(now);
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

    if (!impl_->started) {
        if (impl_->server) impl_->server->shutdown(reason);
        impl_->owner.release();
        return;
    }

    // The formal shutdown fallback. Every OS-visible obligation falls in
    // exactly one of three sets, and NONE of them requires the engine's owner
    // to answer:
    //
    //   1. Presses forwarded natively. Each is a key the user is still
    //      physically holding, so uninstalling the input backend is what
    //      discharges them -- the physical release then reaches the OS
    //      directly.
    //   2. Dispatcher state: buttons, drag locks, held directions, precision.
    //      Core-owned throughout.
    //   3. Keys and buttons this process synthesised. The output backend
    //      tracks its own and lifts them, which is the only way to release one
    //      that no physical key will ever release.
    //
    // The owner is asked to unwind first because it produces a tidier result,
    // but the wait is bounded and nothing below depends on it succeeding.
    impl_->submitAndWait(Control{Control::Kind::ReleaseAll, false, 0, nullptr});

    // Stop seeing input BEFORE draining, so no new work arrives mid-drain.
    if (impl_->backends.input) impl_->backends.input->stop();
    impl_->submitAndWait(Control{Control::Kind::Stop, false, 0, nullptr});

    impl_->drainWork();
    impl_->pendingDoubleClicks.clear();

    impl_->effects.clear();
    impl_->dispatcher.releaseAll(impl_->effects);
    for (const auto& effect : impl_->effects) {
        if (effect.kind == Effect::Kind::Button && impl_->backends.output) {
            impl_->backends.output->button(effect.button, effect.down);
        }
    }
    impl_->effects.clear();
    if (impl_->backends.output) impl_->backends.output->releaseAll();

    if (impl_->server) impl_->server->shutdown(reason);
    impl_->owner.release();
}

void Core::publishPhysicalForTests(const PhysicalRecord& record) {
    if (!impl_->publication.push(record)) {
        impl_->publicationDrops.fetch_add(1);
    }
}

void Core::pushWorkForTests(const WorkItem& item) { impl_->work.push(item); }

void Core::setEngineOwnerForTests(std::unique_ptr<EngineOwner> owner) {
    impl_->engineOwner = std::move(owner);
}

void Core::setWindowsForTests(const std::vector<WindowInfo>& windows) {
    impl_->lastWindows = windows;
    impl_->slots.update(windows);
}

void Core::applyWarpForTests(const Action& action) { impl_->applyWarp(action); }

void Core::applyWindowForTests(const Action& action) { impl_->applyWindow(action); }

void Core::doubleClickForTests(MouseButton button) {
    impl_->scheduleDoubleClick(button);
}

void Core::releaseAllForTests() { impl_->releaseEverything(); }

Server* Core::server() { return impl_->server.get(); }
const Diagnostics& Core::diagnostics() const { return impl_->diagnostics; }
const HelloInfo& Core::hello() const { return impl_->hello; }
bool Core::running() const { return impl_->started && !impl_->stopped; }

}  // namespace kgn

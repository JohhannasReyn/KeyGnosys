// The three platform capability interfaces.
//
// Principle P5: platform differences are backends, not #ifdef thickets. The
// layer engine depends only on what is declared here. Adding a platform means
// adding files under src/platform/<os>/ and one line in the factory -- it must
// never mean editing the engine.
//
// See docs/SPEC.md sections 6.2 and 8.

#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "kgn/diagnostics.hpp"
#include "kgn/hookchannel.hpp"
#include "kgn/keycode.hpp"

namespace kgn {

struct Point {
    int x = 0;
    int y = 0;
};

struct Rect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;

    [[nodiscard]] Point center() const { return {x + w / 2, y + h / 2}; }
};

struct MonitorInfo {
    int index = 0;
    Rect bounds;
    bool primary = false;
    std::string name;
};

using WindowId = std::uint64_t;

struct WindowInfo {
    WindowId id = 0;
    std::string process;    // executable name, lowercased
    std::string wmClass;    // X11 WM_CLASS; empty on Windows
    std::string title;
    int monitor = 0;
};

// What a backend can actually do. Reported to the UI verbatim so an
// unavailable capability is visible rather than silently absent (P6).
//
// Each interface reports its OWN properties, and only those. Reading a
// pointer-warp capability off the input backend would be asserting something
// the input backend has no way to know, which is how a build ends up claiming
// a capability nothing implements.
struct Capabilities {
    bool canSuppress = false;        // InputBackend
    bool canWarpAbsolute = false;    // OutputBackend
    bool canMoveWindows = false;     // WindowBackend
    // Human-readable, user-facing. e.g. "Cannot intercept keys while an
    // elevated window has focus." Surfaced in `hello.limitations`.
    std::vector<std::string> limitations;
};

// ---------------------------------------------------------------------------

// Observes physical key events and decides whether the OS should see them.
class InputBackend {
public:
    // Returns true to SUPPRESS the event (the OS never sees it).
    //
    // On Windows this runs inside the low-level hook, which must return within
    // the LowLevelHooksTimeout window or Windows silently unhooks us. The
    // handler must therefore decide and enqueue, and nothing more: no IPC, no
    // logging, no allocation on the hot path.
    using Handler = std::function<bool(KeyCode, KeyState)>;

    virtual ~InputBackend() = default;

    virtual bool start(Handler handler) = 0;
    virtual void stop() = 0;
    [[nodiscard]] virtual Capabilities capabilities() const = 0;

    // What `hello` calls this backend. A real name, so a client can tell a
    // hook-based build from a driver-based one rather than being told "input".
    [[nodiscard]] virtual std::string_view name() const = 0;

    // The engine owner this backend provides, or null when it does not own one.
    //
    // A backend that must answer the OS synchronously -- the Windows low-level
    // hook -- has to run the layer engine on its own thread, because the
    // suppression verdict is a function of engine state and the OS will not
    // wait. So the backend IS the owner, and the core submits control to it
    // rather than calling the engine directly. Declared here rather than
    // guessed at by the core, so a backend that does not need this simply
    // returns null and the core supplies a local owner.
    // Conditions the backend noticed on its own thread and could not report
    // there. A hook procedure must not log, allocate or touch IPC, so it
    // counts and the core drains. Called from the core loop.
    virtual void drainDiagnostics(Diagnostics&) {}

    [[nodiscard]] virtual std::unique_ptr<EngineOwner> engineOwner(
        WorkRing&, PublicationRing&, StatePublisher&, const EngineConfig&) {
        return nullptr;
    }
};

// Synthesizes input: pointer motion, buttons, scroll, and replayed keys.
class OutputBackend {
public:
    virtual ~OutputBackend() = default;

    virtual void moveCursorBy(int dx, int dy) = 0;
    virtual void moveCursorTo(int x, int y) = 0;
    [[nodiscard]] virtual Point cursorPosition() = 0;

    virtual void button(MouseButton button, bool down) = 0;
    virtual void scroll(int dx, int dy) = 0;
    virtual void sendKey(KeyCode code, bool down) = 0;

    // Release every key and mouse button this backend is currently holding
    // down. Principle P7: called on EVERY exit path -- mode change, config
    // reload, client disconnect, shutdown, and crash handler. A stranded key
    // press leaves the compositor believing a key is held forever.
    virtual void releaseAll() = 0;

    // The OS double-click interval. Lives here because only the platform can
    // know it, and the dispatcher must not block waiting for it.
    [[nodiscard]] virtual std::chrono::milliseconds doubleClickInterval() const = 0;

    // Conditions the backend noticed and could not report where it noticed
    // them. Synthesis that the OS refused belongs here: silence would leave a
    // key held with nothing saying so (P6).
    virtual void drainDiagnostics(Diagnostics&) {}

    [[nodiscard]] virtual Capabilities capabilities() const = 0;
    [[nodiscard]] virtual std::string_view name() const = 0;
};

// Enumerates and manipulates windows and monitors.
class WindowBackend {
public:
    virtual ~WindowBackend() = default;

    [[nodiscard]] virtual std::vector<WindowInfo> windows() = 0;
    [[nodiscard]] virtual std::optional<WindowInfo> focused() = 0;
    virtual bool focus(WindowId id) = 0;

    [[nodiscard]] virtual std::vector<MonitorInfo> monitors() = 0;
    virtual bool moveWindowToMonitor(WindowId id, int monitorIndex) = 0;

    [[nodiscard]] virtual Capabilities capabilities() const = 0;
    [[nodiscard]] virtual std::string_view name() const = 0;
};

// ---------------------------------------------------------------------------
// What a core is built out of. A null member means this build has no such
// backend, and the core reports that rather than substituting something that
// merely looks similar (P6).
//
// The FACTORY that fills this in does not live here -- see kgn/platform.hpp.
// Keeping it out is what lets kgn_ipc compose a core from backends it is given
// without knowing how to make any, so a test can build one from fakes with no
// platform library linked at all.
struct Backends {
    std::unique_ptr<InputBackend> input;
    std::unique_ptr<OutputBackend> output;
    std::unique_ptr<WindowBackend> window;
};

}  // namespace kgn

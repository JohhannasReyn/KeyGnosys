// The three platform capability interfaces.
//
// Principle P5: platform differences are backends, not #ifdef thickets. The
// layer engine depends only on what is declared here. Adding a platform means
// adding files under src/platform/<os>/ and one line in the factory -- it must
// never mean editing the engine.
//
// See docs/SPEC.md sections 6.2 and 8.

#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "mtk/keycode.hpp"

namespace mtk {

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
struct Capabilities {
    bool canSuppress = false;
    bool canWarpAbsolute = false;
    bool canMoveWindows = false;
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
};

// ---------------------------------------------------------------------------
// Factory. The single place that knows which platform is being built for.

struct Backends {
    std::unique_ptr<InputBackend> input;
    std::unique_ptr<OutputBackend> output;
    std::unique_ptr<WindowBackend> window;
};

// Returns null members for capabilities unavailable on this platform; the
// caller reports them rather than substituting something that merely looks
// similar.
Backends createBackends();

}  // namespace mtk

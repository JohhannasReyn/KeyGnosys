// The action catalog and the dispatcher that runs it.
//
// The catalog is the contract between bindings documents, this dispatcher and
// the overlay's legend renderer (SPEC section 7). The dispatcher turns layer
// engine decisions into platform-neutral *effects* -- it decides what should
// happen, and performs none of it. A later backend consumes the effects.
//
// Like the layer engine, this is pure: no OS API, no device, no thread.
//
// The fail-safe rule is inherited deliberately. Several actions create an
// obligation -- a button that must come back up, a drag lock that must be
// released, a direction that must stop. Where such an obligation cannot be
// recorded, the effect that would create it is NOT emitted. Half an obligation
// is a mouse button held down forever, which is strictly worse than an action
// that visibly did nothing.
//
// See docs/SPEC.md sections 6.4 and 7.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "kgn/clock.hpp"
#include "kgn/json.hpp"
#include "kgn/keycode.hpp"
#include "kgn/layer_engine.hpp"
#include "kgn/motion.hpp"

namespace kgn {

// ---------------------------------------------------------------------------
// Catalog

enum class ActionId : std::uint8_t {
    Unknown = 0,

    PointerMove,          // pointer.move          dir
    PointerPrecision,     // pointer.precision     -

    ButtonClick,          // button.click          button
    ButtonDoubleClick,    // button.double_click   button
    ButtonDragLock,       // button.drag_lock      button

    ScrollScroll,         // scroll.scroll         dir
    ScrollPage,           // scroll.page           dir (up|down)

    WarpGrid,             // warp.grid             cell 1-9
    WarpCorner,           // warp.corner           corner
    WarpMonitor,          // warp.monitor          target

    WindowCycle,          // window.cycle          dir (next|prev)
    WindowSlot,           // window.slot           index 1-9
    WindowFocusMonitor,   // window.focus_monitor  target
    WindowMoveToMonitor,  // window.move_to_monitor target

    LayerRelease,         // layer.release         -
    OverlayToggle,        // overlay.toggle        -
    SystemReload,         // system.reload         -
    KeyPassthrough,       // key.passthrough       code
};

enum class Corner : std::uint8_t { TopLeft, TopRight, BottomLeft, BottomRight, Center };

// `next` | `prev` | an explicit 0-based monitor index.
struct MonitorTarget {
    enum class Kind : std::uint8_t { Next, Prev, Index };
    Kind kind = Kind::Next;
    int index = 0;
};

// Cycle direction for window.cycle. Distinct from Direction, which is spatial.
enum class Cycle : std::uint8_t { Next, Prev };

// One resolved binding. Only the members meaningful for `id` are populated;
// which those are is fixed by the catalog, so nothing has to guess.
struct Action {
    ActionId id = ActionId::Unknown;

    Direction dir = Direction::Up;        // pointer.move, scroll.*
    MouseButton button = MouseButton::Left;  // button.*
    Corner corner = Corner::Center;       // warp.corner
    MonitorTarget target{};               // warp.monitor, window.*monitor
    Cycle cycle = Cycle::Next;            // window.cycle
    int index = 0;                        // warp.grid cell, window.slot index
    KeyCode code{};                       // key.passthrough
};

// Catalog names, exactly as they appear in a bindings document.
[[nodiscard]] std::string_view actionName(ActionId id);
[[nodiscard]] ActionId actionFromName(std::string_view name);

// Decode `params` for `name`. Returns false with a reason on an unknown action
// or an invalid parameter set; the caller skips that one binding and keeps the
// rest of the document (SPEC section 7, final paragraph).
bool parseAction(std::string_view name, const Json& params, Action& out,
                 std::string& error);

// How the engine should classify this action: `key.passthrough` reaches the OS
// inside the layer, everything else is suppressed and dispatched.
[[nodiscard]] BindingKind bindingKindFor(ActionId id);

// True for actions that are held rather than triggered -- those whose effect
// persists until the key comes back up.
[[nodiscard]] bool isHeldAction(ActionId id);

// ---------------------------------------------------------------------------
// Effects

// One large discrete scroll, in notches, for `scroll.page`.
//
// SPEC section 7.3 specifies "one large discrete scroll per press" without
// fixing the amount. Twelve notches is three times a typical wheel detent's
// three-line step, which lands close to a screenful in most applications
// without depending on any of them. Recorded here as the single place to
// change it if a real number ever supersedes the estimate.
inline constexpr int kPageScrollNotches = 12;

struct Effect {
    enum class Kind : std::uint8_t {
        Button,          // press or release `button`
        DoubleClick,     // two press/release pairs; timing belongs to the backend
        DragLock,        // drag lock on `button` became `down`
        Scroll,          // discrete scroll by (dx, dy) notches
        Warp,            // move the pointer; parameters in `action`
        Window,          // window or monitor operation; parameters in `action`
        LayerRelease,    // leave the cursor layer and release everything
        OverlayToggle,   // show/hide the keyboard window
        ReloadConfig,    // reload configuration
    };

    Kind kind = Kind::Button;
    MouseButton button = MouseButton::Left;
    bool down = false;
    int dx = 0;
    int dy = 0;
    // The originating binding, carried so the core can name the action in a
    // diagnostic when no backend can perform it.
    Action action{};
};

// Worst case is releaseAll() unwinding every held action, each of which can
// owe at most a button release and a drag-lock notification.
inline constexpr std::size_t kEffectCapacity = 2 * kMaxHeld + 16;

class EffectBuffer {
public:
    void clear() {
        size_ = 0;
        overflowed_ = false;
    }

    void push(const Effect& effect) {
        if (size_ < kEffectCapacity) {
            items_[size_++] = effect;
            return;
        }
        overflowed_ = true;
    }

    [[nodiscard]] std::size_t size() const { return size_; }
    [[nodiscard]] bool empty() const { return size_ == 0; }
    [[nodiscard]] bool overflowed() const { return overflowed_; }
    [[nodiscard]] const Effect& operator[](std::size_t i) const { return items_[i]; }
    [[nodiscard]] const Effect* begin() const { return items_.data(); }
    [[nodiscard]] const Effect* end() const { return items_.data() + size_; }

private:
    std::array<Effect, kEffectCapacity> items_{};
    std::size_t size_ = 0;
    bool overflowed_ = false;
};

// ---------------------------------------------------------------------------
// Dispatcher

struct TickResult {
    MotionDelta pointer;
    MotionDelta scroll;

    [[nodiscard]] bool zero() const { return pointer.zero() && scroll.zero(); }
};

class Dispatcher {
public:
    Dispatcher();

    // Replace the whole binding table. Like the engine's setBindings this is
    // atomic: there is no observable state in which half the bindings are new.
    // Anything currently held is released first, because a binding that has
    // gone must not leave its obligation behind (P7).
    void setBindings(const std::unordered_map<KeyCode, Action>& bindings,
                     EffectBuffer& out);

    void setPointerSettings(const MotionSettings& settings);
    void setScrollSettings(const MotionSettings& settings);

    // Feed one layer engine decision. Only RunAction and ReleaseAction carry
    // dispatch meaning; the rest describe what happened to the OS and are
    // ignored here.
    void onDecision(const Decision& decision, TimePoint now, EffectBuffer& out);

    // Advance the integrators one tick.
    TickResult tick(TimePoint now);

    // Release every obligation this dispatcher holds: buttons down, drag
    // locks, held directions, precision. Called on every exit path (P7).
    void releaseAll(EffectBuffer& out);

    [[nodiscard]] bool dragLockActive(MouseButton button) const;
    [[nodiscard]] bool buttonDown(MouseButton button) const;
    [[nodiscard]] std::size_t heldCount() const { return heldCount_; }
    [[nodiscard]] const Integrator& pointer() const { return pointer_; }
    [[nodiscard]] const Integrator& scroll() const { return scroll_; }

    // Non-zero means an action was refused because its obligation could not be
    // recorded. Unreachable from a physical keyboard; surfaced if it happens.
    [[nodiscard]] std::uint64_t capacityDrops() const { return capacityDrops_; }
    // Decisions naming a key with no binding, or a binding this build cannot
    // dispatch. Counted rather than acted on.
    [[nodiscard]] std::uint64_t unknownActions() const { return unknownActions_; }

private:
    static constexpr std::size_t kButtonCount = 3;

    struct Held {
        KeyCode code;
        Action action;
    };

    [[nodiscard]] static std::size_t buttonIndex(MouseButton button) {
        return static_cast<std::size_t>(button);
    }

    // Emits Button(down/up) only on the 0<->1 edge of "should this button be
    // down", which is the OR of the click refcount and the drag lock. Two keys
    // bound to left-click, or a click and a drag lock on one button, therefore
    // cannot produce a doubled press or a lost release.
    void syncButton(MouseButton button, EffectBuffer& out);

    bool beginHeld(KeyCode code, const Action& action);
    bool endHeld(KeyCode code, Action& action);

    void startAction(const Action& action, EffectBuffer& out, TimePoint now);
    void stopAction(const Action& action, EffectBuffer& out);

    std::unordered_map<KeyCode, Action> bindings_;

    Integrator pointer_;
    Integrator scroll_;

    // Refcounts, because one command may sit on several keys (SPEC section
    // 4.2). Releasing one key must not stop what another still holds.
    std::array<int, kButtonCount> clickHolders_{};
    std::array<bool, kButtonCount> dragLock_{};
    std::array<bool, kButtonCount> buttonDown_{};
    std::array<int, 4> directionHolders_{};    // indexed by Direction
    std::array<int, 4> scrollHolders_{};
    int precisionHolders_ = 0;

    std::array<Held, kMaxHeld> held_{};
    std::size_t heldCount_ = 0;

    std::uint64_t capacityDrops_ = 0;
    std::uint64_t unknownActions_ = 0;
};

}  // namespace kgn

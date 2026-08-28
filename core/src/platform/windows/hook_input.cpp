#include "hook_input.hpp"

#include <algorithm>

namespace kgn::win {

HookInput* HookInput::instance_ = nullptr;

// ---------------------------------------------------------------------------
// The engine owner face
//
// The core submits here; the hook thread applies. A control message is left in
// the ring until the thread wakes, and the thread never waits on the core --
// only the other way round, which is safe because it stalls the core loop and
// Windows measures only the hook callback.

class HookInput::Owner final : public EngineOwner {
public:
    explicit Owner(HookInput& hook) : hook_(hook) {}

    bool submit(const Control& control) override {
        if (!hook_.control_.push(control)) return false;
        ::SetEvent(hook_.wake_);
        return true;
    }

    bool awaitApplied(std::uint32_t seq, std::chrono::milliseconds timeout) override {
        // If the thread never started there is nobody to apply anything, and
        // saying so is better than a timeout the caller reads as a hang.
        if (!hook_.thread_.joinable()) return false;

        const DWORD budget = static_cast<DWORD>(
            std::max<std::int64_t>(0, timeout.count()));
        const DWORD deadline = ::GetTickCount() + budget;
        while (hook_.appliedSeq_.load(std::memory_order_acquire) < seq) {
            const DWORD now = ::GetTickCount();
            if (now >= deadline) {
                return hook_.appliedSeq_.load(std::memory_order_acquire) >= seq;
            }
            ::WaitForSingleObject(hook_.applied_, deadline - now);
        }
        return true;
    }

private:
    HookInput& hook_;
};

// ---------------------------------------------------------------------------

HookInput::HookInput()
    : engine_(config_), physicallyDown_(kKeyIdSpace, false) {
    wake_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);      // auto-reset
    applied_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

HookInput::~HookInput() {
    stop();
    if (wake_ != nullptr) ::CloseHandle(wake_);
    if (applied_ != nullptr) ::CloseHandle(applied_);
}

std::unique_ptr<EngineOwner> HookInput::engineOwner(WorkRing& work,
                                                    PublicationRing& publication,
                                                    StatePublisher& published,
                                                    const EngineConfig& config) {
    work_ = &work;
    publication_ = &publication;
    published_ = &published;
    config_ = config;
    engine_.setConfig(config_);
    republish();
    return std::make_unique<Owner>(*this);
}

bool HookInput::start(Handler) {
    // The Handler is unused: this backend owns the engine and answers Windows
    // itself. The seam stays in the interface for a backend that does not.
    if (work_ == nullptr) return false;      // engineOwner() was never called
    if (thread_.joinable()) return true;
    if (wake_ == nullptr || applied_ == nullptr) return false;

    instance_ = this;
    stopping_.store(false);
    thread_ = std::thread([this] { run(); });

    // Wait for the thread to have created its message queue and attempted the
    // hook, so start() reporting success means the hook is actually live.
    for (int i = 0; i < 200 && !ready_.load(); ++i) ::Sleep(1);
    return installed_.load();
}

void HookInput::stop() {
    if (!thread_.joinable()) return;
    stopping_.store(true);
    ::SetEvent(wake_);
    thread_.join();
    instance_ = nullptr;
}

Capabilities HookInput::capabilities() const {
    Capabilities capabilities;
    capabilities.canSuppress = true;
    // Surfaced in `hello` and in the UI rather than discovered by a user
    // wondering why the layer stopped working (SPEC section 8.2).
    capabilities.limitations.emplace_back(
        "Keys cannot be intercepted while an elevated window has focus, "
        "because this process is not elevated. The cursor layer is inert "
        "until focus returns to an ordinary window.");
    capabilities.limitations.emplace_back(
        "Ctrl+Alt+Del and the Secure Attention Sequence are never "
        "interceptable. This is by design in Windows, not a defect.");
    return capabilities;
}

std::uint64_t HookInput::takeAdmissionRefusals() {
    return admissionRefusals_.exchange(0);
}

std::uint64_t HookInput::takeHookLosses() { return hookLosses_.exchange(0); }

// ---------------------------------------------------------------------------
// The hook

LRESULT CALLBACK HookInput::trampoline(int code, WPARAM wParam, LPARAM lParam) {
    if (instance_ != nullptr) return instance_->onHook(code, wParam, lParam);
    return ::CallNextHookEx(nullptr, code, wParam, lParam);
}

LRESULT HookInput::onHook(int code, WPARAM wParam, LPARAM lParam) {
    if (code != HC_ACTION) return ::CallNextHookEx(nullptr, code, wParam, lParam);
    const auto* event = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);

    // Our own SendInput output, or it feeds straight back into this hook and
    // the core drives itself in a loop (SPEC section 8.2).
    if ((event->flags & LLKHF_INJECTED) != 0) {
        return ::CallNextHookEx(nullptr, code, wParam, lParam);
    }

    const bool up = wParam == WM_KEYUP || wParam == WM_SYSKEYUP;
    const KeyCode key = keymap_.toKeyCode(
        event->scanCode, (event->flags & LLKHF_EXTENDED) != 0, event->vkCode);
    if (!key.valid()) {
        // A key this build has no name for. Passed through untouched rather
        // than swallowed: refusing to forward what we cannot identify would
        // make an unknown key stop working entirely.
        return ::CallNextHookEx(nullptr, code, wParam, lParam);
    }

    // KBDLLHOOKSTRUCT carries no repeat count, so autorepeat arrives as a
    // second Down. Deriving Repeat here keeps ordinary held-key autorepeat on
    // the native passthrough path: without it the engine answers
    // Forward(c, Repeat) for a Down, the passthrough rule does not match, and
    // every autorepeat of every key gets suppressed and re-injected.
    KeyState state = up ? KeyState::Up : KeyState::Down;
    const std::size_t slot = key.id();
    if (!up && physicallyDown_[slot]) state = KeyState::Repeat;
    physicallyDown_[slot] = !up;

    if (!enabled_) {
        // The master switch. The engine is not consulted at all, so it creates
        // no obligations while disabled and restarts clean when re-enabled.
        return ::CallNextHookEx(nullptr, code, wParam, lParam);
    }

    // The admission gate. A Down or a Repeat may create an obligation, so it
    // is admitted only while the ring can certainly carry whatever the engine
    // emits. An Up DISCHARGES obligations and is never refused -- refusing one
    // is exactly how a key gets stranded, because the engine would keep a
    // record no later event can clear.
    if (state != KeyState::Up && work_->free() < kWorkAdmissionGate) {
        admissionRefusals_.fetch_add(1, std::memory_order_relaxed);
        publishPhysical(key, state, true);
        return 1;   // a missing keystroke, never a stranded one
    }

    decisions_.clear();
    engine_.onKey(key, state, Clock::now(), decisions_);
    const bool native = translateDecisions(decisions_, key, state, *work_);
    republish();
    publishPhysical(key, state, !native);

    if (native) return ::CallNextHookEx(nullptr, code, wParam, lParam);
    return 1;
}

void HookInput::publishPhysical(KeyCode code, KeyState state, bool suppressed) {
    PhysicalRecord record;
    record.code = code.id();
    record.state = state;
    record.suppressed = suppressed;
    record.version = published_->version();
    // A full publication ring drops the oldest by refusing the newest. Losing
    // one costs an overlay highlight; it can never cost a release, because no
    // release lives in this stream.
    publication_->push(record);
}

void HookInput::republish() {
    PublishedState state;
    state.mode = engine_.mode();
    state.latched = engine_.latched();
    state.activation = config_.activation;
    state.shift = physicallyDown_[KeyCode::fromString("ShiftLeft").id()] ||
                  physicallyDown_[KeyCode::fromString("ShiftRight").id()];
    state.control = physicallyDown_[KeyCode::fromString("ControlLeft").id()] ||
                    physicallyDown_[KeyCode::fromString("ControlRight").id()];
    state.alt = physicallyDown_[KeyCode::fromString("AltLeft").id()] ||
                physicallyDown_[KeyCode::fromString("AltRight").id()];
    state.meta = physicallyDown_[KeyCode::fromString("MetaLeft").id()] ||
                 physicallyDown_[KeyCode::fromString("MetaRight").id()];
    published_->publish(state);
}

// ---------------------------------------------------------------------------
// The thread

void HookInput::run() {
    // Force the message queue into existence before anything can post to it,
    // and before the hook is installed.
    MSG message;
    ::PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    install();
    ready_.store(true);

    while (!stopping_.load()) {
        // One wait serves both jobs: the event carries control messages, and
        // the timeout IS the grace-window deadline. QS_ALLINPUT is what lets
        // the system call the hook while we are here.
        ::MsgWaitForMultipleObjectsEx(1, &wake_, waitTimeout(), QS_ALLINPUT,
                                      MWMO_INPUTAVAILABLE);

        // Retrieving messages is what dispatches the hook.
        while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != 0) {
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }

        drainControl();
        expireGrace();

        // Windows removes a hook whose procedure overran its timeout, and does
        // it silently. Re-installing is the difference between a transient
        // stall and a layer that is dead until the next restart.
        if (!installed_.load() && !stopping_.load()) {
            hookLosses_.fetch_add(1, std::memory_order_relaxed);
            install();
        }
    }
    uninstall();
}

DWORD HookInput::waitTimeout() const {
    const std::optional<TimePoint> deadline = engine_.nextDeadline();
    if (!deadline.has_value()) return INFINITE;
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                               *deadline - Clock::now())
                               .count();
    if (remaining <= 0) return 0;
    return static_cast<DWORD>(std::min<std::int64_t>(remaining, 1000));
}

void HookInput::install() {
    hook_ = ::SetWindowsHookExW(WH_KEYBOARD_LL, &HookInput::trampoline,
                                ::GetModuleHandleW(nullptr), 0);
    installed_.store(hook_ != nullptr);
}

void HookInput::uninstall() {
    if (hook_ != nullptr) ::UnhookWindowsHookEx(hook_);
    hook_ = nullptr;
    installed_.store(false);
}

void HookInput::drainControl() {
    Control control{};
    while (control_.pop(control)) {
        // Every one of these can emit decisions, so each needs the same
        // headroom guarantee an event does. Deferring rather than dropping
        // keeps the message and retries on the next wake.
        if (work_->free() < kWorkAdmissionGate &&
            control.kind != Control::Kind::Stop) {
            ::SetEvent(wake_);   // come back to it
            return;
        }

        switch (control.kind) {
            case Control::Kind::SetConfig:
                config_ = *static_cast<const EngineConfig*>(control.payload);
                engine_.setConfig(config_);
                break;
            case Control::Kind::SetBindings:
                decisions_.clear();
                engine_.setBindings(*static_cast<const BindingMap*>(control.payload),
                                    decisions_);
                translateDecisions(decisions_, KeyCode{}, KeyState::Down, *work_);
                break;
            case Control::Kind::ReleaseAll:
                decisions_.clear();
                engine_.releaseAll(decisions_);
                translateDecisions(decisions_, KeyCode{}, KeyState::Down, *work_);
                std::fill(physicallyDown_.begin(), physicallyDown_.end(), false);
                break;
            case Control::Kind::SetEnabled:
                decisions_.clear();
                engine_.releaseAll(decisions_);
                translateDecisions(decisions_, KeyCode{}, KeyState::Down, *work_);
                std::fill(physicallyDown_.begin(), physicallyDown_.end(), false);
                enabled_ = control.flag;
                break;
            case Control::Kind::Stop:
                stopping_.store(true);
                break;
        }
        republish();
        appliedSeq_.store(control.seq, std::memory_order_release);
        ::SetEvent(applied_);
    }
}

void HookInput::expireGrace() {
    const std::optional<TimePoint> deadline = engine_.nextDeadline();
    if (!deadline.has_value() || Clock::now() < *deadline) return;
    if (work_->free() < kWorkAdmissionGate) return;   // retried next wake

    decisions_.clear();
    engine_.tick(Clock::now(), decisions_);
    // No physical event is in hand, so nothing here can be native and no
    // PhysicalRecord is produced -- a grace expiry is not a key the user just
    // pressed, and reporting it as one would light a key twice.
    translateDecisions(decisions_, KeyCode{}, KeyState::Down, *work_);
    republish();
}

}  // namespace kgn::win

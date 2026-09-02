#include "hook_input.hpp"

#include <algorithm>
#include <string>

namespace kgn::win {

namespace {
// Doorbells, not payload. The control ring stays authoritative for both the
// data and its ordering; these only wake the loop.
constexpr UINT kMsgControl   = WM_APP + 1;   // drain the control ring
constexpr UINT kMsgGraceSync = WM_APP + 2;   // re-evaluate the grace timer
}  // namespace

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
        // Push first: the push must be visible to a consumer that is woken by
        // the post below. See the ordering proof on kgn::Doorbell.
        if (!hook_.control_.push(control)) return false;
        hook_.requestWake();
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

HookInput::HookInput() : engine_(config_) {
    applied_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

HookInput::~HookInput() {
    stop();
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
    if (applied_ == nullptr) return false;

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
    // Unconditional, not doorbell-coalesced: shutdown must not depend on
    // whether a wake happened to be outstanding already.
    const DWORD tid = threadId_.load(std::memory_order_acquire);
    if (tid != 0) ::PostThreadMessageW(tid, kMsgControl, 0, 0);
    thread_.join();
    threadId_.store(0, std::memory_order_release);
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

void HookInput::drainDiagnostics(Diagnostics& out) {
    // Counted on the hook thread, reported here. One diagnostic per episode
    // rather than per event: a condition that repeats every keystroke would
    // otherwise flood the very channel meant to explain it.
    const std::uint64_t losses = hookLosses_.exchange(0);
    if (losses != 0) {
        out.emplace_back(DiagLevel::Warn, "input.hook_lost",
                         "Windows removed the keyboard hook " +
                             std::to_string(losses) +
                             " time(s); it has been re-installed");
    }
    const std::uint64_t refused = admissionRefusals_.exchange(0);
    if (refused != 0) {
        out.emplace_back(DiagLevel::Error, "input.queue_overflow",
                         "the core loop fell far enough behind that " +
                             std::to_string(refused) +
                             " key press(es) were suppressed rather than "
                             "risk an obligation that could not be delivered");
    }
    if (!installed_.load()) {
        out.emplace_back(DiagLevel::Error, "input.permission_denied",
                         "the keyboard hook is not installed; keys are not "
                         "intercepted");
    }
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
    const KeyState state = physical_.observe(key, up);

    // Published before the enabled check, and from PHYSICAL state. Disabling
    // is a master switch for interception, not for reporting: the overlay
    // should still show a modifier the user is holding.
    republish();

    if (!enabled_) {
        // The engine is not consulted at all, so it creates no obligations
        // while disabled and restarts clean when re-enabled. The physical
        // bitmap above still tracks the keyboard, which is why re-enabling
        // does not misread the next autorepeat as a fresh press.
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

    // A buffered press is the engine's only timed work. The loop is blocked in
    // GetMessageW around this callback and cannot see the new deadline by
    // itself, so nudge it -- but only on the transition, so ordinary typing
    // adds no syscall. nextDeadline() is pure; the post is one bounded,
    // non-blocking call, which is all the hook path is allowed.
    if (!graceTimerArmed_ && engine_.nextDeadline().has_value()) {
        requestGraceSync();
    }

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
    // Modifiers describe the KEYBOARD, not the engine. Every code compared
    // against was resolved once, in PhysicalKeyState's constructor: doing it
    // here took the intern table's lock eight times per key event, inside the
    // hook procedure that must not block on a lock at all.
    physical_.fillModifiers(state);
    published_->publish(state);
}

// ---------------------------------------------------------------------------
// The thread

void HookInput::run() {
    // Force the message queue into existence before anything can post to it.
    // PeekMessage on an empty range is the documented way to do that, and it
    // must happen before threadId_ becomes visible: a producer that reads a
    // thread id is entitled to assume a queue is behind it.
    MSG message;
    ::PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    threadId_.store(::GetCurrentThreadId(), std::memory_order_release);
    install();
    ready_.store(true, std::memory_order_release);

    // Anything submitted before the thread id was published got no post, on
    // purpose -- the producer had nowhere to send it. It is still in the ring,
    // so drain once here. Disarm first, as always.
    doorbell_.disarm();
    drainControl();
    syncGraceTimer();

    while (!stopping_.load()) {
        // GetMessageW, not MsgWaitForMultipleObjectsEx.
        //
        // WH_KEYBOARD_LL callbacks are delivered to the installing thread and
        // dispatched only while that thread is inside a message-RETRIEVAL
        // call. A wait is not a retrieval call. The previous design blocked in
        // MsgWaitForMultipleObjectsEx with the grace deadline as its timeout,
        // which meant that with no deadline pending it blocked forever and the
        // PeekMessageW below it was never reached -- the hook installed, said
        // it was healthy, and received nothing. Live validation on 2026-09-01
        // measured 0 callbacks against 529 seen by an independent hook over
        // the same keystrokes.
        //
        // GetMessageW blocks with zero wakeups when idle AND dispatches the
        // callback, which is why the timeout is no longer needed for anything.
        const BOOL got = ::GetMessageW(&message, nullptr, 0, 0);
        if (got == 0 || got == -1) break;      // WM_QUIT, or a queue error

        switch (message.message) {
            case kMsgControl:
                // Disarm BEFORE draining. The reverse order loses a control
                // pushed between "ring looks empty" and "flag cleared"; see
                // the proof on kgn::Doorbell and test_hookpump.
                doorbell_.disarm();
                drainControl();
                break;

            case kMsgGraceSync:
                break;   // the sync below is the whole point of this message

            case WM_TIMER:
                // Kill first, so the engine is consulted with armed == false
                // and a stale or coalesced WM_TIMER cannot re-arm itself into
                // a poll. expireGrace re-reads the clock and does nothing
                // unless a deadline has genuinely passed.
                killGraceTimer();
                expireGrace();
                break;

            default:
                ::TranslateMessage(&message);
                ::DispatchMessageW(&message);
                break;
        }

        if (stopping_.load()) break;
        syncGraceTimer();

        // Windows removes a hook whose procedure overran its timeout, and does
        // it silently. Re-installing is the difference between a transient
        // stall and a layer that is dead until the next restart.
        if (!installed_.load() && !stopping_.load()) {
            hookLosses_.fetch_add(1, std::memory_order_relaxed);
            install();
        }
    }

    killGraceTimer();
    uninstall();
    // Only here, and only because the hook is now gone: from this point the
    // bitmap describes a keyboard nobody is observing, so keeping it would be
    // stale rather than physical.
    physical_.forgetAll();
}

void HookInput::requestWake() {
    // One post per un-consumed episode. A thousand controls in a burst still
    // cost a single thread message, which is what keeps this clear of the
    // Windows 10 000-message queue limit.
    if (!doorbell_.arm()) return;
    const DWORD tid = threadId_.load(std::memory_order_acquire);
    // A zero id means the thread has not published itself yet, so it has not
    // reached its startup drain either -- and that drain runs after the id is
    // stored, so it is guaranteed to observe whatever was just pushed.
    if (tid != 0) ::PostThreadMessageW(tid, kMsgControl, 0, 0);
}

void HookInput::requestGraceSync() {
    // Called from the hook callback, which runs on this same thread while the
    // loop is blocked inside GetMessageW. Without this the loop would not
    // learn that a buffered press just created a deadline until some other
    // message happened along.
    const DWORD tid = threadId_.load(std::memory_order_relaxed);
    if (tid != 0) ::PostThreadMessageW(tid, kMsgGraceSync, 0, 0);
}

void HookInput::syncGraceTimer() {
    const GraceTimerPlan::Decision decision = GraceTimerPlan::decide(
        graceTimerArmed_, engine_.nextDeadline(), Clock::now());
    switch (decision.action) {
        case GraceTimerPlan::Action::Arm:
            graceTimer_ = ::SetTimer(nullptr, 0, decision.delayMs, nullptr);
            graceTimerArmed_ = graceTimer_ != 0;
            break;
        case GraceTimerPlan::Action::Kill:
            killGraceTimer();
            break;
        case GraceTimerPlan::Action::None:
            break;
    }
}

void HookInput::killGraceTimer() {
    if (!graceTimerArmed_) return;
    ::KillTimer(nullptr, graceTimer_);
    graceTimer_ = 0;
    graceTimerArmed_ = false;
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
            requestWake();   // come back to it
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
                // Engine obligations only. The physical bitmap is deliberately
                // untouched: release_all discharges what the software owes, it
                // does not lift the user's finger off a key. Clearing it would
                // make the next autorepeat look like a fresh press and publish
                // a held modifier as released.
                decisions_.clear();
                engine_.releaseAll(decisions_);
                translateDecisions(decisions_, KeyCode{}, KeyState::Down, *work_);
                break;
            case Control::Kind::SetEnabled:
                decisions_.clear();
                engine_.releaseAll(decisions_);
                translateDecisions(decisions_, KeyCode{}, KeyState::Down, *work_);
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

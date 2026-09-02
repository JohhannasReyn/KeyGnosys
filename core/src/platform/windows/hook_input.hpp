// The Windows low-level keyboard hook, and the thread that owns the engine.
//
// This backend is also the EngineOwner, and it has to be. Windows asks the
// hook procedure a synchronous question -- suppress this event or not -- and
// the answer is a function of mode, latch, grace and per-key state that only
// the layer engine holds. There is no way to answer it from another thread
// without making the hook wait, and a hook that waits past
// LowLevelHooksTimeout is silently unhooked.
//
// So the engine lives here, on this thread, and nothing else mutates it. The
// core submits control messages instead of calling it, and reads results off
// two rings and one atomic.
//
// What the hook procedure is allowed to do, in full: derive the physical
// state, ask the engine, translate the decisions, publish, return. No IPC, no
// logging, no allocation, no lock the core thread can hold.
//
// See docs/SPEC.md section 8.2 and
// docs/superpowers/specs/2026-08-28-m3-threading-design.md.

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "kgn/backends.hpp"
#include "kgn/hookchannel.hpp"
#include "kgn/hookpump.hpp"
#include "kgn/layer_engine.hpp"
#include "kgn/physical.hpp"
#include "scancode_keymap.hpp"

namespace kgn::win {

class HookInput final : public InputBackend {
public:
    HookInput();
    ~HookInput() override;

    HookInput(const HookInput&) = delete;
    HookInput& operator=(const HookInput&) = delete;

    bool start(Handler handler) override;
    void stop() override;
    [[nodiscard]] Capabilities capabilities() const override;
    [[nodiscard]] std::string_view name() const override { return "windows-hook"; }

    void drainDiagnostics(Diagnostics& out) override;

    [[nodiscard]] std::unique_ptr<EngineOwner> engineOwner(
        WorkRing& work, PublicationRing& publication, StatePublisher& published,
        const EngineConfig& config) override;

    // TEST-ONLY seams. Installing a real low-level hook inside a test would
    // need a desktop and a human, so these let the control path and the
    // physical bitmap be driven directly. Nothing in production calls them.
    // Mirrors what onHook does with a physical event, minus the engine: the
    // bitmap is updated and the state republished, in that order.
    KeyState observeForTests(KeyCode code, bool up) {
        const KeyState state = physical_.observe(code, up);
        republish();
        return state;
    }
    [[nodiscard]] bool physicallyDownForTests(KeyCode code) const {
        return physical_.down(code);
    }
    void applyControlForTests(const Control& control) {
        control_.push(control);
        drainControl();
    }
    [[nodiscard]] PublishedState publishedForTests() const {
        return published_ != nullptr ? published_->state() : PublishedState{};
    }

    // Counters the core turns into diagnostics. Read from the core thread;
    // written from the hook thread. Exchanged rather than merely read, so an
    // episode is reported once.
    [[nodiscard]] std::uint64_t takeAdmissionRefusals();

    // NARROW MEANING, and it matters: this is true iff we called
    // SetWindowsHookExW successfully and have not ourselves uninstalled it.
    //
    // It is NOT evidence that Windows is still dispatching callbacks. Windows
    // may silently remove a hook whose procedure overran LowLevelHooksTimeout,
    // and Microsoft documents that "there is no way for the application to know
    // whether the hook is removed". Nothing here can tell the difference
    // between a live hook and a removed one, so no code may treat this as
    // liveness.
    [[nodiscard]] bool installed() const { return installed_.load(); }

private:
    class Owner;
    friend class Owner;

    static LRESULT CALLBACK trampoline(int code, WPARAM wParam, LPARAM lParam);
    LRESULT onHook(int code, WPARAM wParam, LPARAM lParam);

    void run();
    void install();
    void uninstall();
    void drainControl();
    void expireGrace();
    void republish();
    void publishPhysical(KeyCode code, KeyState state, bool suppressed);

    // Wake the hook thread's message loop. Coalesced through doorbell_, so a
    // burst of controls costs one thread message, not one per control.
    void requestWake();
    // Post a bare wake so the loop re-evaluates the grace timer. Called from
    // the hook callback -- which runs on the hook thread, inside GetMessageW,
    // so the loop cannot otherwise notice that a deadline just appeared.
    void requestGraceSync();
    // Arm, leave or kill the thread timer to match the engine's next deadline.
    void syncGraceTimer();
    void killGraceTimer();

    // Only ever touched on the hook thread.
    ScancodeKeymap keymap_;
    // Declared before engine_, which is constructed from it.
    EngineConfig config_;
    LayerEngine engine_;
    DecisionBuffer decisions_;
    // What the HARDWARE is doing, as distinct from what the engine owes. No
    // control operation may clear it: release_all discharges obligations, it
    // does not lift the user's finger off a key.
    PhysicalKeyState physical_;
    bool enabled_ = true;

    // Shared with the core.
    WorkRing* work_ = nullptr;
    PublicationRing* publication_ = nullptr;
    StatePublisher* published_ = nullptr;
    ControlRing control_;

    HHOOK hook_ = nullptr;
    std::thread thread_;
    // No wake event any more. GetMessageW is the blocking primitive, because
    // only a message-RETRIEVAL call dispatches WH_KEYBOARD_LL callbacks; the
    // loop is woken by posted thread messages instead.
    HANDLE applied_ = nullptr;
    // Published before ready_, so a producer that sees a thread id knows the
    // queue behind it exists and can accept a post.
    std::atomic<DWORD> threadId_{0};
    Doorbell doorbell_;

    // Hook thread only.
    UINT_PTR graceTimer_ = 0;
    bool graceTimerArmed_ = false;
    std::atomic<std::uint32_t> appliedSeq_{0};
    std::atomic<bool> stopping_{false};
    std::atomic<bool> installed_{false};
    std::atomic<bool> ready_{false};
    std::atomic<std::uint64_t> admissionRefusals_{0};
    // Retries of an install that never succeeded -- NOT recoveries of a hook
    // Windows took away. See the comment on installed().
    std::atomic<std::uint64_t> installRetries_{0};

    // WH_KEYBOARD_LL gives the callback no user pointer, so the instance has
    // to be reachable from a static. Exactly one core owns the endpoint, so
    // exactly one of these exists.
    static HookInput* instance_;
};

}  // namespace kgn::win

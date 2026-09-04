// A one-shot deadline that wakes the hook thread's message loop.
//
// WHY THIS EXISTS. The grace deadline was first served by SetTimer/WM_TIMER.
// Live measurement showed that cost a roughly fixed ~21 ms median on the tested
// machine -- more than a system tick, because WM_TIMER is both clamped to the
// tick and generated only when the queue is otherwise empty. With a 50 ms grace
// window the user felt ~71 ms, so most of the observed latency was the timer
// rather than the window being measured. See the threading design note.
//
// A high-resolution waitable timer fixes the precision. It cannot, however,
// become the hook thread's wait primitive: that thread must stay inside
// GetMessageW, because a message-retrieval call is the only thing that
// dispatches WH_KEYBOARD_LL callbacks. So a small helper thread owns the timer
// and converts its expiry into a posted thread message.
//
// The helper owns NO engine state. It is a wake source and nothing else; every
// decision still happens on the hook thread, which remains the sole mutating
// owner of LayerEngine, grace state, physical state and suppression verdicts.

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <thread>

#include "kgn/clock.hpp"

namespace kgn::win {

class GraceTimer {
public:
    GraceTimer() = default;
    ~GraceTimer();

    GraceTimer(const GraceTimer&) = delete;
    GraceTimer& operator=(const GraceTimer&) = delete;

    // `message` is posted to `threadId` when a deadline expires, with the
    // generation that was armed in wParam. Returns false if the timer or its
    // helper could not be created, in which case the caller has no deadline
    // service and must say so rather than pretend.
    bool start(DWORD threadId, UINT message);
    void stop();

    // Both are called from the hook thread, outside the hook callback. They
    // take a mutex the helper also takes; it is held for two stores and never
    // across a syscall, and the CORE thread never touches it, so the rule that
    // the callback path holds no lock the core can contend is preserved.
    void arm(TimePoint deadline, std::uint32_t generation);
    void cancel(std::uint32_t generation);

    [[nodiscard]] bool highResolution() const { return highResolution_; }
    [[nodiscard]] bool running() const { return helper_.joinable(); }

private:
    void run();
    void applyPending();

    HANDLE timer_ = nullptr;     // auto-reset waitable timer
    HANDLE rearm_ = nullptr;     // auto-reset: "the pending deadline changed"
    HANDLE stop_ = nullptr;      // manual-reset: shutdown
    std::thread helper_;

    DWORD threadId_ = 0;
    UINT message_ = 0;
    bool highResolution_ = false;

    std::mutex pendingMutex_;
    std::optional<TimePoint> pendingDeadline_;
    std::uint32_t pendingGeneration_ = 0;

    // Helper thread only: the generation the live timer was armed for.
    std::uint32_t armedGeneration_ = 0;
};

}  // namespace kgn::win

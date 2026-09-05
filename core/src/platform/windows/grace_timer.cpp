#include "grace_timer.hpp"

#include <algorithm>

namespace kgn::win {
namespace {

// Documented as "supported in Windows 10, version 1803, and later". Defined
// here because older SDK headers may not carry it.
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

// SetWaitableTimer takes 100 ns units, negative meaning relative.
LARGE_INTEGER relativeDue(TimePoint deadline) {
    const auto remaining =
        std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - Clock::now())
            .count();
    // Never arm for the past or for zero: a zero-delay timer is a spin, and the
    // loop re-reads the clock when it wakes anyway.
    const std::int64_t hundredNs = std::max<std::int64_t>(remaining / 100, 1);
    LARGE_INTEGER due;
    due.QuadPart = -hundredNs;
    return due;
}

}  // namespace

GraceTimer::~GraceTimer() { stop(); }

bool GraceTimer::start(DWORD threadId, UINT message) {
    if (helper_.joinable()) return true;
    threadId_ = threadId;
    message_ = message;

    // High resolution first; fall back rather than fail, because an ordinary
    // waitable timer is still far better than WM_TIMER and a working coarse
    // deadline beats no deadline at all (P6: degrade, and say so).
    timer_ = ::CreateWaitableTimerExW(nullptr, nullptr,
                                      CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                      TIMER_ALL_ACCESS);
    highResolution_ = timer_ != nullptr;
    if (timer_ == nullptr) {
        timer_ = ::CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    }
    rearm_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);   // auto-reset
    stop_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);     // manual-reset
    if (timer_ == nullptr || rearm_ == nullptr || stop_ == nullptr) {
        stop();
        return false;
    }

    helper_ = std::thread([this] { run(); });
    return true;
}

void GraceTimer::stop() {
    if (helper_.joinable()) {
        ::SetEvent(stop_);
        helper_.join();
    }
    // Only after the helper is joined. Closing these while it could still wait
    // on them is how a shutdown races a timer into posting to a dead thread.
    if (timer_ != nullptr) { ::CancelWaitableTimer(timer_); ::CloseHandle(timer_); }
    if (rearm_ != nullptr) ::CloseHandle(rearm_);
    if (stop_ != nullptr) ::CloseHandle(stop_);
    timer_ = nullptr;
    rearm_ = nullptr;
    stop_ = nullptr;
    threadId_ = 0;
}

void GraceTimer::arm(TimePoint deadline, std::uint32_t generation) {
    {
        std::lock_guard<std::mutex> guard(pendingMutex_);
        pendingDeadline_ = deadline;
        pendingGeneration_ = generation;
    }
    if (rearm_ != nullptr) ::SetEvent(rearm_);
}

void GraceTimer::cancel(std::uint32_t generation) {
    {
        std::lock_guard<std::mutex> guard(pendingMutex_);
        pendingDeadline_.reset();
        pendingGeneration_ = generation;
    }
    if (rearm_ != nullptr) ::SetEvent(rearm_);
}

void GraceTimer::applyPending() {
    std::optional<TimePoint> deadline;
    std::uint32_t generation = 0;
    {
        std::lock_guard<std::mutex> guard(pendingMutex_);
        deadline = pendingDeadline_;
        generation = pendingGeneration_;
    }
    if (!deadline.has_value()) {
        ::CancelWaitableTimer(timer_);
        armedGeneration_ = generation;
        return;
    }
    LARGE_INTEGER due = relativeDue(*deadline);
    // SetWaitableTimer on an already-armed timer replaces the previous
    // deadline, so a rearm cannot leave two expiries outstanding.
    ::SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE);
    armedGeneration_ = generation;
}

void GraceTimer::run() {
    // stop_ first: on simultaneous signal, WaitForMultipleObjects reports the
    // lowest index, so shutdown wins over one more expiry.
    HANDLE handles[3] = {stop_, rearm_, timer_};
    while (true) {
        const DWORD which = ::WaitForMultipleObjects(3, handles, FALSE, INFINITE);
        if (which == WAIT_OBJECT_0) break;                  // stop
        if (which == WAIT_OBJECT_0 + 1) { applyPending(); continue; }
        if (which == WAIT_OBJECT_0 + 2) {
            // Re-check shutdown before posting. Without this a timer that fires
            // in the same instant as stop() could post to a thread that is on
            // its way out.
            if (::WaitForSingleObject(stop_, 0) == WAIT_OBJECT_0) break;
            // The generation travels with the message, so the hook thread can
            // discard an expiry belonging to a window it has already replaced.
            ::PostThreadMessageW(threadId_, message_,
                                 static_cast<WPARAM>(armedGeneration_), 0);
            continue;
        }
        break;   // WAIT_FAILED or abandoned: stop rather than spin
    }
}

}  // namespace kgn::win

#include "kgn/shutdown_coordinator.hpp"

#include <algorithm>
#include <utility>

namespace kgn {

// ---------------------------------------------------------------------------
// Attachment

ShutdownCoordinator::Attachment::Attachment(Attachment&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)) {}

ShutdownCoordinator::Attachment::~Attachment() { markUnwound(); }

void ShutdownCoordinator::Attachment::markUnwound() {
    if (owner_ == nullptr) return;
    owner_->markUnwound();
    owner_ = nullptr;
}

// ---------------------------------------------------------------------------
// Coordinator

ShutdownCoordinator::Attachment ShutdownCoordinator::attach(std::function<void()> stop) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != State::Detached) return Attachment(nullptr);
    stop_ = std::move(stop);
    state_ = State::Attached;
    return Attachment(this);
}

bool ShutdownCoordinator::invokeIfAttached() {
    std::function<void()> stop;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != State::Attached) return false;
        stop = stop_;
        ++inFlight_;
    }
    // Outside the lock, by design. markUnwound() cannot complete while this
    // caller is counted in-flight, so whatever the callback refers to is still
    // alive for the whole call.
    if (stop) stop();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        --inFlight_;
    }
    changed_.notify_all();
    return true;
}

bool ShutdownCoordinator::requestStop() { return invokeIfAttached(); }

ShutdownCoordinator::WaitResult ShutdownCoordinator::requestStopAndWait(
    std::chrono::milliseconds bound) {
    if (!invokeIfAttached()) return WaitResult::NotRunning;

    std::unique_lock<std::mutex> lock(mutex_);
    const bool done =
        changed_.wait_for(lock, bound, [this] { return state_ == State::Unwound; });
    return done ? WaitResult::Completed : WaitResult::TimedOut;
}

void ShutdownCoordinator::markUnwound() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (state_ != State::Attached) return;
    // Close the door first: no caller can enter the callback after this.
    state_ = State::Unwound;
    // Then wait out any caller already inside it.
    changed_.wait(lock, [this] { return inFlight_ == 0; });
    stop_ = nullptr;
    lock.unlock();
    changed_.notify_all();
}

bool ShutdownCoordinator::unwound() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_ == State::Unwound;
}

// ---------------------------------------------------------------------------

std::chrono::milliseconds closeWaitBound(long hungAppTimeoutMs) {
    const long timeout = hungAppTimeoutMs > 0 ? hungAppTimeoutMs : 5000;
    // At least 1 ms spare, so even a pathological 1 ms timeout yields a bound
    // strictly below it.
    const long spare = std::max(1L, std::min(1000L, timeout / 2));
    return std::chrono::milliseconds(timeout - spare);
}

}  // namespace kgn

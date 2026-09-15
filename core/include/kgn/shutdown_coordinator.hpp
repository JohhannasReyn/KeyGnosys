#pragma once

// Lets an asynchronous "stop now" request wait for the core's own unwind.
//
// Why it exists. On Windows, CTRL_CLOSE_EVENT (console window closed, Windows
// Terminal tab closed, Task Manager "End task" on a console process) runs the
// console handler on a thread the system creates, and the system terminates
// the process as soon as that handler returns. A handler that only sets the
// stop flag therefore returns before Core::stop() has released anything, and a
// synthesized mouse button stays down in Windows (M3 finding O-3). The fix is
// for the handler to wait, bounded, until the unwind has finished -- while the
// release itself stays on the core thread, which owns the dispatcher and the
// output backend.
//
// The coordinator carries no knowledge of Core. It holds one stop callback and
// a three-state lifecycle:
//
//     Detached  --attach()-->  Attached  --markUnwound()-->  Unwound
//
// Lifetime guarantee: once markUnwound() has returned, the stop callback is not
// running and will never run again. That is what makes it safe to destroy the
// object the callback refers to immediately afterwards. It is achieved WITHOUT
// running the callback under the coordinator's mutex: callers count themselves
// in-flight, release the lock, run the callback, and count themselves out;
// markUnwound() closes the door to new callers and then waits for in-flight ones
// to finish.
//
// Callback contract (deliberately narrow): it must return promptly, must not
// block, and must not call back into the coordinator -- markUnwound() waits for
// it, so a callback that did either could hold shutdown open or deadlock. The
// one intended callback is Core::requestStop(), an atomic store. This is not a
// general callback facility.
//
// Thread-safety: every member may be called from any thread, concurrently.
// Async-signal-safety: NONE -- it takes a mutex -- so POSIX signal handlers must
// not call it.

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>

namespace kgn {

class ShutdownCoordinator {
public:
    enum class WaitResult : std::uint8_t {
        // A core was attached when the request arrived, the stop callback ran,
        // and the unwind was marked complete within the bound.
        Completed,
        // A core was attached and the stop callback ran, but the unwind was not
        // marked complete within the bound.
        TimedOut,
        // No core was attached when the request arrived -- never attached, or
        // already unwound. The stop callback did not run; nothing needed waiting.
        NotRunning,
    };

    // Detaches on destruction, so every exit path after attach() -- normal
    // return, early return, an exception that unwinds the stack -- marks the
    // unwind complete. Declare it AFTER the object the callback refers to, so it
    // is destroyed first. (An uncaught exception that reaches std::terminate
    // without unwinding skips it; nothing in-process can cover that.)
    class Attachment {
    public:
        Attachment(Attachment&& other) noexcept;
        Attachment& operator=(Attachment&&) = delete;
        Attachment(const Attachment&) = delete;
        Attachment& operator=(const Attachment&) = delete;
        ~Attachment();

        // Marks the unwind complete now. Idempotent; the destructor calls it too.
        void markUnwound();

    private:
        friend class ShutdownCoordinator;
        explicit Attachment(ShutdownCoordinator* owner) : owner_(owner) {}
        ShutdownCoordinator* owner_;
    };

    ShutdownCoordinator() = default;
    ShutdownCoordinator(const ShutdownCoordinator&) = delete;
    ShutdownCoordinator& operator=(const ShutdownCoordinator&) = delete;

    // Attach the one stop callback. May be called once per coordinator; a
    // second call is a programming error and returns an inert attachment
    // without replacing the first callback.
    [[nodiscard]] Attachment attach(std::function<void()> stop);

    // Run the stop callback if a core is attached, and return at once.
    // Returns false when nothing is attached (the callback did not run).
    bool requestStop();

    // Run the stop callback if a core is attached, then wait up to `bound` for
    // markUnwound(). See WaitResult.
    WaitResult requestStopAndWait(std::chrono::milliseconds bound);

    // Whether markUnwound() has completed. Diagnostic only.
    [[nodiscard]] bool unwound() const;

private:
    enum class State : std::uint8_t { Detached, Attached, Unwound };

    // Enters the callback if attached; returns false (and runs nothing) if not.
    bool invokeIfAttached();
    void markUnwound();

    mutable std::mutex mutex_;
    std::condition_variable changed_;
    State state_ = State::Detached;
    std::function<void()> stop_;
    int inFlight_ = 0;
};

// How long a console close handler may wait before returning control to
// Windows, given the system's close timeout (SPI_GETHUNGAPPTIMEOUT, in ms).
// Always strictly below the timeout: it leaves min(1000 ms, timeout / 2) spare.
// A timeout of zero or less means "unknown" and uses Windows' 5000 ms default.
[[nodiscard]] std::chrono::milliseconds closeWaitBound(long hungAppTimeoutMs);

}  // namespace kgn

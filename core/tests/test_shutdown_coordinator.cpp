// The handshake that lets a console close wait for the core's unwind (O-3).
//
// Live M3 validation found that closing the core's console left a synthesized
// mouse button held: Windows ends the process as soon as the console handler
// returns, and the handler returned before Core::stop() had run. These tests pin
// the coordinator the handler now waits on -- its three outcomes, its lifetime
// guarantee, and the bound that keeps the wait inside Windows' own timeout.

#include "kgn/shutdown_coordinator.hpp"

#include "kgn_test.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

using namespace kgn;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using Result = ShutdownCoordinator::WaitResult;

namespace {

// ---------------------------------------------------------------------------
// Outcomes

KGN_TEST(a_request_with_nothing_attached_is_not_running_and_runs_nothing) {
    ShutdownCoordinator c;
    KGN_CHECK(!c.requestStop());
    KGN_CHECK(c.requestStopAndWait(50ms) == Result::NotRunning);
    KGN_CHECK(!c.unwound());
}

KGN_TEST(a_wait_completes_when_the_unwind_is_marked_from_another_thread) {
    ShutdownCoordinator c;
    std::atomic<int> stops{0};
    auto attachment = c.attach([&] { ++stops; });

    std::thread core([&] {
        std::this_thread::sleep_for(30ms);
        attachment.markUnwound();
    });
    const auto started = Clock::now();
    const Result result = c.requestStopAndWait(2000ms);
    const auto waited = Clock::now() - started;
    core.join();

    KGN_CHECK(result == Result::Completed);
    KGN_CHECK_EQ(stops.load(), 1);
    KGN_CHECK(waited < 1500ms);   // woke on the mark, not on the bound
    KGN_CHECK(c.unwound());
}

KGN_TEST(a_wait_that_is_never_marked_times_out_no_earlier_than_its_bound) {
    ShutdownCoordinator c;
    std::atomic<int> stops{0};
    auto attachment = c.attach([&] { ++stops; });

    const auto started = Clock::now();
    const Result result = c.requestStopAndWait(60ms);
    const auto waited = Clock::now() - started;

    // TimedOut is distinct from Completed: a core WAS attached and asked to
    // stop, and it did not finish. A diagnostic that confused the two would lie.
    KGN_CHECK(result == Result::TimedOut);
    KGN_CHECK_EQ(stops.load(), 1);
    KGN_CHECK(waited >= 60ms);
    KGN_CHECK(waited < 1000ms);
    KGN_CHECK(!c.unwound());
}

KGN_TEST(once_unwound_a_request_is_not_running_and_the_callback_never_runs_again) {
    ShutdownCoordinator c;
    std::atomic<int> stops{0};
    auto attachment = c.attach([&] { ++stops; });
    attachment.markUnwound();

    KGN_CHECK(!c.requestStop());
    KGN_CHECK(c.requestStopAndWait(50ms) == Result::NotRunning);
    KGN_CHECK_EQ(stops.load(), 0);
}

KGN_TEST(request_stop_runs_the_callback_once_and_returns_without_waiting) {
    ShutdownCoordinator c;
    std::atomic<int> stops{0};
    auto attachment = c.attach([&] { ++stops; });

    const auto started = Clock::now();
    KGN_CHECK(c.requestStop());
    KGN_CHECK(Clock::now() - started < 500ms);
    KGN_CHECK_EQ(stops.load(), 1);
    KGN_CHECK(!c.unwound());   // Ctrl+C semantics: ask, do not wait
}

// ---------------------------------------------------------------------------
// Attachment lifetime

KGN_TEST(destroying_the_attachment_marks_the_unwind_on_any_exit_path) {
    ShutdownCoordinator c;
    std::atomic<int> stops{0};
    {
        auto attachment = c.attach([&] { ++stops; });
        KGN_CHECK(!c.unwound());
    }   // an early return or a stack-unwinding exception leaves the same way
    KGN_CHECK(c.unwound());
    KGN_CHECK(c.requestStopAndWait(50ms) == Result::NotRunning);
    KGN_CHECK_EQ(stops.load(), 0);
}

KGN_TEST(a_moved_attachment_marks_the_unwind_exactly_once) {
    ShutdownCoordinator c;
    std::atomic<int> stops{0};
    {
        auto first = c.attach([&] { ++stops; });
        auto second = std::move(first);
        first.markUnwound();   // moved-from: inert
        KGN_CHECK(!c.unwound());
        KGN_CHECK(c.requestStop());
    }
    KGN_CHECK(c.unwound());
    KGN_CHECK_EQ(stops.load(), 1);
}

KGN_TEST(a_second_attach_is_inert_and_does_not_replace_the_first) {
    ShutdownCoordinator c;
    std::atomic<int> first{0}, second{0};
    auto attachment = c.attach([&] { ++first; });
    {
        auto extra = c.attach([&] { ++second; });
    }   // destroying the inert attachment must not unwind the real one
    KGN_CHECK(!c.unwound());
    KGN_CHECK(c.requestStop());
    KGN_CHECK_EQ(first.load(), 1);
    KGN_CHECK_EQ(second.load(), 0);
}

// The guarantee main() relies on to destroy Core right after the attachment:
// markUnwound() does not return while a callback is still running.
KGN_TEST(mark_unwound_waits_for_a_callback_already_in_flight) {
    ShutdownCoordinator c;
    std::atomic<bool> entered{false}, finished{false};
    auto attachment = c.attach([&] {
        entered = true;
        std::this_thread::sleep_for(100ms);   // deliberately slow, to widen the window
        finished = true;
    });

    std::thread handler([&] { c.requestStop(); });
    while (!entered) std::this_thread::yield();
    attachment.markUnwound();
    KGN_CHECK(finished.load());   // it waited
    handler.join();
    KGN_CHECK(!c.requestStop());   // and no new caller gets in
}

// The callback runs outside the coordinator's mutex. A callback that reads the
// coordinator would deadlock if it did not; it must finish instead.
KGN_TEST(the_callback_does_not_run_under_the_coordinator_mutex) {
    ShutdownCoordinator c;
    std::atomic<bool> readInside{false};
    auto attachment = c.attach([&] {
        (void)c.unwound();   // takes the mutex
        readInside = true;
    });

    auto done = std::async(std::launch::async, [&] { return c.requestStop(); });
    KGN_CHECK(done.wait_for(2000ms) == std::future_status::ready);
    KGN_CHECK(readInside.load());
}

// Many handlers racing the unwind: the object the callback touches is "destroyed"
// the moment markUnwound() returns, and no callback may observe it dead.
KGN_TEST(no_callback_ever_runs_after_mark_unwound_has_returned) {
    for (int round = 0; round < 200; ++round) {
        ShutdownCoordinator c;
        std::atomic<bool> alive{true};
        std::atomic<bool> violated{false};
        auto attachment = c.attach([&] {
            if (!alive.load()) violated = true;
        });

        std::atomic<bool> go{false};
        std::thread handler([&] {
            while (!go) std::this_thread::yield();
            for (int i = 0; i < 50; ++i) {
                (void)c.requestStopAndWait(0ms);
            }
        });
        go = true;
        std::this_thread::sleep_for(std::chrono::microseconds(round % 7 * 50));
        attachment.markUnwound();
        alive = false;   // the Core would be destroyed here
        handler.join();
        KGN_CHECK(!violated.load());
    }
}

// ---------------------------------------------------------------------------
// The bound

KGN_TEST(close_wait_bound_leaves_a_margin_below_the_windows_timeout) {
    KGN_CHECK(closeWaitBound(5000) == 4000ms);   // the Windows default
    KGN_CHECK(closeWaitBound(3000) == 2000ms);
    KGN_CHECK(closeWaitBound(1500) == 750ms);
    KGN_CHECK(closeWaitBound(1000) == 500ms);
    KGN_CHECK(closeWaitBound(2) == 1ms);
    KGN_CHECK(closeWaitBound(1) == 0ms);
}

KGN_TEST(close_wait_bound_is_strictly_below_every_timeout) {
    for (long timeout = 1; timeout <= 20000; ++timeout) {
        const auto bound = closeWaitBound(timeout);
        KGN_CHECK(bound.count() < timeout);
        KGN_CHECK(bound.count() >= 0);
    }
}

KGN_TEST(close_wait_bound_uses_the_windows_default_when_the_timeout_is_unknown) {
    KGN_CHECK(closeWaitBound(0) == 4000ms);
    KGN_CHECK(closeWaitBound(-1) == 4000ms);
}

}  // namespace

int main() { return kgn::test::runAll(); }

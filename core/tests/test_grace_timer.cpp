// The waitable-timer deadline service.
//
// This one CAN be tested in CI, unlike the hook itself: GraceTimer posts a
// thread message and owns no engine state, so the test thread can be its
// target and assert on what arrives. That covers arm, cancel, rearm, stale
// generations, shutdown races and precision without a desktop or a human.
//
// Precision is the reason the class exists. SetTimer/WM_TIMER cost a roughly
// fixed ~21 ms median on the machine where this was measured, which swamped a
// 50 ms grace window. These tests assert the deadline is not early and lands
// far closer than that.

#if defined(_WIN32)

#include "../src/platform/windows/grace_timer.hpp"

#include "kgn/hookpump.hpp"

#include "kgn_test.hpp"

#include <algorithm>
#include <chrono>
#include <vector>

using namespace kgn;
using kgn::win::GraceTimer;

namespace {

constexpr UINT kMsg = WM_APP + 71;

// Pumps for up to `ms`, collecting (generation, elapsed-ms) for every expiry.
struct Collected {
    std::vector<std::pair<std::uint32_t, double>> hits;
};

// Waits on the queue rather than polling with Sleep(1). Sleep's own
// granularity is ~15.6 ms, so a polling harness would measure itself and
// report the timer as far worse than it is -- which it did, on the first
// attempt at this test.
Collected pump(std::chrono::steady_clock::time_point from, int ms) {
    Collected out;
    MSG m;
    ::PeekMessageW(&m, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (true) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;
        const auto remaining = static_cast<DWORD>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
                .count());
        ::MsgWaitForMultipleObjectsEx(0, nullptr, remaining, QS_ALLPOSTMESSAGE,
                                      MWMO_INPUTAVAILABLE);
        while (::PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE) != 0) {
            if (m.message == kMsg) {
                out.hits.emplace_back(
                    static_cast<std::uint32_t>(m.wParam),
                    std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - from).count());
            }
        }
    }
    return out;
}

TimePoint inMs(int ms) { return Clock::now() + std::chrono::milliseconds(ms); }

}  // namespace

KGN_TEST(the_timer_starts_and_reports_whether_it_is_high_resolution) {
    GraceTimer t;
    KGN_CHECK(t.start(::GetCurrentThreadId(), kMsg));
    KGN_CHECK(t.running());
    // Not asserted as true: the flag is documented for Windows 10 1803+, and a
    // fallback to a coarse timer is a supported outcome, not a failure.
    (void)t.highResolution();
    t.stop();
    KGN_CHECK(!t.running());
}

KGN_TEST(an_armed_deadline_fires_once_with_its_generation) {
    GraceTimer t;
    KGN_CHECK(t.start(::GetCurrentThreadId(), kMsg));
    const auto t0 = std::chrono::steady_clock::now();
    t.arm(inMs(40), 5);
    const Collected c = pump(t0, 300);
    t.stop();

    KGN_CHECK_EQ(c.hits.size(), static_cast<std::size_t>(1));
    KGN_CHECK_EQ(c.hits[0].first, 5u);
}

KGN_TEST(the_deadline_is_never_early) {
    // The load-bearing property. A grace window that expires early resolves the
    // CapsLock race the wrong way; one that expires late merely feels slower.
    GraceTimer t;
    KGN_CHECK(t.start(::GetCurrentThreadId(), kMsg));
    for (int i = 0; i < 5; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        t.arm(inMs(30), static_cast<std::uint32_t>(100 + i));
        const Collected c = pump(t0, 300);
        KGN_CHECK_EQ(c.hits.size(), static_cast<std::size_t>(1));
        KGN_CHECK(c.hits[0].second >= 29.0);      // never before the deadline
    }
    t.stop();
}

KGN_TEST(the_deadline_lands_far_closer_than_the_old_wm_timer_did) {
    // WM_TIMER measured a ~21 ms median overshoot. A generous ceiling here
    // still fails loudly if someone puts SetTimer back.
    GraceTimer t;
    KGN_CHECK(t.start(::GetCurrentThreadId(), kMsg));
    std::vector<double> overshoot;
    for (int i = 0; i < 5; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        t.arm(inMs(30), static_cast<std::uint32_t>(200 + i));
        const Collected c = pump(t0, 300);
        KGN_CHECK_EQ(c.hits.size(), static_cast<std::size_t>(1));
        overshoot.push_back(c.hits[0].second - 30.0);
    }
    t.stop();
    std::sort(overshoot.begin(), overshoot.end());
    const double median = overshoot[overshoot.size() / 2];
    KGN_CHECK(median < 15.0);
}

KGN_TEST(a_cancelled_deadline_never_fires) {
    GraceTimer t;
    KGN_CHECK(t.start(::GetCurrentThreadId(), kMsg));
    const auto t0 = std::chrono::steady_clock::now();
    t.arm(inMs(40), 11);
    ::Sleep(5);
    t.cancel(12);
    const Collected c = pump(t0, 250);
    t.stop();
    KGN_CHECK(c.hits.empty());
}

KGN_TEST(rearming_replaces_the_deadline_rather_than_adding_one) {
    // Two arms must not produce two expiries: SetWaitableTimer on a live timer
    // replaces its deadline, and anything else would double-expire a window.
    GraceTimer t;
    KGN_CHECK(t.start(::GetCurrentThreadId(), kMsg));
    const auto t0 = std::chrono::steady_clock::now();
    t.arm(inMs(40), 21);
    ::Sleep(5);
    t.arm(inMs(60), 22);
    const Collected c = pump(t0, 400);
    t.stop();

    KGN_CHECK_EQ(c.hits.size(), static_cast<std::size_t>(1));
    KGN_CHECK_EQ(c.hits[0].first, 22u);          // the newest generation only
    KGN_CHECK(c.hits[0].second >= 59.0);         // and the newest deadline
}

KGN_TEST(a_rapid_rearm_storm_yields_exactly_one_expiry) {
    GraceTimer t;
    KGN_CHECK(t.start(::GetCurrentThreadId(), kMsg));
    const auto t0 = std::chrono::steady_clock::now();
    for (std::uint32_t g = 30; g < 60; ++g) {
        t.arm(inMs(50), g);
        ::Sleep(1);
    }
    const Collected c = pump(t0, 400);
    t.stop();
    KGN_CHECK_EQ(c.hits.size(), static_cast<std::size_t>(1));
    KGN_CHECK_EQ(c.hits[0].first, 59u);
}

KGN_TEST(an_expiry_that_arrives_for_a_replaced_generation_is_discarded) {
    // The consumer-side rule, exercised against real posted messages: whatever
    // arrives, only an expiry matching the current generation may be acted on.
    GraceTimer t;
    KGN_CHECK(t.start(::GetCurrentThreadId(), kMsg));
    const auto t0 = std::chrono::steady_clock::now();
    t.arm(inMs(20), 41);
    const Collected c = pump(t0, 200);
    t.stop();
    KGN_CHECK_EQ(c.hits.size(), static_cast<std::size_t>(1));

    const std::uint32_t arrived = c.hits[0].first;
    KGN_CHECK(GraceTimerPlan::acceptExpiry(arrived, 41));    // current: acted on
    KGN_CHECK(!GraceTimerPlan::acceptExpiry(arrived, 42));   // replaced: ignored
}

KGN_TEST(stopping_with_a_deadline_armed_posts_nothing_afterwards) {
    // Shutdown must not race an expiry into a thread on its way out. The helper
    // is joined before any handle it waits on is closed.
    GraceTimer t;
    KGN_CHECK(t.start(::GetCurrentThreadId(), kMsg));
    const auto t0 = std::chrono::steady_clock::now();
    t.arm(inMs(30), 51);
    t.stop();                       // armed, and deliberately not yet fired
    const Collected c = pump(t0, 200);
    KGN_CHECK(c.hits.empty());
}

KGN_TEST(stopping_twice_is_harmless) {
    GraceTimer t;
    KGN_CHECK(t.start(::GetCurrentThreadId(), kMsg));
    t.arm(inMs(20), 61);
    t.stop();
    t.stop();
    KGN_CHECK(!t.running());
}

KGN_TEST(an_idle_timer_posts_nothing_at_all) {
    // Zero idle wakeups, stated as a test: started but never armed must be
    // silent, or the design has become a poll again.
    GraceTimer t;
    KGN_CHECK(t.start(::GetCurrentThreadId(), kMsg));
    const auto t0 = std::chrono::steady_clock::now();
    const Collected c = pump(t0, 250);
    t.stop();
    KGN_CHECK(c.hits.empty());
}

KGN_TEST(a_deadline_already_past_still_fires_promptly) {
    GraceTimer t;
    KGN_CHECK(t.start(::GetCurrentThreadId(), kMsg));
    const auto t0 = std::chrono::steady_clock::now();
    t.arm(Clock::now() - std::chrono::milliseconds(10), 71);
    const Collected c = pump(t0, 200);
    t.stop();
    KGN_CHECK_EQ(c.hits.size(), static_cast<std::size_t>(1));
    KGN_CHECK(c.hits[0].second < 50.0);
}

int main() { return kgn::test::runAll(); }

#else
int main() { return 0; }
#endif

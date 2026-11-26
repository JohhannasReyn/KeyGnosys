// When to respawn a lost crash-release guardian. Pure policy: no clock is read
// and no process is touched here, so the whole schedule is testable with
// made-up time points.
//
// The schedule (spec "Decisions of 2026-11-25"): the first respawn 1 s after a
// loss, the second 5 s after, the third 30 s after; at most 3 attempts in any
// 10-minute window, after which there is no next attempt and the core stays
// Unprotected{restart_exhausted}. The window slides: once an attempt is more
// than 10 minutes old it no longer counts, against the budget or the backoff.

#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <deque>
#include <optional>

namespace kgn::win {

class RestartPolicy {
public:
    using Clock = std::chrono::steady_clock;

    static constexpr std::size_t kMaxAttempts = 3;
    static constexpr Clock::duration kWindow = std::chrono::minutes(10);

    // `start` is when supervision began. No attempt can predate it, so it
    // bounds the window: nothing older than it is ever counted.
    explicit RestartPolicy(Clock::time_point start) : start_(start) {}

    // When to make the next respawn attempt for a guardian lost (or a respawn
    // failed) at `lostAt`; nullopt once 3 attempts fall within the 10 minutes
    // before `lostAt`.
    [[nodiscard]] std::optional<Clock::time_point> nextAttempt(Clock::time_point lostAt) {
        prune(lostAt);
        if (attempts_.size() >= kMaxAttempts) return std::nullopt;
        static constexpr std::array<Clock::duration, kMaxAttempts> kBackoff = {
            std::chrono::seconds(1), std::chrono::seconds(5), std::chrono::seconds(30)};
        return lostAt + kBackoff[attempts_.size()];
    }

    void attemptMade(Clock::time_point at) {
        prune(at);
        attempts_.push_back(at);
    }

private:
    void prune(Clock::time_point now) {
        const Clock::time_point horizon = now - kWindow;
        while (!attempts_.empty() &&
               (attempts_.front() <= horizon || attempts_.front() < start_)) {
            attempts_.pop_front();
        }
    }

    Clock::time_point start_;
    std::deque<Clock::time_point> attempts_;
};

}  // namespace kgn::win

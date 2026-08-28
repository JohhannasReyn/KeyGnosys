// The motion integrator: held direction keys in, whole-unit deltas out.
//
// Pure, like the layer engine. It takes held directions and a clock and
// returns displacement; it touches no OS API and knows nothing about pointers
// or wheels. One instance drives the pointer and a second drives scrolling --
// same structure, its own settings, its own accumulator (SPEC section 6.4).
//
// Two properties are worth stating up front, because both are easy to get
// wrong in ways that look fine until someone uses it:
//
//   * A diagonal is not faster than a cardinal. The direction vector is
//     normalised, so holding two keys travels at the configured speed along
//     the diagonal rather than sqrt(2) times it.
//
//   * A speed below one unit per tick still moves. Fractional displacement
//     accumulates across ticks and is never discarded, so a slow drift emits a
//     pixel every few ticks rather than truncating to zero forever.
//
// See docs/SPEC.md section 6.4.

#pragma once

#include <chrono>
#include <cstdint>

#include "kgn/clock.hpp"

namespace kgn {

// The nominal cadence. Speeds are expressed per tick, so the caller owes the
// integrator one tick() per interval; the integrator does not measure how long
// the caller actually took, because a speed that varied with scheduler jitter
// would not be reproducible in a test or predictable under the hand.
inline constexpr std::chrono::microseconds kTickInterval{16667};   // ~60 Hz

enum class Direction : std::uint8_t {
    Up,
    Down,
    Left,
    Right,
};

struct MotionSettings {
    // Units per tick at the ramp floor and ceiling. "Unit" is pixels for the
    // pointer and notches for scrolling.
    double baseSpeed = 2.0;
    double maxSpeed = 28.0;
    std::chrono::milliseconds rampMs{420};
    // Multiplier applied while a `pointer.precision` binding is held. Applies
    // to pointer and scroll alike (SPEC section 7.1).
    double precisionFactor = 0.25;
};

struct MotionDelta {
    int x = 0;
    int y = 0;

    [[nodiscard]] bool zero() const { return x == 0 && y == 0; }
    bool operator==(const MotionDelta& other) const {
        return x == other.x && y == other.y;
    }
    bool operator!=(const MotionDelta& other) const { return !(*this == other); }
};

class Integrator {
public:
    explicit Integrator(MotionSettings settings = {});

    void setSettings(const MotionSettings& settings);
    [[nodiscard]] const MotionSettings& settings() const { return settings_; }

    // Direction held / released. Both are idempotent, so a key repeat does not
    // restart the ramp and a duplicate release does not underflow.
    void press(Direction direction, TimePoint now);
    void release(Direction direction);
    [[nodiscard]] bool held(Direction direction) const;

    void setPrecision(bool on) { precision_ = on; }
    [[nodiscard]] bool precision() const { return precision_; }

    // Advance one tick and return the whole units to emit. Sub-unit remainder
    // is carried, never dropped.
    MotionDelta tick(TimePoint now);

    // True while at least one direction is held. Note this is not the same as
    // "the last tick moved": a speed below one unit per tick is moving and
    // emitting nothing yet.
    [[nodiscard]] bool moving() const { return heldMask_ != 0; }

    // Ramp position in [0,1] as of `now`, before easing. Exposed for tests and
    // diagnostics; the tick path computes it internally.
    [[nodiscard]] double rampPosition(TimePoint now) const;

    // The speed one tick would use right now, after easing and precision.
    [[nodiscard]] double speedAt(TimePoint now) const;

    // Drop everything: held directions, ramp, and the carried remainder. This
    // is the P7 exit path, not the end of an ordinary motion -- see reset()
    // versus releaseDirections() below.
    void reset();

    // Release every direction but keep the carried remainder. This is what the
    // end of an ordinary motion does.
    //
    // The remainder deliberately survives. Discarding it is exactly how a
    // slow, repeatedly-tapped motion rounds to zero forever: each tap would
    // accumulate a fraction of a pixel and then throw it away, and the pointer
    // would never move at all. What survives is bounded by one unit.
    void releaseDirections();

    // The carried sub-unit remainder. Exposed so tests can assert that motion
    // is conserved rather than merely that pixels came out.
    [[nodiscard]] double residualX() const { return residualX_; }
    [[nodiscard]] double residualY() const { return residualY_; }

private:
    [[nodiscard]] std::uint8_t bit(Direction direction) const {
        return static_cast<std::uint8_t>(1u << static_cast<unsigned>(direction));
    }

    MotionSettings settings_;

    // Held directions as a bitmask. Opposing directions are kept, not
    // cancelled at press time, so releasing one of them resumes travel in the
    // other -- which is what the hand expects when it lets go of one key.
    std::uint8_t heldMask_ = 0;
    bool precision_ = false;

    // When the current continuous motion began. Ramp time is measured from the
    // FIRST direction of a motion, not from the most recent one, so changing
    // direction mid-travel does not drop the pointer back to a crawl
    // (SPEC section 6.4).
    TimePoint motionStart_{};
    bool inMotion_ = false;

    double residualX_ = 0.0;
    double residualY_ = 0.0;
};

}  // namespace kgn

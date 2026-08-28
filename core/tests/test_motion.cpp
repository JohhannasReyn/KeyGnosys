// The motion integrator.
//
// Everything here runs on a synthetic timeline, so the assertions are on exact
// values rather than on approximations. The properties that matter are the
// ones that are invisible until someone uses the thing: that a diagonal is not
// faster than a cardinal, and that a speed below one pixel per tick still
// eventually moves the pointer.

#include <cmath>
#include <cstdlib>
#include <tuple>
#include <utility>
#include <vector>

#include "kgn/motion.hpp"
#include "kgn_test.hpp"

using kgn::Direction;
using kgn::Integrator;
using kgn::MotionDelta;
using kgn::MotionSettings;
using kgn::TimePoint;

namespace {

TimePoint t0() { return TimePoint{} + std::chrono::seconds(1000); }

TimePoint at(long long ms) { return t0() + std::chrono::milliseconds(ms); }

// A ramp of zero milliseconds pins the integrator at full speed, which makes
// the speed constant and every displacement assertion exact.
MotionSettings flat(double speed) {
    MotionSettings settings;
    settings.baseSpeed = speed;
    settings.maxSpeed = speed;
    settings.rampMs = std::chrono::milliseconds(0);
    settings.precisionFactor = 0.25;
    return settings;
}

// Sum the deltas over `ticks` ticks, one nominal interval apart.
MotionDelta run(Integrator& integrator, int ticks, long long startMs = 0) {
    MotionDelta total;
    for (int i = 0; i < ticks; ++i) {
        const long long ms = startMs + (i + 1) * 17;
        const MotionDelta step = integrator.tick(at(ms));
        total.x += step.x;
        total.y += step.y;
    }
    return total;
}

}  // namespace

// ---------------------------------------------------------------------------
// Nothing held

KGN_TEST(a_tick_with_nothing_held_moves_nothing) {
    Integrator integrator(flat(10.0));
    KGN_CHECK(!integrator.moving());
    for (int i = 0; i < 10; ++i) {
        KGN_CHECK(integrator.tick(at(i * 17)).zero());
    }
}

KGN_TEST(opposing_directions_cancel_to_a_standstill) {
    Integrator integrator(flat(10.0));
    integrator.press(Direction::Left, t0());
    integrator.press(Direction::Right, t0());
    // Still "moving" -- the hand has not let go -- but going nowhere.
    KGN_CHECK(integrator.moving());
    KGN_CHECK(run(integrator, 10).zero());
}

KGN_TEST(releasing_one_of_two_opposing_keys_resumes_travel) {
    // Opposing keys are kept rather than cancelled at press time, so letting
    // go of one leaves the other still held.
    Integrator integrator(flat(10.0));
    integrator.press(Direction::Left, t0());
    integrator.press(Direction::Right, t0());
    KGN_CHECK(run(integrator, 3).zero());
    integrator.release(Direction::Right);
    KGN_CHECK_EQ(run(integrator, 3, 100).x, -30);
}

// ---------------------------------------------------------------------------
// Cardinal movement

KGN_TEST(cardinal_movement_travels_at_exactly_the_configured_speed) {
    for (const auto& [direction, dx, dy] :
         {std::tuple{Direction::Left, -40, 0}, std::tuple{Direction::Right, 40, 0},
          std::tuple{Direction::Up, 0, -40}, std::tuple{Direction::Down, 0, 40}}) {
        Integrator integrator(flat(10.0));
        integrator.press(direction, t0());
        const MotionDelta total = run(integrator, 4);
        KGN_CHECK_EQ(total.x, dx);
        KGN_CHECK_EQ(total.y, dy);
    }
}

KGN_TEST(a_repeated_press_of_a_held_direction_changes_nothing) {
    Integrator integrator(flat(10.0));
    integrator.press(Direction::Right, t0());
    integrator.press(Direction::Right, at(500));   // a key repeat
    KGN_CHECK_EQ(run(integrator, 2).x, 20);
}

KGN_TEST(a_duplicate_release_does_not_underflow) {
    Integrator integrator(flat(10.0));
    integrator.press(Direction::Right, t0());
    integrator.release(Direction::Right);
    integrator.release(Direction::Right);
    KGN_CHECK(!integrator.moving());
    KGN_CHECK(run(integrator, 4).zero());
}

// ---------------------------------------------------------------------------
// Diagonals

KGN_TEST(a_diagonal_is_not_faster_than_a_cardinal) {
    // The whole point of normalising. Without it a diagonal would travel
    // sqrt(2) times as far in the same time.
    Integrator diagonal(flat(10.0));
    diagonal.press(Direction::Right, t0());
    diagonal.press(Direction::Down, t0());
    const MotionDelta moved = run(diagonal, 10);

    const double distance =
        std::sqrt(static_cast<double>(moved.x) * moved.x +
                  static_cast<double>(moved.y) * moved.y);
    // Ten ticks at 10 px/tick is 100 px of travel along the diagonal.
    KGN_CHECK(std::fabs(distance - 100.0) < 1.5);
    // And the two axes share it equally.
    KGN_CHECK_EQ(moved.x, moved.y);
}

KGN_TEST(all_four_diagonals_normalise_the_same_way) {
    const std::pair<Direction, Direction> pairs[] = {
        {Direction::Right, Direction::Down},
        {Direction::Right, Direction::Up},
        {Direction::Left, Direction::Down},
        {Direction::Left, Direction::Up},
    };
    for (const auto& [a, b] : pairs) {
        Integrator integrator(flat(10.0));
        integrator.press(a, t0());
        integrator.press(b, t0());
        const MotionDelta moved = run(integrator, 10);
        KGN_CHECK_EQ(std::abs(moved.x), 70);   // 100 / sqrt(2), truncated
        KGN_CHECK_EQ(std::abs(moved.y), 70);
    }
}

KGN_TEST(three_directions_held_resolve_to_the_surviving_cardinal) {
    // Left+Right cancel; Down survives, and must travel at full cardinal speed
    // rather than at a normalised fraction of it.
    Integrator integrator(flat(10.0));
    integrator.press(Direction::Left, t0());
    integrator.press(Direction::Right, t0());
    integrator.press(Direction::Down, t0());
    const MotionDelta moved = run(integrator, 4);
    KGN_CHECK_EQ(moved.x, 0);
    KGN_CHECK_EQ(moved.y, 40);
}

// ---------------------------------------------------------------------------
// The ramp

KGN_TEST(the_ramp_runs_from_base_to_max_and_is_monotonic) {
    MotionSettings settings;
    settings.baseSpeed = 2.0;
    settings.maxSpeed = 28.0;
    settings.rampMs = std::chrono::milliseconds(400);
    Integrator integrator(settings);
    integrator.press(Direction::Right, t0());

    KGN_CHECK(std::fabs(integrator.speedAt(at(0)) - 2.0) < 1e-9);
    KGN_CHECK(std::fabs(integrator.speedAt(at(400)) - 28.0) < 1e-9);
    KGN_CHECK(std::fabs(integrator.speedAt(at(4000)) - 28.0) < 1e-9);   // clamped

    double previous = -1.0;
    for (long long ms = 0; ms <= 400; ms += 10) {
        const double speed = integrator.speedAt(at(ms));
        KGN_CHECK(speed >= previous);
        previous = speed;
    }
}

KGN_TEST(the_ramp_is_eased_so_the_first_moments_stay_precise) {
    MotionSettings settings;
    settings.baseSpeed = 0.0;
    settings.maxSpeed = 100.0;
    settings.rampMs = std::chrono::milliseconds(100);
    Integrator integrator(settings);
    integrator.press(Direction::Right, t0());
    // Quadratic-in: half way through the ramp is a quarter of the range, not
    // half of it. A linear ramp would give 50 here.
    KGN_CHECK(std::fabs(integrator.speedAt(at(50)) - 25.0) < 1e-9);
}

KGN_TEST(ramp_time_is_measured_from_the_first_direction_of_a_motion) {
    // Changing direction mid-travel must not drop the pointer back to a crawl.
    MotionSettings settings;
    settings.baseSpeed = 1.0;
    settings.maxSpeed = 101.0;
    settings.rampMs = std::chrono::milliseconds(100);
    Integrator integrator(settings);

    integrator.press(Direction::Right, t0());
    const double atCeiling = integrator.speedAt(at(100));
    KGN_CHECK(std::fabs(atCeiling - 101.0) < 1e-9);

    // Add a second direction well into the motion; the ramp keeps its origin.
    integrator.press(Direction::Down, at(100));
    KGN_CHECK(std::fabs(integrator.speedAt(at(100)) - 101.0) < 1e-9);

    // Release the first; the second alone still inherits the ramp.
    integrator.release(Direction::Right);
    KGN_CHECK(std::fabs(integrator.speedAt(at(120)) - 101.0) < 1e-9);
}

KGN_TEST(the_ramp_restarts_only_after_a_full_stop) {
    MotionSettings settings;
    settings.baseSpeed = 1.0;
    settings.maxSpeed = 101.0;
    settings.rampMs = std::chrono::milliseconds(100);
    Integrator integrator(settings);

    integrator.press(Direction::Right, t0());
    KGN_CHECK(std::fabs(integrator.speedAt(at(100)) - 101.0) < 1e-9);
    integrator.release(Direction::Right);
    // A new motion begins at the floor again.
    integrator.press(Direction::Right, at(200));
    KGN_CHECK(std::fabs(integrator.speedAt(at(200)) - 1.0) < 1e-9);
}

KGN_TEST(a_zero_length_ramp_pins_the_speed_at_the_ceiling) {
    Integrator integrator(flat(7.0));
    integrator.press(Direction::Right, t0());
    KGN_CHECK(std::fabs(integrator.speedAt(at(0)) - 7.0) < 1e-9);
}

// ---------------------------------------------------------------------------
// Precision

KGN_TEST(precision_multiplies_the_speed_by_its_factor) {
    Integrator integrator(flat(8.0));
    integrator.press(Direction::Right, t0());
    KGN_CHECK_EQ(run(integrator, 2).x, 16);

    integrator.setPrecision(true);
    KGN_CHECK(std::fabs(integrator.speedAt(at(100)) - 2.0) < 1e-9);
    KGN_CHECK_EQ(run(integrator, 2, 100).x, 4);

    integrator.setPrecision(false);
    KGN_CHECK_EQ(run(integrator, 2, 200).x, 16);
}

// ---------------------------------------------------------------------------
// Fractional accumulation
//
// This is where a naive implementation fails silently: a speed under one pixel
// per tick truncates to nothing and the pointer never moves at all.

KGN_TEST(a_sub_pixel_speed_still_moves_the_pointer) {
    // 0.125 rather than 0.1 so the assertion is about the accumulator and not
    // about binary floating point: an eighth is exact, a tenth is not.
    Integrator integrator(flat(0.125));
    integrator.press(Direction::Right, t0());

    // Nothing for the first seven ticks, then exactly one pixel.
    for (int i = 0; i < 7; ++i) {
        KGN_CHECK(integrator.tick(at((i + 1) * 17)).zero());
    }
    KGN_CHECK_EQ(integrator.tick(at(136)).x, 1);
}

KGN_TEST(fractional_motion_is_conserved_over_a_long_run) {
    // 1000 ticks at 0.375 px/tick is 375 px, and not one of them may be lost
    // to truncation along the way.
    Integrator integrator(flat(0.375));
    integrator.press(Direction::Right, t0());
    const MotionDelta total = run(integrator, 1000);
    KGN_CHECK_EQ(total.x, 375);
    KGN_CHECK(std::fabs(integrator.residualX()) < 1e-6);
}

KGN_TEST(diagonal_fractions_are_conserved_too) {
    Integrator integrator(flat(1.0));
    integrator.press(Direction::Right, t0());
    integrator.press(Direction::Down, t0());
    const MotionDelta total = run(integrator, 1000);
    // 1000 ticks at 1/sqrt(2) per axis is 707 px on each.
    KGN_CHECK_EQ(total.x, 707);
    KGN_CHECK_EQ(total.y, 707);
}

KGN_TEST(the_remainder_survives_the_end_of_a_motion) {
    // Tapping a slow direction repeatedly must accumulate. Discarding the
    // remainder at each release is exactly how such a motion rounds to zero
    // forever.
    Integrator integrator(flat(0.25));
    int moved = 0;
    for (int tap = 0; tap < 8; ++tap) {
        integrator.press(Direction::Right, at(tap * 100));
        moved += integrator.tick(at(tap * 100 + 17)).x;
        integrator.releaseDirections();
    }
    KGN_CHECK_EQ(moved, 2);   // 8 taps x 0.25 px
}

KGN_TEST(a_reversal_carries_its_fraction_across_zero) {
    // Truncation toward zero, not floor: a reversal must not gain or lose a
    // pixel at the sign change.
    Integrator integrator(flat(0.5));
    integrator.press(Direction::Right, t0());
    KGN_CHECK(integrator.tick(at(17)).zero());       // +0.5 banked
    KGN_CHECK_EQ(integrator.tick(at(34)).x, 1);      // +1.0 -> emit 1
    integrator.release(Direction::Right);

    integrator.press(Direction::Left, at(50));
    KGN_CHECK(integrator.tick(at(67)).zero());       // -0.5
    KGN_CHECK_EQ(integrator.tick(at(84)).x, -1);     // -1.0 -> emit -1
}

KGN_TEST(reset_discards_the_remainder_but_release_does_not) {
    Integrator integrator(flat(0.5));
    integrator.press(Direction::Right, t0());
    integrator.tick(at(17));
    KGN_CHECK(std::fabs(integrator.residualX() - 0.5) < 1e-9);

    integrator.releaseDirections();
    KGN_CHECK(std::fabs(integrator.residualX() - 0.5) < 1e-9);

    integrator.reset();
    KGN_CHECK(std::fabs(integrator.residualX()) < 1e-9);
    KGN_CHECK(!integrator.moving());
    KGN_CHECK(!integrator.precision());
}

// ---------------------------------------------------------------------------
// Determinism

KGN_TEST(the_same_timeline_produces_the_same_displacement_every_time) {
    const auto play = []() {
        MotionSettings settings;
        settings.baseSpeed = 2.0;
        settings.maxSpeed = 28.0;
        settings.rampMs = std::chrono::milliseconds(420);
        Integrator integrator(settings);
        std::vector<MotionDelta> steps;
        integrator.press(Direction::Right, t0());
        for (int i = 0; i < 30; ++i) steps.push_back(integrator.tick(at(i * 17)));
        integrator.press(Direction::Down, at(500));
        for (int i = 30; i < 60; ++i) steps.push_back(integrator.tick(at(i * 17)));
        integrator.release(Direction::Right);
        for (int i = 60; i < 90; ++i) steps.push_back(integrator.tick(at(i * 17)));
        return steps;
    };
    KGN_CHECK(play() == play());
}

KGN_TEST(speed_does_not_depend_on_how_late_a_tick_arrives) {
    // Speed is per tick, not per elapsed millisecond, so a late tick moves the
    // pointer the same distance. A speed that varied with scheduler jitter
    // would be neither reproducible nor predictable under the hand.
    Integrator a(flat(5.0));
    Integrator b(flat(5.0));
    a.press(kgn::Direction::Right, t0());
    b.press(kgn::Direction::Right, t0());
    KGN_CHECK_EQ(a.tick(at(17)).x, 5);
    KGN_CHECK_EQ(b.tick(at(900)).x, 5);
}

int main() { return kgn::test::runAll(); }

#include "kgn/motion.hpp"

#include <algorithm>
#include <cmath>

namespace kgn {
namespace {

// Truncation toward zero, with the remainder left behind. Using trunc rather
// than round or floor is what makes the accumulator symmetric: a reversal
// carries its fraction across zero instead of gaining or losing a unit at the
// sign change.
int takeWhole(double& accumulator) {
    const double whole = std::trunc(accumulator);
    accumulator -= whole;
    return static_cast<int>(whole);
}

}  // namespace

Integrator::Integrator(MotionSettings settings) : settings_(settings) {}

void Integrator::setSettings(const MotionSettings& settings) {
    settings_ = settings;
}

bool Integrator::held(Direction direction) const {
    return (heldMask_ & bit(direction)) != 0;
}

void Integrator::press(Direction direction, TimePoint now) {
    const std::uint8_t mask = bit(direction);
    if ((heldMask_ & mask) != 0) return;   // already held; a repeat changes nothing
    heldMask_ = static_cast<std::uint8_t>(heldMask_ | mask);
    if (!inMotion_) {
        inMotion_ = true;
        motionStart_ = now;
    }
}

void Integrator::release(Direction direction) {
    const std::uint8_t mask = bit(direction);
    if ((heldMask_ & mask) == 0) return;
    heldMask_ = static_cast<std::uint8_t>(heldMask_ & static_cast<std::uint8_t>(~mask));
    if (heldMask_ == 0) inMotion_ = false;
}

void Integrator::releaseDirections() {
    heldMask_ = 0;
    inMotion_ = false;
    // residual_ deliberately survives -- see the header.
}

void Integrator::reset() {
    heldMask_ = 0;
    inMotion_ = false;
    precision_ = false;
    residualX_ = 0.0;
    residualY_ = 0.0;
}

double Integrator::rampPosition(TimePoint now) const {
    if (!inMotion_) return 0.0;
    if (settings_.rampMs.count() <= 0) return 1.0;
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             now - motionStart_)
                             .count();
    if (elapsed <= 0) return 0.0;
    const double position = static_cast<double>(elapsed) /
                            static_cast<double>(settings_.rampMs.count());
    return std::clamp(position, 0.0, 1.0);
}

double Integrator::speedAt(TimePoint now) const {
    const double position = rampPosition(now);
    // Quadratic-in easing: the first moments stay precise and the tail is
    // fast, so one binding serves both "nudge two pixels" and "cross three
    // monitors" (SPEC section 6.4 step 2).
    const double eased = position * position;
    double speed = settings_.baseSpeed +
                   (settings_.maxSpeed - settings_.baseSpeed) * eased;
    if (precision_) speed *= settings_.precisionFactor;
    return speed;
}

MotionDelta Integrator::tick(TimePoint now) {
    if (heldMask_ == 0) return {};

    // 1. Sum the unit direction vectors of everything held. Opposing keys
    //    cancel here, which is where a left+right chord correctly stops.
    double dx = 0.0;
    double dy = 0.0;
    if (held(Direction::Left)) dx -= 1.0;
    if (held(Direction::Right)) dx += 1.0;
    if (held(Direction::Up)) dy -= 1.0;
    if (held(Direction::Down)) dy += 1.0;

    const double length = std::sqrt(dx * dx + dy * dy);
    if (length == 0.0) {
        // Held, but cancelling out. Emit nothing and leave the remainder
        // alone; the ramp keeps running, because the hand has not let go.
        return {};
    }

    // 2-4. Ramp, precision, then normalise so a diagonal travels at the
    //      configured speed rather than sqrt(2) times it.
    const double speed = speedAt(now);
    dx = dx / length * speed;
    dy = dy / length * speed;

    // 5. Accumulate fractional units and emit only whole ones.
    residualX_ += dx;
    residualY_ += dy;
    return {takeWhole(residualX_), takeWhole(residualY_)};
}

}  // namespace kgn

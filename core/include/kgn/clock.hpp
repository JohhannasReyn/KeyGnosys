// The one clock the core measures time against.
//
// Steady, not system: every interval the core cares about -- the grace window,
// the hybrid tap threshold, the motion ramp -- is a duration between two of its
// own observations, and a wall clock that a time-zone change or an NTP step can
// move backwards would corrupt all three.
//
// Nothing here reads the clock. Every module that needs a time takes one as a
// parameter, which is what makes the engine and the integrator testable on a
// synthetic timeline (SPEC section 13).

#pragma once

#include <chrono>

namespace kgn {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

}  // namespace kgn

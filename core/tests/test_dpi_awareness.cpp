// The core must see physical pixels on every monitor (M3 finding O-11).
//
// A DPI-unaware process is handed virtualised, logical coordinates on any
// monitor scaled above 100%: a 3840x1080 panel at 150% reads as 2560x720, and
// warps, slot moves and the monitor list are all computed in the wrong space.
// Declaring per-monitor-v2 awareness at startup is what makes GetMonitorInfo,
// GetCursorPos, GetSystemMetrics and SetWindowPos speak physical pixels.

#if defined(_WIN32)

#include "../src/platform/windows/dpi_awareness.hpp"

#include "kgn_test.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace {

bool processIsPerMonitorV2() {
    return ::AreDpiAwarenessContextsEqual(
               ::GetThreadDpiAwarenessContext(),
               DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != FALSE;
}

}  // namespace

KGN_TEST(enabling_makes_the_process_per_monitor_v2_aware) {
    KGN_CHECK(kgn::win::enablePerMonitorDpiAwareness());
    KGN_CHECK(processIsPerMonitorV2());
}

KGN_TEST(enabling_twice_is_still_success) {
    // Windows refuses a second SetProcessDpiAwarenessContext with
    // ERROR_ACCESS_DENIED. Already being per-monitor-v2 is the goal, not a
    // failure, so a second call must report success.
    KGN_CHECK(kgn::win::enablePerMonitorDpiAwareness());
    KGN_CHECK(kgn::win::enablePerMonitorDpiAwareness());
    KGN_CHECK(processIsPerMonitorV2());
}

int main() { return kgn::test::runAll(); }

#else

int main() { return 0; }

#endif

// How big a window should be after moving to a monitor of a different DPI
// (M3 finding O-13).
//
// A per-monitor-aware window rescales itself when Windows sends it
// WM_DPICHANGED. If the core has already scaled it, it is scaled twice:
// 1517x918 moved from 100% to 150% became 3413x2066 (x2.25) rather than
// 2275x1377 (x1.5). Only a window that does NOT handle DPI changes needs the
// core to scale it.

#if defined(_WIN32)

#include "../src/platform/windows/win32_window.hpp"

#include "kgn_test.hpp"

using kgn::win::sizeAfterDpiMove;

KGN_TEST(a_window_that_rescales_itself_keeps_its_size_from_the_core) {
    const SIZE out = sizeAfterDpiMove(SIZE{1517, 918}, 1.5, /*selfScaling=*/true);
    KGN_CHECK_EQ(out.cx, 1517L);
    KGN_CHECK_EQ(out.cy, 918L);
}

KGN_TEST(a_window_that_does_not_rescale_is_scaled_by_the_dpi_ratio) {
    const SIZE out = sizeAfterDpiMove(SIZE{1517, 918}, 1.5, /*selfScaling=*/false);
    KGN_CHECK_EQ(out.cx, 2275L);
    KGN_CHECK_EQ(out.cy, 1377L);
}

KGN_TEST(the_same_dpi_leaves_any_window_unchanged) {
    const SIZE out = sizeAfterDpiMove(SIZE{1517, 918}, 1.0, /*selfScaling=*/false);
    KGN_CHECK_EQ(out.cx, 1517L);
    KGN_CHECK_EQ(out.cy, 918L);
}

namespace {

// A window takes its DPI awareness from the thread that creates it.
HWND probeWindow(DPI_AWARENESS_CONTEXT context) {
    const DPI_AWARENESS_CONTEXT previous = ::SetThreadDpiAwarenessContext(context);
    HWND window = ::CreateWindowExW(0, L"STATIC", L"kgn-dpi-probe", WS_OVERLAPPED,
                                    0, 0, 100, 100, nullptr, nullptr,
                                    ::GetModuleHandleW(nullptr), nullptr);
    ::SetThreadDpiAwarenessContext(previous);
    return window;
}

}  // namespace

KGN_TEST(a_dpi_unaware_window_does_not_rescale_itself) {
    HWND window = probeWindow(DPI_AWARENESS_CONTEXT_UNAWARE);
    KGN_CHECK(window != nullptr);
    KGN_CHECK(!kgn::win::windowRescalesItself(window));
    ::DestroyWindow(window);
}

KGN_TEST(a_per_monitor_aware_window_rescales_itself) {
    HWND window = probeWindow(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    KGN_CHECK(window != nullptr);
    KGN_CHECK(kgn::win::windowRescalesItself(window));
    ::DestroyWindow(window);
}

int main() { return kgn::test::runAll(); }

#else

int main() { return 0; }

#endif

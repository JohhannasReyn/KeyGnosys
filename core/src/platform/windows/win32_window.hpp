// Window and monitor enumeration and manipulation through Win32.
//
// See docs/SPEC.md section 8.4.

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dwmapi.h>
#include <shellscalingapi.h>

#include <optional>
#include <string>
#include <vector>

#include "kgn/backends.hpp"

namespace kgn::win {

// True when `window` handles WM_DPICHANGED itself -- it is per-monitor DPI
// aware -- so moving it to a monitor of another DPI must NOT also be scaled by
// the core, or it is scaled twice (M3 finding O-13).
bool windowRescalesItself(HWND window);

// The size to give a window moved across a DPI ratio of `dpiScale`. A window
// that rescales itself keeps its current size; any other is scaled.
SIZE sizeAfterDpiMove(SIZE current, double dpiScale, bool selfScaling);

// True when `process` runs at a higher integrity level than this process.
// Windows withholds input to such a window from our low-level hook (M3
// finding O-10). False when either token cannot be read.
bool runsAboveOurIntegrity(HANDLE process);

class Win32Window final : public WindowBackend {
public:
    [[nodiscard]] std::vector<WindowInfo> windows() override;
    [[nodiscard]] std::optional<WindowInfo> focused() override;
    bool focus(WindowId id) override;

    [[nodiscard]] std::vector<MonitorInfo> monitors() override;
    bool moveWindowToMonitor(WindowId id, int monitorIndex) override;

    [[nodiscard]] Capabilities capabilities() const override;
    [[nodiscard]] std::string_view name() const override { return "windows-win32"; }

private:
    static bool isEligible(HWND window);
    static BOOL CALLBACK enumerate(HWND window, LPARAM param);
    static BOOL CALLBACK enumerateMonitors(HMONITOR monitor, HDC, LPRECT, LPARAM param);
    static std::vector<MonitorInfo> enumerateMonitorList();
    static int monitorIndexFor(HWND window);
    static UINT dpiFor(const MonitorInfo& monitor);
};

}  // namespace kgn::win

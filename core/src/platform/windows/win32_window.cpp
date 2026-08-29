#include "win32_window.hpp"

#include <algorithm>
#include <cctype>

namespace kgn::win {
namespace {

std::string lowercased(std::wstring_view wide) {
    std::string out;
    out.reserve(wide.size());
    for (wchar_t c : wide) {
        if (c < 128) {
            out.push_back(static_cast<char>(
                std::tolower(static_cast<unsigned char>(c))));
        } else {
            out.push_back('?');
        }
    }
    return out;
}

std::string narrow(std::wstring_view wide) {
    if (wide.empty()) return {};
    const int needed = ::WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0,
        nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<std::size_t>(needed), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                          out.data(), needed, nullptr, nullptr);
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Enumeration

bool Win32Window::isEligible(HWND window) {
    if (::IsWindowVisible(window) == 0) return false;
    if ((::GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0) {
        return false;
    }
    if (::GetWindowTextLengthW(window) == 0) return false;

    // DWMWA_CLOAKED is not optional. Without it, UWP applications leave ghost
    // top-level windows that are invisible on screen but occupy slots, so the
    // numbered bindings point at windows the user cannot see (SPEC 8.4).
    int cloaked = 0;
    if (SUCCEEDED(::DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked,
                                          sizeof(cloaked))) &&
        cloaked != 0) {
        return false;
    }
    return true;
}

BOOL CALLBACK Win32Window::enumerate(HWND window, LPARAM param) {
    auto* out = reinterpret_cast<std::vector<WindowInfo>*>(param);
    if (!isEligible(window)) return TRUE;

    WindowInfo info;
    info.id = static_cast<WindowId>(reinterpret_cast<std::uintptr_t>(window));

    std::wstring title(static_cast<std::size_t>(::GetWindowTextLengthW(window)) + 1,
                       L'\0');
    const int length = ::GetWindowTextW(window, title.data(),
                                        static_cast<int>(title.size()));
    title.resize(static_cast<std::size_t>(std::max(0, length)));
    info.title = narrow(title);

    DWORD processId = 0;
    ::GetWindowThreadProcessId(window, &processId);
    if (processId != 0) {
        HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                       processId);
        if (process != nullptr) {
            wchar_t path[MAX_PATH]{};
            DWORD size = MAX_PATH;
            if (::QueryFullProcessImageNameW(process, 0, path, &size) != 0) {
                std::wstring_view full(path, size);
                const std::size_t slash = full.find_last_of(L"\\/");
                info.process = lowercased(
                    slash == std::wstring_view::npos ? full : full.substr(slash + 1));
            }
            ::CloseHandle(process);
        }
    }

    // wmClass stays empty on Windows: there is no such concept, and inventing
    // one would make a profile written against it silently wrong.
    info.monitor = monitorIndexFor(window);
    out->push_back(info);
    return TRUE;
}

std::vector<WindowInfo> Win32Window::windows() {
    std::vector<WindowInfo> found;
    // EnumWindows walks in z-order, topmost first, which is the order
    // window.focus_monitor wants.
    ::EnumWindows(&Win32Window::enumerate, reinterpret_cast<LPARAM>(&found));
    return found;
}

std::optional<WindowInfo> Win32Window::focused() {
    const HWND window = ::GetForegroundWindow();
    if (window == nullptr) return std::nullopt;
    std::vector<WindowInfo> one;
    if (enumerate(window, reinterpret_cast<LPARAM>(&one)) == FALSE || one.empty()) {
        return std::nullopt;
    }
    return one.front();
}

bool Win32Window::focus(WindowId id) {
    const HWND window = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(id));
    if (::IsWindow(window) == 0) return false;

    if (::IsIconic(window) != 0) ::ShowWindow(window, SW_RESTORE);

    // The foreground lock: Windows refuses SetForegroundWindow from a process
    // that does not own the foreground. Attaching to the foreground thread's
    // input queue is the standard way through it.
    const HWND foreground = ::GetForegroundWindow();
    const DWORD self = ::GetCurrentThreadId();
    const DWORD other = foreground != nullptr
                            ? ::GetWindowThreadProcessId(foreground, nullptr)
                            : 0;
    if (other != 0 && other != self) ::AttachThreadInput(self, other, TRUE);
    const BOOL ok = ::SetForegroundWindow(window);
    if (other != 0 && other != self) ::AttachThreadInput(self, other, FALSE);
    return ok != FALSE;
}

// ---------------------------------------------------------------------------
// Monitors

BOOL CALLBACK Win32Window::enumerateMonitors(HMONITOR monitor, HDC, LPRECT,
                                             LPARAM param) {
    auto* out = reinterpret_cast<std::vector<MonitorInfo>*>(param);
    MONITORINFOEXW raw{};
    raw.cbSize = sizeof(raw);
    if (::GetMonitorInfoW(monitor, &raw) == 0) return TRUE;

    MonitorInfo info;
    info.index = static_cast<int>(out->size());
    info.bounds = {raw.rcMonitor.left, raw.rcMonitor.top,
                   raw.rcMonitor.right - raw.rcMonitor.left,
                   raw.rcMonitor.bottom - raw.rcMonitor.top};
    info.primary = (raw.dwFlags & MONITORINFOF_PRIMARY) != 0;
    info.name = narrow(raw.szDevice);
    out->push_back(info);
    return TRUE;
}

std::vector<MonitorInfo> Win32Window::monitors() {
    std::vector<MonitorInfo> found;
    ::EnumDisplayMonitors(nullptr, nullptr, &Win32Window::enumerateMonitors,
                          reinterpret_cast<LPARAM>(&found));
    return found;
}

int Win32Window::monitorIndexFor(HWND window) {
    const HMONITOR handle = ::MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    const std::vector<MonitorInfo> all = enumerateMonitorList();
    MONITORINFOEXW raw{};
    raw.cbSize = sizeof(raw);
    if (::GetMonitorInfoW(handle, &raw) == 0) return 0;
    for (const MonitorInfo& info : all) {
        if (info.bounds.x == raw.rcMonitor.left &&
            info.bounds.y == raw.rcMonitor.top) {
            return info.index;
        }
    }
    return 0;
}

std::vector<MonitorInfo> Win32Window::enumerateMonitorList() {
    std::vector<MonitorInfo> found;
    ::EnumDisplayMonitors(nullptr, nullptr, &Win32Window::enumerateMonitors,
                          reinterpret_cast<LPARAM>(&found));
    return found;
}

// ---------------------------------------------------------------------------
// Moving a window between monitors

bool Win32Window::moveWindowToMonitor(WindowId id, int monitorIndex) {
    const HWND window = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(id));
    if (::IsWindow(window) == 0) return false;

    const std::vector<MonitorInfo> all = enumerateMonitorList();
    if (monitorIndex < 0 || static_cast<std::size_t>(monitorIndex) >= all.size()) {
        return false;
    }
    const MonitorInfo& target = all[static_cast<std::size_t>(monitorIndex)];

    const int from = monitorIndexFor(window);
    if (from == monitorIndex) return true;
    const MonitorInfo& source = all[static_cast<std::size_t>(from)];

    // A maximised window has to come down first: SetWindowPos on a maximised
    // window is ignored, and re-maximising afterwards is what puts it back the
    // way the user had it.
    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);
    ::GetWindowPlacement(window, &placement);
    const bool wasMaximised = placement.showCmd == SW_SHOWMAXIMIZED;
    if (wasMaximised) ::ShowWindow(window, SW_RESTORE);

    RECT rect{};
    if (::GetWindowRect(window, &rect) == 0) return false;
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;

    // Relative position preserved, and scaled: two monitors of the same size
    // can still differ in DPI, and a window that keeps its pixel size across
    // that boundary changes apparent size on screen.
    const double scaleX = source.bounds.w != 0
                              ? static_cast<double>(target.bounds.w) / source.bounds.w
                              : 1.0;
    const double scaleY = source.bounds.h != 0
                              ? static_cast<double>(target.bounds.h) / source.bounds.h
                              : 1.0;
    const int offsetX = static_cast<int>((rect.left - source.bounds.x) * scaleX);
    const int offsetY = static_cast<int>((rect.top - source.bounds.y) * scaleY);

    const UINT sourceDpi = dpiFor(source);
    const UINT targetDpi = dpiFor(target);
    const double dpiScale = sourceDpi != 0
                                ? static_cast<double>(targetDpi) / sourceDpi
                                : 1.0;

    const BOOL moved = ::SetWindowPos(
        window, nullptr, target.bounds.x + offsetX, target.bounds.y + offsetY,
        static_cast<int>(width * dpiScale), static_cast<int>(height * dpiScale),
        SWP_NOZORDER | SWP_NOACTIVATE);
    if (wasMaximised) ::ShowWindow(window, SW_SHOWMAXIMIZED);
    return moved != FALSE;
}

UINT Win32Window::dpiFor(const MonitorInfo& monitor) {
    const POINT point{monitor.bounds.x + monitor.bounds.w / 2,
                      monitor.bounds.y + monitor.bounds.h / 2};
    const HMONITOR handle = ::MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST);
    UINT dpiX = 96;
    UINT dpiY = 96;
    if (FAILED(::GetDpiForMonitor(handle, MDT_EFFECTIVE_DPI, &dpiX, &dpiY))) {
        return 96;
    }
    return dpiX;
}

Capabilities Win32Window::capabilities() const {
    Capabilities capabilities;
    capabilities.canMoveWindows = true;
    return capabilities;
}

}  // namespace kgn::win

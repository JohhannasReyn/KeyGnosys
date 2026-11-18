#include "dpi_awareness.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace kgn::win {

bool enablePerMonitorDpiAwareness() {
    // Per-monitor-v2 needs Windows 10 1703 or later. A second call fails with
    // ERROR_ACCESS_DENIED once the awareness is set; what matters is the state
    // afterwards, so success is judged by asking, not by the call's result.
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    return ::AreDpiAwarenessContextsEqual(
               ::GetThreadDpiAwarenessContext(),
               DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != FALSE;
}

}  // namespace kgn::win

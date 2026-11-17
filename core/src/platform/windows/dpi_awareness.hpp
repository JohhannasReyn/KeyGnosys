// Per-monitor DPI awareness for the core process (M3 finding O-11).
#pragma once

namespace kgn::win {

// Declares the process per-monitor-v2 DPI aware, so every Win32 coordinate the
// core reads or writes is in physical pixels. Must run before any backend
// queries a monitor, the cursor or a window. Returns true when the process is
// per-monitor-v2 aware afterwards -- including when it already was.
bool enablePerMonitorDpiAwareness();

}  // namespace kgn::win

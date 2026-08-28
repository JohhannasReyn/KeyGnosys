// The backend factory: the single place that knows which platform this build
// targets.
//
// It lives here rather than in backends.hpp so the DEPENDENCY DIRECTION stays
// honest. kgn_ipc composes a core out of backends it is handed; it does not
// know how to make one. Only the executable -- the composition root -- calls
// this, which is why a test can build a Core out of fakes without linking
// user32, dwmapi or libevdev, and why adding a platform still means adding
// files under src/platform/<os>/ plus one line here (SPEC section 6.2).

#pragma once

#include "kgn/backends.hpp"

namespace kgn {

// Returns null members for capabilities unavailable on this platform; the
// caller reports each absence rather than substituting something that merely
// looks similar (P6).
Backends createBackends();

}  // namespace kgn

// The backend factory on a build with no platform backends.
//
// Returning null members is exactly what backends.hpp specifies for a
// capability this platform does not have, and the core reports each absence in
// `hello` rather than substituting something that merely looks similar (P6).
//
// It is deliberately NOT a fake backend. A stub that swallowed calls and
// returned plausible values would let the core look like it was driving the
// pointer while nothing moved, which is the state P6 exists to forbid.

#include "kgn/platform.hpp"

namespace kgn {

Backends createBackends() { return {}; }

}  // namespace kgn

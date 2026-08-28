// The Windows backend factory.
//
// SPEC section 6.2: adding a platform means adding files under
// src/platform/<os>/ plus one line in the factory. This is that line.

#include "kgn/platform.hpp"

#include "hook_input.hpp"
#include "sendinput_output.hpp"
#include "win32_window.hpp"

namespace kgn {

Backends createBackends() {
    Backends backends;
    backends.output = std::make_unique<win::SendInputOutput>();
    backends.window = std::make_unique<win::Win32Window>();
    // The input backend is also the engine owner: it must answer Windows'
    // suppression question synchronously, and the answer is a function of
    // engine state (SPEC section 8.2).
    backends.input = std::make_unique<win::HookInput>();
    return backends;
}

}  // namespace kgn

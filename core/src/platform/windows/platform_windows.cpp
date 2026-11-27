// The Windows backend factory.
//
// SPEC section 6.2: adding a platform means adding files under
// src/platform/<os>/ plus one line in the factory. This is that line.

#include "kgn/platform.hpp"

#include "dpi_awareness.hpp"
#include "guardian_supervisor.hpp"
#include "hook_input.hpp"
#include "obligation_journal.hpp"
#include "sendinput_output.hpp"
#include "win32_window.hpp"

namespace kgn {

Backends createBackends() {
    // First, before any backend exists: every monitor rectangle, cursor
    // position and window rectangle the backends read must be physical pixels,
    // not the virtualised ones a DPI-unaware process gets on a scaled monitor
    // (M3 finding O-11).
    win::enablePerMonitorDpiAwareness();

    Backends backends;
    auto output = std::make_unique<win::SendInputOutput>();
    // The crash-release guardian is spawned and attached BEFORE the input
    // hook exists (spec section 3.1), so no input -- and so no button press
    // or drag lock -- can arrive before every button is journaled. start()
    // waits up to 2 s for the guardian to attach; whatever it reports is the
    // state hello carries.
    if (auto journal = win::ObligationJournal::create()) {
        auto supervisor = std::make_unique<win::GuardianSupervisor>(
            *journal, win::guardianPathBesideThisExecutable());
        supervisor->start();
        output->adoptGuardian(std::move(journal), std::move(supervisor));
    } else {
        // No journal, so nothing for a guardian to read: not protected.
        output->setProtectionFallback({Protection::Unprotected, "spawn_failed"});
    }
    backends.output = std::move(output);
    backends.window = std::make_unique<win::Win32Window>();
    // The input backend is also the engine owner: it must answer Windows'
    // suppression question synchronously, and the answer is a function of
    // engine state (SPEC section 8.2).
    backends.input = std::make_unique<win::HookInput>();
    return backends;
}

}  // namespace kgn

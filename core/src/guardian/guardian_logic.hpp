// Pure logic of the crash-release guardian: argument parsing, header identity
// check and the recovery rule. No Win32 call is made here; the OS sits behind
// RecoveryPorts so every row of the recovery table can be driven by a fake.
//
// Header-only dependencies only (journal_layout.hpp, kgn/keycode.hpp): the
// guardian links no kgn library.

#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>

#include "platform/windows/journal_layout.hpp"

namespace kgn::guardian {

using kgn::win::JournalHeader;
using kgn::win::JournalSlots;

enum class GuardianExit : int {
    Clean = 0,
    BadArgs = 2,
    IdentityMismatch = 3,
    ReleaseUnconfirmed = 4,
    Relayed = 5,
};

struct GuardianArgs {
    std::uintptr_t section;
    std::uintptr_t coreProcess;
    std::array<std::uint8_t, 16> nonce;
    bool relay;
};

// keygnosys-guardian [--relay] --section <hex> --core <hex> --nonce <32 hex>
std::optional<GuardianArgs> parseArgs(int argc, char** argv);

// True when the header is a complete journal of this layout, written by the
// core instance this guardian was told to watch.
bool validateHeader(const JournalHeader& header,
                    const std::array<std::uint8_t, 16>& nonce, DWORD actualPid,
                    FILETIME actualCreated);

// True when the core marked a clean exit before it ended: nothing to release.
bool cleanExitMarked(const JournalHeader& header);

struct RecoveryPorts {
    std::function<bool(kgn::MouseButton)> windowsReportsDown;
    std::function<void(kgn::MouseButton)> sendUp;
    std::function<std::chrono::steady_clock::time_point()> now;
    std::function<void(std::chrono::milliseconds)> sleep;
};

// Lifts every journaled button Windows still reports down. Sends Up only, never
// to a button Windows reports up, and never after `window` has elapsed.
GuardianExit recover(const JournalSlots& slots, RecoveryPorts& ports,
                     std::chrono::milliseconds window = std::chrono::milliseconds(1000));

// Virtual-key code GetAsyncKeyState reports for the button. Measured: an
// injected LEFTDOWN reports VK_LBUTTON even with the buttons swapped, so the
// mapping is the identity for both values of `swapped`.
int physicalVk(kgn::MouseButton button, bool swapped);

}  // namespace kgn::guardian

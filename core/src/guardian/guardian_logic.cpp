#include "guardian/guardian_logic.hpp"

#include <cstring>

namespace kgn::guardian {

namespace {

int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// 1..16 hex digits, no prefix, no sign, nonzero (a handle is never null).
bool parseHandle(const char* s, std::uintptr_t& out) {
    if (!s) return false;
    std::uint64_t v = 0;
    std::size_t n = 0;
    for (; s[n] != '\0'; ++n) {
        const int d = hexDigit(s[n]);
        if (d < 0 || n >= 16) return false;
        v = (v << 4) | static_cast<std::uint64_t>(d);
    }
    if (n == 0 || v == 0) return false;
    out = static_cast<std::uintptr_t>(v);
    return static_cast<std::uint64_t>(out) == v;
}

bool parseNonce(const char* s, std::array<std::uint8_t, 16>& out) {
    if (!s || std::strlen(s) != 32) return false;
    for (std::size_t i = 0; i < 16; ++i) {
        const int hi = hexDigit(s[2 * i]);
        const int lo = hexDigit(s[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    return true;
}

}  // namespace

std::optional<GuardianArgs> parseArgs(int argc, char** argv) {
    GuardianArgs a{0, 0, {}, false};
    bool haveSection = false, haveCore = false, haveNonce = false, haveRelay = false;
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (std::strcmp(arg, "--relay") == 0) {
            if (haveRelay) return std::nullopt;
            haveRelay = a.relay = true;
        } else if (std::strcmp(arg, "--section") == 0) {
            if (haveSection || i + 1 >= argc || !parseHandle(argv[++i], a.section))
                return std::nullopt;
            haveSection = true;
        } else if (std::strcmp(arg, "--core") == 0) {
            if (haveCore || i + 1 >= argc || !parseHandle(argv[++i], a.coreProcess))
                return std::nullopt;
            haveCore = true;
        } else if (std::strcmp(arg, "--nonce") == 0) {
            if (haveNonce || i + 1 >= argc || !parseNonce(argv[++i], a.nonce))
                return std::nullopt;
            haveNonce = true;
        } else {
            return std::nullopt;
        }
    }
    if (!haveSection || !haveCore || !haveNonce) return std::nullopt;
    return a;
}

bool validateHeader(const JournalHeader& header,
                    const std::array<std::uint8_t, 16>& nonce, DWORD actualPid,
                    FILETIME actualCreated) {
    if (header.magic.load(std::memory_order_acquire) != kgn::win::kJournalMagic) return false;
    if (header.layout_version != kgn::win::kJournalLayoutVersion) return false;
    if (std::memcmp(header.instance_nonce, nonce.data(), nonce.size()) != 0) return false;
    if (header.core_pid != actualPid) return false;
    const std::uint64_t created =
        (static_cast<std::uint64_t>(actualCreated.dwHighDateTime) << 32) |
        actualCreated.dwLowDateTime;
    return header.core_creation_time == created;
}

bool cleanExitMarked(const JournalHeader& header) {
    return (header.flags.load(std::memory_order_acquire) & kgn::win::kFlagCleanExit) != 0;
}

GuardianExit recover(const JournalSlots& slots, RecoveryPorts& ports,
                     std::chrono::milliseconds window) {
    using kgn::MouseButton;
    constexpr MouseButton kButtons[] = {MouseButton::Left, MouseButton::Right,
                                        MouseButton::Middle};
    constexpr std::chrono::milliseconds kRetryPause{10};

    const auto deadline = ports.now() + window;
    bool unconfirmed = false;

    for (const MouseButton b : kButtons) {
        const auto state = static_cast<kgn::win::SlotState>(
            slots.buttons[static_cast<std::size_t>(b)].state.load(std::memory_order_acquire));
        if (state == kgn::win::SlotState::UP) continue;

        // Windows is the authority on whether the button is physically held:
        // never send an Up to a button it reports up.
        for (;;) {
            if (!ports.windowsReportsDown(b)) break;
            if (ports.now() >= deadline) {
                unconfirmed = true;
                break;
            }
            ports.sendUp(b);
            ports.sleep(kRetryPause);
        }
    }
    return unconfirmed ? GuardianExit::ReleaseUnconfirmed : GuardianExit::Clean;
}

int physicalVk(kgn::MouseButton button, bool /*swapped*/) {
    // Measured, not assumed: with the buttons swapped an injected LEFTDOWN still
    // reports VK_LBUTTON down, so the mapping is the identity either way.
    switch (button) {
        case kgn::MouseButton::Left: return VK_LBUTTON;
        case kgn::MouseButton::Right: return VK_RBUTTON;
        case kgn::MouseButton::Middle: return VK_MBUTTON;
    }
    return 0;
}

}  // namespace kgn::guardian

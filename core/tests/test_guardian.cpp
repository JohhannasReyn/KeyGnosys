#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "guardian/guardian_logic.hpp"
#include "kgn_test.hpp"

using namespace kgn::guardian;
using kgn::MouseButton;
using kgn::win::SlotState;
using std::chrono::milliseconds;

namespace {

constexpr MouseButton kAll[3] = {MouseButton::Left, MouseButton::Right, MouseButton::Middle};

void setSlot(JournalSlots& s, MouseButton b, SlotState st) {
    s.buttons[static_cast<int>(b)].state.store(static_cast<std::uint32_t>(st));
}

// Fake world: the clock advances only through sleep(). Once an Up has been
// sent for a button it goes up after `staleReads` further reads that still
// say down (unless neverReleases).
struct Fake {
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    milliseconds elapsed{0};
    bool down[3] = {false, false, false};
    int staleReads[3] = {0, 0, 0};
    bool neverReleases = false;
    std::vector<MouseButton> ups;
    std::vector<milliseconds> upTimes;
    RecoveryPorts ports;

    Fake() {
        ports.windowsReportsDown = [this](MouseButton b) {
            const int i = static_cast<int>(b);
            if (!down[i]) return false;
            if (neverReleases || count(b) == 0) return true;
            if (staleReads[i] > 0) {
                --staleReads[i];
                return true;
            }
            down[i] = false;
            return false;
        };
        ports.sendUp = [this](MouseButton b) {
            ups.push_back(b);
            upTimes.push_back(elapsed);
        };
        ports.now = [this] { return t0 + elapsed; };
        ports.sleep = [this](milliseconds d) { elapsed += d; };
    }
    int count(MouseButton b) const {
        int n = 0;
        for (auto u : ups) n += (u == b);
        return n;
    }
};

std::unique_ptr<JournalSlots> makeSlots() { return std::make_unique<JournalSlots>(); }
std::unique_ptr<JournalHeader> makeHeader() { return std::make_unique<JournalHeader>(); }

}  // namespace

KGN_TEST(recovery_sends_nothing_when_every_slot_is_up) {
    auto s = makeSlots();
    Fake f;
    for (int i = 0; i < 3; ++i) f.down[i] = true;   // even if Windows says down
    KGN_CHECK(recover(*s, f.ports) == GuardianExit::Clean);
    KGN_CHECK(f.ups.empty());
}

KGN_TEST(recovery_skips_a_journaled_button_windows_reports_up) {
    for (auto st : {SlotState::DOWN_INTENT, SlotState::UP_INTENT}) {
        auto s = makeSlots();
        setSlot(*s, MouseButton::Left, st);
        Fake f;   // Windows reports every button up
        KGN_CHECK(recover(*s, f.ports) == GuardianExit::Clean);
        KGN_CHECK(f.ups.empty());
    }
}

KGN_TEST(recovery_lifts_a_down_intent_windows_reports_down) {
    auto s = makeSlots();
    setSlot(*s, MouseButton::Right, SlotState::DOWN_INTENT);
    Fake f;
    f.down[1] = true;
    KGN_CHECK(recover(*s, f.ports) == GuardianExit::Clean);
    KGN_CHECK_EQ(f.count(MouseButton::Right), 1);
    KGN_CHECK_EQ(f.ups.size(), 1u);
}

KGN_TEST(recovery_lifts_a_held_button) {
    auto s = makeSlots();
    setSlot(*s, MouseButton::Left, SlotState::DOWN);
    Fake f;
    f.down[0] = true;
    KGN_CHECK(recover(*s, f.ports) == GuardianExit::Clean);
    KGN_CHECK_EQ(f.count(MouseButton::Left), 1);
}

KGN_TEST(recovery_lifts_an_up_intent_windows_still_reports_down) {
    auto s = makeSlots();
    setSlot(*s, MouseButton::Middle, SlotState::UP_INTENT);
    Fake f;
    f.down[2] = true;
    KGN_CHECK(recover(*s, f.ports) == GuardianExit::Clean);
    KGN_CHECK_EQ(f.count(MouseButton::Middle), 1);
}

KGN_TEST(recovery_lifts_each_held_button_exactly_once) {
    auto s = makeSlots();
    for (auto b : kAll) setSlot(*s, b, SlotState::DOWN);
    Fake f;
    for (int i = 0; i < 3; ++i) f.down[i] = true;
    KGN_CHECK(recover(*s, f.ports) == GuardianExit::Clean);
    for (auto b : kAll) KGN_CHECK_EQ(f.count(b), 1);
    KGN_CHECK_EQ(f.ups.size(), 3u);
}

KGN_TEST(recovery_retries_until_windows_reports_up) {
    auto s = makeSlots();
    setSlot(*s, MouseButton::Left, SlotState::DOWN);
    Fake f;
    f.down[0] = true;
    f.staleReads[0] = 2;   // stays down for two reads after the first Up
    KGN_CHECK(recover(*s, f.ports) == GuardianExit::Clean);
    KGN_CHECK_EQ(f.count(MouseButton::Left), 3);
}

KGN_TEST(recovery_gives_up_after_the_window) {
    auto s = makeSlots();
    setSlot(*s, MouseButton::Left, SlotState::DOWN);
    Fake f;
    f.down[0] = true;
    f.neverReleases = true;
    KGN_CHECK(recover(*s, f.ports) == GuardianExit::ReleaseUnconfirmed);
    KGN_CHECK(!f.ups.empty());
    for (auto t : f.upTimes) KGN_CHECK(t < milliseconds(1000));
    KGN_CHECK(f.elapsed <= milliseconds(1100));
}

namespace {
std::array<std::uint8_t, 16> nonce16(std::uint8_t seed) {
    std::array<std::uint8_t, 16> n{};
    for (std::size_t i = 0; i < n.size(); ++i) n[i] = static_cast<std::uint8_t>(seed + i);
    return n;
}
std::uint64_t ticks(FILETIME ft) {
    return (static_cast<std::uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}
std::unique_ptr<JournalHeader> goodHeader(DWORD pid, FILETIME ft) {
    auto h = makeHeader();
    h->magic.store(kgn::win::kJournalMagic);
    h->layout_version = kgn::win::kJournalLayoutVersion;
    h->core_pid = pid;
    h->core_creation_time = ticks(ft);
    auto n = nonce16(7);
    std::memcpy(h->instance_nonce, n.data(), 16);
    return h;
}
const FILETIME kCreated = {0x89ABCDEFu, 0x01234567u};
}  // namespace

KGN_TEST(header_validation_accepts_a_match) {
    auto h = goodHeader(1234, kCreated);
    KGN_CHECK(validateHeader(*h, nonce16(7), 1234, kCreated));
}

KGN_TEST(header_validation_rejects_bad_magic) {
    auto h = goodHeader(1234, kCreated);
    h->magic.store(0);
    KGN_CHECK(!validateHeader(*h, nonce16(7), 1234, kCreated));
}

KGN_TEST(header_validation_rejects_bad_version) {
    auto h = goodHeader(1234, kCreated);
    h->layout_version = 2;
    KGN_CHECK(!validateHeader(*h, nonce16(7), 1234, kCreated));
}

KGN_TEST(header_validation_rejects_wrong_nonce) {
    auto h = goodHeader(1234, kCreated);
    KGN_CHECK(!validateHeader(*h, nonce16(8), 1234, kCreated));
}

KGN_TEST(header_validation_rejects_pid_mismatch) {
    auto h = goodHeader(1234, kCreated);
    KGN_CHECK(!validateHeader(*h, nonce16(7), 1235, kCreated));
}

KGN_TEST(header_validation_rejects_creation_time_mismatch) {
    auto h = goodHeader(1234, kCreated);
    FILETIME other = kCreated;
    other.dwLowDateTime += 1;
    KGN_CHECK(!validateHeader(*h, nonce16(7), 1234, other));
}

KGN_TEST(clean_exit_flag_is_detected) {
    auto h = makeHeader();
    KGN_CHECK(!cleanExitMarked(*h));
    h->flags.store(kgn::win::kFlagAttached);
    KGN_CHECK(!cleanExitMarked(*h));
    h->flags.store(kgn::win::kFlagAttached | kgn::win::kFlagCleanExit);
    KGN_CHECK(cleanExitMarked(*h));
}

namespace {
std::optional<GuardianArgs> parse(std::vector<std::string> v) {
    std::vector<char*> argv;
    for (auto& s : v) argv.push_back(s.data());
    return parseArgs(static_cast<int>(argv.size()), argv.data());
}
const char* kN = "000102030405060708090a0b0c0d0e0f";
}  // namespace

KGN_TEST(parse_args_rejects_missing_or_malformed_values) {
    KGN_CHECK(!parse({"g"}));
    KGN_CHECK(!parse({"g", "--section", "1f", "--core", "2a"}));                 // no nonce
    KGN_CHECK(!parse({"g", "--section", "1f", "--nonce", kN}));                  // no core
    KGN_CHECK(!parse({"g", "--core", "2a", "--nonce", kN}));                     // no section
    KGN_CHECK(!parse({"g", "--section", "zz", "--core", "2a", "--nonce", kN}));  // not hex
    KGN_CHECK(!parse({"g", "--section", "", "--core", "2a", "--nonce", kN}));    // empty
    KGN_CHECK(!parse({"g", "--section", "0", "--core", "2a", "--nonce", kN}));   // null handle
    KGN_CHECK(!parse({"g", "--section", "1f", "--core", "2a", "--nonce", "0102"}));  // short
    KGN_CHECK(!parse({"g", "--section", "1f", "--core", "2a", "--nonce",
                      "000102030405060708090a0b0c0d0e0g"}));                     // bad digit
    KGN_CHECK(!parse({"g", "--section", "1f", "--core"}));                       // dangling
    KGN_CHECK(!parse({"g", "--section", "1f", "--core", "2a", "--nonce", kN, "--bogus"}));
    KGN_CHECK(!parse({"g", "--section", "1f", "--section", "1f", "--core", "2a", "--nonce", kN}));
}

KGN_TEST(parse_args_reads_relay_mode) {
    auto plain = parse({"g", "--section", "1f", "--core", "2A", "--nonce", kN});
    KGN_CHECK(plain.has_value());
    if (plain) {
        KGN_CHECK(!plain->relay);
        KGN_CHECK_EQ(plain->section, std::uintptr_t{0x1f});
        KGN_CHECK_EQ(plain->coreProcess, std::uintptr_t{0x2a});
        KGN_CHECK_EQ(static_cast<int>(plain->nonce[0]), 0);
        KGN_CHECK_EQ(static_cast<int>(plain->nonce[15]), 15);
    }
    auto relay = parse({"g", "--relay", "--section", "1f", "--core", "2a", "--nonce", kN});
    KGN_CHECK(relay.has_value());
    if (relay) KGN_CHECK(relay->relay);
}

KGN_TEST(physical_vk_maps_per_swap_ruling) {
    for (bool swapped : {false, true}) {
        KGN_CHECK_EQ(physicalVk(MouseButton::Left, swapped), 1);
        KGN_CHECK_EQ(physicalVk(MouseButton::Right, swapped), 2);
        KGN_CHECK_EQ(physicalVk(MouseButton::Middle, swapped), 4);
    }
}

int main() { return kgn::test::runAll(); }

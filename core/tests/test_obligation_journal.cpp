// The obligation journal: write-ahead records of every mouse-button
// transition, in shared memory the crash-release guardian reads.

#if defined(_WIN32)

#include "../src/platform/windows/obligation_journal.hpp"
#include "../src/platform/windows/sendinput_output.hpp"

#include "kgn_test.hpp"

#include <cstddef>

using namespace kgn;
using namespace kgn::win;

namespace {

// Reads the journal at dispatch time and returns a scripted result.
class JournalProbe : public SendInputOutput {
public:
    ObligationJournal* probe = nullptr;
    bool refuse = false;
    SlotState seenInDispatch = SlotState::UP;

protected:
    bool dispatch(const INPUT&) override {
        if (probe) seenInDispatch = probe->slot(MouseButton::Left);
        return !refuse;
    }
};

}  // namespace

KGN_TEST(a_press_writes_intent_before_dispatch_and_down_after) {
    auto journal = ObligationJournal::create();
    KGN_CHECK(journal != nullptr);
    JournalProbe output;
    output.probe = journal.get();
    output.setJournal(journal.get());

    output.button(MouseButton::Left, true);
    KGN_CHECK(output.seenInDispatch == SlotState::DOWN_INTENT);
    KGN_CHECK(journal->slot(MouseButton::Left) == SlotState::DOWN);
}

KGN_TEST(a_refused_press_leaves_the_slot_up) {
    auto journal = ObligationJournal::create();
    JournalProbe output;
    output.probe = journal.get();
    output.setJournal(journal.get());

    output.refuse = true;
    output.button(MouseButton::Left, true);
    KGN_CHECK(output.seenInDispatch == SlotState::DOWN_INTENT);
    KGN_CHECK(journal->slot(MouseButton::Left) == SlotState::UP);
}

KGN_TEST(a_release_writes_intent_before_dispatch_and_up_after) {
    auto journal = ObligationJournal::create();
    JournalProbe output;
    output.probe = journal.get();
    output.setJournal(journal.get());

    output.button(MouseButton::Left, true);
    output.button(MouseButton::Left, false);
    KGN_CHECK(output.seenInDispatch == SlotState::UP_INTENT);
    KGN_CHECK(journal->slot(MouseButton::Left) == SlotState::UP);
}

KGN_TEST(a_refused_release_leaves_the_slot_down) {
    auto journal = ObligationJournal::create();
    JournalProbe output;
    output.probe = journal.get();
    output.setJournal(journal.get());

    output.button(MouseButton::Left, true);
    output.refuse = true;
    output.button(MouseButton::Left, false);
    KGN_CHECK(output.seenInDispatch == SlotState::UP_INTENT);
    KGN_CHECK(journal->slot(MouseButton::Left) == SlotState::DOWN);
}

KGN_TEST(each_transition_bumps_the_generation) {
    auto journal = ObligationJournal::create();
    JournalProbe output;
    output.setJournal(journal.get());

    // Read the generation through a second view of the same section, as the
    // guardian would.
    void* view = ::MapViewOfFile(journal->sectionHandle(), FILE_MAP_READ, 0, 0,
                                 kJournalSectionSize);
    KGN_CHECK(view != nullptr);
    const auto* slots = reinterpret_cast<const JournalSlots*>(
        static_cast<const std::uint8_t*>(view) + kJournalPageSize);
    const auto& left = slots->buttons[static_cast<std::size_t>(MouseButton::Left)];

    const std::uint32_t g0 = left.generation.load();
    output.button(MouseButton::Left, true);
    const std::uint32_t g1 = left.generation.load();
    output.button(MouseButton::Left, false);
    const std::uint32_t g2 = left.generation.load();
    // Each transition writes intent and result: at least one bump each.
    KGN_CHECK(g1 > g0);
    KGN_CHECK(g2 > g1);
    // Other buttons are untouched.
    KGN_CHECK_EQ(slots->buttons[static_cast<std::size_t>(MouseButton::Right)]
                     .generation.load(),
                 0u);
    ::UnmapViewOfFile(view);
}

KGN_TEST(without_a_journal_buttons_still_work) {
    JournalProbe output;   // no journal set
    output.button(MouseButton::Left, true);
    output.button(MouseButton::Left, false);
    output.refuse = true;
    output.button(MouseButton::Right, true);
    KGN_CHECK(true);
}

KGN_TEST(the_header_round_trips_and_the_slots_sit_on_their_own_page) {
    static_assert(sizeof(JournalHeader) == kJournalPageSize);
    static_assert(sizeof(JournalSlots) == kJournalPageSize);

    auto journal = ObligationJournal::create();
    KGN_CHECK(journal != nullptr);

    FILETIME created{0xAABBCCDDu, 0x11223344u};
    std::array<std::uint8_t, 16> nonce{};
    for (std::size_t i = 0; i < nonce.size(); ++i) nonce[i] = static_cast<std::uint8_t>(i + 1);
    journal->writeHeader(4242, created, nonce);
    journal->setSlot(MouseButton::Middle, SlotState::DOWN);

    void* view = ::MapViewOfFile(journal->sectionHandle(), FILE_MAP_READ, 0, 0,
                                 kJournalSectionSize);
    KGN_CHECK(view != nullptr);
    const auto* base = static_cast<const std::uint8_t*>(view);
    const auto* header = reinterpret_cast<const JournalHeader*>(base);
    const auto* slots = reinterpret_cast<const JournalSlots*>(base + kJournalPageSize);

    KGN_CHECK_EQ(header->magic.load(), kJournalMagic);
    KGN_CHECK_EQ(header->layout_version, kJournalLayoutVersion);
    KGN_CHECK_EQ(header->core_pid, 4242u);
    KGN_CHECK(header->core_creation_time == 0x11223344AABBCCDDull);
    KGN_CHECK(header->instance_nonce[0] == 1 && header->instance_nonce[15] == 16);
    KGN_CHECK_EQ(slots->buttons[2].state.load(),
                 static_cast<std::uint32_t>(SlotState::DOWN));

    KGN_CHECK(!journal->attached());
    KGN_CHECK_EQ(journal->guardianPid(), 0u);
    journal->markCleanExit();
    KGN_CHECK((header->flags.load() & kFlagCleanExit) != 0);
    ::UnmapViewOfFile(view);
}

KGN_TEST(clearing_the_attachment_resets_attached_and_the_guardian_pid) {
    auto journal = ObligationJournal::create();
    void* view = ::MapViewOfFile(journal->sectionHandle(), FILE_MAP_ALL_ACCESS, 0, 0,
                                 kJournalSectionSize);
    KGN_CHECK(view != nullptr);
    auto* header = static_cast<JournalHeader*>(view);
    // What an attaching guardian writes.
    header->guardian_pid.store(777);
    header->flags.fetch_or(kFlagAttached | kFlagCleanExit);
    KGN_CHECK(journal->attached());

    journal->clearAttachment();
    KGN_CHECK(!journal->attached());
    KGN_CHECK_EQ(journal->guardianPid(), 0u);
    // CLEAN_EXIT is the core's, not the guardian's: left alone.
    KGN_CHECK((header->flags.load() & kFlagCleanExit) != 0);
    ::UnmapViewOfFile(view);
}

int main() { return kgn::test::runAll(); }

#else

int main() { return 0; }

#endif

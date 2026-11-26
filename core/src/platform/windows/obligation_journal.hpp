// The core's record of which mouse buttons it holds, in an unnamed shared
// memory section the crash-release guardian can read after the core dies.
// Written write-ahead: intent before SendInput, result after.

#pragma once

#include "journal_layout.hpp"

#include <array>
#include <memory>

namespace kgn::win {

class ObligationJournal {
public:
    // Creates an unnamed, pagefile-backed, inheritable 2-page section.
    // Returns null on failure.
    [[nodiscard]] static std::unique_ptr<ObligationJournal> create();
    ~ObligationJournal();
    ObligationJournal(const ObligationJournal&) = delete;
    ObligationJournal& operator=(const ObligationJournal&) = delete;

    [[nodiscard]] HANDLE sectionHandle() const { return section_; }

    void writeHeader(DWORD corePid, FILETIME created,
                     const std::array<std::uint8_t, 16>& nonce);

    // Atomic release-store of the state, then a generation bump.
    void setSlot(MouseButton button, SlotState state);
    [[nodiscard]] SlotState slot(MouseButton button) const;

    [[nodiscard]] bool attached() const;
    [[nodiscard]] DWORD guardianPid() const;
    void markCleanExit();

    // Clears ATTACHED and guardian_pid. The supervisor calls it before
    // respawning a lost guardian, so the dead one's marker is never mistaken
    // for the new one's.
    void clearAttachment();

private:
    ObligationJournal() = default;

    HANDLE section_ = nullptr;
    void* view_ = nullptr;
    JournalHeader* header_ = nullptr;
    JournalSlots* slots_ = nullptr;
};

}  // namespace kgn::win

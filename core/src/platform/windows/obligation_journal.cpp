#include "obligation_journal.hpp"

#include <cstring>

namespace kgn::win {

std::unique_ptr<ObligationJournal> ObligationJournal::create() {
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;

    HANDLE section = ::CreateFileMappingW(
        INVALID_HANDLE_VALUE, &attributes, PAGE_READWRITE, 0,
        static_cast<DWORD>(kJournalSectionSize), nullptr);
    if (section == nullptr) return nullptr;

    void* view = ::MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0,
                                 kJournalSectionSize);
    if (view == nullptr) {
        ::CloseHandle(section);
        return nullptr;
    }

    std::unique_ptr<ObligationJournal> journal(new ObligationJournal());
    journal->section_ = section;
    journal->view_ = view;
    // The section is zero-filled, so every slot starts UP.
    journal->header_ = reinterpret_cast<JournalHeader*>(view);
    journal->slots_ = reinterpret_cast<JournalSlots*>(
        static_cast<std::uint8_t*>(view) + kJournalPageSize);
    return journal;
}

ObligationJournal::~ObligationJournal() {
    if (view_ != nullptr) ::UnmapViewOfFile(view_);
    if (section_ != nullptr) ::CloseHandle(section_);
}

void ObligationJournal::writeHeader(DWORD corePid, FILETIME created,
                                    const std::array<std::uint8_t, 16>& nonce) {
    header_->layout_version = kJournalLayoutVersion;
    header_->core_pid = static_cast<std::uint32_t>(corePid);
    header_->core_creation_time =
        (static_cast<std::uint64_t>(created.dwHighDateTime) << 32) |
        created.dwLowDateTime;
    std::memcpy(header_->instance_nonce, nonce.data(), nonce.size());
    header_->magic.store(kJournalMagic, std::memory_order_release);
}

void ObligationJournal::setSlot(MouseButton button, SlotState state) {
    JournalSlot& s = slots_->buttons[static_cast<std::size_t>(button)];
    s.state.store(static_cast<std::uint32_t>(state), std::memory_order_release);
    s.generation.fetch_add(1, std::memory_order_release);
}

SlotState ObligationJournal::slot(MouseButton button) const {
    const JournalSlot& s = slots_->buttons[static_cast<std::size_t>(button)];
    return static_cast<SlotState>(s.state.load(std::memory_order_acquire));
}

bool ObligationJournal::attached() const {
    return (header_->flags.load(std::memory_order_acquire) & kFlagAttached) != 0;
}

DWORD ObligationJournal::guardianPid() const {
    return header_->guardian_pid.load(std::memory_order_acquire);
}

void ObligationJournal::markCleanExit() {
    header_->flags.fetch_or(kFlagCleanExit, std::memory_order_release);
}

void ObligationJournal::clearAttachment() {
    header_->flags.fetch_and(~kFlagAttached, std::memory_order_acq_rel);
    header_->guardian_pid.store(0, std::memory_order_release);
}

}  // namespace kgn::win

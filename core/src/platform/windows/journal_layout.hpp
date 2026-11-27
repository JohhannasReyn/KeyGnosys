// The on-the-wire layout of the obligation journal, shared by the core (which
// writes it) and the crash-release guardian (which reads it).
//
// Header-only and self-contained on purpose: the guardian includes this file
// without linking any kgn library. It may include kgn/keycode.hpp (header-only,
// for MouseButton), <windows.h> and the standard library, nothing else.
//
// The section is two pages. Page 0 is the JournalHeader; page 1 is the
// JournalSlots. They are separate so the guardian's view of the slots can be
// read-only while the header stays writable (guardian_pid, flags).

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "kgn/keycode.hpp"

namespace kgn::win {

inline constexpr std::uint32_t kJournalMagic = 0x4B474A31;   // "KGJ1"
inline constexpr std::uint32_t kJournalLayoutVersion = 1;
inline constexpr std::size_t kJournalPageSize = 4096;
inline constexpr std::size_t kJournalSectionSize = 2 * kJournalPageSize;
inline constexpr std::size_t kJournalButtonCount = 3;   // Left, Right, Middle

// Header flag bits.
inline constexpr std::uint32_t kFlagAttached = 1;
inline constexpr std::uint32_t kFlagCleanExit = 2;

enum class SlotState : std::uint32_t {
    UP = 0,
    DOWN_INTENT = 1,
    DOWN = 2,
    UP_INTENT = 3,
};

struct alignas(kJournalPageSize) JournalHeader {
    // Stored last by the core with release order: a reader that sees the magic
    // sees a complete header.
    std::atomic<std::uint32_t> magic;
    std::uint32_t layout_version;
    std::uint32_t core_pid;
    std::uint32_t reserved0;
    std::uint64_t core_creation_time;    // FILETIME as a 64-bit tick count
    std::uint8_t instance_nonce[16];     // 128-bit
    // Written by the guardian, read by the core: cross-process atomics.
    std::atomic<std::uint32_t> guardian_pid;
    std::atomic<std::uint32_t> flags;    // kFlagAttached | kFlagCleanExit
};

struct JournalSlot {
    std::atomic<std::uint32_t> state;        // a SlotState
    std::atomic<std::uint32_t> generation;   // bumped on every transition
};

struct alignas(kJournalPageSize) JournalSlots {
    JournalSlot buttons[kJournalButtonCount];   // indexed by MouseButton
};

static_assert(sizeof(JournalHeader) == kJournalPageSize,
              "the header occupies exactly one page");
static_assert(sizeof(JournalSlots) == kJournalPageSize,
              "the slots occupy exactly one page");
static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
              "cross-process atomics must be lock-free");
static_assert(sizeof(std::atomic<std::uint32_t>) == sizeof(std::uint32_t),
              "an atomic word is exactly one word");
static_assert(std::is_standard_layout_v<JournalHeader> &&
              std::is_standard_layout_v<JournalSlots>,
              "the journal is shared memory and must be standard layout");

}  // namespace kgn::win

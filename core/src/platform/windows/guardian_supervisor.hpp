// Spawns, attaches and supervises the crash-release guardian.
//
// The supervisor owns the guardian side of one core instance's journal:
//
//   start()  writes the journal header (core pid, creation time, a fresh
//            128-bit nonce), launches `keygnosys-guardian --relay ...` so the
//            guardian's parent is a dead process rather than the core, and
//            waits up to 2 s for the guardian to write its pid and ATTACHED.
//            The only call here that blocks.
//   poll()   once per core tick: a non-blocking WaitForSingleObject(h, 0) on
//            the guardian's handle, and the restart schedule after a loss.
//
// The journal and the duplicated core handle reach the relay through
// PROC_THREAD_ATTRIBUTE_HANDLE_LIST and nothing else: the section handle is
// inheritable, and blanket inheritance would leak it into any child.
//
// Pending gaps count as unprotected (P6): from a guardian's death until a new
// one is seen ATTACHED, the state is Unprotected{guardian_lost}.

#pragma once

#include "kgn/backends.hpp"
#include "obligation_journal.hpp"
#include "restart_policy.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <string>

namespace kgn::win {

class GuardianSupervisor {
public:
    using Clock = std::chrono::steady_clock;

    static constexpr Clock::duration kAttachTimeout = std::chrono::seconds(2);
    // The exit code given to a second guardian the supervisor terminates.
    static constexpr UINT kEvictedExitCode = 0xE5;

    // TEST-ONLY seams (ruling R8). Production code uses the two-argument
    // constructor, which guards the current process with an honest nonce.
    struct TestSeams {
        // The process the guardian is told to guard. Null: the current
        // process. Tests pass a stand-in child, because a guardian guarding
        // the test binary would outlive every assertion.
        HANDLE guarded = nullptr;
        // Put a nonce on the command line that differs from the header's.
        bool mismatchNonce = false;
    };

    GuardianSupervisor(ObligationJournal& journal, std::wstring guardianPath);
    GuardianSupervisor(ObligationJournal& journal, std::wstring guardianPath,
                       TestSeams seams);
    ~GuardianSupervisor();
    GuardianSupervisor(const GuardianSupervisor&) = delete;
    GuardianSupervisor& operator=(const GuardianSupervisor&) = delete;

    // Runs the spawn ladder and waits up to 2 s for ATTACHED.
    ProtectionStatus start();

    // Never blocks. Detects a lost guardian and drives the restart schedule.
    ProtectionStatus poll(Clock::time_point now);

    // Sets CLEAN_EXIT: the core has released everything and is ending.
    void markCleanExit();

    // The pid of the guardian currently supervised, 0 when there is none.
    [[nodiscard]] DWORD guardianPid() const { return guardianPid_; }

    // TEST-ONLY: the nonce written to the header, so a test can launch a
    // second, otherwise valid guardian with the same arguments.
    [[nodiscard]] const std::array<std::uint8_t, 16>& nonceForTests() const { return nonce_; }

private:
    // Failed: start() failed; terminal (R13). Exhausted: the restart policy
    // gave up, but a late guardian may still be adopted (R14).
    enum class Phase { Idle, Protected, Lost, Pending, Failed, Exhausted };

    enum class SpawnResult { Ok, SpawnFailed, BreakawayDenied };

    SpawnResult spawn();
    // True once the header shows ATTACHED and a pid whose handle opened.
    bool tryAdopt();
    // While Protected: a second guardian wrote its pid over the held one's.
    // Terminates it and restores the header (R14: never two guardians).
    void evictNewcomer();
    void closeRelay();
    void closeGuardian();
    // A loss or a failed respawn at `now`: schedule the next attempt or give up.
    ProtectionStatus scheduleRestart(Clock::time_point now);
    ProtectionStatus status() const;

    ObligationJournal& journal_;
    std::wstring guardianPath_;
    TestSeams seams_;
    std::array<std::uint8_t, 16> nonce_{};
    RestartPolicy policy_;

    Phase phase_ = Phase::Idle;
    std::string reason_;
    HANDLE relay_ = nullptr;
    HANDLE guardian_ = nullptr;
    DWORD guardianPid_ = 0;
    Clock::time_point nextAttemptAt_{};
    Clock::time_point attachDeadline_{};
};

// `keygnosys-guardian.exe` in the directory of the running executable.
std::wstring guardianPathBesideThisExecutable();

}  // namespace kgn::win

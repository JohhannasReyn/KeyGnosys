#include "guardian_supervisor.hpp"

#include <bcrypt.h>

#include <cstdio>
#include <thread>
#include <vector>

namespace kgn::win {

namespace {

// keygnosys-guardian's exit code when --relay launched the real guardian.
constexpr DWORD kRelayed = 5;

// Ladder step 2 applies: the core is in a job, and that job does not kill its
// processes when it closes, so a guardian inside it survives the core.
bool jobAllowsSpawningInside() {
    BOOL inJob = FALSE;
    if (!IsProcessInJob(GetCurrentProcess(), nullptr, &inJob) || !inJob) return false;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    if (!QueryInformationJobObject(nullptr, JobObjectExtendedLimitInformation, &info,
                                   sizeof info, nullptr))
        return false;
    return (info.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE) == 0;
}

bool inAnyJob() {
    BOOL inJob = FALSE;
    return IsProcessInJob(GetCurrentProcess(), nullptr, &inJob) && inJob;
}

std::wstring hexHandle(HANDLE h) {
    wchar_t buf[24];
    std::swprintf(buf, 24, L"%llx",
                  static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(h)));
    return buf;
}

std::wstring hexNonce(const std::array<std::uint8_t, 16>& nonce, bool mismatch) {
    wchar_t buf[40];
    for (std::size_t i = 0; i < nonce.size(); ++i) {
        const unsigned byte = mismatch ? (nonce[i] ^ 0xFFu) : nonce[i];
        std::swprintf(buf + 2 * i, 3, L"%02x", byte);
    }
    return std::wstring(buf, 32);
}

}  // namespace

GuardianSupervisor::GuardianSupervisor(ObligationJournal& journal, std::wstring guardianPath)
    : GuardianSupervisor(journal, std::move(guardianPath), TestSeams{}) {}

GuardianSupervisor::GuardianSupervisor(ObligationJournal& journal, std::wstring guardianPath,
                                       TestSeams seams)
    : journal_(journal),
      guardianPath_(std::move(guardianPath)),
      seams_(seams),
      policy_(Clock::now()) {}

GuardianSupervisor::~GuardianSupervisor() {
    // The guardian is not ours to stop: it ends on its own after the core.
    closeRelay();
    closeGuardian();
}

ProtectionStatus GuardianSupervisor::start() {
    HANDLE guarded = seams_.guarded ? seams_.guarded : GetCurrentProcess();

    FILETIME created{}, exited{}, kernel{}, user{};
    const DWORD pid = GetProcessId(guarded);
    const bool identified = pid != 0 && GetProcessTimes(guarded, &created, &exited, &kernel, &user);
    const bool random = BCRYPT_SUCCESS(BCryptGenRandom(nullptr, nonce_.data(),
                                                       static_cast<ULONG>(nonce_.size()),
                                                       BCRYPT_USE_SYSTEM_PREFERRED_RNG));
    if (!identified || !random) {
        phase_ = Phase::Failed;
        reason_ = "spawn_failed";
        return status();
    }
    journal_.writeHeader(pid, created, nonce_);

    switch (spawn()) {
        case SpawnResult::Ok: break;
        case SpawnResult::SpawnFailed:
            phase_ = Phase::Failed;
            reason_ = "spawn_failed";
            return status();
        case SpawnResult::BreakawayDenied:
            phase_ = Phase::Failed;
            reason_ = "breakaway_denied";
            return status();
    }

    // The one bounded wait. The input hook is not installed yet, so no input
    // (and no drag lock) can arrive during it.
    const Clock::time_point deadline = Clock::now() + kAttachTimeout;
    for (;;) {
        if (tryAdopt()) {
            phase_ = Phase::Protected;
            return status();
        }
        DWORD relayExit = STILL_ACTIVE;
        if (relay_ && WaitForSingleObject(relay_, 0) == WAIT_OBJECT_0 &&
            GetExitCodeProcess(relay_, &relayExit) && relayExit != kRelayed) {
            // The relay could not launch the guardian.
            closeRelay();
            phase_ = Phase::Failed;
            reason_ = "spawn_failed";
            return status();
        }
        if (Clock::now() >= deadline) {
            // A guardian that refused the header (nonce, pid, creation time)
            // exits without a word, so a refusal looks like a timeout here.
            closeRelay();
            phase_ = Phase::Failed;
            reason_ = "attach_timeout";
            return status();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

ProtectionStatus GuardianSupervisor::poll(Clock::time_point now) {
    switch (phase_) {
        case Phase::Protected:
            if (WaitForSingleObject(guardian_, 0) == WAIT_TIMEOUT) {
                evictNewcomer();
                return status();
            }
            closeGuardian();
            // The dead guardian's pid must never be adopted: a reused pid
            // would make a stranger pass for it.
            journal_.clearAttachment();
            return scheduleRestart(now);

        case Phase::Lost:
            // A respawn counted as failed may still attach late. With no live
            // guardian held, adopt it and cancel the pending respawn (R14).
            if (tryAdopt()) {
                phase_ = Phase::Protected;
                reason_.clear();
                return status();
            }
            if (now < nextAttemptAt_) return status();
            policy_.attemptMade(now);
            // No clearAttachment() here: the header was cleared when the loss
            // was detected, so anything in it now is a late guardian, which
            // tryAdopt() above would have taken. Clearing again could erase
            // one that attached a moment ago and leave it unsupervised.
            if (spawn() != SpawnResult::Ok) return scheduleRestart(now);
            phase_ = Phase::Pending;
            attachDeadline_ = now + kAttachTimeout;
            return status();

        case Phase::Pending: {
            if (tryAdopt()) {
                phase_ = Phase::Protected;
                reason_.clear();
                return status();
            }
            DWORD relayExit = STILL_ACTIVE;
            const bool relayFailed = relay_ && WaitForSingleObject(relay_, 0) == WAIT_OBJECT_0 &&
                                     GetExitCodeProcess(relay_, &relayExit) &&
                                     relayExit != kRelayed;
            if (relayFailed || now >= attachDeadline_) {
                closeRelay();
                return scheduleRestart(now);
            }
            return status();
        }

        case Phase::Exhausted:
            if (tryAdopt()) {
                phase_ = Phase::Protected;
                reason_.clear();
            }
            break;

        case Phase::Idle:
        case Phase::Failed:
            break;
    }
    return status();
}

void GuardianSupervisor::markCleanExit() { journal_.markCleanExit(); }

GuardianSupervisor::SpawnResult GuardianSupervisor::spawn() {
    closeRelay();
    HANDLE guarded = seams_.guarded ? seams_.guarded : GetCurrentProcess();

    // An inheritable handle the guardian can wait on and identify the core by.
    HANDLE core = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), guarded, GetCurrentProcess(), &core,
                         SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, TRUE, 0))
        return SpawnResult::SpawnFailed;

    HANDLE handles[2] = {journal_.sectionHandle(), core};
    std::wstring cmd = L"\"" + guardianPath_ + L"\" --relay --section " + hexHandle(handles[0]) +
                       L" --core " + hexHandle(handles[1]) + L" --nonce " +
                       hexNonce(nonce_, seams_.mismatchNonce);

    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<unsigned char> storage(size);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(attrs, 1, 0, &size)) {
        CloseHandle(core);
        return SpawnResult::SpawnFailed;
    }
    // Exactly these two handles are inherited, never anything else the core
    // holds inheritable.
    if (!UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles,
                                   sizeof handles, nullptr, nullptr)) {
        DeleteProcThreadAttributeList(attrs);
        CloseHandle(core);
        return SpawnResult::SpawnFailed;
    }

    auto launch = [&](DWORD extraFlags, PROCESS_INFORMATION& pi) {
        STARTUPINFOEXW si{};
        si.StartupInfo.cb = sizeof si;
        si.lpAttributeList = attrs;
        std::vector<wchar_t> line(cmd.begin(), cmd.end());
        line.push_back(L'\0');
        return CreateProcessW(guardianPath_.c_str(), line.data(), nullptr, nullptr, TRUE,
                              DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP |
                                  EXTENDED_STARTUPINFO_PRESENT | extraFlags,
                              nullptr, nullptr, &si.StartupInfo, &pi) != FALSE;
    };

    SpawnResult result = SpawnResult::Ok;
    PROCESS_INFORMATION pi{};
    // Ladder step 1: out of any job the core is in.
    if (!launch(CREATE_BREAKAWAY_FROM_JOB, pi)) {
        const DWORD error = GetLastError();
        if (error != ERROR_ACCESS_DENIED || !inAnyJob()) {
            result = SpawnResult::SpawnFailed;
        } else if (!jobAllowsSpawningInside()) {
            // Step 3: the job kills on close and will not let the guardian out.
            result = SpawnResult::BreakawayDenied;
        } else if (!launch(0, pi)) {
            // Step 2 tried: inside the job, which does not kill on close.
            result = SpawnResult::SpawnFailed;
        }
    }
    DeleteProcThreadAttributeList(attrs);
    // The relay has its own copy now; the core keeps none.
    CloseHandle(core);
    if (result != SpawnResult::Ok) return result;

    CloseHandle(pi.hThread);
    relay_ = pi.hProcess;
    return SpawnResult::Ok;
}

bool GuardianSupervisor::tryAdopt() {
    if (!journal_.attached()) return false;
    const DWORD pid = journal_.guardianPid();
    if (pid == 0) return false;
    // Opened once, here, and kept: liveness is a wait on this handle, never a
    // fresh open by pid, which a reused pid would answer for a stranger.
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!h) return false;
    if (WaitForSingleObject(h, 0) != WAIT_TIMEOUT) {
        CloseHandle(h);
        return false;
    }
    closeGuardian();
    guardian_ = h;
    guardianPid_ = pid;
    closeRelay();
    return true;
}

void GuardianSupervisor::evictNewcomer() {
    const DWORD pid = journal_.guardianPid();
    if (pid == 0 || pid == guardianPid_) return;
    // The newcomer wrote its pid and is waiting on the core: it is alive, so
    // the pid is still its own.
    if (HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid)) {
        TerminateProcess(h, kEvictedExitCode);
        WaitForSingleObject(h, 100);
        CloseHandle(h);
    }
    journal_.restoreAttachment(guardianPid_);
}

void GuardianSupervisor::closeRelay() {
    if (relay_) CloseHandle(relay_);
    relay_ = nullptr;
}

void GuardianSupervisor::closeGuardian() {
    if (guardian_) CloseHandle(guardian_);
    guardian_ = nullptr;
    guardianPid_ = 0;
}

ProtectionStatus GuardianSupervisor::scheduleRestart(Clock::time_point now) {
    const auto next = policy_.nextAttempt(now);
    if (!next) {
        phase_ = Phase::Exhausted;
        reason_ = "restart_exhausted";
    } else {
        phase_ = Phase::Lost;
        reason_ = "guardian_lost";
        nextAttemptAt_ = *next;
    }
    return status();
}

ProtectionStatus GuardianSupervisor::status() const {
    switch (phase_) {
        case Phase::Protected: return {Protection::Protected, ""};
        case Phase::Lost:
        case Phase::Pending: return {Protection::Unprotected, "guardian_lost"};
        case Phase::Failed:
        case Phase::Exhausted: return {Protection::Unprotected, reason_};
        case Phase::Idle: break;
    }
    // Not started: nothing has been spawned, so nothing protects.
    return {Protection::Unprotected, "spawn_failed"};
}

std::wstring guardianPathBesideThisExecutable() {
    std::vector<wchar_t> buf(32768);
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0 || n >= buf.size()) return L"keygnosys-guardian.exe";
    std::wstring path(buf.data(), n);
    const auto slash = path.find_last_of(L"\\/");
    path.erase(slash == std::wstring::npos ? 0 : slash + 1);
    return path + L"keygnosys-guardian.exe";
}

}  // namespace kgn::win

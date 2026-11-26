// The guardian supervisor: the restart schedule (pure) and, against the real
// keygnosys-guardian.exe, spawn, attach, liveness and restart.
//
// The integration tests never let a guardian guard this test binary: each one
// guards a throwaway ping.exe the test owns and terminates. Windows reports no
// button down while they run, so no guardian here injects anything; only the
// one test that needs a journaled DOWN slot sets one.

#if defined(_WIN32)

#include "../src/platform/windows/guardian_supervisor.hpp"
#include "../src/platform/windows/obligation_journal.hpp"
#include "../src/platform/windows/restart_policy.hpp"
#include "../src/platform/windows/sendinput_output.hpp"

#include <tlhelp32.h>

#include "kgn_test.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

using namespace kgn;
using namespace kgn::win;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace {

const std::wstring kGuardianExe = L"" KGN_GUARDIAN_EXE;

// A process for the guardian to guard, killed when the test is done with it.
class StandIn {
public:
    StandIn() {
        wchar_t sys[MAX_PATH];
        GetSystemDirectoryW(sys, MAX_PATH);
        std::wstring exe = std::wstring(sys) + L"\\PING.EXE";
        std::wstring cmd = L"\"" + exe + L"\" -n 120 127.0.0.1";
        STARTUPINFOW si{};
        si.cb = sizeof si;
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                           CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            process_ = pi.hProcess;
        }
    }
    ~StandIn() {
        end();
        if (process_) CloseHandle(process_);
    }
    StandIn(const StandIn&) = delete;
    StandIn& operator=(const StandIn&) = delete;

    [[nodiscard]] HANDLE handle() const { return process_; }
    void end() {
        if (process_ && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT) {
            TerminateProcess(process_, 1);
            WaitForSingleObject(process_, 5000);
        }
    }

private:
    HANDLE process_ = nullptr;
};

// A handle to a live guardian, enough to wait on, read and end it.
class Watched {
public:
    explicit Watched(DWORD pid)
        : h_(pid ? OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION |
                                   PROCESS_TERMINATE,
                               FALSE, pid)
                 : nullptr) {}
    ~Watched() {
        // Never leave a guardian behind, whatever the test concluded.
        if (h_ && WaitForSingleObject(h_, 0) == WAIT_TIMEOUT) {
            TerminateProcess(h_, 99);
            WaitForSingleObject(h_, 5000);
        }
        if (h_) CloseHandle(h_);
    }
    Watched(const Watched&) = delete;
    Watched& operator=(const Watched&) = delete;

    [[nodiscard]] bool valid() const { return h_ != nullptr; }
    // The exit code, or -1 if it did not exit within `within`.
    [[nodiscard]] long exitCode(std::chrono::milliseconds within) const {
        if (!h_ || WaitForSingleObject(h_, static_cast<DWORD>(within.count())) != WAIT_OBJECT_0)
            return -1;
        DWORD code = 0;
        GetExitCodeProcess(h_, &code);
        return static_cast<long>(code);
    }
    void kill() const {
        TerminateProcess(h_, 9);
        WaitForSingleObject(h_, 5000);
    }

private:
    HANDLE h_;
};

DWORD parentOf(DWORD pid) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof entry;
    DWORD parent = 0;
    for (BOOL ok = Process32FirstW(snap, &entry); ok; ok = Process32NextW(snap, &entry)) {
        if (entry.th32ProcessID == pid) {
            parent = entry.th32ParentProcessID;
            break;
        }
    }
    CloseHandle(snap);
    return parent;
}

bool anyButtonDown() {
    return (GetAsyncKeyState(VK_LBUTTON) & 0x8000) || (GetAsyncKeyState(VK_RBUTTON) & 0x8000) ||
           (GetAsyncKeyState(VK_MBUTTON) & 0x8000);
}

// Launches the guardian directly (no relay) and returns its process handle.
HANDLE launchGuardianDirectly(ObligationJournal& journal, HANDLE guarded, const char* nonceHex) {
    HANDLE core = nullptr;
    DuplicateHandle(GetCurrentProcess(), guarded, GetCurrentProcess(), &core,
                    SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, TRUE, 0);
    HANDLE handles[2] = {journal.sectionHandle(), core};
    wchar_t cmd[512];
    std::swprintf(cmd, 512, L"\"%ls\" --section %llx --core %llx --nonce %hs",
                  kGuardianExe.c_str(),
                  static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(handles[0])),
                  static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(handles[1])),
                  nonceHex);
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::string storage(size, '\0');
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    InitializeProcThreadAttributeList(attrs, 1, 0, &size);
    UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles,
                              sizeof handles, nullptr, nullptr);
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    si.lpAttributeList = attrs;
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(kGuardianExe.c_str(), cmd, nullptr, nullptr, TRUE,
                                   DETACHED_PROCESS | EXTENDED_STARTUPINFO_PRESENT, nullptr,
                                   nullptr, &si.StartupInfo, &pi);
    DeleteProcThreadAttributeList(attrs);
    CloseHandle(core);
    if (!ok) return nullptr;
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

// Runs the guardian directly, so its exit code is ours to read.
long runGuardianDirectly(ObligationJournal& journal, HANDLE guarded, const char* nonceHex) {
    PROCESS_INFORMATION pi{};
    pi.hProcess = launchGuardianDirectly(journal, guarded, nonceHex);
    if (!pi.hProcess) return -2;
    long code = -1;
    if (WaitForSingleObject(pi.hProcess, 5000) == WAIT_OBJECT_0) {
        DWORD c = 0;
        GetExitCodeProcess(pi.hProcess, &c);
        code = static_cast<long>(c);
    } else {
        TerminateProcess(pi.hProcess, 99);
    }
    CloseHandle(pi.hProcess);
    return code;
}

std::string hex(const std::array<std::uint8_t, 16>& bytes) {
    char out[33];
    for (std::size_t i = 0; i < bytes.size(); ++i)
        std::snprintf(out + 2 * i, 3, "%02x", static_cast<unsigned>(bytes[i]));
    return std::string(out, 32);
}

// Live keygnosys-guardian.exe processes on the machine.
int liveGuardians() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return -1;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof entry;
    int n = 0;
    for (BOOL ok = Process32FirstW(snap, &entry); ok; ok = Process32NextW(snap, &entry))
        if (_wcsicmp(entry.szExeFile, L"keygnosys-guardian.exe") == 0) ++n;
    CloseHandle(snap);
    return n;
}

// Polls with the real clock until `done` or `within` elapses.
template <typename Pred>
ProtectionStatus pollUntil(GuardianSupervisor& s, std::chrono::milliseconds within, Pred done) {
    const auto deadline = Clock::now() + within;
    ProtectionStatus st = s.poll(Clock::now());
    while (!done(st) && Clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
        st = s.poll(Clock::now());
    }
    return st;
}

}  // namespace

// --- RestartPolicy ----------------------------------------------------------

KGN_TEST(restart_policy_follows_1_5_30_then_exhausts) {
    const auto t0 = Clock::now();
    RestartPolicy policy(t0);

    auto next = policy.nextAttempt(t0);
    KGN_CHECK(next.has_value() && *next == t0 + 1s);
    policy.attemptMade(t0 + 1s);

    next = policy.nextAttempt(t0 + 2s);
    KGN_CHECK(next.has_value() && *next == t0 + 2s + 5s);
    policy.attemptMade(t0 + 7s);

    next = policy.nextAttempt(t0 + 8s);
    KGN_CHECK(next.has_value() && *next == t0 + 8s + 30s);
    policy.attemptMade(t0 + 38s);

    KGN_CHECK(!policy.nextAttempt(t0 + 40s).has_value());
    KGN_CHECK(!policy.nextAttempt(t0 + 9min).has_value());
}

KGN_TEST(restart_policy_window_slides_after_10_minutes) {
    const auto t0 = Clock::now();
    RestartPolicy policy(t0);
    policy.attemptMade(t0 + 1s);
    policy.attemptMade(t0 + 7s);
    policy.attemptMade(t0 + 38s);
    KGN_CHECK(!policy.nextAttempt(t0 + 10min).has_value());

    // The first attempt has aged out; two remain, so the backoff is the third.
    auto next = policy.nextAttempt(t0 + 10min + 2s);
    KGN_CHECK(next.has_value() && *next == t0 + 10min + 2s + 30s);

    // All three aged out: the schedule starts over at 1 s.
    next = policy.nextAttempt(t0 + 11min);
    KGN_CHECK(next.has_value() && *next == t0 + 11min + 1s);
}

// --- Integration: the real guardian ------------------------------------------

KGN_TEST(a_guardian_attaches_within_the_timeout) {
    // Which spawn-ladder step this run exercises (step 1 outside a job or with
    // breakaway allowed, step 2 in a job without KILL_ON_JOB_CLOSE).
    BOOL inJob = FALSE;
    IsProcessInJob(GetCurrentProcess(), nullptr, &inJob);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION job{};
    if (inJob)
        QueryInformationJobObject(nullptr, JobObjectExtendedLimitInformation, &job, sizeof job,
                                  nullptr);
    std::printf("  test process in a job: %s, LimitFlags 0x%lx\n", inJob ? "yes" : "no",
                static_cast<unsigned long>(job.BasicLimitInformation.LimitFlags));

    auto journal = ObligationJournal::create();
    StandIn child;
    KGN_CHECK(journal != nullptr && child.handle() != nullptr);
    GuardianSupervisor supervisor(*journal, kGuardianExe, {child.handle(), false});

    const auto began = Clock::now();
    const ProtectionStatus st = supervisor.start();
    KGN_CHECK(st.state == Protection::Protected);
    KGN_CHECK(st.reason.empty());
    KGN_CHECK(Clock::now() - began <= GuardianSupervisor::kAttachTimeout + 200ms);
    KGN_CHECK(journal->attached());
    KGN_CHECK(journal->guardianPid() != 0);
    KGN_CHECK_EQ(journal->guardianPid(), supervisor.guardianPid());
    // The relay is the guardian's parent, not the process that spawned it.
    KGN_CHECK(parentOf(journal->guardianPid()) != GetCurrentProcessId());

    Watched guardian(journal->guardianPid());
    KGN_CHECK(supervisor.poll(Clock::now()).state == Protection::Protected);
    supervisor.markCleanExit();
    child.end();
    KGN_CHECK_EQ(guardian.exitCode(3000ms), 0L);
}

KGN_TEST(a_guardian_exits_clean_when_nothing_is_held) {
    auto journal = ObligationJournal::create();
    StandIn child;
    GuardianSupervisor supervisor(*journal, kGuardianExe, {child.handle(), false});
    KGN_CHECK(supervisor.start().state == Protection::Protected);
    Watched guardian(journal->guardianPid());
    KGN_CHECK(guardian.valid());

    // The clean-close path: everything released, CLEAN_EXIT set, then the
    // guarded process ends. The guardian returns before recovery is reached,
    // so no sendUp path can run.
    for (auto b : {MouseButton::Left, MouseButton::Right, MouseButton::Middle})
        KGN_CHECK(journal->slot(b) == SlotState::UP);
    supervisor.markCleanExit();
    child.end();
    KGN_CHECK_EQ(guardian.exitCode(3000ms), 0L);
}

KGN_TEST(a_guardian_skips_a_journaled_button_windows_reports_up) {
    if (anyButtonDown()) {
        std::printf("  skipped: a mouse button is physically down\n");
        return;
    }
    auto journal = ObligationJournal::create();
    StandIn child;
    GuardianSupervisor supervisor(*journal, kGuardianExe, {child.handle(), false});
    KGN_CHECK(supervisor.start().state == Protection::Protected);
    Watched guardian(journal->guardianPid());

    // A death between a successful Up and the UP record: the journal still
    // says DOWN, Windows says up. No clean exit marked, so recovery runs.
    journal->setSlot(MouseButton::Left, SlotState::DOWN);
    child.end();
    KGN_CHECK_EQ(guardian.exitCode(3000ms), 0L);
    KGN_CHECK(!anyButtonDown());
}

KGN_TEST(a_wrong_nonce_is_refused) {
    StandIn child;

    // Run directly so the exit code is readable: header nonce 00..00,
    // command-line nonce ff..ff.
    {
        auto journal = ObligationJournal::create();
        FILETIME created{}, x{}, k{}, u{};
        GetProcessTimes(child.handle(), &created, &x, &k, &u);
        journal->writeHeader(GetProcessId(child.handle()), created, {});
        KGN_CHECK_EQ(runGuardianDirectly(*journal, child.handle(),
                                         "ffffffffffffffffffffffffffffffff"),
                     3L);
        KGN_CHECK(!journal->attached());
    }

    // Through the supervisor: the guardian refuses and never attaches.
    auto journal = ObligationJournal::create();
    GuardianSupervisor supervisor(*journal, kGuardianExe, {child.handle(), true});
    const ProtectionStatus st = supervisor.start();
    KGN_CHECK(st.state == Protection::Unprotected);
    KGN_CHECK(st.reason == "attach_timeout" || st.reason == "guardian_identity_mismatch");
    KGN_CHECK(!journal->attached());
    KGN_CHECK_EQ(journal->guardianPid(), 0u);
    KGN_CHECK(supervisor.poll(Clock::now()).reason == st.reason);
}

KGN_TEST(a_missing_guardian_binary_reports_spawn_failed) {
    auto journal = ObligationJournal::create();
    StandIn child;
    GuardianSupervisor supervisor(*journal, L"C:\\nonexistent-kgn\\keygnosys-guardian.exe",
                                  {child.handle(), false});
    const ProtectionStatus st = supervisor.start();
    KGN_CHECK(st.state == Protection::Unprotected);
    KGN_CHECK(st.reason == "spawn_failed");
    const ProtectionStatus later = supervisor.poll(Clock::now() + 1min);
    KGN_CHECK(later.state == Protection::Unprotected);
    KGN_CHECK(later.reason == "spawn_failed");
}

KGN_TEST(a_killed_guardian_reports_guardian_lost) {
    auto journal = ObligationJournal::create();
    StandIn child;
    GuardianSupervisor supervisor(*journal, kGuardianExe, {child.handle(), false});
    KGN_CHECK(supervisor.start().state == Protection::Protected);
    Watched guardian(journal->guardianPid());
    KGN_CHECK(guardian.valid());

    guardian.kill();
    const ProtectionStatus st = supervisor.poll(Clock::now());
    KGN_CHECK(st.state == Protection::Unprotected);
    KGN_CHECK(st.reason == "guardian_lost");
    // Still inside the 1 s backoff: still lost, nothing respawned.
    const ProtectionStatus again = supervisor.poll(Clock::now());
    KGN_CHECK(again.reason == "guardian_lost");
    KGN_CHECK_EQ(supervisor.guardianPid(), 0u);
}

KGN_TEST(a_restarted_guardian_reattaches_to_the_same_journal) {
    auto journal = ObligationJournal::create();
    StandIn child;
    GuardianSupervisor supervisor(*journal, kGuardianExe, {child.handle(), false});
    KGN_CHECK(supervisor.start().state == Protection::Protected);
    const DWORD firstPid = journal->guardianPid();
    {
        Watched first(firstPid);
        first.kill();
    }
    const auto lostAt = Clock::now();
    KGN_CHECK(supervisor.poll(Clock::now()).reason == "guardian_lost");

    const ProtectionStatus st = pollUntil(supervisor, 5000ms, [](const ProtectionStatus& s) {
        return s.state == Protection::Protected;
    });
    KGN_CHECK(st.state == Protection::Protected);
    KGN_CHECK(Clock::now() - lostAt >= 1s);   // after the backoff, not before
    const DWORD secondPid = journal->guardianPid();
    KGN_CHECK(secondPid != 0 && secondPid != firstPid);
    KGN_CHECK_EQ(supervisor.guardianPid(), secondPid);
    KGN_CHECK(journal->attached());

    Watched second(secondPid);
    supervisor.markCleanExit();
    child.end();
    KGN_CHECK_EQ(second.exitCode(3000ms), 0L);
}

KGN_TEST(a_respawn_that_attaches_after_its_window_is_adopted_not_doubled) {
    const int before = liveGuardians();   // a real core may have its own
    auto journal = ObligationJournal::create();
    StandIn child;
    GuardianSupervisor supervisor(*journal, kGuardianExe, {child.handle(), false});
    KGN_CHECK(supervisor.start().state == Protection::Protected);
    {
        Watched first(journal->guardianPid());
        first.kill();
    }
    // A made-up clock drives the schedule; the guardians run in real time.
    const auto t = Clock::now();
    KGN_CHECK(supervisor.poll(t).reason == "guardian_lost");
    KGN_CHECK(supervisor.poll(t + 1s).reason == "guardian_lost");   // respawn now
    // Microseconds later by the real clock, but 3 s by the passed one: the
    // window is counted failed before the guardian can have attached.
    KGN_CHECK(supervisor.poll(t + 4s).reason == "guardian_lost");
    // The next attempt is due at t + 9 s; stay before it while it attaches.
    const auto deadline = Clock::now() + 3s;
    ProtectionStatus st = supervisor.poll(t + 5s);
    while (st.state != Protection::Protected && Clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
        st = supervisor.poll(t + 5s);
    }
    KGN_CHECK(st.state == Protection::Protected);
    const DWORD adopted = supervisor.guardianPid();
    KGN_CHECK(adopted != 0 && adopted == journal->guardianPid());
    Watched late(adopted);

    // The pending respawn was cancelled: past its due time, nothing new.
    KGN_CHECK(supervisor.poll(t + 60s).state == Protection::Protected);
    std::this_thread::sleep_for(300ms);
    KGN_CHECK(supervisor.poll(t + 61s).state == Protection::Protected);
    KGN_CHECK_EQ(liveGuardians(), before + 1);
    KGN_CHECK_EQ(supervisor.guardianPid(), adopted);

    supervisor.markCleanExit();
    child.end();
    KGN_CHECK_EQ(late.exitCode(3000ms), 0L);
}

KGN_TEST(a_guardian_that_attaches_after_the_initial_timeout_is_adopted) {
    const int before = liveGuardians();   // a real core may have its own
    auto journal = ObligationJournal::create();
    StandIn child;
    // The spawned guardian is given a wrong nonce and refuses, so start()
    // runs out its 2 s and reports attach_timeout.
    GuardianSupervisor supervisor(*journal, kGuardianExe, {child.handle(), true});
    const ProtectionStatus st = supervisor.start();
    KGN_CHECK(st.state == Protection::Unprotected);
    KGN_CHECK(st.reason == "attach_timeout");
    KGN_CHECK(supervisor.poll(Clock::now()).reason == "attach_timeout");

    // A slow guardian for the same header, attaching after the window.
    HANDLE late = launchGuardianDirectly(*journal, child.handle(),
                                         hex(supervisor.nonceForTests()).c_str());
    KGN_CHECK(late != nullptr);
    const DWORD latePid = late ? GetProcessId(late) : 0;
    const ProtectionStatus adopted = pollUntil(supervisor, 3000ms, [](const ProtectionStatus& s) {
        return s.state == Protection::Protected;
    });
    KGN_CHECK(adopted.state == Protection::Protected);
    KGN_CHECK(adopted.reason.empty());
    KGN_CHECK_EQ(supervisor.guardianPid(), latePid);

    // Adopted, not retried: long past any schedule, still one guardian.
    KGN_CHECK(supervisor.poll(Clock::now() + 60s).state == Protection::Protected);
    std::this_thread::sleep_for(300ms);
    KGN_CHECK(supervisor.poll(Clock::now() + 61s).state == Protection::Protected);
    KGN_CHECK_EQ(liveGuardians(), before + 1);

    supervisor.markCleanExit();
    child.end();
    if (late) {
        KGN_CHECK(WaitForSingleObject(late, 3000) == WAIT_OBJECT_0);
        DWORD code = 99;
        GetExitCodeProcess(late, &code);
        KGN_CHECK_EQ(code, 0u);
        if (WaitForSingleObject(late, 0) == WAIT_TIMEOUT) TerminateProcess(late, 99);
        CloseHandle(late);
    }
}

KGN_TEST(a_failed_start_other_than_a_timeout_adopts_nothing) {
    auto journal = ObligationJournal::create();
    StandIn child;
    GuardianSupervisor supervisor(*journal, L"C:\\nonexistent-kgn\\keygnosys-guardian.exe",
                                  {child.handle(), false});
    KGN_CHECK(supervisor.start().reason == "spawn_failed");

    HANDLE stray = launchGuardianDirectly(*journal, child.handle(),
                                          hex(supervisor.nonceForTests()).c_str());
    KGN_CHECK(stray != nullptr);
    const auto deadline = Clock::now() + 3s;
    while (!journal->attached() && Clock::now() < deadline) std::this_thread::sleep_for(5ms);
    KGN_CHECK(journal->attached());
    // Only an attach_timeout can be a late attach; spawn_failed stays failed.
    const ProtectionStatus st = supervisor.poll(Clock::now());
    KGN_CHECK(st.state == Protection::Unprotected);
    KGN_CHECK(st.reason == "spawn_failed");
    KGN_CHECK_EQ(supervisor.guardianPid(), 0u);

    supervisor.markCleanExit();
    child.end();
    if (stray) {
        WaitForSingleObject(stray, 3000);
        if (WaitForSingleObject(stray, 0) == WAIT_TIMEOUT) TerminateProcess(stray, 99);
        CloseHandle(stray);
    }
}

KGN_TEST(a_second_guardian_is_terminated_and_the_held_one_restored) {
    const int before = liveGuardians();   // a real core may have its own
    auto journal = ObligationJournal::create();
    StandIn child;
    GuardianSupervisor supervisor(*journal, kGuardianExe, {child.handle(), false});
    KGN_CHECK(supervisor.start().state == Protection::Protected);
    const DWORD held = supervisor.guardianPid();
    Watched heldGuardian(held);

    // An otherwise valid guardian for the same core and journal.
    HANDLE second = launchGuardianDirectly(*journal, child.handle(),
                                           hex(supervisor.nonceForTests()).c_str());
    KGN_CHECK(second != nullptr);
    const DWORD secondPid = second ? GetProcessId(second) : 0;
    const auto deadline = Clock::now() + 3s;
    while (journal->guardianPid() != secondPid && Clock::now() < deadline)
        std::this_thread::sleep_for(5ms);
    KGN_CHECK_EQ(journal->guardianPid(), secondPid);   // it attached over the held one

    KGN_CHECK(supervisor.poll(Clock::now()).state == Protection::Protected);
    if (second) {
        KGN_CHECK(WaitForSingleObject(second, 2000) == WAIT_OBJECT_0);
        DWORD code = 0;
        GetExitCodeProcess(second, &code);
        KGN_CHECK_EQ(code, static_cast<DWORD>(GuardianSupervisor::kEvictedExitCode));
        if (WaitForSingleObject(second, 0) == WAIT_TIMEOUT) TerminateProcess(second, 99);
        CloseHandle(second);
    }
    KGN_CHECK_EQ(journal->guardianPid(), held);
    KGN_CHECK(journal->attached());
    KGN_CHECK_EQ(supervisor.guardianPid(), held);
    KGN_CHECK_EQ(liveGuardians(), before + 1);

    supervisor.markCleanExit();
    child.end();
    KGN_CHECK_EQ(heldGuardian.exitCode(3000ms), 0L);
}

// --- The output backend reports what its supervisor sees -----------------------

KGN_TEST(the_output_backend_without_a_guardian_reports_not_applicable) {
    SendInputOutput output;
    KGN_CHECK(output.protection().state == Protection::NotApplicable);
    output.markCleanExit();   // harmless with nothing to mark
}

KGN_TEST(the_output_backend_reports_its_supervisors_state) {
    StandIn child;
    auto journal = ObligationJournal::create();
    auto supervisor = std::make_unique<GuardianSupervisor>(
        *journal, L"C:\\nonexistent-kgn\\keygnosys-guardian.exe",
        GuardianSupervisor::TestSeams{child.handle(), false});
    KGN_CHECK(supervisor->start().reason == "spawn_failed");
    SendInputOutput output;
    output.adoptGuardian(std::move(journal), std::move(supervisor));
    const ProtectionStatus st = output.protection();
    KGN_CHECK(st.state == Protection::Unprotected);
    KGN_CHECK(st.reason == "spawn_failed");
}

KGN_TEST(the_output_backend_marks_a_clean_exit_in_its_journal) {
    StandIn child;
    auto journal = ObligationJournal::create();
    ObligationJournal* seen = journal.get();
    auto supervisor = std::make_unique<GuardianSupervisor>(
        *journal, kGuardianExe, GuardianSupervisor::TestSeams{child.handle(), false});
    KGN_CHECK(supervisor->start().state == Protection::Protected);
    Watched guardian(seen->guardianPid());
    SendInputOutput output;
    output.adoptGuardian(std::move(journal), std::move(supervisor));
    KGN_CHECK(output.protection().state == Protection::Protected);

    output.markCleanExit();
    void* view = ::MapViewOfFile(seen->sectionHandle(), FILE_MAP_READ, 0, 0, kJournalPageSize);
    KGN_CHECK(view != nullptr);
    if (view) {
        const auto* header = static_cast<const JournalHeader*>(view);
        KGN_CHECK((header->flags.load() & kFlagCleanExit) != 0);
        ::UnmapViewOfFile(view);
    }
    child.end();
    KGN_CHECK_EQ(guardian.exitCode(3000ms), 0L);
}

KGN_TEST(an_output_backend_whose_journal_failed_reports_spawn_failed) {
    SendInputOutput output;
    output.setProtectionFallback({Protection::Unprotected, "spawn_failed"});
    const ProtectionStatus st = output.protection();
    KGN_CHECK(st.state == Protection::Unprotected);
    KGN_CHECK(st.reason == "spawn_failed");
}

// --- The spawn ladder inside a job -------------------------------------------
//
// The ladder reads the job of the CURRENT process, so steps 2 and 3 can only
// be reached from a process inside a job of the right kind. This binary
// re-launches itself with `--ladder <LimitFlags hex>`: the child puts itself
// in a new (nested) job with those flags, starts a supervisor, and reports the
// outcome in its exit code.

namespace {

constexpr int kLadderProtected = 10;
constexpr int kLadderBreakawayDenied = 11;
constexpr int kLadderOther = 12;
constexpr int kLadderSetupFailed = 13;

int ladderProbe(DWORD limitFlags) {
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    info.BasicLimitInformation.LimitFlags = limitFlags;
    if (!job ||
        !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof info) ||
        !AssignProcessToJobObject(job, GetCurrentProcess()))
        return kLadderSetupFailed;

    int result = kLadderOther;
    {
        StandIn child;   // inside the job too; a kill-on-close job ends it with us
        auto journal = ObligationJournal::create();
        if (!journal || !child.handle()) return kLadderSetupFailed;
        GuardianSupervisor supervisor(*journal, kGuardianExe, {child.handle(), false});
        const ProtectionStatus st = supervisor.start();
        if (st.state == Protection::Protected) {
            result = kLadderProtected;
            Watched guardian(journal->guardianPid());
            supervisor.markCleanExit();
            child.end();
            if (guardian.exitCode(3000ms) != 0) result = kLadderOther;
        } else if (st.reason == "breakaway_denied") {
            result = kLadderBreakawayDenied;
        }
    }
    // The job handle is deliberately not closed: closing the last handle of a
    // kill-on-close job would terminate this process before it could return
    // its result. Process exit closes it.
    return result;
}

long runLadderProbe(DWORD limitFlags) {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    wchar_t cmd[MAX_PATH + 40];
    std::swprintf(cmd, MAX_PATH + 40, L"\"%ls\" --ladder %lx", self,
                  static_cast<unsigned long>(limitFlags));
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(self, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                        &si, &pi))
        return -2;
    CloseHandle(pi.hThread);
    long code = -1;
    if (WaitForSingleObject(pi.hProcess, 10000) == WAIT_OBJECT_0) {
        DWORD c = 0;
        GetExitCodeProcess(pi.hProcess, &c);
        code = static_cast<long>(c);
    } else {
        TerminateProcess(pi.hProcess, 99);
    }
    CloseHandle(pi.hProcess);
    return code;
}

}  // namespace

KGN_TEST(ladder_step_2_spawns_inside_a_job_that_does_not_kill_on_close) {
    // LimitFlags 0: no breakaway, no kill on close -- the detached-start host.
    KGN_CHECK_EQ(runLadderProbe(0), static_cast<long>(kLadderProtected));
}

KGN_TEST(ladder_step_3_reports_breakaway_denied_in_a_job_that_kills_on_close) {
    KGN_CHECK_EQ(runLadderProbe(JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE),
                 static_cast<long>(kLadderBreakawayDenied));
}

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--ladder")
        return ladderProbe(static_cast<DWORD>(std::strtoul(argv[2], nullptr, 16)));
    return kgn::test::runAll();
}

#else

int main() { return 0; }

#endif

// keygnosys-guardian: Win32 glue around guardian_logic.
//
// It waits for the core process to end. If the core did not mark a clean exit
// it lifts mouse buttons the journal says were held, and only if Windows still
// reports them down. The only input it can ever inject is a mouse-button Up.
// No hook, IPC, window, registry or network.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "guardian/guardian_logic.hpp"

using namespace kgn::guardian;

namespace {

// Wide APIs for every path: a profile or install directory need not be
// representable in the ANSI code page.
void logLine(const char* text) {
    std::wstring base(32768, L'\0');
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base.data(),
                                            static_cast<DWORD>(base.size()));
    if (n == 0 || n >= base.size()) return;
    base.resize(n);
    std::wstring dir = base + L"\\KeyGnosys";
    CreateDirectoryW(dir.c_str(), nullptr);
    dir += L"\\logs";
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring path = dir + L"\\guardian.log";
    HANDLE f = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    char line[512];
    const int len = std::snprintf(line, sizeof line, "%04d-%02d-%02d %02d:%02d:%02d guardian[%lu]: %s\r\n",
                                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                                  static_cast<unsigned long>(GetCurrentProcessId()), text);
    DWORD written = 0;
    if (len > 0) WriteFile(f, line, static_cast<DWORD>(len), &written, nullptr);
    CloseHandle(f);
}

int finish(GuardianExit code, const char* logMessage = nullptr) {
    if (logMessage) logLine(logMessage);
    return static_cast<int>(code);
}


// Re-launch this executable without --relay, detached, inheriting exactly the
// two handles, then return so the caller can exit at once.
int relay(const GuardianArgs& a) {
    std::wstring exe(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
    if (n == 0 || n >= exe.size()) return finish(GuardianExit::BadArgs, "relay: no module path");
    exe.resize(n);

    HANDLE handles[2] = {reinterpret_cast<HANDLE>(a.section),
                         reinterpret_cast<HANDLE>(a.coreProcess)};
    // HANDLE_LIST requires the handles to be inheritable.
    for (HANDLE h : handles) SetHandleInformation(h, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);

    // The arguments are hex; only the executable path needs to be wide.
    char args[128];
    int len = std::snprintf(args, sizeof args, " --section %llx --core %llx --nonce ",
                            static_cast<unsigned long long>(a.section),
                            static_cast<unsigned long long>(a.coreProcess));
    if (len <= 0 || len + 33 > static_cast<int>(sizeof args))
        return finish(GuardianExit::BadArgs, "relay: command line too long");
    for (std::size_t i = 0; i < 16; ++i)
        std::snprintf(args + len + 2 * i, 3, "%02x", static_cast<unsigned>(a.nonce[i]));
    std::wstring cmd = L"\"" + exe + L"\"";
    for (const char* c = args; *c; ++c) cmd += static_cast<wchar_t>(*c);
    std::vector<wchar_t> line(cmd.begin(), cmd.end());   // CreateProcessW may write to it
    line.push_back(L'\0');

    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::string storage(size, '\0');
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(attrs, 1, 0, &size))
        return finish(GuardianExit::BadArgs, "relay: attribute list init failed");
    if (!UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles,
                                   sizeof handles, nullptr, nullptr)) {
        DeleteProcThreadAttributeList(attrs);
        return finish(GuardianExit::BadArgs, "relay: handle list failed");
    }

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    si.lpAttributeList = attrs;
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(exe.c_str(), line.data(), nullptr, nullptr, TRUE,
                                   DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP |
                                       EXTENDED_STARTUPINFO_PRESENT,
                                   nullptr, nullptr, &si.StartupInfo, &pi);
    DeleteProcThreadAttributeList(attrs);
    if (!ok) return finish(GuardianExit::BadArgs, "relay: CreateProcess failed");
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(GuardianExit::Relayed);
}

void sendMouseUp(kgn::MouseButton b) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    switch (b) {
        case kgn::MouseButton::Left: in.mi.dwFlags = MOUSEEVENTF_LEFTUP; break;
        case kgn::MouseButton::Right: in.mi.dwFlags = MOUSEEVENTF_RIGHTUP; break;
        case kgn::MouseButton::Middle: in.mi.dwFlags = MOUSEEVENTF_MIDDLEUP; break;
        default: return;
    }
    SendInput(1, &in, sizeof in);
}

int run(const GuardianArgs& a) {
    HANDLE section = reinterpret_cast<HANDLE>(a.section);
    HANDLE core = reinterpret_cast<HANDLE>(a.coreProcess);

    // One mapping of both pages. The slot page is then made read-only: the
    // guardian can never write a slot. Failing that, do not run at all.
    void* base = MapViewOfFile(section, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0,
                               kgn::win::kJournalSectionSize);
    if (!base) return finish(GuardianExit::BadArgs, "cannot map the journal section");
    auto* bytes = static_cast<unsigned char*>(base);
    DWORD oldProtect = 0;
    if (!VirtualProtect(bytes + kgn::win::kJournalPageSize, kgn::win::kJournalPageSize,
                        PAGE_READONLY, &oldProtect))
        return finish(GuardianExit::BadArgs, "cannot make the slot page read-only; refusing to run");

    auto& header = *reinterpret_cast<JournalHeader*>(bytes);
    const auto& slots = *reinterpret_cast<const JournalSlots*>(bytes + kgn::win::kJournalPageSize);

    FILETIME created{}, exited{}, kernel{}, user{};
    const DWORD pid = GetProcessId(core);
    if (pid == 0 || !GetProcessTimes(core, &created, &exited, &kernel, &user))
        return finish(GuardianExit::IdentityMismatch, "cannot identify the core process");
    if (!validateHeader(header, a.nonce, pid, created))
        return finish(GuardianExit::IdentityMismatch, "journal header does not match the core process");

    header.guardian_pid.store(GetCurrentProcessId(), std::memory_order_release);
    header.flags.fetch_or(kgn::win::kFlagAttached, std::memory_order_acq_rel);

    if (WaitForSingleObject(core, INFINITE) != WAIT_OBJECT_0)
        return finish(GuardianExit::BadArgs, "waiting on the core process failed");

    if (cleanExitMarked(header)) return static_cast<int>(GuardianExit::Clean);

    RecoveryPorts ports;
    ports.windowsReportsDown = [](kgn::MouseButton b) {
        const int vk = physicalVk(b, GetSystemMetrics(SM_SWAPBUTTON) != 0);
        return (GetAsyncKeyState(vk) & 0x8000) != 0;
    };
    ports.sendUp = sendMouseUp;
    ports.now = [] { return std::chrono::steady_clock::now(); };
    ports.sleep = [](std::chrono::milliseconds d) { Sleep(static_cast<DWORD>(d.count())); };

    const GuardianExit result = recover(slots, ports);
    if (result == GuardianExit::ReleaseUnconfirmed)
        return finish(result, "a journaled button was still reported down after the release window");
    return static_cast<int>(result);
}

}  // namespace

int main(int argc, char** argv) {
    const auto args = parseArgs(argc, argv);
    if (!args) return finish(GuardianExit::BadArgs, "bad command line");
    return args->relay ? relay(*args) : run(*args);
}

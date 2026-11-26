// Fault injection for the crash-release guardian: dies at a chosen point in a
// mouse button's journalled press/release, so the guardian can be watched
// doing (or not doing) the right thing.
//
// Deliberately NOT registered with ctest: it injects a real mouse press and
// then kills itself. Run it by hand:
//
//     PATH=/c/msys64/ucrt64/bin:$PATH ./build/default/core/kgn_guardian_fault.exe --button left --die held
//
// It never calls markCleanExit, so every death looks like a crash. It injects
// only mouse button events for the chosen button, over its own window.

#if defined(_WIN32)

#include "../src/platform/windows/guardian_supervisor.hpp"
#include "../src/platform/windows/obligation_journal.hpp"
#include "../src/platform/windows/sendinput_output.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace kgn;
using kgn::win::GuardianSupervisor;
using kgn::win::ObligationJournal;
using kgn::win::SendInputOutput;

namespace {

enum class DieAt { BeforeDown, AfterDown, Held, BeforeUp, AfterUp };

struct Args {
    MouseButton button = MouseButton::Left;
    DieAt die = DieAt::Held;
    std::string dieName;
};

void usage(std::FILE* out) {
    std::fprintf(out,
        "usage: kgn_guardian_fault --button left|right|middle "
        "--die before-down|after-down|held|before-up|after-up\n"
        "\n"
        "Presses a mouse button over its own window and kills itself "
        "(TerminateProcess, never a clean exit) at the chosen point:\n"
        "  before-down  after DOWN_INTENT, before SendInput(down)\n"
        "  after-down   after SendInput(down), before DOWN is recorded\n"
        "  held         with the slot at DOWN\n"
        "  before-up    after UP_INTENT, before SendInput(up)\n"
        "  after-up     after SendInput(up), before UP is recorded\n");
}

// 0 ok, 1 bad arguments, 2 help requested.
int parse(int argc, char** argv, Args& args) {
    bool haveButton = false;
    bool haveDie = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--help" || a == "-h") return 2;
        if ((a == "--button" || a == "--die") && i + 1 < argc) {
            const std::string v = argv[++i];
            if (a == "--button") {
                if (v == "left") args.button = MouseButton::Left;
                else if (v == "right") args.button = MouseButton::Right;
                else if (v == "middle") args.button = MouseButton::Middle;
                else return 1;
                haveButton = true;
            } else {
                if (v == "before-down") args.die = DieAt::BeforeDown;
                else if (v == "after-down") args.die = DieAt::AfterDown;
                else if (v == "held") args.die = DieAt::Held;
                else if (v == "before-up") args.die = DieAt::BeforeUp;
                else if (v == "after-up") args.die = DieAt::AfterUp;
                else return 1;
                args.dieName = v;
                haveDie = true;
            }
        } else {
            return 1;
        }
    }
    return (haveButton && haveDie) ? 0 : 1;
}

[[noreturn]] void die() {
    ::TerminateProcess(::GetCurrentProcess(), 9);
    for (;;) ::Sleep(1000);  // unreachable
}

constexpr DWORD kDownMask =
    MOUSEEVENTF_LEFTDOWN | MOUSEEVENTF_RIGHTDOWN | MOUSEEVENTF_MIDDLEDOWN;
constexpr DWORD kUpMask =
    MOUSEEVENTF_LEFTUP | MOUSEEVENTF_RIGHTUP | MOUSEEVENTF_MIDDLEUP;

class FaultOutput final : public SendInputOutput {
public:
    DieAt dieAt = DieAt::Held;

protected:
    bool dispatch(const INPUT& input) override {
        // Classify by the button flags only: the cursor placement also comes
        // through here (MOVE|ABSOLUTE) and must pass untouched.
        const bool down = (input.mi.dwFlags & kDownMask) != 0;
        const bool up = (input.mi.dwFlags & kUpMask) != 0;
        if (down && dieAt == DieAt::BeforeDown) die();
        if (up && dieAt == DieAt::BeforeUp) die();
        const bool ok = SendInputOutput::dispatch(input);
        if (down && dieAt == DieAt::AfterDown) die();
        if (up && dieAt == DieAt::AfterUp) die();
        return ok;
    }
};

LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    return ::DefWindowProcW(h, m, w, l);
}

void pump(std::chrono::milliseconds span) {
    const auto end = std::chrono::steady_clock::now() + span;
    MSG msg;
    while (std::chrono::steady_clock::now() < end) {
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
        ::Sleep(10);
    }
}

}  // namespace

int main(int argc, char** argv) {
    Args args;
    const int parsed = parse(argc, argv, args);
    if (parsed == 2) {
        usage(stdout);
        return 0;
    }
    if (parsed != 0) {
        usage(stderr);
        return 1;
    }

    // Declared in this order so the supervisor is destroyed before the journal.
    auto journal = ObligationJournal::create();
    if (!journal) {
        std::fprintf(stderr, "FAIL: could not create the journal.\n");
        return 2;
    }
    GuardianSupervisor supervisor(*journal, win::guardianPathBesideThisExecutable());
    const ProtectionStatus status = supervisor.start();
    if (status.state != Protection::Protected) {
        std::fprintf(stderr, "not protected: state=%d reason=%s\nNothing was pressed.\n",
                     static_cast<int>(status.state), status.reason.c_str());
        return 3;
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = L"KgnGuardianFaultWindow";
    wc.hCursor = ::LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    ::RegisterClassW(&wc);
    const int w = 320, h = 200;
    const int x = (::GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    const int y = (::GetSystemMetrics(SM_CYSCREEN) - h) / 2;
    HWND window = ::CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, wc.lpszClassName,
                                    L"kgn_guardian_fault", WS_POPUP | WS_VISIBLE | WS_BORDER,
                                    x, y, w, h, nullptr, nullptr, wc.hInstance, nullptr);
    if (!window) {
        std::fprintf(stderr, "FAIL: could not create the window.\n");
        return 2;
    }

    FaultOutput output;
    output.setJournal(journal.get());
    output.dieAt = args.die;

    pump(std::chrono::milliseconds(200));
    output.moveCursorTo(x + w / 2, y + h / 2);
    pump(std::chrono::milliseconds(300));

    const Point at = output.cursorPosition();
    if (std::abs(at.x - (x + w / 2)) > 2 || std::abs(at.y - (y + h / 2)) > 2) {
        std::fprintf(stderr, "FAIL: cursor is at (%d,%d), not the window centre. "
                             "Nothing was pressed.\n", at.x, at.y);
        return 5;
    }

    std::fprintf(stderr, "protected; will die at %s\n", args.dieName.c_str());
    std::fflush(stderr);

    output.button(args.button, true);
    if (args.die == DieAt::Held) die();
    output.button(args.button, false);

    // Every --die point has killed us by now; reaching here is a bug.
    std::fprintf(stderr, "FAIL: survived the chosen death point.\n");
    return 4;
}

#else
int main() { return 0; }
#endif

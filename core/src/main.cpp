// keygnosys-core: the native core's entry point.
//
// Parses a small command line, installs a console-control / signal handler that
// asks the core to stop and -- where the operating system will terminate the
// process as soon as the handler returns -- waits, bounded, for the core's own
// unwind to finish first. Everything else is kgn::Core.
//
// What the handler covers, and what it does not:
//
//   Ctrl+C, Ctrl+Break   ask and return; Windows imposes no deadline, and main()
//                        runs the unwind to completion.
//   console close        (window closed, Windows Terminal tab closed, Task
//                        Manager "End task" on a console process) ask, then wait
//                        up to closeWaitBound(SPI_GETHUNGAPPTIMEOUT) for main()
//                        to mark the unwind complete. Without the wait Windows
//                        ends the process before Core::stop() releases anything
//                        (M3 finding O-3).
//   logoff, shutdown     same wait, for correctness if these are ever delivered.
//                        They currently are NOT: a process that loads user32 is
//                        not sent them by the console subsystem, and this one
//                        does. That gap is open and needs a hidden window
//                        handling WM_QUERYENDSESSION / WM_ENDSESSION.
//   TerminateProcess,    not covered and not coverable in-process: no code runs.
//   crash, taskkill /F   (M3 row 10.2.)
//
// The release work never moves: it stays in Core::stop() on the core thread,
// which owns the dispatcher and the output backend. The handler only sets the
// existing stop flag and waits.
//
// What this executable does NOT do at M2 is intercept a key or move a pointer.
// There are no backends yet; the core says so in `hello`, in a diagnostic, and
// on stderr at startup, rather than running as something that looks like it is
// working (P6).

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "kgn/core.hpp"
#include "kgn/platform.hpp"
#include "kgn/shutdown_coordinator.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <csignal>
#endif

namespace {

#if defined(_WIN32)
// Created once and deliberately never destroyed: the console handler runs on a
// thread the system creates and may still be inside a wait while main() returns
// and static destructors run. A coordinator torn down under it would be a
// use-after-free at exactly the moment P7 matters.
kgn::ShutdownCoordinator& shutdownCoordinator() {
    static auto* coordinator = new kgn::ShutdownCoordinator();
    return *coordinator;
}

// Best-effort, and deliberately not through the C runtime: console functions
// "may not work reliably" during close, logoff and shutdown (SetConsoleCtrlHandler
// documentation). Written to stderr -- which the launcher redirects to the log
// under --background -- and to the debugger.
void reportCloseTimeout(DWORD type, long boundMs) {
    char line[256];
    const int n = std::snprintf(line, sizeof(line),
                                "keygnosys-core: console control %lu: the unwind did not "
                                "finish within %ld ms; Windows will now end the process "
                                "and synthesized input may remain held\n",
                                static_cast<unsigned long>(type), boundMs);
    if (n <= 0) return;
    ::OutputDebugStringA(line);
    const HANDLE err = ::GetStdHandle(STD_ERROR_HANDLE);
    if (err != nullptr && err != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        ::WriteFile(err, line, static_cast<DWORD>(n), &written, nullptr);
    }
}

BOOL WINAPI onConsoleEvent(DWORD type) {
    switch (type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
            // No deadline: main() finishes the unwind after this returns.
            shutdownCoordinator().requestStop();
            return TRUE;
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT: {
            // Windows terminates the process as soon as this returns, so the
            // unwind must be finished first. Bounded below the system's own
            // timeout, so this handler -- not the system -- decides when to give up.
            UINT hung = 0;
            if (!::SystemParametersInfoA(SPI_GETHUNGAPPTIMEOUT, 0, &hung, 0)) hung = 0;
            const auto bound = kgn::closeWaitBound(static_cast<long>(hung));
            if (shutdownCoordinator().requestStopAndWait(bound) ==
                kgn::ShutdownCoordinator::WaitResult::TimedOut) {
                reportCloseTimeout(type, static_cast<long>(bound.count()));
            }
            return TRUE;
        }
        default:
            return FALSE;
    }
}
#else
kgn::Core* g_core = nullptr;

extern "C" void onSignal(int) {
    // Only a flag is set here. Anything else in a signal handler is not
    // async-signal-safe, and the unwind that P7 requires has to happen on the
    // loop's own thread where it can take its time.
    if (g_core != nullptr) g_core->requestStop();
}
#endif

void printUsage() {
    std::printf(
        "keygnosys-core -- the KeyGnosys native input core\n"
        "\n"
        "Usage: keygnosys-core [options]\n"
        "\n"
        "  --bindings <id>         Bindings document id (default: default)\n"
        "  --bindings-file <path>  Load this bindings document, ignoring the id\n"
        "  --config-dir <path>     User configuration root\n"
        "  --data-dir <path>       Bundled data root\n"
        "  --version               Print the version and exit\n"
        "  --help                  Print this and exit\n"
        "\n"
        "Milestone M2: the engine, motion, action dispatch and the IPC server\n"
        "are present. There are no platform backends yet, so this build does\n"
        "not intercept keys or drive the pointer.\n");
}

bool takeValue(int argc, char** argv, int& i, const char* name, std::string& out) {
    if (i + 1 >= argc) {
        std::fprintf(stderr, "keygnosys-core: %s needs a value\n", name);
        return false;
    }
    out = argv[++i];
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    kgn::CoreOptions options;

    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--help" || argument == "-h") {
            printUsage();
            return 0;
        }
        if (argument == "--version") {
            std::printf("keygnosys-core 0.1.0\n");
            return 0;
        }
        // There is deliberately no option to move the endpoint. SPEC 5.1.1
        // gives one rule that the core, the overlay and the launcher all
        // derive from; a core listening elsewhere is a core its clients cannot
        // find.
        if (argument == "--bindings") {
            if (!takeValue(argc, argv, i, "--bindings", options.bindingsId)) return 2;
        } else if (argument == "--bindings-file") {
            if (!takeValue(argc, argv, i, "--bindings-file", options.bindingsFile)) {
                return 2;
            }
        } else if (argument == "--config-dir") {
            if (!takeValue(argc, argv, i, "--config-dir", options.configDir)) return 2;
        } else if (argument == "--data-dir") {
            if (!takeValue(argc, argv, i, "--data-dir", options.dataDir)) return 2;
        } else {
            std::fprintf(stderr, "keygnosys-core: unknown option '%s'\n",
                         argument.c_str());
            printUsage();
            return 2;
        }
    }

    // The composition root: the one place that knows what platform this build
    // targets (SPEC section 6.2). Everything below it is handed its backends.
    kgn::Core core(std::move(options), kgn::createBackends());

#if defined(_WIN32)
    // Declared AFTER core, so it is destroyed BEFORE it on every path out of
    // main() -- the start() failure returns below, a normal return, or an
    // exception that unwinds the stack. Its destructor marks the unwind
    // complete, and does not return while a handler is still inside
    // core.requestStop(), so no handler can reach a destroyed Core.
    auto shutdownAttachment = shutdownCoordinator().attach([&core] { core.requestStop(); });
    ::SetConsoleCtrlHandler(onConsoleEvent, TRUE);
#else
    g_core = &core;
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    // A client that vanishes mid-write must surface as an error on the write,
    // not as a signal that ends the process holding the keyboard.
    std::signal(SIGPIPE, SIG_IGN);
#endif

    const kgn::OwnResult owned = core.start();
    if (!owned.ok()) {
        // The diagnostic code is printed alongside the message: it is the part
        // a script can act on, and the message is the part a person reads.
        std::fprintf(stderr, "keygnosys-core: %s: %s\n", owned.code.c_str(),
                     owned.message.c_str());
        return owned.status == kgn::OwnStatus::InUse ? 3 : 4;
    }

    std::fprintf(stderr, "keygnosys-core 0.1.0 listening\n");
    for (const auto& limitation : core.hello().limitations) {
        std::fprintf(stderr, "  limitation: %s\n", limitation.c_str());
    }
    for (const auto& diagnostic : core.diagnostics()) {
        std::fprintf(stderr, "  %s %s: %s\n", kgn::diagLevelName(diagnostic.level),
                     diagnostic.code.c_str(), diagnostic.message.c_str());
    }

    core.run();   // returns only after Core::stop() has released every obligation
#if defined(_WIN32)
    // Explicit here because this is the moment that matters: a close handler
    // waiting in requestStopAndWait() may now return control to Windows.
    shutdownAttachment.markUnwound();
#else
    g_core = nullptr;
#endif
    return 0;
}

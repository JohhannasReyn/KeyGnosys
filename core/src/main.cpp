// keygnosys-core: the native core's entry point.
//
// Parses a small command line, installs a signal handler so that every way of
// stopping the process runs the same unwind, and hands everything else to
// kgn::Core.
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

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <csignal>
#endif

namespace {

kgn::Core* g_core = nullptr;

#if defined(_WIN32)
BOOL WINAPI onConsoleEvent(DWORD type) {
    switch (type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            if (g_core != nullptr) g_core->requestStop();
            return TRUE;
        default:
            return FALSE;
    }
}
#else
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
    g_core = &core;

#if defined(_WIN32)
    ::SetConsoleCtrlHandler(onConsoleEvent, TRUE);
#else
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

    core.run();
    g_core = nullptr;
    return 0;
}

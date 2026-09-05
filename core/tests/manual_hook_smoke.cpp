// A twelve-second answer to "is the hook actually receiving anything?"
//
// This is deliberately NOT registered with ctest. It needs a real desktop and
// a human pressing a key, so making automated CI depend on it would make CI a
// liar. Run it by hand:
//
//     PATH=/c/msys64/ucrt64/bin:$PATH ./build/default/core/kgn_hook_smoke.exe
//
// Why it exists. During live M3 validation the Windows hook installed
// successfully, reported itself healthy, kept a valid HHOOK, never lost the
// hook -- and received zero callbacks, because the thread that owned it was
// parked in a wait rather than a message-retrieval call. The entire automated
// suite was green while the product intercepted nothing. Nothing in it
// depended on the hook thread actually retrieving messages.
//
// So this is the boundary test: it drives the real HookInput, through the real
// rings, and reports PASS only after a genuine keystroke has travelled the
// whole path into the publication ring. Run it before the manual matrix, so a
// dead hook is found in twelve seconds rather than forty rows in.

#if defined(_WIN32)

#include "../src/platform/windows/hook_input.hpp"

#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>

using namespace kgn;
using kgn::win::HookInput;

int main() {
    WorkRing work;
    PublicationRing publication;
    StatePublisher published;
    EngineConfig config;
    HookInput hook;

    std::unique_ptr<EngineOwner> owner =
        hook.engineOwner(work, publication, published, config);

    if (!hook.start(nullptr)) {
        std::fprintf(stderr,
                     "FAIL: the hook did not install. This is an installation "
                     "problem, not a pump problem.\n");
        return 2;
    }

    std::fprintf(stderr,
                 "hook installed. Press any key within 12 seconds...\n"
                 "(nothing is suppressed: no bindings are loaded, so the layer "
                 "is inert and every key passes through natively)\n");

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
    PhysicalRecord record{};
    std::size_t seen = 0;

    while (std::chrono::steady_clock::now() < deadline) {
        while (publication.pop(record)) {
            if (seen == 0) {
                std::fprintf(stderr, "  first record: code=%u state=%d suppressed=%d\n",
                             static_cast<unsigned>(record.code),
                             static_cast<int>(record.state),
                             record.suppressed ? 1 : 0);
            }
            ++seen;
        }
        if (seen > 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    hook.stop();

    if (seen == 0) {
        std::fprintf(stderr,
                     "FAIL: no callback reached the publication ring in 12 s.\n"
                     "The hook was installed and reported healthy, which is "
                     "exactly how the 2026-09-01 defect presented. Suspect the "
                     "message pump before suspecting the engine.\n");
        return 1;
    }

    std::fprintf(stderr, "PASS: %zu physical record(s) observed.\n", seen);
    return 0;
}

#else

#include <cstdio>
int main() {
    std::fprintf(stderr, "kgn_hook_smoke is Windows-only; nothing to do.\n");
    return 0;
}

#endif

// The core: the thing that assembles everything else and keeps it running.
//
// It owns the layer engine, the dispatcher and its integrators, the bindings
// it loaded, the endpoint it holds and the IPC server that talks over it. What
// it does NOT own is a way to see or synthesise input: that is the backends'
// job, and at M2 there are none. The core says so rather than pretending
// otherwise -- `hello` reports every backend as absent, and a diagnostic says
// what that costs (P6).
//
// One thread. The engine, the dispatcher and the sessions are all touched from
// the same loop, which is why none of them needs a lock. The loop ticks at the
// motion cadence and polls IPC on the same beat; no keystroke ever waits on a
// client, because writes are non-blocking and every queue is bounded.
//
// P7 is the invariant that outranks everything here: every exit path -- clean
// shutdown, signal, endpoint loss, config reload -- unwinds the engine and the
// dispatcher before it does anything else.

#pragma once

#include <memory>
#include <string>

#include "kgn/actions.hpp"
#include "kgn/backends.hpp"
#include "kgn/clock.hpp"
#include "kgn/config.hpp"
#include "kgn/diagnostics.hpp"
#include "kgn/endpoint.hpp"
#include "kgn/ipc.hpp"
#include "kgn/layer_engine.hpp"

namespace kgn {

struct CoreOptions {
    // Empty means the address resolveEndpoint() derives (SPEC 5.1.1). Set
    // explicitly by tests, which must not contend for the real one.
    std::string endpoint;
    std::string bindingsId = "default";
    // An explicit path wins over the id search entirely.
    std::string bindingsFile;
    // Empty means the SPEC 3.1 user configuration root for this platform.
    std::string configDir;
    // Empty means a search from the executable's own location.
    std::string dataDir;
};

// Where the core looked for a bindings document, in order, so a failure to
// find one can say where it looked rather than only that it failed.
std::vector<std::string> bindingsSearchPaths(const CoreOptions& options);

// The SPEC 3.1 user configuration root.
[[nodiscard]] std::string userConfigRoot();

class Core {
public:
    explicit Core(CoreOptions options);
    ~Core();

    Core(const Core&) = delete;
    Core& operator=(const Core&) = delete;

    // Load configuration, take the endpoint, and start listening. On success
    // the endpoint is answering before this returns, so a client that can
    // connect will be greeted -- the readiness definition the launcher
    // contract depends on (LAUNCHING.md section 4.2).
    OwnResult start();

    // One iteration of the loop. Exposed so a test can drive the core on its
    // own timeline instead of in real time.
    void step(TimePoint now);

    // Loop at the motion cadence until stopped.
    void run();

    // Ask the loop to finish. Safe from a signal handler: it sets a flag and
    // nothing else.
    void requestStop();

    // Unwind everything and close. Idempotent, and called on every exit path.
    void stop(const std::string& reason);

    [[nodiscard]] Server* server();
    [[nodiscard]] const Diagnostics& diagnostics() const;
    [[nodiscard]] const HelloInfo& hello() const;
    [[nodiscard]] bool running() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace kgn

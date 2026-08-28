// Owning the IPC endpoint.
//
// This is the only place that knows what a socket is, and it is where SPEC
// sections 5.1.1 to 5.1.3 are implemented. Those rules are not conveniences;
// the endpoint carries every keystroke the user types, and anything able to
// substitute its own socket at that path receives them.
//
// Three obligations, in order:
//
//   5.1.1  One resolution rule, derived identically by core, overlay and
//          launcher. Nothing here composes a path of its own.
//
//   5.1.2  Every directory component the core owns is verified -- a real
//          directory, not a symlink, owned by this uid, closed to group and
//          other -- and verification is anchored to DESCRIPTORS. A path is
//          checked once and then never re-resolved, because re-resolving it is
//          exactly the race the checks exist to close. Nothing unsafe is ever
//          repaired.
//
//   5.1.3  Claiming is serialized by a lock held for the lifetime of the
//          process. The lock owns the single-instance invariant; the socket is
//          only the communication endpoint. Probing is not an ownership
//          primitive -- connect() returns ECONNREFUSED for a healthy core
//          between bind() and listen() -- so the probe happens only with the
//          lock already held.
//
// On Windows none of the lock machinery exists, because CreateNamedPipe with
// FILE_FLAG_FIRST_PIPE_INSTANCE is a single kernel test-and-own: there is
// nothing observable between deciding and claiming.

#pragma once

#include <memory>
#include <string>

#include "kgn/ipc.hpp"

namespace kgn {

// The endpoint address, per SPEC section 5.1.1. This is the ONE rule; the
// overlay's `keygnosys.paths.ipc_endpoint()` implements the same one.
[[nodiscard]] std::string resolveEndpoint();

// Linux only: the directory the endpoint and its lock live in, i.e. the
// endpoint's parent. Empty on Windows, where a pipe has no directory.
[[nodiscard]] std::string resolveRuntimeDirectory();

enum class OwnStatus {
    Ok,
    // A path component failed a type, ownership or permission check. The core
    // refuses to bind and does not attempt to repair it.
    Unsafe,
    // Another core owns or is claiming this endpoint, or it could not be
    // proven stale.
    InUse,
    // Something else went wrong -- out of descriptors, a full filesystem.
    Failed,
};

struct OwnResult {
    OwnStatus status = OwnStatus::Failed;
    // The SPEC section 11 diagnostic code to emit, or empty on success.
    std::string code;
    std::string message;

    [[nodiscard]] bool ok() const { return status == OwnStatus::Ok; }
};

// Exclusive ownership of one resolved endpoint, held for as long as this
// object lives.
class EndpointOwner {
public:
    EndpointOwner();
    ~EndpointOwner();

    EndpointOwner(const EndpointOwner&) = delete;
    EndpointOwner& operator=(const EndpointOwner&) = delete;

    // Verify, lock, probe, bind and listen. On success the endpoint is
    // listening before this returns, so a client that can connect can also be
    // greeted -- which is the readiness definition the launcher contract
    // depends on (LAUNCHING.md section 4.2).
    OwnResult acquire(const std::string& address);

    // The listening transport. Null until acquire() has succeeded.
    [[nodiscard]] Transport* transport() { return transport_.get(); }
    std::unique_ptr<Transport> takeTransport() { return std::move(transport_); }

    // Stop listening and remove the endpoint. The LOCK FILE is never removed,
    // on any path: deleting a lock file is itself a race, because two
    // processes can then hold exclusive locks on two different inodes that
    // were briefly reachable by one name.
    void release();

    [[nodiscard]] const std::string& address() const { return address_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::unique_ptr<Transport> transport_;
    std::string address_;
};

}  // namespace kgn

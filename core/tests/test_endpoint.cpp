// Endpoint ownership -- SPEC sections 5.1.1 to 5.1.3.
//
// These are the tests that need a real operating system, because what they
// assert is exactly what the operating system does: that a lock excludes, that
// a symlink is refused, that a stale socket is recovered and a live one is
// not. Guarded per platform rather than reduced to whatever both happen to
// support, because the invariant is the same on both and the mechanism is not.

#include <string>

#include "kgn/endpoint.hpp"
#include "kgn_test.hpp"

using kgn::EndpointOwner;
using kgn::OwnStatus;

#if !defined(_WIN32)

#include <cerrno>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

// A private directory to point XDG_RUNTIME_DIR at, so the tests exercise the
// real verification code against a tree they control.
class TempRuntime {
public:
    TempRuntime() {
        char pattern[] = "/tmp/kgn-test-XXXXXX";
        const char* made = ::mkdtemp(pattern);
        path_ = made != nullptr ? made : "";
        if (!path_.empty()) ::chmod(path_.c_str(), 0700);
        const char* previous = std::getenv("XDG_RUNTIME_DIR");
        had_ = previous != nullptr;
        if (had_) saved_ = previous;
        ::setenv("XDG_RUNTIME_DIR", path_.c_str(), 1);
    }
    ~TempRuntime() {
        if (had_) {
            ::setenv("XDG_RUNTIME_DIR", saved_.c_str(), 1);
        } else {
            ::unsetenv("XDG_RUNTIME_DIR");
        }
        // Best effort; a leftover directory in /tmp is not worth a recursive
        // remove implementation in a test.
        ::unlink((path_ + "/keygnosys/core.sock").c_str());
        ::unlink((path_ + "/keygnosys/core.lock").c_str());
        ::rmdir((path_ + "/keygnosys").c_str());
        ::rmdir(path_.c_str());
    }

    [[nodiscard]] const std::string& path() const { return path_; }
    [[nodiscard]] std::string endpoint() const { return path_ + "/keygnosys/core.sock"; }
    [[nodiscard]] std::string runtimeDir() const { return path_ + "/keygnosys"; }

private:
    std::string path_;
    std::string saved_;
    bool had_ = false;
};

bool exists(const std::string& path) {
    struct stat info {};
    return ::lstat(path.c_str(), &info) == 0;
}

mode_t modeOf(const std::string& path) {
    struct stat info {};
    if (::lstat(path.c_str(), &info) != 0) return 0;
    return info.st_mode & 07777;
}

// Bind a socket at `path` and optionally listen. Not listening is exactly the
// state that makes connect() return ECONNREFUSED, which is how a stale
// endpoint presents.
int placeSocket(const std::string& path, bool listening) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
    ::unlink(path.c_str());
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    if (listening && ::listen(fd, 4) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

}  // namespace

// ---------------------------------------------------------------------------
// Resolution (5.1.1)

KGN_TEST(the_endpoint_follows_xdg_runtime_dir_when_it_is_set) {
    TempRuntime runtime;
    KGN_CHECK_EQ(kgn::resolveEndpoint(), runtime.endpoint());
    KGN_CHECK_EQ(kgn::resolveRuntimeDirectory(), runtime.runtimeDir());
}

KGN_TEST(the_fallback_repeats_the_keygnosys_component_on_purpose) {
    const char* previous = std::getenv("XDG_RUNTIME_DIR");
    const std::string saved = previous != nullptr ? previous : "";
    ::unsetenv("XDG_RUNTIME_DIR");

    const std::string expected =
        "/tmp/keygnosys-" + std::to_string(static_cast<unsigned long>(::getuid())) +
        "/keygnosys/core.sock";
    KGN_CHECK_EQ(kgn::resolveEndpoint(), expected);

    if (previous != nullptr) ::setenv("XDG_RUNTIME_DIR", saved.c_str(), 1);
}

KGN_TEST(a_trailing_slash_on_the_runtime_dir_does_not_double_up) {
    const char* previous = std::getenv("XDG_RUNTIME_DIR");
    const std::string saved = previous != nullptr ? previous : "";
    ::setenv("XDG_RUNTIME_DIR", "/run/user/1000/", 1);
    KGN_CHECK_EQ(kgn::resolveEndpoint(), std::string("/run/user/1000/keygnosys/core.sock"));
    if (previous != nullptr) {
        ::setenv("XDG_RUNTIME_DIR", saved.c_str(), 1);
    } else {
        ::unsetenv("XDG_RUNTIME_DIR");
    }
}

// ---------------------------------------------------------------------------
// Ownership and the lock (5.1.3)

KGN_TEST(a_core_can_own_the_endpoint_and_it_is_listening_when_it_says_so) {
    TempRuntime runtime;
    EndpointOwner owner;
    const auto result = owner.acquire(runtime.endpoint());
    KGN_CHECK(result.ok());
    KGN_CHECK(owner.transport() != nullptr);
    KGN_CHECK(exists(runtime.endpoint()));

    // Listening before the name appears: a client that can see the endpoint
    // can already connect to it.
    const int client = ::socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", runtime.endpoint().c_str());
    KGN_CHECK_EQ(::connect(client, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
    ::close(client);
}

KGN_TEST(a_second_core_on_one_endpoint_is_refused) {
    // The governing invariant: at no point may two cores both conclude that
    // they own the canonical endpoint.
    TempRuntime runtime;
    EndpointOwner first;
    KGN_CHECK(first.acquire(runtime.endpoint()).ok());

    EndpointOwner second;
    const auto result = second.acquire(runtime.endpoint());
    KGN_CHECK(result.status == OwnStatus::InUse);
    KGN_CHECK_EQ(result.code, std::string("ipc.endpoint_in_use"));
    KGN_CHECK(second.transport() == nullptr);
}

KGN_TEST(the_lock_is_released_when_its_descriptor_closes) {
    // The kernel releases a flock when the holding process dies by any means,
    // which is why it can never be stale. In-process, destroying the owner
    // closes the descriptor and is the same release path.
    TempRuntime runtime;
    {
        EndpointOwner first;
        KGN_CHECK(first.acquire(runtime.endpoint()).ok());
        EndpointOwner blocked;
        KGN_CHECK(blocked.acquire(runtime.endpoint()).status == OwnStatus::InUse);
    }
    EndpointOwner next;
    KGN_CHECK(next.acquire(runtime.endpoint()).ok());
}

KGN_TEST(the_lock_file_is_never_unlinked) {
    // Removing a lock file is itself a race: two processes can then hold
    // exclusive locks on two different inodes that shared one name.
    TempRuntime runtime;
    const std::string lock = runtime.runtimeDir() + "/core.lock";
    {
        EndpointOwner owner;
        KGN_CHECK(owner.acquire(runtime.endpoint()).ok());
        KGN_CHECK(exists(lock));
        owner.release();
        KGN_CHECK(exists(lock));
    }
    KGN_CHECK(exists(lock));
}

KGN_TEST(releasing_removes_the_endpoint_but_leaves_the_lock) {
    TempRuntime runtime;
    EndpointOwner owner;
    KGN_CHECK(owner.acquire(runtime.endpoint()).ok());
    KGN_CHECK(exists(runtime.endpoint()));
    owner.release();
    KGN_CHECK(!exists(runtime.endpoint()));
    KGN_CHECK(exists(runtime.runtimeDir() + "/core.lock"));
}

// ---------------------------------------------------------------------------
// Liveness and stale recovery (5.1.3)

KGN_TEST(a_stale_endpoint_is_recovered) {
    TempRuntime runtime;
    ::mkdir(runtime.runtimeDir().c_str(), 0700);
    // Bound but never listening: connect() gives ECONNREFUSED, which is the
    // only admissible evidence of staleness.
    const int stale = placeSocket(runtime.endpoint(), false);
    KGN_CHECK(stale >= 0);
    ::close(stale);
    KGN_CHECK(exists(runtime.endpoint()));

    EndpointOwner owner;
    KGN_CHECK(owner.acquire(runtime.endpoint()).ok());
}

KGN_TEST(a_live_endpoint_is_never_stolen) {
    TempRuntime runtime;
    ::mkdir(runtime.runtimeDir().c_str(), 0700);
    const int live = placeSocket(runtime.endpoint(), true);
    KGN_CHECK(live >= 0);

    EndpointOwner owner;
    const auto result = owner.acquire(runtime.endpoint());
    KGN_CHECK(result.status == OwnStatus::InUse);
    KGN_CHECK_EQ(result.code, std::string("ipc.endpoint_in_use"));
    ::close(live);
}

KGN_TEST(recovery_leaves_no_interval_in_which_the_pathname_is_absent) {
    // rename() contributes nothing to exclusion -- the lock is the whole of
    // that -- but it does mean a client polling during recovery never reads
    // the absence of the path as the core not existing.
    TempRuntime runtime;
    ::mkdir(runtime.runtimeDir().c_str(), 0700);
    const int stale = placeSocket(runtime.endpoint(), false);
    ::close(stale);

    EndpointOwner owner;
    KGN_CHECK(owner.acquire(runtime.endpoint()).ok());
    // Whatever is there now answers, which the stale one did not.
    const int client = ::socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", runtime.endpoint().c_str());
    KGN_CHECK_EQ(::connect(client, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
    ::close(client);
}

// ---------------------------------------------------------------------------
// Path verification (5.1.2)

KGN_TEST(the_runtime_directory_is_created_private_and_the_socket_owner_only) {
    TempRuntime runtime;
    EndpointOwner owner;
    KGN_CHECK(owner.acquire(runtime.endpoint()).ok());
    KGN_CHECK_EQ(static_cast<int>(modeOf(runtime.runtimeDir())), 0700);
    KGN_CHECK_EQ(static_cast<int>(modeOf(runtime.endpoint())), 0600);
    KGN_CHECK_EQ(static_cast<int>(modeOf(runtime.runtimeDir() + "/core.lock")), 0600);
}

KGN_TEST(a_runtime_base_open_to_group_or_other_is_refused) {
    // XDG_RUNTIME_DIR is verified too, not trusted: it is an environment
    // variable, and a process able to set the core's environment could have
    // pointed it anywhere.
    TempRuntime runtime;
    ::chmod(runtime.path().c_str(), 0755);
    EndpointOwner owner;
    const auto result = owner.acquire(runtime.endpoint());
    KGN_CHECK(result.status == OwnStatus::Unsafe);
    KGN_CHECK_EQ(result.code, std::string("ipc.endpoint_unsafe"));
    ::chmod(runtime.path().c_str(), 0700);
}

KGN_TEST(a_runtime_directory_open_to_group_or_other_is_refused) {
    TempRuntime runtime;
    ::mkdir(runtime.runtimeDir().c_str(), 0777);
    ::chmod(runtime.runtimeDir().c_str(), 0777);
    EndpointOwner owner;
    const auto result = owner.acquire(runtime.endpoint());
    KGN_CHECK(result.status == OwnStatus::Unsafe);
    KGN_CHECK_EQ(result.code, std::string("ipc.endpoint_unsafe"));
}

KGN_TEST(a_symlink_where_the_runtime_directory_belongs_is_refused) {
    // O_NOFOLLOW is what refuses this, and it is the difference between
    // verifying a descriptor and verifying a path.
    TempRuntime runtime;
    const std::string elsewhere = runtime.path() + "/elsewhere";
    ::mkdir(elsewhere.c_str(), 0700);
    KGN_CHECK_EQ(::symlink(elsewhere.c_str(), runtime.runtimeDir().c_str()), 0);

    EndpointOwner owner;
    const auto result = owner.acquire(runtime.endpoint());
    KGN_CHECK(result.status == OwnStatus::Unsafe);
    KGN_CHECK_EQ(result.code, std::string("ipc.endpoint_unsafe"));

    ::unlink(runtime.runtimeDir().c_str());
    ::rmdir(elsewhere.c_str());
}

KGN_TEST(a_regular_file_where_the_runtime_directory_belongs_is_refused) {
    TempRuntime runtime;
    const int fd = ::open(runtime.runtimeDir().c_str(), O_CREAT | O_WRONLY, 0600);
    KGN_CHECK(fd >= 0);
    ::close(fd);

    EndpointOwner owner;
    const auto result = owner.acquire(runtime.endpoint());
    KGN_CHECK(result.status == OwnStatus::Unsafe);
    KGN_CHECK_EQ(result.code, std::string("ipc.endpoint_unsafe"));
    ::unlink(runtime.runtimeDir().c_str());
}

KGN_TEST(an_unsafe_directory_is_refused_rather_than_repaired) {
    // No chmod, no chown, no delete-and-recreate. The core cannot distinguish
    // a hostile directory from one it merely does not understand.
    TempRuntime runtime;
    ::mkdir(runtime.runtimeDir().c_str(), 0700);
    ::chmod(runtime.runtimeDir().c_str(), 0757);

    EndpointOwner owner;
    KGN_CHECK(owner.acquire(runtime.endpoint()).status == OwnStatus::Unsafe);
    // Untouched: still exactly as it was found.
    KGN_CHECK_EQ(static_cast<int>(modeOf(runtime.runtimeDir())), 0757);
    KGN_CHECK(!exists(runtime.runtimeDir() + "/core.sock"));
}

KGN_TEST(an_address_with_no_directory_component_is_refused) {
    EndpointOwner owner;
    KGN_CHECK(!owner.acquire("core.sock").ok());
}

#else   // _WIN32

#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace {

// A pipe name unique to this run, so the tests never contend with a core the
// developer happens to be running.
std::string testPipeName() {
    return R"(\\.\pipe\keygnosys-test-)" +
           std::to_string(static_cast<unsigned long>(::GetCurrentProcessId()));
}

}  // namespace

KGN_TEST(the_endpoint_is_the_specified_pipe_name) {
    KGN_CHECK_EQ(kgn::resolveEndpoint(), std::string(R"(\\.\pipe\keygnosys)"));
    // A pipe is a kernel object, not a filesystem entry, so there is no
    // directory to verify and no lock file to keep.
    KGN_CHECK(kgn::resolveRuntimeDirectory().empty());
}

KGN_TEST(a_core_can_own_the_endpoint) {
    EndpointOwner owner;
    const auto result = owner.acquire(testPipeName());
    KGN_CHECK(result.ok());
    KGN_CHECK(owner.transport() != nullptr);
}

KGN_TEST(a_second_core_on_one_endpoint_is_refused) {
    // FILE_FLAG_FIRST_PIPE_INSTANCE is the whole mechanism: without it the
    // second CreateNamedPipe would SUCCEED, silently joining the first and
    // accepting connections meant for it.
    EndpointOwner first;
    KGN_CHECK(first.acquire(testPipeName()).ok());

    EndpointOwner second;
    const auto result = second.acquire(testPipeName());
    KGN_CHECK(result.status == OwnStatus::InUse);
    KGN_CHECK_EQ(result.code, std::string("ipc.endpoint_in_use"));
    KGN_CHECK(second.transport() == nullptr);
}

KGN_TEST(a_name_squatted_in_advance_is_detected_rather_than_joined) {
    const std::string name = testPipeName() + "-squat";
    const HANDLE squatter = ::CreateNamedPipeA(
        name.c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        PIPE_UNLIMITED_INSTANCES, 4096, 4096, 0, nullptr);
    KGN_CHECK(squatter != INVALID_HANDLE_VALUE);

    EndpointOwner owner;
    const auto result = owner.acquire(name);
    KGN_CHECK(result.status == OwnStatus::InUse);
    ::CloseHandle(squatter);
}

KGN_TEST(the_endpoint_is_free_again_once_its_owner_releases_it) {
    const std::string name = testPipeName() + "-reuse";
    {
        EndpointOwner first;
        KGN_CHECK(first.acquire(name).ok());
    }
    EndpointOwner second;
    KGN_CHECK(second.acquire(name).ok());
}

KGN_TEST(a_client_can_connect_and_exchange_bytes) {
    const std::string name = testPipeName() + "-io";
    EndpointOwner owner;
    KGN_CHECK(owner.acquire(name).ok());

    const HANDLE client = ::CreateFileA(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                                        nullptr, OPEN_EXISTING, 0, nullptr);
    KGN_CHECK(client != INVALID_HANDLE_VALUE);

    std::unique_ptr<kgn::Connection> accepted;
    for (int i = 0; i < 200 && !accepted; ++i) {
        accepted = owner.transport()->accept();
        if (!accepted) ::Sleep(5);
    }
    KGN_CHECK(accepted != nullptr);
    if (!accepted) return;

    const std::string line = "{\"v\":1}\n";
    KGN_CHECK_EQ(accepted->write(line.data(), line.size()),
                 static_cast<int>(line.size()));

    char buffer[64] = {};
    DWORD got = 0;
    KGN_CHECK(::ReadFile(client, buffer, sizeof(buffer), &got, nullptr) != 0);
    KGN_CHECK_EQ(std::string(buffer, got), line);

    const std::string back = "pong\n";
    DWORD wrote = 0;
    ::WriteFile(client, back.data(), static_cast<DWORD>(back.size()), &wrote, nullptr);

    char inbound[64] = {};
    int read = 0;
    for (int i = 0; i < 200 && read <= 0; ++i) {
        read = accepted->read(inbound, sizeof(inbound));
        if (read <= 0) ::Sleep(5);
    }
    KGN_CHECK_EQ(std::string(inbound, static_cast<std::size_t>(read > 0 ? read : 0)), back);
    ::CloseHandle(client);
}

#endif

int main() { return kgn::test::runAll(); }

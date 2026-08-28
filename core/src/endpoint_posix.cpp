// Endpoint ownership on Linux. See kgn/endpoint.hpp and SPEC 5.1.1 - 5.1.3.

#if !defined(_WIN32)

#include "kgn/endpoint.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

namespace kgn {
namespace {

constexpr const char* kSocketName = "core.sock";
constexpr const char* kLockName = "core.lock";

std::string describeErrno(int code) { return std::strerror(code); }

// A verified directory descriptor. Every later operation goes through it;
// nothing re-resolves the path it came from.
class DirFd {
public:
    DirFd() = default;
    explicit DirFd(int fd) : fd_(fd) {}
    ~DirFd() { reset(); }

    DirFd(DirFd&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
    DirFd& operator=(DirFd&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }
    DirFd(const DirFd&) = delete;
    DirFd& operator=(const DirFd&) = delete;

    [[nodiscard]] int get() const { return fd_; }
    [[nodiscard]] bool valid() const { return fd_ >= 0; }
    void reset() {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }

private:
    int fd_ = -1;
};

// "Fully verified", per SPEC 5.1.2: a real directory, not a symlink, owned by
// the uid the core runs as, and granting nothing to group or other.
//
// The fstat is on the DESCRIPTOR, never on the path. O_NOFOLLOW has already
// refused a symlink at the final component, and because every later operation
// is relative to this descriptor, a rename of the name it came from cannot
// redirect us afterwards.
bool verifyDirFd(int fd, const char* what, OwnResult& result) {
    struct stat info {};
    if (::fstat(fd, &info) != 0) {
        result = {OwnStatus::Failed, "ipc.endpoint_unsafe",
                  std::string("cannot stat ") + what + ": " + describeErrno(errno)};
        return false;
    }
    if (!S_ISDIR(info.st_mode)) {
        result = {OwnStatus::Unsafe, "ipc.endpoint_unsafe",
                  std::string(what) + " is not a directory"};
        return false;
    }
    if (info.st_uid != ::getuid()) {
        result = {OwnStatus::Unsafe, "ipc.endpoint_unsafe",
                  std::string(what) + " is owned by uid " +
                      std::to_string(info.st_uid) + ", not by this user"};
        return false;
    }
    if ((info.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        result = {OwnStatus::Unsafe, "ipc.endpoint_unsafe",
                  std::string(what) + " is accessible to group or other"};
        return false;
    }
    return true;
}

// Open (creating if asked) one component beneath `parent`, then verify it.
//
// O_NOFOLLOW is what refuses a symlink planted at this name; O_DIRECTORY is
// what refuses a regular file. A directory this core creates is created 0700
// and then verified anyway, because creating it and WINNING THE RACE to create
// it are not the same thing.
bool openVerifiedChild(int parent, const char* name, bool create, DirFd& out,
                       const std::string& what, OwnResult& result) {
    if (create && ::mkdirat(parent, name, 0700) != 0 && errno != EEXIST) {
        result = {OwnStatus::Failed, "ipc.endpoint_unsafe",
                  "cannot create " + what + ": " + describeErrno(errno)};
        return false;
    }
    const int fd = ::openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        // ELOOP and ENOTDIR both mean something that is not a directory of
        // ours is sitting at this name, which is an unsafe condition and not
        // an operational failure. Which of the two Linux reports depends on
        // the order it checks O_NOFOLLOW against O_DIRECTORY: a symlink
        // caught by O_DIRECTORY surfaces as ENOTDIR, not ELOOP.
        const int code = errno;
        const bool occupied = code == ELOOP || code == ENOTDIR;
        result = {occupied ? OwnStatus::Unsafe : OwnStatus::Failed,
                  "ipc.endpoint_unsafe",
                  "cannot open " + what + ": " +
                      (code == ELOOP ? std::string("it is a symbolic link")
                       : code == ENOTDIR
                           ? std::string("it is not a directory")
                           : describeErrno(code))};
        return false;
    }
    out = DirFd(fd);
    return verifyDirFd(out.get(), what.c_str(), result);
}

// The fallback root, `/tmp`. It is world-writable by design and MUST NOT be
// required to be private -- but its sticky bit is load-bearing. Without it
// another user can rename our verified directory aside and put their own at
// the name clients resolve, leaving the core running safely inside its own
// inode while every client is sent somewhere else.
bool openFallbackRoot(const std::string& path, DirFd& out, OwnResult& result) {
    // Deliberately no O_NOFOLLOW: /tmp is legitimately a symlink on some
    // systems, and it is not a component this core owns.
    const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        result = {OwnStatus::Failed, "ipc.endpoint_unsafe",
                  "cannot open " + path + ": " + describeErrno(errno)};
        return false;
    }
    out = DirFd(fd);

    struct stat info {};
    if (::fstat(out.get(), &info) != 0) {
        result = {OwnStatus::Failed, "ipc.endpoint_unsafe",
                  "cannot stat " + path + ": " + describeErrno(errno)};
        return false;
    }
    if (!S_ISDIR(info.st_mode)) {
        result = {OwnStatus::Unsafe, "ipc.endpoint_unsafe", path + " is not a directory"};
        return false;
    }
    if ((info.st_mode & S_ISVTX) == 0) {
        result = {OwnStatus::Unsafe, "ipc.endpoint_unsafe",
                  path + " has no sticky bit; a per-user directory beneath it "
                         "could be replaced by another user"};
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Transport

class PosixConnection : public Connection {
public:
    explicit PosixConnection(int fd) : fd_(fd) {}
    ~PosixConnection() override { close(); }

    int read(char* buffer, std::size_t length) override {
        if (fd_ < 0) return -1;
        for (;;) {
            const ssize_t got = ::read(fd_, buffer, length);
            if (got > 0) return static_cast<int>(got);
            if (got == 0) return -1;   // orderly shutdown by the peer
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            return -1;
        }
    }

    int write(const char* buffer, std::size_t length) override {
        if (fd_ < 0) return -1;
        for (;;) {
            // MSG_NOSIGNAL: a client that vanishes mid-write must return an
            // error here, not deliver SIGPIPE and take the core down with it.
            const ssize_t wrote = ::send(fd_, buffer, length, MSG_NOSIGNAL);
            if (wrote >= 0) return static_cast<int>(wrote);
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            return -1;
        }
    }

    void close() override {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }

private:
    int fd_ = -1;
};

class PosixTransport : public Transport {
public:
    explicit PosixTransport(int listenFd) : fd_(listenFd) {}
    ~PosixTransport() override { close(); }

    std::unique_ptr<Connection> accept() override {
        if (fd_ < 0) return nullptr;
        for (;;) {
            const int client = ::accept(fd_, nullptr, nullptr);
            if (client >= 0) {
                const int flags = ::fcntl(client, F_GETFL, 0);
                ::fcntl(client, F_SETFL, (flags < 0 ? 0 : flags) | O_NONBLOCK);
                ::fcntl(client, F_SETFD, FD_CLOEXEC);
                return std::make_unique<PosixConnection>(client);
            }
            if (errno == EINTR) continue;
            return nullptr;   // EAGAIN: nobody waiting
        }
    }

    void close() override {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }

private:
    int fd_ = -1;
};

}  // namespace

// ---------------------------------------------------------------------------
// Resolution (SPEC 5.1.1)

std::string resolveRuntimeDirectory() {
    const char* xdg = std::getenv("XDG_RUNTIME_DIR");
    std::string base;
    if (xdg != nullptr && xdg[0] != '\0') {
        base = xdg;
    } else {
        // The fallback exists because XDG_RUNTIME_DIR is absent on non-systemd
        // installations without elogind, in bare startx sessions, and in
        // minimal containers.
        base = "/tmp/keygnosys-" + std::to_string(static_cast<unsigned long>(::getuid()));
    }
    if (!base.empty() && base.back() == '/') base.pop_back();
    // The `keygnosys` component repeats in the fallback form. That is the
    // intended consequence of there being ONE composition rule rather than
    // two: a second rule existing only to tidy a path is a second rule the
    // core and the overlay can implement inconsistently.
    return base + "/keygnosys";
}

std::string resolveEndpoint() {
    return resolveRuntimeDirectory() + "/" + kSocketName;
}

// ---------------------------------------------------------------------------

struct EndpointOwner::Impl {
    DirFd runtimeDir;
    int lockFd = -1;
    bool bound = false;

    ~Impl() {
        // The lock descriptor closing is what releases the flock, and that
        // happens when the process ends by any means -- which is the whole
        // reason it is a lock and not a PID file. The lock FILE is never
        // unlinked.
        if (lockFd >= 0) ::close(lockFd);
    }
};

EndpointOwner::EndpointOwner() : impl_(std::make_unique<Impl>()) {}

EndpointOwner::~EndpointOwner() { release(); }

OwnResult EndpointOwner::acquire(const std::string& address) {
    address_ = address;
    OwnResult result;

    // -- 1. Verify every component this core owns (SPEC 5.1.2) --------------
    const std::size_t lastSlash = address.find_last_of('/');
    if (lastSlash == std::string::npos || lastSlash == 0) {
        return {OwnStatus::Failed, "ipc.endpoint_unsafe",
                "endpoint address has no directory component"};
    }
    const std::string dirPath = address.substr(0, lastSlash);
    const std::string leaf = address.substr(lastSlash + 1);

    const char* xdg = std::getenv("XDG_RUNTIME_DIR");
    const bool usingFallback = (xdg == nullptr || xdg[0] == '\0');

    DirFd base;
    if (usingFallback) {
        // /tmp -> /tmp/keygnosys-<uid> -> .../keygnosys
        const std::size_t baseSlash = dirPath.find_last_of('/');
        const std::string basePath = dirPath.substr(0, baseSlash);      // /tmp/keygnosys-<uid>
        const std::size_t rootSlash = basePath.find_last_of('/');
        const std::string rootPath = basePath.substr(0, rootSlash);     // /tmp
        const std::string baseName = basePath.substr(rootSlash + 1);

        DirFd root;
        if (!openFallbackRoot(rootPath.empty() ? "/" : rootPath, root, result)) {
            return result;
        }
        if (!openVerifiedChild(root.get(), baseName.c_str(), true, base, basePath,
                               result)) {
            return result;
        }
    } else {
        // $XDG_RUNTIME_DIR is verified too, not trusted. It is an environment
        // variable, and a process able to set the core's environment could
        // have pointed it anywhere.
        const std::size_t baseSlash = dirPath.find_last_of('/');
        const std::string basePath = dirPath.substr(0, baseSlash);
        const int fd = ::open(basePath.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd < 0) {
            return {OwnStatus::Failed, "ipc.endpoint_unsafe",
                    "cannot open " + basePath + ": " + describeErrno(errno)};
        }
        base = DirFd(fd);
        if (!verifyDirFd(base.get(), basePath.c_str(), result)) return result;
    }

    const std::string childName = dirPath.substr(dirPath.find_last_of('/') + 1);
    if (!openVerifiedChild(base.get(), childName.c_str(), true, impl_->runtimeDir,
                           dirPath, result)) {
        return result;
    }

    // -- 2/3. Take the startup lock BEFORE probing (SPEC 5.1.3) ------------
    //
    // O_CLOEXEC so a process the core execs cannot inherit the descriptor and
    // hold the lock past the core's own death.
    impl_->lockFd = ::openat(impl_->runtimeDir.get(), kLockName,
                             O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (impl_->lockFd < 0) {
        return {OwnStatus::Failed, "ipc.endpoint_unsafe",
                std::string("cannot open the startup lock: ") + describeErrno(errno)};
    }
    // flock, not fcntl(F_SETLK): an fcntl record lock is dropped when ANY
    // descriptor to the file is closed anywhere in the process, so an
    // unrelated open-and-close would silently surrender it.
    if (::flock(impl_->lockFd, LOCK_EX | LOCK_NB) != 0) {
        const int code = errno;
        ::close(impl_->lockFd);
        impl_->lockFd = -1;
        if (code == EWOULDBLOCK) {
            // No blocking and no retry loop: a core that waits on startup is
            // indistinguishable from a core that has hung.
            return {OwnStatus::InUse, "ipc.endpoint_in_use",
                    "another core owns or is claiming this endpoint"};
        }
        return {OwnStatus::Failed, "ipc.endpoint_in_use",
                std::string("cannot take the startup lock: ") + describeErrno(code)};
    }

    // Bind and connect have no *at form, so the verified descriptor is made
    // the working directory and every name below is relative to it. This is
    // the one step where the descriptor discipline is easy to abandon by
    // accident, and reassembling the absolute path here would undo every
    // check above. It also keeps every name far inside sun_path's 108 bytes.
    const int cwd = ::open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (::fchdir(impl_->runtimeDir.get()) != 0) {
        if (cwd >= 0) ::close(cwd);
        return {OwnStatus::Failed, "ipc.endpoint_unsafe",
                std::string("cannot enter the runtime directory: ") +
                    describeErrno(errno)};
    }
    struct CwdRestore {
        int fd;
        ~CwdRestore() {
            if (fd >= 0) {
                if (::fchdir(fd) != 0) { /* nothing useful to do */ }
                ::close(fd);
            }
        }
    } restore{cwd};

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", leaf.c_str());

    // -- 4. Probe, with the lock already held ------------------------------
    {
        const int probe = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (probe < 0) {
            return {OwnStatus::Failed, "ipc.endpoint_in_use",
                    std::string("cannot create a probe socket: ") + describeErrno(errno)};
        }
        const int connected =
            ::connect(probe, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        const int code = errno;
        ::close(probe);
        if (connected == 0) {
            return {OwnStatus::InUse, "ipc.endpoint_in_use",
                    "a core is already listening on this endpoint"};
        }
        if (code == ECONNREFUSED) {
            // The only admissible evidence of staleness. A process listing, a
            // PID file and the socket's age are not evidence, and each can be
            // wrong in the direction that removes a live core's endpoint.
        } else if (code != ENOENT) {
            return {OwnStatus::InUse, "ipc.endpoint_in_use",
                    std::string("endpoint cannot be proven stale: ") +
                        describeErrno(code)};
        }
    }

    // -- 5/6. Bind a temporary name, listen, then rename onto the canonical --
    //
    // rename() contributes nothing to mutual exclusion -- the lock is the
    // whole of that -- but it leaves no interval in which the pathname is
    // absent, which a polling client would otherwise read as the core not
    // existing. Listening BEFORE the rename goes further than the
    // specification requires: the endpoint is answering the moment its name
    // appears, so the bind-to-listen window that makes ECONNREFUSED
    // ambiguous never exists for a name any client can see.
    const std::string temporary =
        "." + leaf + "." + std::to_string(static_cast<long>(::getpid()));
    ::unlinkat(impl_->runtimeDir.get(), temporary.c_str(), 0);

    const int listenFd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listenFd < 0) {
        return {OwnStatus::Failed, "ipc.endpoint_in_use",
                std::string("cannot create the listening socket: ") +
                    describeErrno(errno)};
    }

    sockaddr_un tempAddr{};
    tempAddr.sun_family = AF_UNIX;
    std::snprintf(tempAddr.sun_path, sizeof(tempAddr.sun_path), "%s",
                  temporary.c_str());

    // umask would otherwise widen the socket's mode behind our back.
    const mode_t previousMask = ::umask(0177);
    const int bound =
        ::bind(listenFd, reinterpret_cast<sockaddr*>(&tempAddr), sizeof(tempAddr));
    ::umask(previousMask);
    if (bound != 0) {
        const int code = errno;
        ::close(listenFd);
        return {OwnStatus::Failed, "ipc.endpoint_in_use",
                std::string("cannot bind the endpoint: ") + describeErrno(code)};
    }
    // Belt and braces: assert the mode rather than trusting umask arithmetic.
    if (::fchmodat(impl_->runtimeDir.get(), temporary.c_str(), 0600, 0) != 0) {
        const int code = errno;
        ::close(listenFd);
        ::unlinkat(impl_->runtimeDir.get(), temporary.c_str(), 0);
        return {OwnStatus::Failed, "ipc.endpoint_unsafe",
                std::string("cannot restrict the endpoint's mode: ") +
                    describeErrno(code)};
    }

    const int flags = ::fcntl(listenFd, F_GETFL, 0);
    ::fcntl(listenFd, F_SETFL, (flags < 0 ? 0 : flags) | O_NONBLOCK);

    if (::listen(listenFd, 16) != 0) {
        const int code = errno;
        ::close(listenFd);
        ::unlinkat(impl_->runtimeDir.get(), temporary.c_str(), 0);
        return {OwnStatus::Failed, "ipc.endpoint_in_use",
                std::string("cannot listen: ") + describeErrno(code)};
    }

    if (::renameat(impl_->runtimeDir.get(), temporary.c_str(),
                   impl_->runtimeDir.get(), leaf.c_str()) != 0) {
        const int code = errno;
        ::close(listenFd);
        ::unlinkat(impl_->runtimeDir.get(), temporary.c_str(), 0);
        return {OwnStatus::Failed, "ipc.endpoint_in_use",
                std::string("cannot publish the endpoint: ") + describeErrno(code)};
    }

    impl_->bound = true;
    transport_ = std::make_unique<PosixTransport>(listenFd);
    return {OwnStatus::Ok, "", "listening on " + address};
}

void EndpointOwner::release() {
    if (!impl_) return;
    transport_.reset();
    if (impl_->bound && impl_->runtimeDir.valid()) {
        // Safe because the lock is still held: no other core can be part-way
        // through claiming this endpoint. Removing it means the next start
        // finds nothing rather than having to recover a stale socket.
        const std::size_t slash = address_.find_last_of('/');
        const std::string leaf =
            slash == std::string::npos ? address_ : address_.substr(slash + 1);
        ::unlinkat(impl_->runtimeDir.get(), leaf.c_str(), 0);
        impl_->bound = false;
    }
    // core.lock is deliberately NOT unlinked. Removing a lock file is itself a
    // race: two processes can hold exclusive locks on two different inodes
    // that were briefly reachable by the same name. An empty core.lock left
    // behind is correct, not litter.
}

}  // namespace kgn

#endif  // !_WIN32

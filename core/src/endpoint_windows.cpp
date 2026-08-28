// Endpoint ownership on Windows. See kgn/endpoint.hpp and SPEC 5.1.1 - 5.1.3.
//
// There is no lock file here, and that is not a shortcut. A named pipe is a
// kernel object with no filesystem persistence, so there is no such thing as a
// stale one, and CreateNamedPipe with FILE_FLAG_FIRST_PIPE_INSTANCE is a
// single kernel test-and-own: nothing is observable between deciding and
// claiming, so there is no probe-then-act sequence to serialize. The same flag
// also defeats a name squatted in advance, because by default
// CreateNamedPipe on an existing name SUCCEEDS and silently joins whoever
// created it.

#if defined(_WIN32)

#include "kgn/endpoint.hpp"

#include <cstring>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <aclapi.h>

namespace kgn {
namespace {

constexpr const char* kPipeName = R"(\\.\pipe\keygnosys)";
constexpr DWORD kBufferSize = 64 * 1024;

std::string describeLastError(DWORD code) {
    char* text = nullptr;
    const DWORD length = ::FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<char*>(&text), 0, nullptr);
    std::string message =
        length > 0 && text != nullptr ? std::string(text, length) : "error " + std::to_string(code);
    if (text != nullptr) ::LocalFree(text);
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r' ||
                               message.back() == '.' || message.back() == ' ')) {
        message.pop_back();
    }
    return message;
}

// A DACL granting the creating user and nobody else. SPEC section 5.1 requires
// it: anything else able to open this pipe would see every keystroke.
class OwnerOnlySecurity {
public:
    bool build(std::string& error) {
        HANDLE token = nullptr;
        if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token) == 0) {
            error = "cannot open the process token: " + describeLastError(::GetLastError());
            return false;
        }
        DWORD needed = 0;
        ::GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
        tokenInfo_.resize(needed);
        if (::GetTokenInformation(token, TokenUser, tokenInfo_.data(), needed,
                                  &needed) == 0) {
            error = "cannot read the token user: " + describeLastError(::GetLastError());
            ::CloseHandle(token);
            return false;
        }
        ::CloseHandle(token);

        auto* user = reinterpret_cast<TOKEN_USER*>(tokenInfo_.data());
        PSID sid = user->User.Sid;
        const DWORD sidLength = ::GetLengthSid(sid);

        const DWORD aclSize = static_cast<DWORD>(sizeof(ACL)) +
                              static_cast<DWORD>(sizeof(ACCESS_ALLOWED_ACE)) -
                              static_cast<DWORD>(sizeof(DWORD)) + sidLength;
        acl_.resize(aclSize);
        auto* acl = reinterpret_cast<ACL*>(acl_.data());
        if (::InitializeAcl(acl, aclSize, ACL_REVISION) == 0 ||
            ::AddAccessAllowedAce(acl, ACL_REVISION, GENERIC_ALL, sid) == 0) {
            error = "cannot build the endpoint ACL: " + describeLastError(::GetLastError());
            return false;
        }

        if (::InitializeSecurityDescriptor(&descriptor_, SECURITY_DESCRIPTOR_REVISION) == 0 ||
            ::SetSecurityDescriptorDacl(&descriptor_, TRUE, acl, FALSE) == 0) {
            error = "cannot build the endpoint security descriptor: " +
                    describeLastError(::GetLastError());
            return false;
        }

        attributes_.nLength = sizeof(SECURITY_ATTRIBUTES);
        attributes_.lpSecurityDescriptor = &descriptor_;
        attributes_.bInheritHandle = FALSE;
        return true;
    }

    SECURITY_ATTRIBUTES* attributes() { return &attributes_; }

private:
    std::vector<char> tokenInfo_;
    std::vector<char> acl_;
    SECURITY_DESCRIPTOR descriptor_{};
    SECURITY_ATTRIBUTES attributes_{};
};

HANDLE createInstance(const std::string& name, OwnerOnlySecurity& security, bool first,
                      DWORD& lastError) {
    DWORD flags = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED;
    if (first) flags |= FILE_FLAG_FIRST_PIPE_INSTANCE;
    // PIPE_TYPE_MESSAGE is the pipe mode SPEC section 5.1 names; the read mode
    // is byte, because the framing that actually carries meaning is the
    // newline, and a reader that consumed whole messages would still have to
    // reassemble lines.
    const HANDLE handle = ::CreateNamedPipeA(
        name.c_str(), flags, PIPE_TYPE_MESSAGE | PIPE_READMODE_BYTE | PIPE_WAIT,
        PIPE_UNLIMITED_INSTANCES, kBufferSize, kBufferSize, 0, security.attributes());
    lastError = handle == INVALID_HANDLE_VALUE ? ::GetLastError() : ERROR_SUCCESS;
    return handle;
}

// ---------------------------------------------------------------------------

class PipeConnection : public Connection {
public:
    explicit PipeConnection(HANDLE handle) : handle_(handle) {
        readOverlapped_.hEvent = ::CreateEventA(nullptr, TRUE, FALSE, nullptr);
        writeOverlapped_.hEvent = ::CreateEventA(nullptr, TRUE, FALSE, nullptr);
    }
    ~PipeConnection() override { close(); }

    int read(char* buffer, std::size_t length) override {
        if (handle_ == INVALID_HANDLE_VALUE) return -1;

        if (available_ == 0) {
            if (!pumpRead()) return -1;
            if (available_ == 0) return 0;
        }
        const std::size_t take = length < available_ ? length : available_;
        std::memcpy(buffer, readBuffer_.data() + consumed_, take);
        consumed_ += take;
        available_ -= take;
        if (available_ == 0) consumed_ = 0;
        return static_cast<int>(take);
    }

    int write(const char* buffer, std::size_t length) override {
        if (handle_ == INVALID_HANDLE_VALUE) return -1;

        if (writePending_) {
            DWORD wrote = 0;
            if (::GetOverlappedResult(handle_, &writeOverlapped_, &wrote, FALSE) == 0) {
                if (::GetLastError() == ERROR_IO_INCOMPLETE) return 0;
                return -1;
            }
            writePending_ = false;
            ::ResetEvent(writeOverlapped_.hEvent);
        }

        // The bytes are copied rather than written from the caller's buffer,
        // because an overlapped write owns its buffer until it completes and
        // the server's queue is free to move on the moment this returns.
        const std::size_t take = length < kBufferSize ? length : kBufferSize;
        writeBuffer_.assign(buffer, buffer + take);
        DWORD wrote = 0;
        if (::WriteFile(handle_, writeBuffer_.data(), static_cast<DWORD>(take), &wrote,
                        &writeOverlapped_) != 0) {
            return static_cast<int>(take);
        }
        if (::GetLastError() == ERROR_IO_PENDING) {
            writePending_ = true;
            return static_cast<int>(take);   // committed; we own the copy
        }
        return -1;
    }

    void close() override {
        if (handle_ != INVALID_HANDLE_VALUE) {
            ::CancelIo(handle_);
            ::DisconnectNamedPipe(handle_);
            ::CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
        if (readOverlapped_.hEvent != nullptr) ::CloseHandle(readOverlapped_.hEvent);
        if (writeOverlapped_.hEvent != nullptr) ::CloseHandle(writeOverlapped_.hEvent);
        readOverlapped_.hEvent = nullptr;
        writeOverlapped_.hEvent = nullptr;
    }

private:
    // Returns false when the peer is gone.
    bool pumpRead() {
        if (!readPending_) {
            readBuffer_.resize(kBufferSize);
            DWORD got = 0;
            ::ResetEvent(readOverlapped_.hEvent);
            if (::ReadFile(handle_, readBuffer_.data(), kBufferSize, &got,
                           &readOverlapped_) != 0) {
                available_ = got;
                consumed_ = 0;
                return true;
            }
            const DWORD error = ::GetLastError();
            if (error != ERROR_IO_PENDING) return false;
            readPending_ = true;
            return true;
        }
        DWORD got = 0;
        if (::GetOverlappedResult(handle_, &readOverlapped_, &got, FALSE) == 0) {
            if (::GetLastError() == ERROR_IO_INCOMPLETE) return true;   // nothing yet
            return false;
        }
        readPending_ = false;
        ::ResetEvent(readOverlapped_.hEvent);
        if (got == 0) return false;   // orderly close
        available_ = got;
        consumed_ = 0;
        return true;
    }

    HANDLE handle_ = INVALID_HANDLE_VALUE;
    OVERLAPPED readOverlapped_{};
    OVERLAPPED writeOverlapped_{};
    std::vector<char> readBuffer_;
    std::vector<char> writeBuffer_;
    std::size_t available_ = 0;
    std::size_t consumed_ = 0;
    bool readPending_ = false;
    bool writePending_ = false;
};

class WinTransport : public Transport {
public:
    WinTransport(std::string name, HANDLE first, OwnerOnlySecurity security)
        : name_(std::move(name)), pending_(first), security_(std::move(security)) {
        connectOverlapped_.hEvent = ::CreateEventA(nullptr, TRUE, FALSE, nullptr);
        armConnect();
    }
    ~WinTransport() override { close(); }

    std::unique_ptr<Connection> accept() override {
        if (pending_ == INVALID_HANDLE_VALUE) return nullptr;
        if (!connected_) {
            DWORD ignored = 0;
            if (::GetOverlappedResult(pending_, &connectOverlapped_, &ignored, FALSE) == 0) {
                return nullptr;   // ERROR_IO_INCOMPLETE: nobody waiting
            }
            connected_ = true;
        }

        HANDLE accepted = pending_;
        pending_ = INVALID_HANDLE_VALUE;
        connected_ = false;
        ::ResetEvent(connectOverlapped_.hEvent);

        // Create the next instance WITHOUT the first-instance flag: this
        // process already owns the name, and asking for it again would fail.
        DWORD error = ERROR_SUCCESS;
        pending_ = createInstance(name_, security_, false, error);
        if (pending_ != INVALID_HANDLE_VALUE) armConnect();

        return std::make_unique<PipeConnection>(accepted);
    }

    void close() override {
        if (pending_ != INVALID_HANDLE_VALUE) {
            ::CancelIo(pending_);
            ::CloseHandle(pending_);
            pending_ = INVALID_HANDLE_VALUE;
        }
        if (connectOverlapped_.hEvent != nullptr) {
            ::CloseHandle(connectOverlapped_.hEvent);
            connectOverlapped_.hEvent = nullptr;
        }
    }

private:
    void armConnect() {
        connected_ = false;
        ::ResetEvent(connectOverlapped_.hEvent);
        if (::ConnectNamedPipe(pending_, &connectOverlapped_) != 0) {
            connected_ = true;
            return;
        }
        const DWORD error = ::GetLastError();
        if (error == ERROR_PIPE_CONNECTED) {
            // A client arrived between creating the instance and connecting it.
            connected_ = true;
            ::SetEvent(connectOverlapped_.hEvent);
        }
        // ERROR_IO_PENDING is the ordinary case; anything else leaves the
        // instance unarmed and accept() simply never reports it, which is
        // visible as the core accepting no clients rather than as a lie.
    }

    std::string name_;
    HANDLE pending_ = INVALID_HANDLE_VALUE;
    OVERLAPPED connectOverlapped_{};
    OwnerOnlySecurity security_;
    bool connected_ = false;
};

}  // namespace

// ---------------------------------------------------------------------------

std::string resolveRuntimeDirectory() { return {}; }

std::string resolveEndpoint() { return kPipeName; }

struct EndpointOwner::Impl {};

EndpointOwner::EndpointOwner() : impl_(std::make_unique<Impl>()) {}
EndpointOwner::~EndpointOwner() { release(); }

OwnResult EndpointOwner::acquire(const std::string& address) {
    address_ = address;

    OwnerOnlySecurity security;
    std::string error;
    if (!security.build(error)) {
        return {OwnStatus::Unsafe, "ipc.endpoint_unsafe", error};
    }

    DWORD code = ERROR_SUCCESS;
    const HANDLE first = createInstance(address, security, true, code);
    if (first == INVALID_HANDLE_VALUE) {
        if (code == ERROR_ACCESS_DENIED || code == ERROR_PIPE_BUSY) {
            // Either a core already owns the name, or something else created
            // it first. Both are refusals, and neither is recoverable: there
            // is nothing to remove, and retrying under a different name would
            // leave a core on an endpoint its clients do not resolve to.
            return {OwnStatus::InUse, "ipc.endpoint_in_use",
                    "another process already owns " + address};
        }
        return {OwnStatus::Failed, "ipc.endpoint_in_use",
                "cannot create the endpoint: " + describeLastError(code)};
    }

    transport_ = std::make_unique<WinTransport>(address, first, std::move(security));
    return {OwnStatus::Ok, "", "listening on " + address};
}

void EndpointOwner::release() { transport_.reset(); }

}  // namespace kgn

#endif  // _WIN32

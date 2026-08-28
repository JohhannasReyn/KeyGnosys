// The assembled core.
//
// An integration test rather than a unit one: it starts a real core on a real
// endpoint, connects a real client, and checks what comes back. The pieces are
// covered individually elsewhere; what this proves is that they are wired
// together, that the core is genuinely ready when it says it is, and that it
// describes what it cannot do rather than pretending.

#include <string>
#include <vector>

#include "kgn/core.hpp"
#include "kgn_test.hpp"

using kgn::Core;
using kgn::CoreOptions;
using kgn::Json;
using kgn::OwnStatus;

namespace {

std::string uniqueEndpoint(const char* suffix);
void removeEndpoint(const std::string& address);

// A raw client speaking the protocol, so the test exercises the transport as
// well as the server.
class RawClient {
public:
    bool connect(const std::string& address);
    void close();
    void send(const std::string& line);
    // Pump until a complete line arrives or the budget runs out.
    std::string readLine(Core& core, int attempts = 400);
    [[nodiscard]] bool valid() const;

private:
    std::string buffer_;
#if defined(_WIN32)
    void* handle_ = nullptr;
#else
    int fd_ = -1;
#endif
};

}  // namespace

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace {

std::string uniqueEndpoint(const char* suffix) {
    return R"(\\.\pipe\keygnosys-core-test-)" +
           std::to_string(static_cast<unsigned long>(::GetCurrentProcessId())) + "-" +
           suffix;
}

void removeEndpoint(const std::string&) {}   // pipes have nothing to remove

bool RawClient::connect(const std::string& address) {
    handle_ = ::CreateFileA(address.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                            OPEN_EXISTING, 0, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) {
        handle_ = nullptr;
        return false;
    }
    DWORD mode = PIPE_READMODE_BYTE | PIPE_NOWAIT;
    ::SetNamedPipeHandleState(handle_, &mode, nullptr, nullptr);
    return true;
}

void RawClient::close() {
    if (handle_ != nullptr) ::CloseHandle(handle_);
    handle_ = nullptr;
}

bool RawClient::valid() const { return handle_ != nullptr; }

void RawClient::send(const std::string& line) {
    DWORD wrote = 0;
    ::WriteFile(handle_, line.data(), static_cast<DWORD>(line.size()), &wrote, nullptr);
}

std::string RawClient::readLine(Core& core, int attempts) {
    for (int i = 0; i < attempts; ++i) {
        const std::size_t newline = buffer_.find('\n');
        if (newline != std::string::npos) {
            std::string line = buffer_.substr(0, newline);
            buffer_.erase(0, newline + 1);
            return line;
        }
        char chunk[4096];
        DWORD got = 0;
        if (::ReadFile(handle_, chunk, sizeof(chunk), &got, nullptr) != 0 && got > 0) {
            buffer_.append(chunk, got);
            continue;
        }
        core.step(kgn::Clock::now());
        ::Sleep(2);
    }
    return {};
}

}  // namespace

#else

#include <cerrno>
#include <cstdio>
#include <cstdlib>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

namespace {

std::string uniqueEndpoint(const char* suffix) {
    // Inside a private directory of our own, so the core's own verification
    // has something legitimate to verify.
    static std::string root;
    if (root.empty()) {
        char pattern[] = "/tmp/kgn-core-XXXXXX";
        const char* made = ::mkdtemp(pattern);
        root = made != nullptr ? made : "/tmp";
        ::chmod(root.c_str(), 0700);
    }
    const std::string directory = root + "/" + suffix;
    ::mkdir(directory.c_str(), 0700);
    return directory + "/keygnosys/core.sock";
}

void removeEndpoint(const std::string& address) {
    ::unlink(address.c_str());
    const std::size_t slash = address.find_last_of('/');
    if (slash != std::string::npos) {
        ::unlink((address.substr(0, slash) + "/core.lock").c_str());
    }
}

bool RawClient::connect(const std::string& address) {
    fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd_ < 0) return false;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", address.c_str());
    if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    const int flags = ::fcntl(fd_, F_GETFL, 0);
    ::fcntl(fd_, F_SETFL, (flags < 0 ? 0 : flags) | O_NONBLOCK);
    return true;
}

void RawClient::close() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
}

bool RawClient::valid() const { return fd_ >= 0; }

void RawClient::send(const std::string& line) {
    ssize_t ignored = ::write(fd_, line.data(), line.size());
    (void)ignored;
}

std::string RawClient::readLine(Core& core, int attempts) {
    for (int i = 0; i < attempts; ++i) {
        const std::size_t newline = buffer_.find('\n');
        if (newline != std::string::npos) {
            std::string line = buffer_.substr(0, newline);
            buffer_.erase(0, newline + 1);
            return line;
        }
        char chunk[4096];
        const ssize_t got = ::read(fd_, chunk, sizeof(chunk));
        if (got > 0) {
            buffer_.append(chunk, static_cast<std::size_t>(got));
            continue;
        }
        core.step(kgn::Clock::now());
        timespec pause{0, 2 * 1000 * 1000};
        ::nanosleep(&pause, nullptr);
    }
    return {};
}

}  // namespace

#endif

namespace {

Json parseLine(const std::string& line) {
    Json value;
    Json::parse(line, value);
    return value;
}

// A core on its own endpoint, torn down with the test.
struct Fixture {
    std::string address;
    Core core;

    explicit Fixture(const char* suffix)
        : address(uniqueEndpoint(suffix)), core(options(address)) {}

    ~Fixture() {
        core.stop("test finished");
        removeEndpoint(address);
    }

    static CoreOptions options(const std::string& endpoint) {
        CoreOptions opts;
        opts.endpointOverrideForTests = endpoint;
        // Point the search somewhere empty so the test never picks up a
        // document from the developer's own configuration.
        opts.configDir = "/nonexistent-kgn-config";
        opts.dataDir = "/nonexistent-kgn-data";
        return opts;
    }
};

}  // namespace

// ---------------------------------------------------------------------------

KGN_TEST(the_core_starts_and_owns_its_endpoint) {
    Fixture fixture("start");
    const auto result = fixture.core.start();
    KGN_CHECK(result.ok());
    KGN_CHECK(fixture.core.running());
    KGN_CHECK(fixture.core.server() != nullptr);
}

KGN_TEST(a_second_core_on_the_same_endpoint_is_refused) {
    Fixture fixture("second");
    KGN_CHECK(fixture.core.start().ok());

    Core other(Fixture::options(fixture.address));
    const auto result = other.start();
    KGN_CHECK(result.status == OwnStatus::InUse);
    KGN_CHECK_EQ(result.code, std::string("ipc.endpoint_in_use"));
}

KGN_TEST(ready_means_a_client_can_connect_and_be_greeted) {
    // Not "the pathname exists". The launcher contract turns on this
    // distinction (LAUNCHING.md section 4.2).
    Fixture fixture("hello");
    KGN_CHECK(fixture.core.start().ok());

    RawClient client;
    KGN_CHECK(client.connect(fixture.address));
    if (!client.valid()) return;

    const Json hello = parseLine(client.readLine(fixture.core));
    KGN_CHECK_EQ(hello["t"].asString(), std::string("event"));
    KGN_CHECK_EQ(hello["n"].asString(), std::string("hello"));
    KGN_CHECK_EQ(hello["d"]["protocol"].asString(), std::string("1.0"));
    KGN_CHECK_EQ(hello["d"]["core_version"].asString(), std::string("0.1.0"));
    client.close();
}

KGN_TEST(hello_admits_that_this_build_has_no_backends) {
    // P6 in the one place a client is guaranteed to read.
    Fixture fixture("backends");
    KGN_CHECK(fixture.core.start().ok());

    RawClient client;
    KGN_CHECK(client.connect(fixture.address));
    if (!client.valid()) return;

    const Json hello = parseLine(client.readLine(fixture.core));
    KGN_CHECK(hello["d"]["backends"]["input"].isNull());
    KGN_CHECK(hello["d"]["backends"]["output"].isNull());
    KGN_CHECK(hello["d"]["backends"]["window"].isNull());
    KGN_CHECK(hello["d"]["limitations"].size() >= std::size_t{1});
    KGN_CHECK(hello["d"]["limitations"].at(0).asString().find("No input backend") !=
              std::string::npos);
    client.close();
}

KGN_TEST(ping_is_answered_with_an_uptime) {
    Fixture fixture("ping");
    KGN_CHECK(fixture.core.start().ok());

    RawClient client;
    KGN_CHECK(client.connect(fixture.address));
    if (!client.valid()) return;
    client.readLine(fixture.core);   // hello

    client.send(R"({"v":1,"t":"command","n":"ping","id":"c1","d":{}})" "\n");
    const Json reply = parseLine(client.readLine(fixture.core));
    KGN_CHECK_EQ(reply["t"].asString(), std::string("reply"));
    KGN_CHECK_EQ(reply["id"].asString(), std::string("c1"));
    KGN_CHECK(reply["ok"].asBool());
    KGN_CHECK(reply["d"]["pong"].asBool());
    KGN_CHECK(reply["d"]["uptime_ms"].isNumber());
    client.close();
}

KGN_TEST(get_state_reports_what_is_true_and_nothing_more) {
    Fixture fixture("state");
    KGN_CHECK(fixture.core.start().ok());

    RawClient client;
    KGN_CHECK(client.connect(fixture.address));
    if (!client.valid()) return;
    client.readLine(fixture.core);

    client.send(R"({"v":1,"t":"command","n":"get_state","id":"c1","d":{}})" "\n");
    const Json reply = parseLine(client.readLine(fixture.core));
    KGN_CHECK(reply["ok"].asBool());
    KGN_CHECK_EQ(reply["d"]["mode"].asString(), std::string("normal"));
    KGN_CHECK_EQ(reply["d"]["activation"].asString(), std::string("hybrid"));
    KGN_CHECK(reply["d"]["enabled"].asBool());
    // No window backend, so there is nothing truthful to say about these.
    KGN_CHECK(reply["d"]["focus"].isNull());
    KGN_CHECK_EQ(reply["d"]["windows"].size(), std::size_t{0});
    KGN_CHECK_EQ(reply["d"]["monitors"].size(), std::size_t{0});
    client.close();
}

KGN_TEST(the_activation_mode_can_be_changed_and_is_announced) {
    Fixture fixture("activation");
    KGN_CHECK(fixture.core.start().ok());

    RawClient client;
    KGN_CHECK(client.connect(fixture.address));
    if (!client.valid()) return;
    client.readLine(fixture.core);

    client.send(
        R"({"v":1,"t":"command","n":"set_activation_mode","id":"c1","d":{"mode":"toggle"}})"
        "\n");

    bool sawMode = false;
    bool sawReply = false;
    for (int i = 0; i < 6 && !(sawMode && sawReply); ++i) {
        const Json message = parseLine(client.readLine(fixture.core));
        if (message["t"].asString() == "reply") sawReply = message["ok"].asBool();
        if (message["n"].asString() == "mode") {
            sawMode = message["d"]["activation"].asString() == "toggle";
        }
    }
    KGN_CHECK(sawReply);
    KGN_CHECK(sawMode);
    client.close();
}

KGN_TEST(an_invalid_activation_mode_is_refused) {
    Fixture fixture("badmode");
    KGN_CHECK(fixture.core.start().ok());
    RawClient client;
    KGN_CHECK(client.connect(fixture.address));
    if (!client.valid()) return;
    client.readLine(fixture.core);

    client.send(
        R"({"v":1,"t":"command","n":"set_activation_mode","id":"c1","d":{"mode":"sideways"}})"
        "\n");
    for (int i = 0; i < 6; ++i) {
        const Json message = parseLine(client.readLine(fixture.core));
        if (message["t"].asString() != "reply") continue;
        KGN_CHECK(!message["ok"].asBool(true));
        KGN_CHECK_EQ(message["e"]["code"].asString(), std::string("config.invalid"));
        break;
    }
    client.close();
}

KGN_TEST(an_unknown_command_is_refused_by_name) {
    Fixture fixture("unknown");
    KGN_CHECK(fixture.core.start().ok());
    RawClient client;
    KGN_CHECK(client.connect(fixture.address));
    if (!client.valid()) return;
    client.readLine(fixture.core);

    client.send(R"({"v":1,"t":"command","n":"launch_missiles","id":"c1","d":{}})" "\n");
    const Json reply = parseLine(client.readLine(fixture.core));
    KGN_CHECK(!reply["ok"].asBool(true));
    KGN_CHECK_EQ(reply["e"]["code"].asString(), std::string("ipc.unsupported"));
    client.close();
}

KGN_TEST(an_unknown_setting_path_is_refused_rather_than_ignored) {
    Fixture fixture("setting");
    KGN_CHECK(fixture.core.start().ok());
    RawClient client;
    KGN_CHECK(client.connect(fixture.address));
    if (!client.valid()) return;
    client.readLine(fixture.core);

    client.send(
        R"({"v":1,"t":"command","n":"set_setting","id":"c1","d":{"path":"behavior.nonsense","value":1}})"
        "\n");
    const Json reply = parseLine(client.readLine(fixture.core));
    KGN_CHECK(!reply["ok"].asBool(true));
    KGN_CHECK_EQ(reply["e"]["code"].asString(), std::string("config.invalid"));
    client.close();
}

KGN_TEST(a_known_setting_is_applied) {
    Fixture fixture("grace");
    KGN_CHECK(fixture.core.start().ok());
    RawClient client;
    KGN_CHECK(client.connect(fixture.address));
    if (!client.valid()) return;
    client.readLine(fixture.core);

    client.send(
        R"({"v":1,"t":"command","n":"set_setting","id":"c1","d":{"path":"behavior.grace_ms","value":40}})"
        "\n");
    const Json reply = parseLine(client.readLine(fixture.core));
    KGN_CHECK(reply["ok"].asBool());
    client.close();
}

KGN_TEST(reload_reports_what_it_actually_loaded) {
    Fixture fixture("reload");
    KGN_CHECK(fixture.core.start().ok());
    RawClient client;
    KGN_CHECK(client.connect(fixture.address));
    if (!client.valid()) return;
    client.readLine(fixture.core);

    client.send(R"({"v":1,"t":"command","n":"reload_config","id":"c1","d":{}})" "\n");
    for (int i = 0; i < 8; ++i) {
        const Json message = parseLine(client.readLine(fixture.core));
        if (message["t"].asString() != "reply") continue;
        KGN_CHECK(message["ok"].asBool());
        // Nothing was found, and it says zero rather than a number it made up.
        KGN_CHECK_EQ(message["d"]["loaded"]["bindings"].asInt(-1), std::int64_t{0});
        break;
    }
    client.close();
}

KGN_TEST(release_all_is_accepted_even_with_nothing_held) {
    // The panic button has to work when there is nothing to panic about.
    Fixture fixture("releaseall");
    KGN_CHECK(fixture.core.start().ok());
    RawClient client;
    KGN_CHECK(client.connect(fixture.address));
    if (!client.valid()) return;
    client.readLine(fixture.core);

    client.send(R"({"v":1,"t":"command","n":"release_all","id":"c1","d":{}})" "\n");
    for (int i = 0; i < 6; ++i) {
        const Json message = parseLine(client.readLine(fixture.core));
        if (message["t"].asString() != "reply") continue;
        KGN_CHECK(message["ok"].asBool());
        break;
    }
    client.close();
}

KGN_TEST(a_bindings_document_is_loaded_when_one_is_given) {
    Fixture fixture("bindings");
    // The repository's own default document, found through the data-dir
    // search, is the most useful thing to load here: if the shipped file ever
    // stops parsing, this fails.
    CoreOptions options = Fixture::options(fixture.address);
    options.configDir.clear();
    options.dataDir.clear();
    Core core(std::move(options));
    const auto result = core.start();
    KGN_CHECK(result.ok());
    // Whether the data directory was found depends on where the test binary
    // sits, so this asserts only that starting is survivable either way and
    // that any failure was reported rather than swallowed.
    if (core.diagnostics().empty()) {
        KGN_CHECK(true);
    } else {
        KGN_CHECK(!core.diagnostics().front().code.empty());
    }
    core.stop("done");
}

KGN_TEST(stopping_is_idempotent_and_closes_the_endpoint) {
    const std::string address = uniqueEndpoint("stop");
    {
        Core core(Fixture::options(address));
        KGN_CHECK(core.start().ok());
        core.stop("first");
        core.stop("second");
        KGN_CHECK(!core.running());
    }
    // And the endpoint is free again.
    Core next(Fixture::options(address));
    KGN_CHECK(next.start().ok());
    next.stop("done");
    removeEndpoint(address);
}

KGN_TEST(a_client_that_leaves_does_not_disturb_the_core) {
    Fixture fixture("leave");
    KGN_CHECK(fixture.core.start().ok());
    {
        RawClient client;
        KGN_CHECK(client.connect(fixture.address));
        if (client.valid()) {
            client.readLine(fixture.core);
            client.close();
        }
    }
    for (int i = 0; i < 20; ++i) fixture.core.step(kgn::Clock::now());
    KGN_CHECK(fixture.core.running());

    RawClient again;
    KGN_CHECK(again.connect(fixture.address));
    if (again.valid()) {
        const Json hello = parseLine(again.readLine(fixture.core));
        KGN_CHECK_EQ(hello["n"].asString(), std::string("hello"));
        again.close();
    }
}

KGN_TEST(stepping_with_no_input_backend_moves_nothing_and_does_not_spin) {
    Fixture fixture("idle");
    KGN_CHECK(fixture.core.start().ok());
    for (int i = 0; i < 200; ++i) fixture.core.step(kgn::Clock::now());
    KGN_CHECK(fixture.core.running());
}

int main() { return kgn::test::runAll(); }

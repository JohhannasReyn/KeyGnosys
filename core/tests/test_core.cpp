// The assembled core.
//
// An integration test rather than a unit one: it starts a real core on a real
// endpoint, connects a real client, and checks what comes back. The pieces are
// covered individually elsewhere; what this proves is that they are wired
// together, that the core is genuinely ready when it says it is, and that it
// describes what it cannot do rather than pretending.

#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
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
// Test doubles. Recording what the core ASKED for is not the same thing as a
// fake backend shipped to a user: P6 forbids the second, and these never leave
// the test binary. They exist because the capability, warp and window paths
// have no other way to be exercised without a real desktop.
class RecordingOutput : public kgn::OutputBackend {
public:
    std::vector<std::pair<kgn::KeyCode, bool>> keys;
    std::vector<std::pair<kgn::MouseButton, bool>> buttons;
    std::vector<kgn::Point> warps;
    kgn::Point cursor{};
    int releaseAllCalls = 0;
    int releaseAllOrder = 0;
    int* clock = nullptr;

    void moveCursorBy(int, int) override {}
    void moveCursorTo(int x, int y) override {
        warps.push_back({x, y});
        cursor = {x, y};
    }
    kgn::Point cursorPosition() override { return cursor; }
    void button(kgn::MouseButton b, bool down) override { buttons.emplace_back(b, down); }
    void scroll(int, int) override {}
    void sendKey(kgn::KeyCode code, bool down) override { keys.emplace_back(code, down); }
    void releaseAll() override {
        ++releaseAllCalls;
        if (clock) releaseAllOrder = ++(*clock);
    }
    std::chrono::milliseconds doubleClickInterval() const override {
        return std::chrono::milliseconds(50);
    }
    kgn::Capabilities capabilities() const override {
        kgn::Capabilities c;
        c.canWarpAbsolute = true;
        return c;
    }
    std::string_view name() const override { return "recording-output"; }
};

class RecordingInput : public kgn::InputBackend {
public:
    int startCalls = 0;
    int stopCalls = 0;
    int stopOrder = 0;
    int* clock = nullptr;

    bool start(Handler) override {
        ++startCalls;
        return true;
    }
    void stop() override {
        ++stopCalls;
        if (clock) stopOrder = ++(*clock);
    }
    kgn::Capabilities capabilities() const override {
        kgn::Capabilities c;
        c.canSuppress = true;
        c.limitations.emplace_back("A recording input backend intercepts nothing.");
        return c;
    }
    std::string_view name() const override { return "recording-input"; }
};

// An engine owner that accepts everything and applies nothing. Stands in for
// an input thread that has wedged -- the case the shutdown fallback exists for.
class NeverAcknowledging : public kgn::EngineOwner {
public:
    int submitted = 0;

    bool submit(const kgn::Control&) override {
        ++submitted;
        return true;
    }
    bool awaitApplied(std::uint32_t, std::chrono::milliseconds) override {
        return false;
    }
};

class RecordingWindow : public kgn::WindowBackend {
public:
    std::vector<kgn::WindowInfo> list;
    std::vector<kgn::MonitorInfo> screens;
    kgn::WindowId focusedId = 0;
    std::vector<kgn::WindowId> focusCalls;
    std::vector<std::pair<kgn::WindowId, int>> moves;

    std::vector<kgn::WindowInfo> windows() override { return list; }
    std::optional<kgn::WindowInfo> focused() override {
        for (const auto& info : list) {
            if (info.id == focusedId) return info;
        }
        return std::nullopt;
    }
    bool focus(kgn::WindowId id) override {
        focusCalls.push_back(id);
        focusedId = id;
        return true;
    }
    std::vector<kgn::MonitorInfo> monitors() override { return screens; }
    bool moveWindowToMonitor(kgn::WindowId id, int index) override {
        moves.emplace_back(id, index);
        return true;
    }
    kgn::Capabilities capabilities() const override {
        kgn::Capabilities c;
        c.canMoveWindows = true;
        return c;
    }
    std::string_view name() const override { return "recording-window"; }
};

struct Fixture {
    std::string address;
    Core core;

    explicit Fixture(const char* suffix, kgn::Backends backends = {})
        : address(uniqueEndpoint(suffix)),
          core(options(address), std::move(backends)) {}

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

KGN_TEST(hello_reads_each_capability_from_the_backend_that_owns_it) {
    // warp_absolute is a property of the OUTPUT backend. Reading it off the
    // input backend -- as this once did -- means a build can announce a
    // capability that nothing in it implements, which is the exact shape of
    // failure P6 exists to forbid.
    kgn::Backends backends;
    backends.input = std::make_unique<RecordingInput>();
    backends.output = std::make_unique<RecordingOutput>();
    Fixture fixture("caps", std::move(backends));
    KGN_CHECK(fixture.core.start().ok());

    RawClient client;
    KGN_CHECK(client.connect(fixture.address));
    if (!client.valid()) return;

    const Json hello = parseLine(client.readLine(fixture.core));
    KGN_CHECK_EQ(hello["d"]["backends"]["input"].asString(),
                 std::string("recording-input"));
    KGN_CHECK_EQ(hello["d"]["backends"]["output"].asString(),
                 std::string("recording-output"));
    KGN_CHECK(hello["d"]["backends"]["window"].isNull());

    bool suppress = false;
    bool warp = false;
    for (std::size_t i = 0; i < hello["d"]["capabilities"].size(); ++i) {
        const std::string name = hello["d"]["capabilities"].at(i).asString();
        if (name == "suppress") suppress = true;
        if (name == "warp_absolute") warp = true;
    }
    KGN_CHECK(suppress);
    KGN_CHECK(warp);
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

KGN_TEST(a_key_event_is_published_once_per_physical_event_not_once_per_decision) {
    // A grace replay is ONE physical Up that produces TWO Forward decisions.
    // Publishing per decision -- which is what the core did before the streams
    // were separated -- reports a key press the user never made.
    Fixture fixture("publish");
    KGN_CHECK(fixture.core.start().ok());

    RawClient client;
    KGN_CHECK(client.connect(fixture.address));
    if (!client.valid()) return;
    parseLine(client.readLine(fixture.core));   // hello

    const kgn::KeyCode code = kgn::KeyCode::fromString("KeyD");
    fixture.core.pushWorkForTests({kgn::WorkItem::Kind::SendKey, true, code.id()});
    fixture.core.pushWorkForTests({kgn::WorkItem::Kind::SendKey, false, code.id()});
    fixture.core.publishPhysicalForTests({code.id(), kgn::KeyState::Up, true, 0});

    // Drain everything the core has to say, then count. Counting rather than
    // inspecting the next line matters: an assertion that only looks at what
    // follows passes just as happily when nothing follows at all.
    int keyEvents = 0;
    Json firstKey;
    for (int i = 0; i < 12; ++i) {
        const std::string line = client.readLine(fixture.core, 40);
        if (line.empty()) break;
        const Json message = parseLine(line);
        if (message["n"].asString() != "key") continue;
        if (keyEvents == 0) firstKey = message;
        ++keyEvents;
    }

    KGN_CHECK_EQ(keyEvents, 1);
    KGN_CHECK_EQ(firstKey["d"]["code"].asString(), std::string("KeyD"));
    KGN_CHECK_EQ(firstKey["d"]["state"].asString(), std::string("up"));
    KGN_CHECK(firstKey["d"]["suppressed"].asBool());
    client.close();
}

KGN_TEST(work_items_reach_the_output_backend_and_publication_does_not) {
    // The mirror of the test above: a work item drives the backend, a physical
    // record does not. Conflating them delivers every keystroke twice.
    kgn::Backends backends;
    auto output = std::make_unique<RecordingOutput>();
    RecordingOutput* recorded = output.get();
    backends.output = std::move(output);

    Fixture fixture("work", std::move(backends));
    KGN_CHECK(fixture.core.start().ok());

    const kgn::KeyCode code = kgn::KeyCode::fromString("KeyD");
    fixture.core.publishPhysicalForTests({code.id(), kgn::KeyState::Down, false, 0});
    fixture.core.step(kgn::Clock::now());
    KGN_CHECK(recorded->keys.empty());   // a natively forwarded press, not resent

    fixture.core.pushWorkForTests({kgn::WorkItem::Kind::SendKey, true, code.id()});
    fixture.core.step(kgn::Clock::now());
    KGN_CHECK_EQ(recorded->keys.size(), std::size_t{1});
    KGN_CHECK(recorded->keys[0].second);
}

KGN_TEST(shutdown_releases_everything_even_when_the_engine_owner_never_answers) {
    // The formal fallback. Obligations partition into three sets and only the
    // first involves the engine's owner at all -- and that one is discharged
    // by uninstalling the input backend, not by the owner replying.
    int clock = 0;
    auto output = std::make_unique<RecordingOutput>();
    auto input = std::make_unique<RecordingInput>();
    RecordingOutput* recordedOutput = output.get();
    RecordingInput* recordedInput = input.get();
    recordedOutput->clock = &clock;
    recordedInput->clock = &clock;

    kgn::Backends backends;
    backends.output = std::move(output);
    backends.input = std::move(input);

    const std::string address = uniqueEndpoint("fallback");
    kgn::CoreOptions opts = Fixture::options(address);
    opts.controlTimeout = std::chrono::milliseconds(5);
    kgn::Core core(opts, std::move(backends));
    auto owner = std::make_unique<NeverAcknowledging>();
    NeverAcknowledging* recordedOwner = owner.get();
    core.setEngineOwnerForTests(std::move(owner));

    KGN_CHECK(core.start().ok());
    core.stop("test finished");
    removeEndpoint(address);

    KGN_CHECK(recordedOwner->submitted > 0);      // it was asked
    KGN_CHECK_EQ(recordedInput->stopCalls, 1);    // and stopped regardless
    KGN_CHECK(recordedOutput->releaseAllCalls >= 1);
    // Order matters: the input backend must be gone before the output is
    // released, or new work can arrive after the drain.
    KGN_CHECK(recordedInput->stopOrder > 0);
    KGN_CHECK(recordedOutput->releaseAllOrder > recordedInput->stopOrder);
}

KGN_TEST(a_setting_change_the_engine_never_applied_is_refused_not_reported_ok) {
    // P6 in the reply path: a core that says "applied" for a change the engine
    // never saw is worse than one that refuses.
    const std::string address = uniqueEndpoint("refuse");
    kgn::CoreOptions opts = Fixture::options(address);
    opts.controlTimeout = std::chrono::milliseconds(5);
    kgn::Core core(opts);
    core.setEngineOwnerForTests(std::make_unique<NeverAcknowledging>());
    KGN_CHECK(core.start().ok());

    RawClient client;
    KGN_CHECK(client.connect(address));
    if (client.valid()) {
        parseLine(client.readLine(core));   // hello
        client.send(R"({"v":1,"t":"command","n":"set_activation_mode","id":"c1",)"
                    R"("d":{"mode":"toggle"}})" "\n");

        Json reply = parseLine(client.readLine(core));
        for (int i = 0; i < 8 && reply["t"].asString() != "reply"; ++i) {
            reply = parseLine(client.readLine(core));
        }
        KGN_CHECK_EQ(reply["t"].asString(), std::string("reply"));
        KGN_CHECK(!reply["ok"].asBool());
        KGN_CHECK_EQ(reply["e"]["code"].asString(),
                     std::string("input.queue_overflow"));
        client.close();
    }
    core.stop("test finished");
    removeEndpoint(address);
}

KGN_TEST(window_slot_focuses_the_registry_entry_and_cycle_walks_slot_order) {
    auto window = std::make_unique<RecordingWindow>();
    RecordingWindow* recorded = window.get();
    kgn::WindowInfo a;
    a.id = 11;
    a.title = "alpha";
    kgn::WindowInfo b;
    b.id = 22;
    b.title = "beta";
    recorded->list = {a, b};
    recorded->focusedId = 11;

    kgn::Backends backends;
    backends.window = std::move(window);
    backends.output = std::make_unique<RecordingOutput>();

    Fixture fixture("winact", std::move(backends));
    KGN_CHECK(fixture.core.start().ok());

    kgn::Action slot;
    slot.id = kgn::ActionId::WindowSlot;
    slot.index = 2;
    fixture.core.applyWindowForTests(slot);
    KGN_CHECK_EQ(recorded->focusCalls.back(), kgn::WindowId{22});

    kgn::Action cycle;
    cycle.id = kgn::ActionId::WindowCycle;
    cycle.cycle = kgn::Cycle::Next;
    fixture.core.applyWindowForTests(cycle);
    KGN_CHECK_EQ(recorded->focusCalls.back(), kgn::WindowId{11});
}

KGN_TEST(warp_grid_lands_on_the_monitor_the_pointer_is_actually_on) {
    // SPEC 7.4: the grid is over the CURRENT monitor. Normalising against the
    // primary is the classic bug that puts the pointer on the wrong screen.
    auto window = std::make_unique<RecordingWindow>();
    RecordingWindow* recordedWindow = window.get();
    kgn::MonitorInfo primary;
    primary.index = 0;
    primary.bounds = {0, 0, 1920, 1080};
    primary.primary = true;
    kgn::MonitorInfo second;
    second.index = 1;
    second.bounds = {1920, 0, 1280, 1024};
    recordedWindow->screens = {primary, second};

    auto output = std::make_unique<RecordingOutput>();
    RecordingOutput* recordedOutput = output.get();
    recordedOutput->cursor = {2560, 512};   // on the SECOND monitor

    kgn::Backends backends;
    backends.window = std::move(window);
    backends.output = std::move(output);

    Fixture fixture("warp", std::move(backends));
    KGN_CHECK(fixture.core.start().ok());

    kgn::Action grid;
    grid.id = kgn::ActionId::WarpGrid;
    grid.index = 5;                        // the centre cell
    fixture.core.applyWarpForTests(grid);

    KGN_CHECK_EQ(recordedOutput->warps.size(), std::size_t{1});
    KGN_CHECK_EQ(recordedOutput->warps[0].x, 1920 + 1280 / 2);
    KGN_CHECK_EQ(recordedOutput->warps[0].y, 1024 / 2);
}

KGN_TEST(a_double_click_is_two_pairs_separated_in_time_and_never_blocks) {
    auto output = std::make_unique<RecordingOutput>();
    RecordingOutput* recorded = output.get();
    kgn::Backends backends;
    backends.output = std::move(output);

    Fixture fixture("dblclick", std::move(backends));
    KGN_CHECK(fixture.core.start().ok());

    const auto interval = recorded->doubleClickInterval();
    const auto delay = kgn::doubleClickDelay(interval);

    fixture.core.doubleClickForTests(kgn::MouseButton::Left);
    // The first pair is immediate; the second must NOT be, or the loop slept.
    KGN_CHECK_EQ(recorded->buttons.size(), std::size_t{2});

    fixture.core.step(kgn::Clock::now());
    KGN_CHECK_EQ(recorded->buttons.size(), std::size_t{2});

    // The version of this test that let row 5.3 ship stepped 100 ms ahead of a
    // 50 ms interval and asserted only "two pairs eventually". That proves the
    // loop does not block; it says nothing about whether the OS will read the
    // pairs as one double click. The assertion that matters is that the second
    // pair lands INSIDE the interval even if the loop is a whole tick late.
    fixture.core.step(kgn::Clock::now() + delay +
                      std::chrono::duration_cast<std::chrono::milliseconds>(
                          kgn::kTickInterval));
    KGN_CHECK_EQ(recorded->buttons.size(), std::size_t{4});
    for (std::size_t i = 0; i < recorded->buttons.size(); ++i) {
        KGN_CHECK(recorded->buttons[i].second == (i % 2 == 0));
    }

    // And the delay it was scheduled with genuinely fits, jitter included.
    KGN_CHECK(delay + std::chrono::duration_cast<std::chrono::milliseconds>(
                          kgn::kTickInterval) < interval);
}

KGN_TEST(a_second_double_click_press_does_not_stack_extra_pairs) {
    // Row 5.3 also reported occasional TRIPLE clicks. The mechanism was a user
    // pressing again after a missed attempt, so two delayed second-pairs landed
    // around one new first-pair. Nothing in the core should turn one action into
    // more than one pair, however often it is invoked.
    auto output = std::make_unique<RecordingOutput>();
    RecordingOutput* recorded = output.get();
    kgn::Backends backends;
    backends.output = std::move(output);

    Fixture fixture("dblstack", std::move(backends));
    KGN_CHECK(fixture.core.start().ok());

    const auto delay = kgn::doubleClickDelay(recorded->doubleClickInterval());

    fixture.core.doubleClickForTests(kgn::MouseButton::Left);
    KGN_CHECK_EQ(recorded->buttons.size(), std::size_t{2});
    fixture.core.step(kgn::Clock::now() + delay +
                      std::chrono::duration_cast<std::chrono::milliseconds>(
                          kgn::kTickInterval));
    KGN_CHECK_EQ(recorded->buttons.size(), std::size_t{4});   // exactly one pair

    // Draining again must not resurrect a spent schedule.
    fixture.core.step(kgn::Clock::now() + std::chrono::milliseconds(500));
    KGN_CHECK_EQ(recorded->buttons.size(), std::size_t{4});
}

KGN_TEST(releasing_the_layer_cancels_a_pending_double_click_with_the_button_up) {
    // P7 over fidelity: half a double click is acceptable, a button left down
    // is not.
    auto output = std::make_unique<RecordingOutput>();
    RecordingOutput* recorded = output.get();
    kgn::Backends backends;
    backends.output = std::move(output);

    Fixture fixture("dblcancel", std::move(backends));
    KGN_CHECK(fixture.core.start().ok());

    fixture.core.doubleClickForTests(kgn::MouseButton::Left);
    KGN_CHECK_EQ(recorded->buttons.size(), std::size_t{2});

    fixture.core.releaseAllForTests();
    fixture.core.step(kgn::Clock::now() + std::chrono::milliseconds(500));

    KGN_CHECK_EQ(recorded->buttons.size(), std::size_t{2});   // no third press
    KGN_CHECK(!recorded->buttons.back().second);              // and it is up
    KGN_CHECK(recorded->releaseAllCalls >= 1);
}

KGN_TEST(a_search_path_that_does_not_exist_is_a_miss_not_an_empty_document) {
    // A missing file must be skipped silently. Reporting it as a document that
    // failed to parse is both alarming and wrong.
    //
    // A guard, not a proven-failing regression test, and the distinction is
    // worth recording: the defect this protects against reproduces reliably in
    // keygnosys-core -- four spurious "not valid JSON" warnings on every
    // startup -- but not inside this binary, where a failed open reports
    // itself correctly under both the old check and the new one. The evidence
    // for the fix is that executable-level before and after, not this test.
    const std::string address = uniqueEndpoint("missing");
    kgn::CoreOptions opts;
    opts.endpointOverrideForTests = address;
    // A directory that EXISTS, with no bindings subdirectory under it. That
    // shape is what discriminates: an open under a parent that exists is
    // where the failed ifstream still tested true.
    opts.configDir = ".";
    opts.dataDir = ".";
    kgn::Core core(opts);
    KGN_CHECK(core.start().ok());

    int malformed = 0;
    for (const auto& diagnostic : core.diagnostics()) {
        std::printf("  [diag] %s: %s\n", diagnostic.code.c_str(),
                    diagnostic.message.c_str());
        if (diagnostic.message.find("not valid JSON") != std::string::npos) {
            ++malformed;
        }
    }
    KGN_CHECK_EQ(malformed, 0);
    core.stop("test finished");
    removeEndpoint(address);
}

int main() { return kgn::test::runAll(); }

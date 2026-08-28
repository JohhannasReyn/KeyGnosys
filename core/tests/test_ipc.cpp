// The JSON Lines protocol server.
//
// Every one of these runs against an in-memory transport, with no socket
// anywhere. That is the point of the Transport seam: the envelope rules, the
// sequence numbers, the bounded queues, reply routing and disconnect handling
// are all protocol, and protocol should not need an operating system to be
// tested. Endpoint ownership is the part that does, and it is in
// test_endpoint.cpp.

#include <memory>
#include <string>
#include <vector>

#include "kgn/ipc.hpp"
#include "kgn_test.hpp"

using kgn::ClientId;
using kgn::Command;
using kgn::HelloInfo;
using kgn::Json;
using kgn::Reply;
using kgn::Server;

namespace {

// One client's byte streams, shared between the test and the Connection the
// server owns.
struct Channel {
    std::string toClient;     // what the server has written
    std::string fromClient;   // what the client has queued to send
    bool closed = false;
    // A client that has stopped reading: writes report "would block" forever.
    bool stalled = false;
};

class FakeConnection : public kgn::Connection {
public:
    explicit FakeConnection(std::shared_ptr<Channel> channel)
        : channel_(std::move(channel)) {}

    int read(char* buffer, std::size_t length) override {
        if (channel_->closed) return -1;
        if (channel_->fromClient.empty()) return 0;
        const std::size_t take =
            length < channel_->fromClient.size() ? length : channel_->fromClient.size();
        std::copy(channel_->fromClient.begin(),
                  channel_->fromClient.begin() + static_cast<std::ptrdiff_t>(take),
                  buffer);
        channel_->fromClient.erase(0, take);
        return static_cast<int>(take);
    }

    int write(const char* buffer, std::size_t length) override {
        if (channel_->closed) return -1;
        if (channel_->stalled) return 0;
        channel_->toClient.append(buffer, length);
        return static_cast<int>(length);
    }

    void close() override { channel_->closed = true; }

private:
    std::shared_ptr<Channel> channel_;
};

class FakeTransport : public kgn::Transport {
public:
    std::unique_ptr<kgn::Connection> accept() override {
        if (waiting_.empty()) return nullptr;
        auto channel = waiting_.front();
        waiting_.erase(waiting_.begin());
        return std::make_unique<FakeConnection>(std::move(channel));
    }
    void close() override { closed_ = true; }

    void queue(const std::shared_ptr<Channel>& channel) { waiting_.push_back(channel); }
    [[nodiscard]] bool closed() const { return closed_; }

private:
    std::vector<std::shared_ptr<Channel>> waiting_;
    bool closed_ = false;
};

// A server plus the transport handle the test needs to poke.
struct Harness {
    std::unique_ptr<Server> server;
    FakeTransport* transport = nullptr;

    std::shared_ptr<Channel> connect() {
        auto channel = std::make_shared<Channel>();
        transport->queue(channel);
        server->poll();
        return channel;
    }
};

HelloInfo testHello() {
    HelloInfo hello;
    hello.coreVersion = "0.1.0";
    hello.platform = "test";
    hello.limitations = {"no input backend on this build"};
    return hello;
}

Harness makeServer(Server::CommandHandler handler = {}) {
    auto transport = std::make_unique<FakeTransport>();
    FakeTransport* raw = transport.get();
    Harness harness;
    harness.server = std::make_unique<Server>(testHello(), std::move(transport));
    harness.transport = raw;
    if (handler) {
        harness.server->setCommandHandler(std::move(handler));
    } else {
        harness.server->setCommandHandler([](const Command& command) {
            Json data = Json::object();
            data.set("echo", Json(command.name));
            return Reply::success(std::move(data));
        });
    }
    return harness;
}

std::vector<Json> linesOf(const Channel& channel) {
    std::vector<Json> messages;
    std::size_t start = 0;
    while (true) {
        const std::size_t newline = channel.toClient.find('\n', start);
        if (newline == std::string::npos) break;
        Json message;
        if (Json::parse(channel.toClient.substr(start, newline - start), message)) {
            messages.push_back(std::move(message));
        }
        start = newline + 1;
    }
    return messages;
}

// The first message matching an event name, or a null Json.
Json eventNamed(const Channel& channel, std::string_view name) {
    for (const auto& message : linesOf(channel)) {
        if (message["t"].asString() == "event" && message["n"].asString() == name) {
            return message;
        }
    }
    return {};
}

int countEvents(const Channel& channel, std::string_view name) {
    int count = 0;
    for (const auto& message : linesOf(channel)) {
        if (message["t"].asString() == "event" && message["n"].asString() == name) ++count;
    }
    return count;
}

Json replyFor(const Channel& channel, std::string_view id) {
    for (const auto& message : linesOf(channel)) {
        if (message["t"].asString() == "reply" && message["id"].asString() == id) {
            return message;
        }
    }
    return {};
}

std::string command(std::string_view name, std::string_view id,
                    std::string_view data = "{}") {
    std::string line = R"({"v":1,"t":"command","n":")";
    line.append(name);
    line += R"(","id":")";
    line.append(id);
    line += R"(","d":)";
    line.append(data);
    line += "}\n";
    return line;
}

}  // namespace

// ---------------------------------------------------------------------------
// Hello

KGN_TEST(a_new_client_is_greeted_immediately) {
    // The launcher's readiness definition rests on this: a client that can
    // connect can also be greeted, so a connection that yields hello is proof
    // the core is up (LAUNCHING.md section 4.2).
    Harness harness = makeServer();
    auto channel = harness.connect();

    const Json hello = eventNamed(*channel, "hello");
    KGN_CHECK(hello.isObject());
    KGN_CHECK_EQ(hello["v"].asInt(), std::int64_t{1});
    KGN_CHECK_EQ(hello["t"].asString(), std::string("event"));
    KGN_CHECK_EQ(hello["seq"].asInt(), std::int64_t{1});
    KGN_CHECK_EQ(hello["d"]["protocol"].asString(), std::string("1.0"));
    KGN_CHECK_EQ(hello["d"]["core_version"].asString(), std::string("0.1.0"));
    KGN_CHECK_EQ(hello["d"]["platform"].asString(), std::string("test"));
}

KGN_TEST(hello_reports_an_absent_backend_as_absent) {
    // P6: a backend that does not exist is reported null rather than described
    // with a plausible name the overlay would believe.
    Harness harness = makeServer();
    auto channel = harness.connect();
    const Json hello = eventNamed(*channel, "hello");
    KGN_CHECK(hello["d"]["backends"]["input"].isNull());
    KGN_CHECK(hello["d"]["backends"]["output"].isNull());
    KGN_CHECK(hello["d"]["backends"]["window"].isNull());
    KGN_CHECK_EQ(hello["d"]["limitations"].size(), std::size_t{1});
}

KGN_TEST(each_client_gets_its_own_hello_and_its_own_sequence) {
    Harness harness = makeServer();
    auto first = harness.connect();
    auto second = harness.connect();
    KGN_CHECK_EQ(harness.server->clientCount(), std::size_t{2});
    KGN_CHECK_EQ(eventNamed(*first, "hello")["seq"].asInt(), std::int64_t{1});
    KGN_CHECK_EQ(eventNamed(*second, "hello")["seq"].asInt(), std::int64_t{1});
}

// ---------------------------------------------------------------------------
// Envelopes

KGN_TEST(events_carry_a_monotonic_per_connection_sequence) {
    Harness harness = makeServer();
    auto channel = harness.connect();
    for (int i = 0; i < 3; ++i) harness.server->broadcast("mode", Json::object());
    harness.server->poll();

    std::int64_t previous = 0;
    for (const auto& message : linesOf(*channel)) {
        if (message["t"].asString() != "event") continue;
        const std::int64_t seq = message["seq"].asInt();
        KGN_CHECK_EQ(seq, previous + 1);
        previous = seq;
    }
    KGN_CHECK_EQ(previous, std::int64_t{4});   // hello plus three
}

KGN_TEST(a_command_is_answered_on_its_own_correlation_id) {
    Harness harness = makeServer();
    auto channel = harness.connect();
    channel->fromClient += command("ping", "c17");
    harness.server->poll();

    const Json reply = replyFor(*channel, "c17");
    KGN_CHECK(reply.isObject());
    KGN_CHECK_EQ(reply["v"].asInt(), std::int64_t{1});
    KGN_CHECK_EQ(reply["t"].asString(), std::string("reply"));
    KGN_CHECK(reply["ok"].asBool());
    KGN_CHECK_EQ(reply["d"]["echo"].asString(), std::string("ping"));
    // Replies carry no seq: they are not part of the event stream a client
    // watches for gaps.
    KGN_CHECK(!reply.contains("seq"));
}

KGN_TEST(a_failing_command_replies_with_a_code_and_a_message) {
    Harness harness = makeServer([](const Command&) {
        return Reply::failure("test.refused", "not today");
    });
    auto channel = harness.connect();
    channel->fromClient += command("set_bindings", "c1");
    harness.server->poll();

    const Json reply = replyFor(*channel, "c1");
    KGN_CHECK(!reply["ok"].asBool(true));
    KGN_CHECK_EQ(reply["e"]["code"].asString(), std::string("test.refused"));
    KGN_CHECK_EQ(reply["e"]["message"].asString(), std::string("not today"));
    KGN_CHECK(!reply.contains("d"));
}

KGN_TEST(a_command_with_no_id_still_runs_but_is_not_answered) {
    int seen = 0;
    Harness harness = makeServer([&seen](const Command&) {
        ++seen;
        return Reply::success();
    });
    auto channel = harness.connect();
    channel->fromClient += R"({"v":1,"t":"command","n":"release_all","d":{}})" "\n";
    harness.server->poll();

    KGN_CHECK_EQ(seen, 1);
    for (const auto& message : linesOf(*channel)) {
        KGN_CHECK(message["t"].asString() != "reply");
    }
}

KGN_TEST(commands_reach_the_handler_with_their_client_and_payload) {
    ClientId seenClient = 0;
    Json seenData;
    Harness harness = makeServer([&](const Command& c) {
        seenClient = c.client;
        seenData = c.data;
        return Reply::success();
    });
    auto channel = harness.connect();
    channel->fromClient += command("set_setting", "c1", R"({"path":"behavior.grace_ms","value":40})");
    harness.server->poll();

    KGN_CHECK(seenClient != 0);
    KGN_CHECK_EQ(seenData["path"].asString(), std::string("behavior.grace_ms"));
    KGN_CHECK_EQ(seenData["value"].asInt(), std::int64_t{40});
}

// ---------------------------------------------------------------------------
// Malformed input
//
// A client can send anything. None of it may take the server down, and none of
// it may leave a client waiting on a reply that will never come.

KGN_TEST(a_line_that_is_not_json_produces_a_diagnostic_and_nothing_else) {
    Harness harness = makeServer();
    auto channel = harness.connect();
    channel->fromClient += "this is not json\n";
    harness.server->poll();

    const Json diagnostic = eventNamed(*channel, "diagnostic");
    KGN_CHECK_EQ(diagnostic["d"]["code"].asString(), std::string("ipc.bad_message"));
    KGN_CHECK_EQ(harness.server->badMessages(), std::uint64_t{1});
    KGN_CHECK_EQ(harness.server->clientCount(), std::size_t{1});
}

KGN_TEST(a_json_value_that_is_not_an_object_is_refused) {
    Harness harness = makeServer();
    auto channel = harness.connect();
    channel->fromClient += "[1,2,3]\n";
    channel->fromClient += "42\n";
    harness.server->poll();
    KGN_CHECK_EQ(harness.server->badMessages(), std::uint64_t{2});
}

KGN_TEST(a_message_that_is_not_a_command_is_refused_on_its_id) {
    Harness harness = makeServer();
    auto channel = harness.connect();
    channel->fromClient += R"({"v":1,"t":"event","n":"key","id":"c9","d":{}})" "\n";
    harness.server->poll();

    const Json reply = replyFor(*channel, "c9");
    KGN_CHECK(!reply["ok"].asBool(true));
    KGN_CHECK_EQ(reply["e"]["code"].asString(), std::string("ipc.bad_message"));
}

KGN_TEST(a_foreign_envelope_version_is_refused_by_name) {
    Harness harness = makeServer();
    auto channel = harness.connect();
    channel->fromClient += R"({"v":2,"t":"command","n":"ping","id":"c1","d":{}})" "\n";
    harness.server->poll();

    const Json reply = replyFor(*channel, "c1");
    KGN_CHECK(!reply["ok"].asBool(true));
    KGN_CHECK_EQ(reply["e"]["code"].asString(), std::string("ipc.version_mismatch"));
}

KGN_TEST(a_command_with_no_name_is_refused) {
    Harness harness = makeServer();
    auto channel = harness.connect();
    channel->fromClient += R"({"v":1,"t":"command","id":"c1","d":{}})" "\n";
    harness.server->poll();
    KGN_CHECK(!replyFor(*channel, "c1")["ok"].asBool(true));
}

KGN_TEST(a_malformed_line_does_not_disturb_the_next_one) {
    Harness harness = makeServer();
    auto channel = harness.connect();
    channel->fromClient += "{oops\n";
    channel->fromClient += command("ping", "c2");
    harness.server->poll();
    KGN_CHECK(replyFor(*channel, "c2")["ok"].asBool());
}

KGN_TEST(a_line_with_no_end_eventually_costs_the_client_its_connection) {
    // Not a message, and not going to become one. The server says so and drops
    // the client rather than growing without bound.
    Harness harness = makeServer();
    auto channel = harness.connect();
    channel->fromClient.assign(kgn::kMaxLineBytes + 16, 'x');
    harness.server->poll();
    KGN_CHECK_EQ(harness.server->clientCount(), std::size_t{0});
}

// ---------------------------------------------------------------------------
// Framing

KGN_TEST(a_command_split_across_reads_is_reassembled) {
    Harness harness = makeServer();
    auto channel = harness.connect();
    const std::string line = command("ping", "c1");
    channel->fromClient += line.substr(0, 12);
    harness.server->poll();
    KGN_CHECK(!replyFor(*channel, "c1").isObject());

    channel->fromClient += line.substr(12);
    harness.server->poll();
    KGN_CHECK(replyFor(*channel, "c1")["ok"].asBool());
}

KGN_TEST(several_commands_in_one_read_are_all_handled) {
    Harness harness = makeServer();
    auto channel = harness.connect();
    channel->fromClient += command("ping", "c1");
    channel->fromClient += command("get_state", "c2");
    channel->fromClient += command("release_all", "c3");
    harness.server->poll();
    KGN_CHECK(replyFor(*channel, "c1")["ok"].asBool());
    KGN_CHECK(replyFor(*channel, "c2")["ok"].asBool());
    KGN_CHECK(replyFor(*channel, "c3")["ok"].asBool());
}

KGN_TEST(blank_lines_and_crlf_are_tolerated) {
    // A developer poking the socket by hand should not have to think about it.
    Harness harness = makeServer();
    auto channel = harness.connect();
    channel->fromClient += "\n\n";
    channel->fromClient += R"({"v":1,"t":"command","n":"ping","id":"c1","d":{}})" "\r\n";
    harness.server->poll();
    KGN_CHECK(replyFor(*channel, "c1")["ok"].asBool());
    KGN_CHECK_EQ(harness.server->badMessages(), std::uint64_t{0});
}

// ---------------------------------------------------------------------------
// Multiple clients

KGN_TEST(events_are_broadcast_to_every_client) {
    Harness harness = makeServer();
    auto first = harness.connect();
    auto second = harness.connect();
    Json data = Json::object();
    data.set("mode", Json("cursor"));
    harness.server->broadcast("mode", data);
    harness.server->poll();

    KGN_CHECK_EQ(eventNamed(*first, "mode")["d"]["mode"].asString(), std::string("cursor"));
    KGN_CHECK_EQ(eventNamed(*second, "mode")["d"]["mode"].asString(), std::string("cursor"));
}

KGN_TEST(a_reply_goes_only_to_the_client_that_asked) {
    Harness harness = makeServer();
    auto first = harness.connect();
    auto second = harness.connect();
    first->fromClient += command("ping", "c1");
    harness.server->poll();

    KGN_CHECK(replyFor(*first, "c1")["ok"].asBool());
    KGN_CHECK(!replyFor(*second, "c1").isObject());
    for (const auto& message : linesOf(*second)) {
        KGN_CHECK(message["t"].asString() != "reply");
    }
}

KGN_TEST(one_stalled_client_does_not_stop_another_being_served) {
    Harness harness = makeServer();
    auto stalled = harness.connect();
    auto healthy = harness.connect();
    stalled->stalled = true;

    for (int i = 0; i < 5; ++i) harness.server->broadcast("mode", Json::object());
    healthy->fromClient += command("ping", "c1");
    harness.server->poll();

    KGN_CHECK_EQ(countEvents(*healthy, "mode"), 5);
    KGN_CHECK(replyFor(*healthy, "c1")["ok"].asBool());
}

// ---------------------------------------------------------------------------
// Bounded queues

KGN_TEST(a_client_that_stops_reading_is_bounded_not_unbounded) {
    Harness harness = makeServer();
    auto channel = harness.connect();
    channel->stalled = true;

    for (std::size_t i = 0; i < kgn::kClientQueueLimit * 3; ++i) {
        harness.server->broadcast("mode", Json::object());
    }
    harness.server->poll();

    // One overflow diagnostic may sit above the bound; nothing else may.
    KGN_CHECK(harness.server->queueDepth(1) <= kgn::kClientQueueLimit + 1);
    KGN_CHECK(harness.server->eventsDropped() > 0);
    KGN_CHECK_EQ(harness.server->clientCount(), std::size_t{1});
}

KGN_TEST(overflow_is_reported_once_per_episode_not_once_per_message) {
    // A client that has stopped reading would otherwise be told several
    // hundred times, in a queue it is not reading.
    Harness harness = makeServer();
    auto channel = harness.connect();
    channel->stalled = true;
    for (std::size_t i = 0; i < kgn::kClientQueueLimit * 2; ++i) {
        harness.server->broadcast("mode", Json::object());
    }
    channel->stalled = false;
    harness.server->poll();
    KGN_CHECK_EQ(countEvents(*channel, "diagnostic"), 1);
    KGN_CHECK_EQ(eventNamed(*channel, "diagnostic")["d"]["code"].asString(),
                 std::string("ipc.client_overflow"));
}

KGN_TEST(replies_are_never_dropped_however_full_the_queue) {
    Harness harness = makeServer();
    auto channel = harness.connect();
    channel->stalled = true;
    for (std::size_t i = 0; i < kgn::kClientQueueLimit * 2; ++i) {
        harness.server->broadcast("mode", Json::object());
    }
    // The queue is full of events. A command arriving now must still be
    // answered: events yield to make room, and a reply is never the victim.
    channel->fromClient += command("ping", "c99");
    harness.server->poll();

    channel->stalled = false;
    for (int i = 0; i < 600; ++i) harness.server->poll();
    KGN_CHECK(replyFor(*channel, "c99")["ok"].asBool());
}

KGN_TEST(a_client_that_floods_commands_without_reading_is_still_bounded) {
    // "One reply per command" does not make the total finite: a client can
    // send commands forever. Replies are never dropped, so the queue cannot
    // shed them -- which leaves only bounding the client, not the memory.
    Harness harness = makeServer();
    auto channel = harness.connect();
    channel->stalled = true;

    for (int i = 0; i < 4000; ++i) {
        channel->fromClient += command("ping", "c" + std::to_string(i));
    }
    for (int i = 0; i < 50; ++i) harness.server->poll();

    KGN_CHECK(harness.server->queueDepth(1) <= kgn::kClientQueueHardLimit);
}

KGN_TEST(a_client_whose_queue_is_full_stops_having_its_commands_read) {
    // Real backpressure: commands that cannot be answered are not consumed,
    // so the queue stops growing at the source rather than at the limit.
    Harness harness = makeServer();
    auto channel = harness.connect();
    channel->stalled = true;
    for (int i = 0; i < 4000; ++i) {
        channel->fromClient += command("ping", "c" + std::to_string(i));
    }
    for (int i = 0; i < 50; ++i) harness.server->poll();
    // Unread input remains: the server declined to consume what it could not
    // answer, instead of reading it all and queueing the replies.
    KGN_CHECK(!channel->fromClient.empty() || !harness.server->clientCount());
}

KGN_TEST(a_client_past_the_hard_ceiling_is_disconnected_not_grown) {
    // The backstop, reached by handing the server replies it cannot shed
    // without the read path's backpressure getting a chance to stop it.
    Harness harness = makeServer();
    auto channel = harness.connect();
    channel->stalled = true;

    // Every command in one read, so the whole batch is parsed before the queue
    // can drain: this is the case the ceiling exists for.
    std::string flood;
    for (int i = 0; i < 6000; ++i) {
        flood += command("ping", "c" + std::to_string(i));
    }
    channel->fromClient = flood;
    for (int i = 0; i < 200; ++i) harness.server->poll();

    // Either backpressure held the queue at the soft bound, or the ceiling
    // ended the connection. What must NOT happen is unbounded growth.
    KGN_CHECK(harness.server->queueDepth(1) <= kgn::kClientQueueHardLimit);
    if (harness.server->clientCount() == 0) {
        KGN_CHECK(harness.server->overloadedClients() >= std::uint64_t{1});
    }
}

KGN_TEST(a_client_that_reads_again_resumes_being_served) {
    // Backpressure must be temporary, not a one-way door.
    Harness harness = makeServer();
    auto channel = harness.connect();
    channel->stalled = true;
    for (int i = 0; i < 2000; ++i) {
        channel->fromClient += command("ping", "c" + std::to_string(i));
    }
    for (int i = 0; i < 20; ++i) harness.server->poll();
    KGN_CHECK(!channel->fromClient.empty());

    channel->stalled = false;
    for (int i = 0; i < 4000; ++i) harness.server->poll();
    KGN_CHECK(harness.server->clientCount() == std::size_t{1});
    KGN_CHECK(channel->fromClient.empty());
    KGN_CHECK(replyFor(*channel, "c1999")["ok"].asBool());
}

KGN_TEST(a_drained_queue_reports_a_later_overflow_again) {
    Harness harness = makeServer();
    auto channel = harness.connect();

    channel->stalled = true;
    for (std::size_t i = 0; i < kgn::kClientQueueLimit * 2; ++i) {
        harness.server->broadcast("mode", Json::object());
    }
    channel->stalled = false;
    for (int i = 0; i < 600; ++i) harness.server->poll();
    KGN_CHECK_EQ(countEvents(*channel, "diagnostic"), 1);

    channel->toClient.clear();
    channel->stalled = true;
    for (std::size_t i = 0; i < kgn::kClientQueueLimit * 2; ++i) {
        harness.server->broadcast("mode", Json::object());
    }
    channel->stalled = false;
    for (int i = 0; i < 600; ++i) harness.server->poll();
    KGN_CHECK_EQ(countEvents(*channel, "diagnostic"), 1);
}

// ---------------------------------------------------------------------------
// Disconnect and shutdown

KGN_TEST(a_client_that_goes_away_is_forgotten) {
    Harness harness = makeServer();
    auto channel = harness.connect();
    KGN_CHECK_EQ(harness.server->clientCount(), std::size_t{1});

    channel->closed = true;
    harness.server->poll();
    KGN_CHECK_EQ(harness.server->clientCount(), std::size_t{0});

    // And broadcasting to nobody is not an error.
    harness.server->broadcast("mode", Json::object());
    harness.server->poll();
}

KGN_TEST(one_client_leaving_does_not_disturb_the_others) {
    Harness harness = makeServer();
    auto leaving = harness.connect();
    auto staying = harness.connect();
    leaving->closed = true;
    harness.server->poll();

    KGN_CHECK_EQ(harness.server->clientCount(), std::size_t{1});
    harness.server->broadcast("mode", Json::object());
    harness.server->poll();
    KGN_CHECK_EQ(countEvents(*staying, "mode"), 1);
}

KGN_TEST(shutdown_says_why_and_then_closes_everything) {
    Harness harness = makeServer();
    auto first = harness.connect();
    auto second = harness.connect();
    harness.server->shutdown("core is exiting");

    for (const auto* channel : {first.get(), second.get()}) {
        const Json event = eventNamed(*channel, "shutdown");
        KGN_CHECK_EQ(event["d"]["reason"].asString(), std::string("core is exiting"));
        KGN_CHECK(channel->closed);
    }
    KGN_CHECK_EQ(harness.server->clientCount(), std::size_t{0});
    KGN_CHECK(harness.transport->closed());
}

KGN_TEST(a_server_that_has_shut_down_does_nothing_further) {
    Harness harness = makeServer();
    auto channel = harness.connect();
    harness.server->shutdown("done");
    const std::size_t before = channel->toClient.size();

    harness.server->broadcast("mode", Json::object());
    harness.server->poll();
    KGN_CHECK_EQ(channel->toClient.size(), before);
}

KGN_TEST(a_diagnostic_broadcast_carries_its_code_and_level) {
    Harness harness = makeServer();
    auto channel = harness.connect();
    harness.server->broadcastDiagnostic(
        {kgn::DiagLevel::Warn, "binding.unknown_action", "skipped one", "default.json"});
    harness.server->poll();

    const Json event = eventNamed(*channel, "diagnostic");
    KGN_CHECK_EQ(event["d"]["level"].asString(), std::string("warn"));
    KGN_CHECK_EQ(event["d"]["code"].asString(), std::string("binding.unknown_action"));
    KGN_CHECK_EQ(event["d"]["file"].asString(), std::string("default.json"));
}

KGN_TEST(with_no_handler_installed_a_command_is_refused_rather_than_ignored) {
    auto transport = std::make_unique<FakeTransport>();
    FakeTransport* raw = transport.get();
    Server server(testHello(), std::move(transport));
    auto channel = std::make_shared<Channel>();
    raw->queue(channel);
    server.poll();

    channel->fromClient += command("ping", "c1");
    server.poll();
    KGN_CHECK(!replyFor(*channel, "c1")["ok"].asBool(true));
}

int main() { return kgn::test::runAll(); }

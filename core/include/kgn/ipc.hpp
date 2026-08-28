// The JSON Lines IPC server.
//
// Split in two on purpose. Everything in this header is the PROTOCOL --
// envelopes, sequence numbers, sessions, bounded queues, broadcast and reply
// routing -- and it is pure: it touches no socket, starts no thread, and reads
// no clock it was not given. It talks to the outside world through the
// Transport and Connection interfaces below, which is what lets the whole
// protocol be tested against an in-memory fake with no operating system
// involved.
//
// The transports themselves, and the endpoint ownership rules that decide who
// is allowed to create one, live in kgn/endpoint.hpp and are the only part
// that knows what a socket is.
//
// The server is poll-driven rather than threaded. The core calls poll() from
// its own loop, so there is exactly one thread touching the engine, the
// dispatcher and the sessions. That is not a simplification of the
// specification's requirement that a slow client never delay a keystroke -- it
// satisfies it, because no keystroke ever waits on IPC in the first place:
// writes are non-blocking and every client's queue is bounded.
//
// See docs/SPEC.md section 5.

#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "kgn/diagnostics.hpp"
#include "kgn/json.hpp"

namespace kgn {

// Envelope version (`v`) and semantic protocol version (`hello.protocol`).
inline constexpr int kEnvelopeVersion = 1;
inline constexpr const char* kProtocolVersion = "1.0";

// Per-client outbound bound, in messages (SPEC section 5.1).
inline constexpr std::size_t kClientQueueLimit = 256;

// The longest single line the server will assemble before giving up on a
// client. A message with no newline is not a message, and a client that sends
// one without end must not be able to grow the server's memory without bound.
// Two orders of magnitude above the largest legitimate message (a `windows`
// event with nine slots).
inline constexpr std::size_t kMaxLineBytes = 1024u * 1024u;

// ---------------------------------------------------------------------------
// Transport seam

// A byte stream to one client.
class Connection {
public:
    virtual ~Connection() = default;

    // Non-blocking. Returns the number of bytes transferred, 0 for "nothing
    // right now", or -1 when the peer is gone. Never blocks, on any path.
    virtual int read(char* buffer, std::size_t length) = 0;
    virtual int write(const char* buffer, std::size_t length) = 0;
    virtual void close() = 0;
};

// Accepts clients. Owning the endpoint is a separate concern -- see
// kgn/endpoint.hpp -- because a transport that could create its own endpoint
// could also create a second one.
class Transport {
public:
    virtual ~Transport() = default;

    // Returns null when no client is waiting. Never blocks.
    virtual std::unique_ptr<Connection> accept() = 0;
    virtual void close() = 0;
};

// ---------------------------------------------------------------------------
// Protocol

// What `hello` reports. Every field is what is actually true right now: a
// backend that does not exist is reported absent rather than described
// optimistically (P6).
struct HelloInfo {
    std::string coreVersion = "0.1.0";
    std::string platform;
    // Empty means "no backend on this build". The overlay renders that
    // faithfully rather than assuming one.
    std::string inputBackend;
    std::string outputBackend;
    std::string windowBackend;
    std::vector<std::string> capabilities;
    std::vector<std::string> limitations;

    [[nodiscard]] Json toJson() const;
};

using ClientId = std::uint64_t;

struct Command {
    ClientId client = 0;
    std::string name;
    std::string id;      // correlation id; empty when the client sent none
    Json data;
};

struct Reply {
    bool ok = true;
    Json data = Json::object();
    std::string errorCode;
    std::string errorMessage;

    static Reply success(Json data = Json::object()) {
        Reply reply;
        reply.data = std::move(data);
        return reply;
    }
    static Reply failure(std::string code, std::string message) {
        Reply reply;
        reply.ok = false;
        reply.errorCode = std::move(code);
        reply.errorMessage = std::move(message);
        return reply;
    }
};

class Server {
public:
    using CommandHandler = std::function<Reply(const Command&)>;

    Server(HelloInfo hello, std::unique_ptr<Transport> transport);
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    void setCommandHandler(CommandHandler handler);

    // Accept, read, dispatch and flush, once. Never blocks.
    void poll();

    // Send an event to every connected client. Subject to each client's bound.
    void broadcast(std::string_view name, Json data);

    // Send a `diagnostic` event to every client. Convenience over broadcast,
    // used often enough to be worth naming.
    void broadcastDiagnostic(const Diagnostic& diagnostic);

    // Emit `shutdown` to everyone, flush what can be flushed, and close.
    void shutdown(std::string_view reason);

    [[nodiscard]] std::size_t clientCount() const { return sessions_.size(); }
    // Events discarded because a client stopped reading. Replies are never
    // counted here, because replies are never dropped.
    [[nodiscard]] std::uint64_t eventsDropped() const { return eventsDropped_; }
    [[nodiscard]] std::uint64_t badMessages() const { return badMessages_; }
    [[nodiscard]] const HelloInfo& hello() const { return hello_; }

    // Test seam: the sequence number the next event to this client will carry.
    [[nodiscard]] std::uint64_t nextSeq(ClientId client) const;
    [[nodiscard]] std::size_t queueDepth(ClientId client) const;

private:
    // Whether a queued message is a reply is remembered rather than
    // re-derived from its bytes. The overflow path has to skip replies, and
    // recognising them by inspecting the serialised line would couple the
    // queue to the exact spelling of an envelope.
    struct Outbound {
        std::string line;
        bool isReply = false;
    };

    struct Session {
        ClientId id = 0;
        std::unique_ptr<Connection> connection;
        std::string inbound;
        std::deque<Outbound> outbound;
        std::size_t writeOffset = 0;
        std::uint64_t seq = 0;
        // One diagnostic per overflow episode, not one per dropped message: a
        // client that has stopped reading would otherwise be told about it
        // several hundred times, in a queue it is not reading.
        bool overflowReported = false;
        bool closing = false;
    };

    void acceptNew();
    void readFrom(Session& session);
    void handleLine(Session& session, const std::string& line);
    void flush(Session& session);
    void dropClosed();

    void enqueueEvent(Session& session, std::string_view name, const Json& data);
    void enqueueReply(Session& session, const std::string& id, const Reply& reply);
    // Replies bypass the bound; events yield to make room for them. Returns
    // false when an event had to be discarded.
    bool enqueue(Session& session, std::string line, bool isReply);

    Session* find(ClientId client);
    [[nodiscard]] const Session* find(ClientId client) const;

    HelloInfo hello_;
    std::unique_ptr<Transport> transport_;
    CommandHandler handler_;
    std::vector<Session> sessions_;
    ClientId nextClientId_ = 1;
    std::uint64_t eventsDropped_ = 0;
    std::uint64_t badMessages_ = 0;
    bool closed_ = false;
};

}  // namespace kgn

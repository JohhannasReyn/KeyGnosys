#include "kgn/ipc.hpp"

#include <algorithm>
#include <utility>

namespace kgn {
namespace {

Json toJsonArray(const std::vector<std::string>& values) {
    Json array = Json::array();
    for (const auto& value : values) array.push(Json(value));
    return array;
}

}  // namespace

Json HelloInfo::toJson() const {
    Json backends = Json::object();
    // A backend that does not exist is reported as null rather than as an
    // empty string or a plausible name. The overlay draws the difference.
    backends.set("input", inputBackend.empty() ? Json() : Json(inputBackend));
    backends.set("output", outputBackend.empty() ? Json() : Json(outputBackend));
    backends.set("window", windowBackend.empty() ? Json() : Json(windowBackend));

    Json payload = Json::object();
    payload.set("core_version", Json(coreVersion));
    payload.set("protocol", Json(kProtocolVersion));
    payload.set("platform", Json(platform));
    payload.set("backends", std::move(backends));
    payload.set("capabilities", toJsonArray(capabilities));
    payload.set("limitations", toJsonArray(limitations));
    return payload;
}

// ---------------------------------------------------------------------------

Server::Server(HelloInfo hello, std::unique_ptr<Transport> transport)
    : hello_(std::move(hello)), transport_(std::move(transport)) {}

Server::~Server() = default;

void Server::setCommandHandler(CommandHandler handler) {
    handler_ = std::move(handler);
}

Server::Session* Server::find(ClientId client) {
    for (auto& session : sessions_) {
        if (session.id == client) return &session;
    }
    return nullptr;
}

const Server::Session* Server::find(ClientId client) const {
    for (const auto& session : sessions_) {
        if (session.id == client) return &session;
    }
    return nullptr;
}

std::uint64_t Server::nextSeq(ClientId client) const {
    const Session* session = find(client);
    return session ? session->seq + 1 : 0;
}

std::size_t Server::queueDepth(ClientId client) const {
    const Session* session = find(client);
    return session ? session->outbound.size() : 0;
}

// ---------------------------------------------------------------------------
// Queueing

bool Server::enqueue(Session& session, std::string line, bool isReply) {
    if (session.closing) return true;

    if (session.outbound.size() >= kClientQueueLimit) {
        // Drop the oldest EVENT to make room. Replies are never dropped, and
        // the message currently half-written cannot be withdrawn, so the
        // search skips both.
        std::size_t victim = session.outbound.size();
        for (std::size_t i = (session.writeOffset > 0 ? 1u : 0u);
             i < session.outbound.size(); ++i) {
            if (!session.outbound[i].isReply) {
                victim = i;
                break;
            }
        }
        if (victim < session.outbound.size()) {
            session.outbound.erase(session.outbound.begin() +
                                   static_cast<std::ptrdiff_t>(victim));
            ++eventsDropped_;
            if (!session.overflowReported) {
                session.overflowReported = true;
                Json data = Json::object();
                data.set("level", Json("warn"));
                data.set("code", Json("ipc.client_overflow"));
                data.set("message", Json("client is not reading; oldest events "
                                         "are being dropped"));
                Json envelope = Json::object();
                envelope.set("v", Json(kEnvelopeVersion));
                envelope.set("t", Json("event"));
                envelope.set("n", Json("diagnostic"));
                envelope.set("seq", Json(++session.seq));
                envelope.set("d", std::move(data));
                std::string diagnostic = envelope.dump();
                diagnostic.push_back('\n');
                // Straight onto the queue: going back through enqueue() would
                // re-enter the overflow path it is reporting.
                session.outbound.push_back(Outbound{std::move(diagnostic), false});
            }
        } else if (!isReply) {
            // Nothing but replies to displace, and this is only an event.
            ++eventsDropped_;
            return false;
        }
        // A reply with no event to displace exceeds the bound deliberately.
        // The bound exists so a stalled client cannot grow memory without
        // limit, and replies are finite: one per command the client sent.
    }

    session.outbound.push_back(Outbound{std::move(line), isReply});
    if (session.outbound.size() < kClientQueueLimit) session.overflowReported = false;
    return true;
}

void Server::enqueueEvent(Session& session, std::string_view name, const Json& data) {
    Json envelope = Json::object();
    envelope.set("v", Json(kEnvelopeVersion));
    envelope.set("t", Json("event"));
    envelope.set("n", Json(std::string(name)));
    envelope.set("seq", Json(++session.seq));
    envelope.set("d", data);
    std::string line = envelope.dump();
    line.push_back('\n');
    enqueue(session, std::move(line), false);
}

void Server::enqueueReply(Session& session, const std::string& id,
                          const Reply& reply) {
    Json envelope = Json::object();
    envelope.set("v", Json(kEnvelopeVersion));
    envelope.set("t", Json("reply"));
    envelope.set("id", Json(id));
    envelope.set("ok", Json(reply.ok));
    if (reply.ok) {
        envelope.set("d", reply.data);
    } else {
        Json error = Json::object();
        error.set("code", Json(reply.errorCode));
        error.set("message", Json(reply.errorMessage));
        envelope.set("e", std::move(error));
    }
    std::string line = envelope.dump();
    line.push_back('\n');
    enqueue(session, std::move(line), true);
}

// ---------------------------------------------------------------------------
// Polling

void Server::acceptNew() {
    if (closed_ || !transport_) return;
    while (auto connection = transport_->accept()) {
        Session session;
        session.id = nextClientId_++;
        session.connection = std::move(connection);
        sessions_.push_back(std::move(session));
        // `hello` is emitted immediately on connect, before anything else can
        // reach this client. A client that has connected but not been greeted
        // is not a ready core, which is precisely what the launcher contract
        // relies on (LAUNCHING.md section 4.2).
        enqueueEvent(sessions_.back(), "hello", hello_.toJson());
        flush(sessions_.back());
    }
}

void Server::handleLine(Session& session, const std::string& line) {
    Json message;
    std::string error;
    if (!Json::parse(line, message, error) || !message.isObject()) {
        ++badMessages_;
        Json data = Json::object();
        data.set("level", Json("warn"));
        data.set("code", Json("ipc.bad_message"));
        data.set("message", Json("line is not a JSON object: " + error));
        enqueueEvent(session, "diagnostic", data);
        return;
    }

    const std::string id = message["id"].asString();

    if (message["t"].asString() != "command") {
        ++badMessages_;
        // Anything with a correlation id gets a reply, so a client is never
        // left waiting on one it will not receive.
        if (!id.empty()) {
            enqueueReply(session, id,
                         Reply::failure("ipc.bad_message",
                                        "only 'command' messages are accepted"));
        }
        return;
    }

    const std::int64_t envelope = message["v"].asInt(0);
    if (envelope != kEnvelopeVersion) {
        ++badMessages_;
        if (!id.empty()) {
            enqueueReply(session, id,
                         Reply::failure("ipc.version_mismatch",
                                        "envelope version " +
                                            std::to_string(envelope) +
                                            " is not " +
                                            std::to_string(kEnvelopeVersion)));
        }
        return;
    }

    Command command;
    command.client = session.id;
    command.name = message["n"].asString();
    command.id = id;
    command.data = message["d"];

    if (command.name.empty()) {
        ++badMessages_;
        if (!id.empty()) {
            enqueueReply(session, id,
                         Reply::failure("ipc.bad_message", "command has no name"));
        }
        return;
    }

    const Reply reply =
        handler_ ? handler_(command)
                 : Reply::failure("ipc.unsupported", "no handler is installed");
    // A command with no correlation id is fire-and-forget; it still runs, but
    // there is nowhere to send the outcome.
    if (!command.id.empty()) enqueueReply(session, command.id, reply);
}

void Server::readFrom(Session& session) {
    char buffer[4096];
    for (;;) {
        const int got = session.connection->read(buffer, sizeof(buffer));
        if (got < 0) {
            session.closing = true;
            return;
        }
        if (got == 0) break;
        session.inbound.append(buffer, static_cast<std::size_t>(got));

        for (;;) {
            const std::size_t newline = session.inbound.find('\n');
            if (newline == std::string::npos) break;
            std::string line = session.inbound.substr(0, newline);
            session.inbound.erase(0, newline + 1);
            // Tolerate CRLF: a developer poking the socket by hand should not
            // have to think about it.
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            handleLine(session, line);
        }

        if (session.inbound.size() > kMaxLineBytes) {
            // Not a message, and not going to become one. Say so and drop the
            // client rather than growing without bound.
            Json data = Json::object();
            data.set("level", Json("error"));
            data.set("code", Json("ipc.bad_message"));
            data.set("message", Json("line exceeded the maximum message size"));
            enqueueEvent(session, "diagnostic", data);
            flush(session);
            session.closing = true;
            ++badMessages_;
            return;
        }
        if (static_cast<std::size_t>(got) < sizeof(buffer)) break;
    }
}

void Server::flush(Session& session) {
    while (!session.outbound.empty()) {
        const std::string& line = session.outbound.front().line;
        const std::size_t remaining = line.size() - session.writeOffset;
        const int wrote = session.connection->write(line.data() + session.writeOffset,
                                                    remaining);
        if (wrote < 0) {
            session.closing = true;
            return;
        }
        if (wrote == 0) return;   // would block; the rest waits for the next poll
        session.writeOffset += static_cast<std::size_t>(wrote);
        if (session.writeOffset >= line.size()) {
            session.outbound.pop_front();
            session.writeOffset = 0;
        }
    }
}

void Server::dropClosed() {
    for (auto& session : sessions_) {
        if (session.closing && session.connection) session.connection->close();
    }
    sessions_.erase(std::remove_if(sessions_.begin(), sessions_.end(),
                                   [](const Session& s) { return s.closing; }),
                    sessions_.end());
}

void Server::poll() {
    if (closed_) return;
    acceptNew();
    // Indexed rather than range-based: a command handler is free to call back
    // into the server, and shutdown() empties this vector.
    for (std::size_t i = 0; i < sessions_.size() && !closed_; ++i) {
        if (sessions_[i].closing) continue;
        readFrom(sessions_[i]);
    }
    for (std::size_t i = 0; i < sessions_.size() && !closed_; ++i) {
        if (sessions_[i].closing) continue;
        flush(sessions_[i]);
    }
    if (!closed_) dropClosed();
}

void Server::broadcast(std::string_view name, Json data) {
    if (closed_) return;
    for (auto& session : sessions_) {
        if (session.closing) continue;
        enqueueEvent(session, name, data);
    }
}

void Server::broadcastDiagnostic(const Diagnostic& diagnostic) {
    Json data = Json::object();
    data.set("level", Json(diagLevelName(diagnostic.level)));
    data.set("code", Json(diagnostic.code));
    data.set("message", Json(diagnostic.message));
    if (!diagnostic.file.empty()) data.set("file", Json(diagnostic.file));
    broadcast("diagnostic", std::move(data));
}

void Server::shutdown(std::string_view reason) {
    if (closed_) return;
    Json data = Json::object();
    data.set("reason", Json(std::string(reason)));
    broadcast("shutdown", std::move(data));
    for (auto& session : sessions_) {
        flush(session);
        if (session.connection) session.connection->close();
    }
    sessions_.clear();
    if (transport_) transport_->close();
    closed_ = true;
}

}  // namespace kgn

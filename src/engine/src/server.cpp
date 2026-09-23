// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/engine/server.hpp"

#include "trackknife/protocol/message.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

namespace trackknife::engine {
namespace {

[[nodiscard]] core::Error system_error(std::string message) {
    return core::Error{.code = core::ErrorCode::io,
                       .message = std::move(message),
                       .context = {{.key = "errno", .value = std::strerror(errno)}}};
}

// A line longer than this is refused rather than buffered without bound: the
// control path carries no binary, so a megabyte-long line is a confused or
// hostile sender, not a large request.
constexpr std::size_t maximum_line_bytes = 1U << 20U;

// Compares without an early exit, so how long a wrong guess takes to be
// refused says nothing about how much of it was right.
[[nodiscard]] bool same_token(const std::string_view offered, const std::string_view expected) {
    unsigned char difference = offered.size() == expected.size() ? 0U : 1U;
    for (std::size_t index = 0; index < expected.size(); ++index) {
        const auto left = index < offered.size() ? static_cast<unsigned char>(offered[index]) : 0U;
        difference = static_cast<unsigned char>(
            difference | (left ^ static_cast<unsigned char>(expected[index])));
    }
    return difference == 0U;
}

// A response as a line. JSON strings must be UTF-8, and an answer carrying
// bytes that are not -- a file name, a tag -- cannot be encoded. That once
// threw out of the engine and ended it; now the caller is told instead, and
// everyone else carries on.
[[nodiscard]] std::string encode_answer(const protocol::Response& response) {
    try {
        return protocol::encode_message(response);
    } catch (const std::exception& failure) {
        protocol::Response refused{
            .id = response.id, .result = std::nullopt, .error = std::nullopt};
        refused.error = protocol::to_protocol_error(
            core::Error{.code = core::ErrorCode::invariant,
                        .message = "the engine could not encode its answer",
                        .context = {{.key = "reason", .value = failure.what()}}});
        return protocol::encode_message(refused);
    }
}

} // namespace

// Owns one client socket. Writes are serialised because a response from this
// connection's own thread and an event from a job thread can race.
struct Server::Connection final {
    int descriptor{-1};
    std::mutex write_mutex;
    std::atomic_bool open{true};
    // ADR-0223: whether this peer may do anything, including hear events.
    // True from the start on a unix socket, earned on TCP.
    std::atomic_bool authenticated{false};

    ~Connection() {
        if (descriptor >= 0) {
            ::close(descriptor);
        }
    }

    // Returns false once the peer has gone, so the caller can retire it.
    bool write_line(const std::string& line) {
        const std::lock_guard guard{write_mutex};
        if (!open.load()) {
            return false;
        }
        auto payload = line;
        payload.push_back('\n');
        std::size_t written = 0;
        while (written < payload.size()) {
            // MSG_NOSIGNAL: a client that hangs up must not kill the engine
            // with SIGPIPE.
            const auto sent = ::send(descriptor, payload.data() + written, payload.size() - written,
                                     MSG_NOSIGNAL);
            if (sent < 0) {
                if (errno == EINTR) {
                    continue;
                }
                open.store(false);
                return false;
            }
            written += static_cast<std::size_t>(sent);
        }
        return true;
    }
};

Server::Server(const int listener, const int wakeup_read, const int wakeup_write,
               std::filesystem::path path, protocol::Dispatcher& dispatcher)
    : listener_(listener), wakeup_read_(wakeup_read), wakeup_write_(wakeup_write),
      path_(std::move(path)), dispatcher_(&dispatcher) {}

core::Result<std::unique_ptr<Server>> Server::listen(std::filesystem::path socket_path,
                                                     protocol::Dispatcher& dispatcher) {
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const auto text = socket_path.string();
    if (text.size() + 1U > sizeof(address.sun_path)) {
        return std::unexpected(core::Error{.code = core::ErrorCode::invalid_argument,
                                           .message = "socket path is too long",
                                           .context = {{.key = "path", .value = text}}});
    }
    std::memcpy(address.sun_path, text.c_str(), text.size() + 1U);

    // A socket file left by a crash is stale; one with a listener behind it is
    // another engine, and clobbering that would steal its clients.
    if (std::filesystem::exists(socket_path)) {
        const auto probe = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (probe >= 0) {
            const auto connected =
                ::connect(probe, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
            ::close(probe);
            if (connected == 0) {
                return std::unexpected(
                    core::Error{.code = core::ErrorCode::conflict,
                                .message = "another engine is listening on this socket",
                                .context = {{.key = "path", .value = text}}});
            }
        }
        std::error_code ignored;
        std::filesystem::remove(socket_path, ignored);
    }

    const auto listener = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listener < 0) {
        return std::unexpected(system_error("could not create the socket"));
    }
    if (::bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0) {
        auto error = system_error("could not bind the socket");
        ::close(listener);
        return std::unexpected(std::move(error));
    }
    return finish(listener, std::move(socket_path), dispatcher);
}

core::Result<std::unique_ptr<Server>> Server::finish(const int listener, std::filesystem::path path,
                                                     protocol::Dispatcher& dispatcher) {
    if (::listen(listener, 16) < 0) {
        auto error = system_error("could not listen on the socket");
        ::close(listener);
        return std::unexpected(std::move(error));
    }

    std::array<int, 2> wakeup{-1, -1};
    if (::pipe(wakeup.data()) < 0) {
        auto error = system_error("could not create the shutdown pipe");
        ::close(listener);
        return std::unexpected(std::move(error));
    }

    return std::unique_ptr<Server>{
        new Server{listener, wakeup[0], wakeup[1], std::move(path), dispatcher}};
}

core::Result<std::unique_ptr<Server>> Server::listen_tcp(const std::string& host,
                                                         const std::uint16_t port,
                                                         protocol::Dispatcher& dispatcher,
                                                         std::string token) {
    if (token.empty()) {
        // A TCP listener without a credential is exactly what ADR-0223 exists
        // to prevent, so it is refused here rather than trusted to callers.
        return std::unexpected(core::Error{.code = core::ErrorCode::invalid_argument,
                                           .message = "a TCP listener requires a token",
                                           .context = {}});
    }
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;
    addrinfo* found = nullptr;
    const auto service = std::to_string(port);
    if (const auto resolved =
            ::getaddrinfo(host.empty() ? nullptr : host.c_str(), service.c_str(), &hints, &found);
        resolved != 0) {
        return std::unexpected(
            core::Error{.code = core::ErrorCode::invalid_argument,
                        .message = "could not resolve the listen address",
                        .context = {{.key = "host", .value = host},
                                    {.key = "reason", .value = ::gai_strerror(resolved)}}});
    }
    int listener = -1;
    for (auto* candidate = found; candidate != nullptr; candidate = candidate->ai_next) {
        listener = ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
        if (listener < 0) {
            continue;
        }
        // A restarted engine must be able to take its port back while the
        // previous one's connections linger in TIME_WAIT.
        const int reuse = 1;
        ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        if (::bind(listener, candidate->ai_addr, candidate->ai_addrlen) == 0) {
            break;
        }
        ::close(listener);
        listener = -1;
    }
    ::freeaddrinfo(found);
    if (listener < 0) {
        return std::unexpected(system_error("could not bind the TCP listener"));
    }

    sockaddr_storage bound{};
    socklen_t length = sizeof(bound);
    std::uint16_t actual = port;
    if (::getsockname(listener, reinterpret_cast<sockaddr*>(&bound), &length) == 0) {
        if (bound.ss_family == AF_INET) {
            actual = ntohs(reinterpret_cast<const sockaddr_in*>(&bound)->sin_port);
        } else if (bound.ss_family == AF_INET6) {
            actual = ntohs(reinterpret_cast<const sockaddr_in6*>(&bound)->sin6_port);
        }
    }

    auto server = finish(listener, {}, dispatcher);
    if (!server) {
        return server;
    }
    (*server)->port_ = actual;
    (*server)->token_ = std::move(token);
    return server;
}

Server::~Server() {
    stop();
    if (listener_ >= 0) {
        ::close(listener_);
    }
    if (wakeup_read_ >= 0) {
        ::close(wakeup_read_);
    }
    if (wakeup_write_ >= 0) {
        ::close(wakeup_write_);
    }
    if (!path_.empty()) {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }
}

void Server::start() {
    if (running_.exchange(true)) {
        return;
    }
    if (listener_ >= 0) {
        acceptor_ = std::thread{[this] { accept_loop(); }};
    }
}

std::unique_ptr<Server> Server::detached(protocol::Dispatcher& dispatcher) {
    std::array<int, 2> wakeup{-1, -1};
    if (::pipe(wakeup.data()) < 0) {
        wakeup = {-1, -1};
    }
    auto server = std::unique_ptr<Server>{new Server{-1, wakeup[0], wakeup[1], {}, dispatcher}};
    server->running_.store(true);
    return server;
}

void Server::attach(const int descriptor) {
    auto connection = std::make_shared<Connection>();
    connection->descriptor = descriptor;
    connection->authenticated.store(true);
    const std::lock_guard guard{mutex_};
    connections_.push_back(connection);
    workers_.emplace_back([this, connection] { serve(connection); });
}

void Server::on_agent(AgentHandler handler) { agent_handler_ = std::move(handler); }

void Server::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    // Wake the acceptor rather than closing its descriptor underneath it.
    const char byte = 'x';
    while (::write(wakeup_write_, &byte, 1) < 0 && errno == EINTR) {
    }
    if (acceptor_.joinable()) {
        acceptor_.join();
    }
    {
        const std::lock_guard guard{mutex_};
        for (const auto& connection : connections_) {
            connection->open.store(false);
            // Half-closing unblocks a worker parked in recv.
            ::shutdown(connection->descriptor, SHUT_RDWR);
        }
    }
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();
    const std::lock_guard guard{mutex_};
    connections_.clear();
}

void Server::accept_loop() {
    while (running_.load()) {
        std::array<pollfd, 2> watched{
            pollfd{.fd = listener_, .events = POLLIN, .revents = 0},
            pollfd{.fd = wakeup_read_, .events = POLLIN, .revents = 0},
        };
        const auto ready = ::poll(watched.data(), watched.size(), -1);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }
        if ((watched[1].revents & POLLIN) != 0) {
            return;
        }
        if ((watched[0].revents & POLLIN) == 0) {
            continue;
        }
        const auto accepted = ::accept(listener_, nullptr, nullptr);
        if (accepted < 0) {
            if (errno == EINTR || errno == ECONNABORTED) {
                continue;
            }
            return;
        }
        auto connection = std::make_shared<Connection>();
        connection->descriptor = accepted;
        // A unix peer got here through the filesystem's permissions; a TCP
        // peer has proven nothing yet.
        connection->authenticated.store(token_.empty());
        {
            const std::lock_guard guard{mutex_};
            connections_.push_back(connection);
            workers_.emplace_back([this, connection] { serve(connection); });
        }
    }
}

void Server::serve(std::shared_ptr<Connection> connection) {
    // Requests are answered in order, on a worker of this connection's own,
    // so reading carries on while one is being handled. Read and handled on
    // one thread, a request that waited -- on the database, say -- stopped the
    // engine reading this connection at all, and a job.cancel sent behind it
    // arrived after the job it was meant to stop had finished.
    std::mutex queue_mutex;
    std::condition_variable queued;
    std::deque<protocol::Request> requests;
    bool reading = true;
    std::thread worker{[&] {
        while (true) {
            protocol::Request request;
            {
                std::unique_lock lock{queue_mutex};
                queued.wait(lock, [&] { return !requests.empty() || !reading; });
                if (requests.empty()) {
                    return;
                }
                request = std::move(requests.front());
                requests.pop_front();
            }
            connection->write_line(encode_answer(dispatcher_->dispatch(request)));
        }
    }};
    // Whatever ends the reading, what was already asked is still answered:
    // `nc` sends a request and closes its side, and wants the reply.
    const auto finish_reading = [&] {
        {
            const std::lock_guard lock{queue_mutex};
            reading = false;
        }
        queued.notify_all();
        if (worker.joinable()) {
            worker.join();
        }
    };

    std::string pending;
    std::array<char, 4096> buffer{};
    while (connection->open.load()) {
        const auto received = ::recv(connection->descriptor, buffer.data(), buffer.size(), 0);
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (received == 0) {
            // The peer half-closed: it will send nothing more, but it is very
            // likely still reading. That is exactly the `nc` idiom -- pipe a
            // request in, stdin reaches EOF, and the answer plus any job
            // events are still wanted. Stop reading, keep writing.
            //
            // The connection stays in the broadcast set and is reaped when a
            // write finally fails, or at shutdown. A client that half-closes
            // and never closes therefore holds one entry until the engine
            // stops, which is acceptable for a local socket whose peers are
            // the user's own programs.
            finish_reading();
            return;
        }
        pending.append(buffer.data(), static_cast<std::size_t>(received));

        std::size_t start = 0;
        while (true) {
            const auto newline = pending.find('\n', start);
            if (newline == std::string::npos) {
                break;
            }
            auto line = pending.substr(start, newline - start);
            start = newline + 1U;
            // Tolerate CRLF so a client written against a line protocol on
            // another platform still works.
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.empty()) {
                continue;
            }
            auto parsed = protocol::parse_message(line);
            if (!parsed) {
                // An unparseable line never silently disappears; without an id
                // there is nothing to answer, so it is reported as an event.
                protocol::Event rejected{.name = "protocol.rejected",
                                         .data = protocol::Json::object()};
                rejected.data["reason"] = parsed.error().message;
                connection->write_line(protocol::encode_message(rejected));
                continue;
            }
            if (const auto* request = std::get_if<protocol::Request>(&*parsed)) {
                if (!connection->authenticated.load()) {
                    if (!admit(*connection, *request)) {
                        connection->open.store(false);
                        break;
                    }
                    continue;
                }
                if (request->method == "agent.register" && agent_handler_) {
                    // ADR-0228: an output agent. What it asked before is
                    // answered first; then the connection turns round and
                    // belongs to whoever drives the agent. Nothing more is
                    // read here: the agent waits for this answer before it
                    // says anything else.
                    finish_reading();
                    protocol::Response accepted{.id = request->id,
                                                .result = protocol::Json::object(),
                                                .error = std::nullopt};
                    accepted.result->emplace("accepted", true);
                    connection->write_line(encode_answer(accepted));
                    const auto handed = ::dup(connection->descriptor);
                    connection->open.store(false);
                    {
                        const std::lock_guard guard{mutex_};
                        std::erase(connections_, connection);
                    }
                    if (handed >= 0) {
                        agent_handler_(request->params, handed);
                    }
                    return;
                }
                if (request->method == "job.cancel") {
                    // Answered at once rather than queued: stopping work is
                    // the one ask that must not wait behind the work.
                    connection->write_line(encode_answer(dispatcher_->dispatch(*request)));
                    continue;
                }
                {
                    const std::lock_guard lock{queue_mutex};
                    requests.push_back(*request);
                }
                queued.notify_one();
            }
            // Notifications are answered with nothing by definition, and a
            // client sending a response or event to an engine is confused;
            // both are dropped rather than escalated.
        }
        pending.erase(0, start);
        if (pending.size() > maximum_line_bytes) {
            protocol::Event rejected{.name = "protocol.rejected", .data = protocol::Json::object()};
            rejected.data["reason"] = "line exceeds the maximum length";
            connection->write_line(protocol::encode_message(rejected));
            break;
        }
    }
    finish_reading();
    connection->open.store(false);
    const std::lock_guard guard{mutex_};
    std::erase(connections_, connection);
}

bool Server::admit(Connection& connection, const protocol::Request& request) {
    protocol::Response response{.id = request.id, .result = std::nullopt, .error = std::nullopt};
    if (request.method != "session.authenticate") {
        response.error = protocol::to_protocol_error(
            core::Error{.code = core::ErrorCode::unauthorized,
                        .message = "authenticate with session.authenticate before anything else",
                        .context = {}});
        connection.write_line(protocol::encode_message(response));
        return true;
    }
    const auto offered = request.params.find("token");
    if (offered != request.params.end() && offered->is_string() &&
        same_token(offered->get<std::string>(), token_)) {
        connection.authenticated.store(true);
        response.result = protocol::Json{{"authenticated", true}};
        connection.write_line(protocol::encode_message(response));
        return true;
    }
    // Answered once, then closed: a client holding the token gets it right
    // first time, and a guesser pays a reconnect per attempt.
    response.error = protocol::to_protocol_error(core::Error{
        .code = core::ErrorCode::unauthorized, .message = "wrong token", .context = {}});
    connection.write_line(protocol::encode_message(response));
    ::shutdown(connection.descriptor, SHUT_RDWR);
    return false;
}

void Server::reap() {
    const std::lock_guard guard{mutex_};
    std::erase_if(connections_,
                  [](const std::shared_ptr<Connection>& held) { return !held->open.load(); });
}

void Server::broadcast(const std::string& line) {
    std::vector<std::shared_ptr<Connection>> targets;
    {
        const std::lock_guard guard{mutex_};
        targets = connections_;
    }
    bool lost = false;
    for (const auto& connection : targets) {
        // ADR-0223: an unauthenticated peer does not hear what is playing.
        if (!connection->authenticated.load()) {
            continue;
        }
        lost = !connection->write_line(line) || lost;
    }
    if (lost) {
        // A half-closed peer is only discovered by writing to it, so this is
        // where those connections are finally let go.
        reap();
    }
}

EventSink Server::sink() {
    return [this](const protocol::Event& event) {
        // An event that cannot be encoded is dropped rather than taking the
        // engine down; the next one describes the state again.
        try {
            broadcast(protocol::encode_message(event));
        } catch (const std::exception&) {
        }
    };
}

std::size_t Server::connections() {
    const std::lock_guard guard{mutex_};
    return connections_.size();
}

} // namespace trackknife::engine

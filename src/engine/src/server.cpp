// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/engine/server.hpp"

#include "trackknife/engine/token.hpp"
#include "trackknife/protocol/message.hpp"

#include <arpa/inet.h>
#include <sys/time.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
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

// How many clients may be connected at once. A person's players, agents and
// scripts are a handful; this is a bound, not a budget.
constexpr std::size_t maximum_connections = 64U;

// Events waiting for a client that does not read them. Past this the client
// is let go: holding its events without bound would grow the engine, and
// waiting for it would hold up everyone else. Answers to its own requests do
// not count -- it asked for those.
constexpr std::size_t maximum_unread_event_bytes = 4U << 20U;

// How long a TCP peer may take to authenticate. Until it has, it holds one of
// the connections above and can do nothing with it.
constexpr time_t authentication_seconds = 10;

void receive_timeout(const int descriptor, const time_t seconds) {
    const timeval timeout{.tv_sec = seconds, .tv_usec = 0};
    ::setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
}

// A response as a line. JSON strings must be UTF-8, and an answer carrying
// bytes that are not -- a file name, a tag -- cannot be encoded. That once
// threw out of the engine and ended it; now the caller is told instead, and
// everyone else carries on.
[[nodiscard]] std::string encode_answer(protocol::Response response) {
    const auto id = response.id;
    try {
        return protocol::encode_message(std::move(response));
    } catch (const std::exception& failure) {
        protocol::Response refused{.id = id, .result = std::nullopt, .error = std::nullopt};
        refused.error = protocol::to_protocol_error(
            core::Error{.code = core::ErrorCode::invariant,
                        .message = "the engine could not encode its answer",
                        .context = {{.key = "reason", .value = failure.what()}}});
        return protocol::encode_message(refused);
    }
}

} // namespace

// Owns one client socket, and the two threads that serve it: a reader, which
// is serve(), and a writer, which drains the outbox. Everything written to the
// client goes through the outbox, so no one -- a job reporting progress, the
// player announcing a track -- ever waits on this client's socket.
struct Server::Connection final {
    int descriptor{-1};
    // False once the connection is closing: nothing more is queued, and the
    // writer sends what is already queued and stops.
    std::atomic_bool open{true};
    // ADR-0223: whether this peer may do anything, including hear events.
    // True from the start on a unix socket, earned on TCP.
    std::atomic_bool authenticated{false};
    // ADR-0228: a connection this side opened and serves -- an agent's, to
    // the engine that drives it. Its peer closing is that engine gone, not a
    // client that has finished asking and still listens.
    bool ends_at_eof{false};
    // Given to an agent handler, which owns the socket now: closing this
    // side's descriptor is fine, shutting the socket down is not.
    std::atomic_bool handed_off{false};
    // The peer said it will send no more, and may or may not still read.
    std::atomic_bool half_closed{false};

    std::thread reader;
    std::thread writer;
    std::atomic_bool reader_done{false};
    std::atomic_bool writer_done{false};

    std::mutex outbox_mutex;
    std::condition_variable outbox_changed;
    // Each line with whether it is an event.
    std::deque<std::pair<std::string, bool>> outbox;
    std::size_t event_bytes{0};
    bool writing{false};

    Connection() = default;
    Connection(const Connection&) = delete;
    Connection(Connection&&) = delete;
    Connection& operator=(const Connection&) = delete;
    Connection& operator=(Connection&&) = delete;
    // The threads are joined by the server before the last reference goes.
    ~Connection() {
        if (descriptor >= 0) {
            ::close(descriptor);
        }
    }

    // Queues a line. Returns false once the connection is closing, so the
    // caller can retire it. Never blocks on the socket.
    bool write_line(std::string line, const bool event = false) {
        {
            const std::lock_guard guard{outbox_mutex};
            if (!open.load()) {
                return false;
            }
            line.push_back('\n');
            if (event) {
                event_bytes += line.size();
                if (event_bytes > maximum_unread_event_bytes) {
                    // Not reading. Shut down rather than drained: what is
                    // queued would never be read either.
                    open.store(false);
                    outbox.clear();
                    event_bytes = 0;
                    ::shutdown(descriptor, SHUT_RDWR);
                    outbox_changed.notify_all();
                    return false;
                }
            }
            outbox.emplace_back(std::move(line), event);
        }
        outbox_changed.notify_all();
        return true;
    }

    // Stops queueing; the writer sends what is queued, then ends the
    // connection so the peer sees it closed.
    void close() {
        {
            const std::lock_guard guard{outbox_mutex};
            open.store(false);
        }
        outbox_changed.notify_all();
    }

    // For shutdown: closes at once, unblocking both threads.
    void abort() {
        close();
        if (!handed_off.load()) {
            ::shutdown(descriptor, SHUT_RDWR);
        }
    }

    // ADR-0228: once everything queued has been sent, stops writing so the
    // socket can be handed to an agent handler without an event landing on
    // it behind that handler's back.
    void hand_off() {
        std::unique_lock lock{outbox_mutex};
        outbox_changed.wait(lock, [this] { return (outbox.empty() && !writing) || !open.load(); });
        handed_off.store(true);
        open.store(false);
        lock.unlock();
        outbox_changed.notify_all();
    }

    void write_loop() {
        while (true) {
            std::string line;
            bool event = false;
            {
                std::unique_lock lock{outbox_mutex};
                outbox_changed.wait(lock, [this] { return !outbox.empty() || !open.load(); });
                if (outbox.empty()) {
                    break;
                }
                line = std::move(outbox.front().first);
                event = outbox.front().second;
                outbox.pop_front();
                writing = true;
            }
            const bool sent = send_all(line);
            {
                const std::lock_guard guard{outbox_mutex};
                writing = false;
                if (event) {
                    event_bytes -= std::min(event_bytes, line.size());
                }
                if (!sent) {
                    open.store(false);
                    outbox.clear();
                    event_bytes = 0;
                }
            }
            outbox_changed.notify_all();
            if (!sent) {
                break;
            }
        }
        // Closed, and everything queued sent: the peer sees the end -- a
        // refused password, say, is answered and then hung up on.
        if (!handed_off.load()) {
            ::shutdown(descriptor, SHUT_RDWR);
        }
    }

  private:
    [[nodiscard]] bool send_all(const std::string& payload) const {
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
                                                         std::string password) {
    // ADR-0223, second amendment: a TCP listener always has a password. Open,
    // anyone who could reach the port controlled the engine and could fetch
    // any file it can read.
    if (password.empty()) {
        return std::unexpected(
            core::Error{.code = core::ErrorCode::invalid_argument,
                        .message = "a TCP listener needs a password",
                        .context = {{.key = "host", .value = host}}});
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
    (*server)->token_ = std::move(password);
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
    connection->ends_at_eof = true;
    reap();
    const std::lock_guard guard{mutex_};
    // Stopped already: an agent that finished registering just as it was
    // told to stop. Its threads would never be joined, and a thread that is
    // not joined ends the whole process when it is destroyed.
    if (!running_.load()) {
        return;
    }
    spawn(std::move(connection));
}

void Server::spawn(std::shared_ptr<Connection> connection) {
    // The server's lists hold the connection until both threads are joined
    // (reap, stop), so the last reference never goes on one of its own
    // threads, where destroying a joinable thread would end the process.
    connection->writer = std::thread{[raw = connection.get()] {
        raw->write_loop();
        raw->writer_done.store(true);
    }};
    connection->reader = std::thread{[this, raw = connection.get()] {
        serve(*raw);
        raw->reader_done.store(true);
    }};
    connections_.push_back(connection);
    all_.push_back(std::move(connection));
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
    // Taken under the lock that attach() holds, so a connection added while
    // this ran is among them rather than added behind it.
    std::vector<std::shared_ptr<Connection>> all;
    {
        const std::lock_guard guard{mutex_};
        for (const auto& connection : all_) {
            // Shutting the socket down unblocks a reader parked in recv and a
            // writer parked in send.
            connection->abort();
        }
        all.swap(all_);
        connections_.clear();
    }
    for (const auto& connection : all) {
        join(*connection);
    }
}

void Server::join(Connection& connection) {
    if (connection.reader.joinable()) {
        connection.reader.join();
    }
    if (connection.writer.joinable()) {
        connection.writer.join();
    }
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
        const auto accepted = ::accept4(listener_, nullptr, nullptr, SOCK_CLOEXEC);
        if (accepted < 0) {
            if (errno == EINTR || errno == ECONNABORTED) {
                continue;
            }
            return;
        }
        reap();
        auto connection = std::make_shared<Connection>();
        connection->descriptor = accepted;
        // A unix peer got here through the filesystem's permissions. A TCP
        // peer must give the password, which a TCP listener always has
        // (ADR-0223), and soon.
        connection->authenticated.store(token_.empty());
        if (!token_.empty()) {
            receive_timeout(accepted, authentication_seconds);
        }
        const std::lock_guard guard{mutex_};
        if (connections_.size() >= maximum_connections) {
            // Room is made first by letting go the longest half-closed
            // connection: most are clients that closed long ago, which TCP
            // cannot tell apart from one still listening without writing.
            const auto oldest = std::ranges::find_if(
                connections_, [](const auto& held) { return held->half_closed.load(); });
            if (oldest != connections_.end()) {
                (*oldest)->abort();
                connections_.erase(oldest);
            }
        }
        if (connections_.size() >= maximum_connections) {
            // Told why rather than just hung up on. The socket is new and
            // empty, so this one line does not block.
            protocol::Event full{.name = "protocol.rejected", .data = protocol::Json::object()};
            full.data["reason"] = "the engine has too many connections";
            const auto line = protocol::encode_message(full) + "\n";
            (void)::send(accepted, line.data(), line.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
            continue;
        }
        spawn(std::move(connection));
    }
}

void Server::serve(Connection& connection_ref) {
    Connection* const connection = &connection_ref;
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
            // EAGAIN is the authentication deadline running out; anything
            // else is the connection gone.
            break;
        }
        if (received == 0 && connection->ends_at_eof) {
            // The engine an agent serves has gone -- restarted, say. Kept
            // like a half-closed client, the connection stayed counted, and
            // an idle agent, writing nothing, never learned it had to
            // reconnect.
            break;
        }
        if (received == 0) {
            // The peer half-closed: it will send nothing more, but it is very
            // likely still reading. That is exactly the `nc` idiom -- pipe a
            // request in, stdin reaches EOF, and the answer plus any job
            // events are still wanted. Stop reading, keep writing.
            //
            // The connection stays in the broadcast set until the peer is
            // gone entirely. A unix peer closing is seen at once, as a
            // hangup; a TCP one only when a write to it fails -- or when its
            // place is wanted for a new connection (accept_loop).
            connection->half_closed.store(true);
            finish_reading();
            pollfd watched{.fd = connection->descriptor, .events = 0, .revents = 0};
            while (connection->open.load()) {
                const auto ready = ::poll(&watched, 1, -1);
                if (ready < 0 && errno == EINTR) {
                    continue;
                }
                if (ready < 0 || (watched.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
                    break;
                }
            }
            connection->close();
            forget(*connection);
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
                        break;
                    }
                    continue;
                }
                if (request->method == "session.authenticate") {
                    // Already admitted -- by the socket's permissions, or an
                    // earlier handshake -- so a client configured with a
                    // password is told it is in, not that the method is
                    // unknown.
                    protocol::Response admitted{.id = request->id,
                                                .result = protocol::Json{{"authenticated", true}},
                                                .error = std::nullopt};
                    connection->write_line(encode_answer(admitted));
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
                    connection->hand_off();
                    const auto handed = ::dup(connection->descriptor);
                    forget(*connection);
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
    connection->close();
    forget(*connection);
}

void Server::forget(const Connection& connection) {
    const std::lock_guard guard{mutex_};
    std::erase_if(connections_, [&connection](const std::shared_ptr<Connection>& held) {
        return held.get() == &connection;
    });
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
    auto offered = request.params.find("password");
    if (offered == request.params.end()) {
        offered = request.params.find("token");
    }
    if (offered != request.params.end() && offered->is_string() &&
        same_token(offered->get<std::string>(), token_)) {
        connection.authenticated.store(true);
        // In, so no longer on the clock: an idle client is a normal one.
        receive_timeout(connection.descriptor, 0);
        response.result = protocol::Json{{"authenticated", true}};
        connection.write_line(protocol::encode_message(response));
        return true;
    }
    // Answered once, then closed: a client holding the password gets it right
    // first time, and a guesser pays a reconnect per attempt.
    response.error = protocol::to_protocol_error(core::Error{
        .code = core::ErrorCode::unauthorized, .message = "wrong password", .context = {}});
    connection.write_line(protocol::encode_message(response));
    // Closed once that answer is sent.
    connection.close();
    return false;
}

void Server::reap() {
    std::vector<std::shared_ptr<Connection>> finished;
    {
        const std::lock_guard guard{mutex_};
        std::erase_if(connections_,
                      [](const std::shared_ptr<Connection>& held) { return !held->open.load(); });
        std::erase_if(all_, [&finished](std::shared_ptr<Connection>& held) {
            if (!held->reader_done.load() || !held->writer_done.load()) {
                return false;
            }
            finished.push_back(std::move(held));
            return true;
        });
    }
    // Both threads have said they are done, so these joins do not wait.
    for (const auto& connection : finished) {
        join(*connection);
    }
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
        lost = !connection->write_line(line, true) || lost;
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

std::size_t Server::threads() {
    reap();
    const std::lock_guard guard{mutex_};
    return 2U * all_.size();
}

} // namespace trackknife::engine

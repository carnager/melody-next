// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/engine/server.hpp"

#include "trackknife/protocol/message.hpp"

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
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

} // namespace

// Owns one client socket. Writes are serialised because a response from this
// connection's own thread and an event from a job thread can race.
struct Server::Connection final {
    int descriptor{-1};
    std::mutex write_mutex;
    std::atomic_bool open{true};

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
        new Server{listener, wakeup[0], wakeup[1], std::move(socket_path), dispatcher}};
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
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
}

void Server::start() {
    if (running_.exchange(true)) {
        return;
    }
    acceptor_ = std::thread{[this] { accept_loop(); }};
}

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
        {
            const std::lock_guard guard{mutex_};
            connections_.push_back(connection);
            workers_.emplace_back([this, connection] { serve(connection); });
        }
    }
}

void Server::serve(std::shared_ptr<Connection> connection) {
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
            break;
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
                connection->write_line(protocol::encode_message(dispatcher_->dispatch(*request)));
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
    connection->open.store(false);
    const std::lock_guard guard{mutex_};
    std::erase(connections_, connection);
}

void Server::broadcast(const std::string& line) {
    std::vector<std::shared_ptr<Connection>> targets;
    {
        const std::lock_guard guard{mutex_};
        targets = connections_;
    }
    for (const auto& connection : targets) {
        connection->write_line(line);
    }
}

EventSink Server::sink() {
    return [this](const protocol::Event& event) { broadcast(protocol::encode_message(event)); };
}

std::size_t Server::connections() {
    const std::lock_guard guard{mutex_};
    return connections_.size();
}

} // namespace trackknife::engine

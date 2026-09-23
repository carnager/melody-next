// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/result.hpp"
#include "trackknife/engine/job_registry.hpp"
#include "trackknife/protocol/dispatch.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace trackknife::engine {

// ADR-0220: protocol v1 over a stream socket.
//
// A unix socket's access control is the filesystem's, which is the right
// answer for a local engine and no answer at all for a remote one -- so a TCP
// listener requires every connection to authenticate first (ADR-0223),
// loopback included, because any local user can reach 127.0.0.1.
//
// One thread per connection. A client is a person's music player, not a web
// crawler, so the count is small and a thread apiece is simpler to reason
// about than a reactor -- and it keeps a slow client from delaying anyone
// else's line.
class Server final {
  public:
    // Binds and listens. Refuses rather than clobbering if the path exists and
    // something is listening on it; a stale socket file left by a crash is
    // removed.
    [[nodiscard]] static core::Result<std::unique_ptr<Server>>
    listen(std::filesystem::path socket_path, protocol::Dispatcher& dispatcher);

    // ADR-0223: a TCP listener whose connections must present `token` through
    // session.authenticate before anything else. Port 0 asks for an ephemeral
    // port, which `port()` then reports.
    [[nodiscard]] static core::Result<std::unique_ptr<Server>>
    listen_tcp(const std::string& host, std::uint16_t port, protocol::Dispatcher& dispatcher,
               std::string token);

    Server(const Server&) = delete;
    Server(Server&&) = delete;
    Server& operator=(const Server&) = delete;
    Server& operator=(Server&&) = delete;
    ~Server();

    // Starts accepting in the background and returns at once.
    void start();
    // Stops accepting, closes every connection and joins. Safe to call twice.
    void stop();

    // Writes an event to every connected client. This is the sink a
    // JobRegistry is given.
    [[nodiscard]] EventSink sink();

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    // The TCP port actually bound, or 0 for a unix socket.
    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
    // For tests: how many clients are connected right now.
    [[nodiscard]] std::size_t connections();

  private:
    struct Connection;

    Server(int listener, int wakeup_read, int wakeup_write, std::filesystem::path path,
           protocol::Dispatcher& dispatcher);
    [[nodiscard]] static core::Result<std::unique_ptr<Server>>
    finish(int listener, std::filesystem::path path, protocol::Dispatcher& dispatcher);

    // Answers a request from a connection that has not authenticated yet.
    // Returns false when the connection should be closed.
    bool admit(Connection& connection, const protocol::Request& request);

    void accept_loop();
    void serve(std::shared_ptr<Connection> connection);
    void broadcast(const std::string& line);
    void reap();

    int listener_{-1};
    // A self-pipe, so a blocking accept can be woken for shutdown without
    // relying on closing the descriptor out from under it.
    int wakeup_read_{-1};
    int wakeup_write_{-1};
    std::filesystem::path path_;
    std::uint16_t port_{0};
    // Empty for a unix socket, whose connections are trusted from their first
    // line. Set for TCP, whose connections are not.
    std::string token_;
    protocol::Dispatcher* dispatcher_{nullptr};

    std::atomic_bool running_{false};
    std::thread acceptor_;
    std::mutex mutex_;
    std::vector<std::shared_ptr<Connection>> connections_;
    std::vector<std::thread> workers_;
};

} // namespace trackknife::engine

// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/result.hpp"
#include "trackknife/engine/job_registry.hpp"
#include "trackknife/protocol/dispatch.hpp"

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace trackknife::engine {

// ADR-0220 Phase 2: a unix socket speaking protocol v1. TCP and
// authentication arrive in Phase 3; binding to a filesystem path means the
// permissions are the filesystem's, which is the right answer for a local
// engine and no answer at all for a remote one.
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
    // For tests: how many clients are connected right now.
    [[nodiscard]] std::size_t connections();

  private:
    struct Connection;

    Server(int listener, int wakeup_read, int wakeup_write, std::filesystem::path path,
           protocol::Dispatcher& dispatcher);

    void accept_loop();
    void serve(std::shared_ptr<Connection> connection);
    void broadcast(const std::string& line);

    int listener_{-1};
    // A self-pipe, so a blocking accept can be woken for shutdown without
    // relying on closing the descriptor out from under it.
    int wakeup_read_{-1};
    int wakeup_write_{-1};
    std::filesystem::path path_;
    protocol::Dispatcher* dispatcher_{nullptr};

    std::atomic_bool running_{false};
    std::thread acceptor_;
    std::mutex mutex_;
    std::vector<std::shared_ptr<Connection>> connections_;
    std::vector<std::thread> workers_;
};

} // namespace trackknife::engine

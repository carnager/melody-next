// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/result.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace trackknife::engine {

// ADR-0228: the files an output agent without its own copy of the music
// fetches, over plain HTTP so the agent's decoder can read, and seek in, them
// as it reads any URL.
//
//   GET /stream?path=<encoded raw path>&token=<stream token>
//
// It serves what the engine is playing and nothing else: a path is served
// only while `serves` says so -- the player holds it in its queue or among
// its asks -- so the token is a key to the music being played, not to the
// machine's disk. Ranges are honoured; every response closes its connection.
class StreamServer final {
  public:
    using Serves = std::function<bool(const std::string& raw_path)>;

    // Port 0 asks for an ephemeral port, which `port()` then reports.
    [[nodiscard]] static core::Result<std::unique_ptr<StreamServer>>
    listen(const std::string& host, std::uint16_t port, std::string token, Serves serves);

    StreamServer(const StreamServer&) = delete;
    StreamServer(StreamServer&&) = delete;
    StreamServer& operator=(const StreamServer&) = delete;
    StreamServer& operator=(StreamServer&&) = delete;
    ~StreamServer();

    void start();
    // Stops accepting, cuts every transfer short and joins. Safe to call twice.
    void stop();

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

  private:
    struct Transfer;

    StreamServer(int listener, int wakeup_read, int wakeup_write, std::uint16_t port,
                 std::string token, Serves serves);
    void accept_loop();
    void serve(const std::shared_ptr<Transfer>& transfer);
    void reap();

    int listener_;
    int wakeup_read_;
    int wakeup_write_;
    std::uint16_t port_;
    const std::string token_;
    const Serves serves_;
    std::atomic<bool> running_{false};
    std::thread acceptor_;
    std::mutex mutex_;
    std::vector<std::shared_ptr<Transfer>> transfers_;
};

} // namespace trackknife::engine

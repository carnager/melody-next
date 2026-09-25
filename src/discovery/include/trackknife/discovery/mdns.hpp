// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/result.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace trackknife::discovery {

// Engines announce themselves under this, as a printer does under _ipp._tcp.
inline constexpr const char* engine_service = "_melody._tcp";

// What an engine says about itself: its name, the port clients and agents
// connect to, and key=value facts (its id, whether it wants a password, its
// stream port).
struct Advertisement final {
    std::string instance;
    std::uint16_t port{0};
    std::map<std::string, std::string> txt;
};

// An engine found on the network: where to reach it, by the address it
// answered from.
struct Found final {
    std::string instance;
    std::string address;
    std::uint16_t port{0};
    std::map<std::string, std::string> txt;

    friend bool operator==(const Found&, const Found&) = default;
};

// Answers questions for one service instance on multicast DNS (RFC 6762),
// announces it on start, now and then, and says goodbye when it stops.
class Announcer final {
  public:
    [[nodiscard]] static core::Result<std::unique_ptr<Announcer>>
    start(Advertisement advertisement, std::string service = engine_service);
    Announcer(const Announcer&) = delete;
    Announcer& operator=(const Announcer&) = delete;
    ~Announcer();

  private:
    Announcer(int socket, Advertisement advertisement, std::string service);
    void run();
    void answer(bool goodbye);
    // To the group on every network, or -- asked for directly -- to the one
    // who asked, at its address and port.
    void answer_to(bool goodbye, std::optional<std::pair<std::uint32_t, std::uint16_t>> asker);

    int socket_;
    Advertisement advertisement_;
    std::string service_;
    std::string instance_name_;
    std::string host_name_;
    std::atomic_bool running_{true};
    std::thread worker_;
};

// Watches for instances of a service: asks at start and now and then, keeps
// what answers until its time to live runs out or it says goodbye.
class Browser final {
  public:
    // `changed` is called from the browser's thread, with everything found.
    [[nodiscard]] static core::Result<std::unique_ptr<Browser>>
    start(std::function<void(const std::vector<Found>&)> changed = {},
          std::string service = engine_service);
    Browser(const Browser&) = delete;
    Browser& operator=(const Browser&) = delete;
    ~Browser();

    [[nodiscard]] std::vector<Found> found() const;

  private:
    struct Entry final {
        Found found;
        std::chrono::steady_clock::time_point expires;
    };
    Browser(int socket, int direct, std::function<void(const std::vector<Found>&)> changed,
            std::string service);
    void run();
    void ask(bool unicast = false);
    void take(const std::uint8_t* data, std::size_t size, std::uint32_t from);
    void expire();
    void notify();

    int socket_;
    // Its own port, for direct answers: one sent to 5353 reaches only one of
    // the sockets that share it on this machine, often not this one.
    int direct_;
    // Rung when it stops, so a short-lived browser -- melody-cli's -- is not
    // kept waiting out the next quarter second.
    int wake_{-1};
    std::function<void(const std::vector<Found>&)> changed_;
    std::string service_;
    mutable std::mutex mutex_;
    std::map<std::string, Entry> entries_;
    std::atomic_bool running_{true};
    std::thread worker_;
};

} // namespace trackknife::discovery

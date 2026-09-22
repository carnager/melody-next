// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/protocol/message.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace trackknife::protocol {

// A connection to an engine.
//
// ADR-0222: responses are matched to requests by id, not by arrival order, so
// several calls may be outstanding and a slow one never blocks a fast one.
// Events arrive at any time, including between a request and its response, and
// are handed to a callback rather than queued for someone to remember to
// drain.
//
// No Qt: a Qt client wraps this and marshals the callback onto its own thread.
class Client final {
  public:
    using EventHandler = std::function<void(const Event&)>;

    [[nodiscard]] static core::Result<std::unique_ptr<Client>>
    connect(const std::filesystem::path& socket_path);

    Client(const Client&) = delete;
    Client(Client&&) = delete;
    Client& operator=(const Client&) = delete;
    Client& operator=(Client&&) = delete;
    ~Client();

    // Set before the first call; the handler runs on the reader thread.
    void on_event(EventHandler handler);

    // Sends and waits for the matching response. A timeout is an error rather
    // than a hang: an engine that has gone away must not wedge a UI thread.
    [[nodiscard]] core::Result<Json>
    call(const std::string& method, const Json& params = Json::object(),
         std::chrono::milliseconds timeout = std::chrono::seconds{10});

    // Fire and forget. No response is expected and none will come.
    [[nodiscard]] core::Result<void> notify(const std::string& method,
                                            const Json& params = Json::object());

    [[nodiscard]] bool connected() const noexcept;
    void close();

  private:
    struct Pending final {
        std::optional<Response> response;
        bool abandoned{false};
    };

    explicit Client(int descriptor);

    void read_loop();
    [[nodiscard]] core::Result<void> write_line(const std::string& line);
    void fail_everything(const std::string& reason);

    int descriptor_{-1};
    // Two distinct facts. open_ is whether the connection is usable, and the
    // reader clears it when the engine hangs up. closed_ is whether close()
    // has run, and only close() sets it. Conflating them meant a connection
    // the engine dropped made close() return without joining the reader,
    // leaving the destructor to free members out from under a live thread.
    std::atomic_bool open_{true};
    std::atomic_bool closed_{false};
    std::thread reader_;

    std::mutex write_mutex_;

    std::mutex mutex_;
    std::condition_variable arrived_;
    std::int64_t next_id_{1};
    std::map<std::int64_t, std::shared_ptr<Pending>> pending_;
    std::string failure_;

    std::mutex handler_mutex_;
    EventHandler handler_;
};

} // namespace trackknife::protocol

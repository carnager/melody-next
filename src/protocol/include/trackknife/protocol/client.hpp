// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/cancellation.hpp"
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
#include <string_view>
#include <thread>

namespace trackknife::protocol {

// Where an engine is, and what it takes to be let in.
//
// ADR-0223: a unix socket is trusted through its filesystem permissions and
// needs nothing more; a TCP address needs the engine's token. Keeping both in
// one value is what lets every caller connect without branching on which kind
// it was given.
struct Endpoint final {
    std::filesystem::path socket;
    std::string host;
    std::uint16_t port{0};
    std::string token;

    [[nodiscard]] bool tcp() const noexcept { return port != 0; }

    // A settings string: "tcp://HOST:PORT", a bare "HOST:PORT", or a socket
    // path. A path always contains a slash and an address never does, which
    // is what tells them apart. Nothing is an empty optional.
    [[nodiscard]] static std::optional<Endpoint> parse(std::string_view text, std::string token);

    // For status text: the address, never the token.
    [[nodiscard]] std::string describe() const;

    friend bool operator==(const Endpoint&, const Endpoint&) = default;
};

// Opens a connection to an endpoint, a unix socket or TCP, and nothing more:
// no handshake. For callers that speak the protocol themselves first, as an
// output agent registering does.
[[nodiscard]] core::Result<int> open_connection(const Endpoint& endpoint);

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
    // Connects to either kind, authenticating first when it is TCP. A refused
    // token is an `unauthorized` error rather than a connection that fails
    // on its first real request.
    [[nodiscard]] static core::Result<std::unique_ptr<Client>> connect(const Endpoint& endpoint);
    // ADR-0228: a client on a connection that already exists -- the one an
    // output agent opened to the engine, which the engine then drives. No
    // handshake: whatever the connection needed has happened.
    [[nodiscard]] static std::unique_ptr<Client> adopt(int descriptor);

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

    // Submits a job, reports its progress, and waits for it to finish.
    //
    // ADR-0222 makes a job submit/events/cancel rather than a long call, but a
    // caller that is already on a worker thread and already polls progress
    // counters -- which is what the library panel does -- wants exactly a
    // blocking call with a progress callback. This is that adaptation, and it
    // belongs here because jobs are a protocol concept rather than something
    // each caller should reassemble.
    //
    // on_progress runs on the reader thread. Cancelling asks the engine to
    // stop; the job's own outcome reports whether it did.
    [[nodiscard]] core::Result<Json> run_job(const std::string& job, const Json& params,
                                             const std::function<void(const Json&)>& on_progress,
                                             const core::CancellationToken& cancellation = {});

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

    struct RunningJob final {
        std::function<void(const Json&)> on_progress;
        std::optional<Json> outcome;
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
    std::map<std::string, std::shared_ptr<RunningJob>> jobs_;
    std::string failure_;

    std::mutex handler_mutex_;
    EventHandler handler_;
};

} // namespace trackknife::protocol

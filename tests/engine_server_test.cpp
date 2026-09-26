// SPDX-License-Identifier: GPL-3.0-only

// ADR-0220 Phase 2 and ADR-0222: the unix socket, talked to as a real client
// would. The "nc and a shell script remain debugging tools" requirement is
// only meaningful if a plain socket and plain lines are enough, so this test
// uses neither the codec's framing helpers nor any library beyond POSIX when
// acting as the client.

#include "trackknife/engine/job_methods.hpp"
#include "trackknife/engine/server.hpp"
#include "trackknife/engine/token.hpp"
#include "trackknife/protocol/message.hpp"

#include "trackknife/protocol/client.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {

namespace protocol = trackknife::protocol;
namespace engine = trackknife::engine;
namespace core = trackknife::core;

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::abort();
    }
}

// A deliberately dumb client: connect, write bytes, read lines.
class Client final {
  public:
    explicit Client(const std::filesystem::path& path) {
        descriptor_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        require(descriptor_ >= 0, "the client socket must open");
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        const auto text = path.string();
        std::memcpy(address.sun_path, text.c_str(), text.size() + 1U);
        const auto connected =
            ::connect(descriptor_, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
        require(connected == 0, "the client must connect");
    }
    // ADR-0223: the same dumb client over TCP, because the point is that the
    // handshake is typeable too -- one more line in nc, not a binary preamble.
    explicit Client(const std::uint16_t port) {
        descriptor_ = ::socket(AF_INET, SOCK_STREAM, 0);
        require(descriptor_ >= 0, "the client socket must open");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        const auto connected =
            ::connect(descriptor_, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
        require(connected == 0, "the client must connect over TCP");
    }
    Client(const Client&) = delete;
    Client(Client&&) = delete;
    Client& operator=(const Client&) = delete;
    Client& operator=(Client&&) = delete;
    ~Client() {
        if (descriptor_ >= 0) {
            ::close(descriptor_);
        }
    }

    void send(const std::string& text) {
        const auto written = ::send(descriptor_, text.data(), text.size(), MSG_NOSIGNAL);
        require(written == static_cast<ssize_t>(text.size()), "the client must write");
    }

    // Reads one line, giving up rather than hanging if the engine says nothing.
    [[nodiscard]] std::string line() {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (true) {
            if (const auto newline = pending_.find('\n'); newline != std::string::npos) {
                auto found = pending_.substr(0, newline);
                pending_.erase(0, newline + 1U);
                return found;
            }
            require(std::chrono::steady_clock::now() < deadline, "the engine must answer");
            std::array<char, 1024> buffer{};
            const auto received = ::recv(descriptor_, buffer.data(), buffer.size(), MSG_DONTWAIT);
            if (received > 0) {
                pending_.append(buffer.data(), static_cast<std::size_t>(received));
                continue;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
        }
    }

    // A line if one arrives within `patience`, otherwise nothing. For
    // asserting that something was *not* sent, which line() cannot do.
    [[nodiscard]] std::optional<std::string> maybe_line(const std::chrono::milliseconds patience) {
        const auto deadline = std::chrono::steady_clock::now() + patience;
        while (std::chrono::steady_clock::now() < deadline) {
            if (const auto newline = pending_.find('\n'); newline != std::string::npos) {
                auto found = pending_.substr(0, newline);
                pending_.erase(0, newline + 1U);
                return found;
            }
            std::array<char, 1024> buffer{};
            const auto received = ::recv(descriptor_, buffer.data(), buffer.size(), MSG_DONTWAIT);
            if (received > 0) {
                pending_.append(buffer.data(), static_cast<std::size_t>(received));
                continue;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
        }
        return std::nullopt;
    }

    // Says it will send no more, and keeps reading: the `nc` idiom.
    void half_close() { ::shutdown(descriptor_, SHUT_WR); }

    // Whether the engine has hung up.
    [[nodiscard]] bool closed_by_peer(const std::chrono::seconds patience = std::chrono::seconds{5}) {
        const auto deadline = std::chrono::steady_clock::now() + patience;
        while (std::chrono::steady_clock::now() < deadline) {
            std::array<char, 1024> buffer{};
            const auto received = ::recv(descriptor_, buffer.data(), buffer.size(), MSG_DONTWAIT);
            if (received == 0) {
                return true;
            }
            if (received < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
        }
        return false;
    }

  private:
    int descriptor_{-1};
    std::string pending_;
};

void the_socket_answers_plain_lines(const std::filesystem::path& path) {
    protocol::Dispatcher dispatcher;
    dispatcher.on("playback.state", [](const protocol::Json&) -> core::Result<protocol::Json> {
        return protocol::Json{{"state", "playing"}};
    });

    auto server = engine::Server::listen(path, dispatcher);
    require(server.has_value(), "the engine must bind its socket");
    (*server)->start();

    Client client{path};
    // Exactly what a person would type into nc.
    client.send("{\"id\":1,\"method\":\"playback.state\"}\n");
    const auto answer = protocol::Json::parse(client.line(), nullptr, false);
    require(!answer.is_discarded(), "the answer must be one JSON object on one line");
    require(answer.at("id") == 1, "answering the id that was asked");
    require(answer.at("result").at("state") == "playing", "with the result");

    // Two requests in one write, answered in order of completion. Both must
    // be answered: a client batching writes is not doing anything unusual.
    client.send(
        "{\"id\":2,\"method\":\"playback.state\"}\n{\"id\":3,\"method\":\"playback.state\"}\n");
    const auto first = protocol::Json::parse(client.line(), nullptr, false);
    const auto second = protocol::Json::parse(client.line(), nullptr, false);
    require(first.at("id") == 2 && second.at("id") == 3, "both requests in one write are answered");

    // A line split across writes is still one message.
    client.send("{\"id\":4,\"method\":\"pla");
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    client.send("yback.state\"}\n");
    const auto split = protocol::Json::parse(client.line(), nullptr, false);
    require(split.at("id") == 4, "a message split across writes is reassembled");

    // CRLF, because a client written on another platform should still work.
    client.send("{\"id\":5,\"method\":\"playback.state\"}\r\n");
    const auto crlf = protocol::Json::parse(client.line(), nullptr, false);
    require(crlf.at("id") == 5, "a CRLF terminated line is accepted");

    // Garbage is reported, never silently dropped.
    client.send("this is not json\n");
    const auto rejected = protocol::Json::parse(client.line(), nullptr, false);
    require(rejected.at("event") == "protocol.rejected", "an unparseable line is reported");

    // An unknown method is answered so the caller never waits forever.
    client.send("{\"id\":6,\"method\":\"nope\"}\n");
    const auto unknown = protocol::Json::parse(client.line(), nullptr, false);
    require(unknown.at("error").at("code") == "unsupported", "an unknown method answers");

    // A notification is answered with nothing at all, so the next request's
    // answer is the next line -- the way a client tells them apart.
    client.send("{\"method\":\"playback.state\"}\n{\"id\":7,\"method\":\"playback.state\"}\n");
    const auto after = protocol::Json::parse(client.line(), nullptr, false);
    require(after.at("id") == 7, "a notification produces no response");

    (*server)->stop();
}

void events_reach_every_client(const std::filesystem::path& path) {
    protocol::Dispatcher dispatcher;
    engine::JobCatalog catalogue;
    catalogue.on(
        "test.counts", [](const protocol::Json&) -> core::Result<engine::JobRegistry::Work> {
            return [](const core::CancellationToken&, const engine::JobRegistry::Reporter& report) {
                report(protocol::Json{{"step", 1}});
                return protocol::Json{{"done", true}};
            };
        });

    auto server = engine::Server::listen(path, dispatcher);
    require(server.has_value(), "the engine must bind");
    engine::JobRegistry jobs{(*server)->sink()};
    engine::register_job_methods(dispatcher, jobs, catalogue);
    (*server)->start();

    Client watcher{path};
    Client submitter{path};
    // Both are connected before the job starts, so both must see its events.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while ((*server)->connections() < 2U && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    require((*server)->connections() == 2U, "both clients must be connected");

    submitter.send("{\"id\":1,\"method\":\"job.submit\",\"params\":{\"job\":\"test.counts\"}}\n");

    // The submitter sees its response plus the events; the watcher, which
    // asked for nothing, sees only the events. That is what makes them events
    // rather than a reply.
    bool submitter_saw_finish = false;
    for (int line = 0; line < 3 && !submitter_saw_finish; ++line) {
        const auto message = protocol::Json::parse(submitter.line(), nullptr, false);
        submitter_saw_finish = message.contains("event") && message.at("event") == "job.finished";
    }
    require(submitter_saw_finish, "the submitting client sees the job finish");

    bool watcher_saw_finish = false;
    for (int line = 0; line < 3 && !watcher_saw_finish; ++line) {
        const auto message = protocol::Json::parse(watcher.line(), nullptr, false);
        require(message.contains("event"), "a client that asked nothing receives only events");
        watcher_saw_finish = message.at("event") == "job.finished";
    }
    require(watcher_saw_finish, "an unrelated client sees the job finish too");

    (*server)->stop();
}

void a_second_engine_refuses_an_occupied_socket(const std::filesystem::path& path) {
    protocol::Dispatcher dispatcher;
    {
        auto first = engine::Server::listen(path, dispatcher);
        require(first.has_value(), "the first engine binds");
        (*first)->start();

        auto second = engine::Server::listen(path, dispatcher);
        require(!second, "a second engine must not steal the socket");
        require(second.error().code == core::ErrorCode::conflict, "reporting the conflict");

        // stop() stops serving; the socket stays bound until the Server is
        // destroyed, so a stopped engine still owns its path.
        (*first)->stop();
        auto stopped = engine::Server::listen(path, dispatcher);
        require(!stopped, "a stopped engine has not released its socket");
    }

    // Destruction releases it. A stale socket file is not a permanent
    // lockout, which is what a crash would otherwise leave behind.
    auto third = engine::Server::listen(path, dispatcher);
    require(third.has_value(), "a released socket path can be bound again");
}

} // namespace

// ADR-0223: TCP authenticates, loopback included, because any local user can
// reach 127.0.0.1 -- where only the owner can reach the unix socket.
void tcp_admits_only_the_token_holder() {
    protocol::Dispatcher dispatcher;
    dispatcher.on("playback.state", [](const protocol::Json&) -> core::Result<protocol::Json> {
        return protocol::Json{{"state", "playing"}};
    });

    const std::string token{"a-token-only-the-owner-has"};
    auto server = engine::Server::listen_tcp("127.0.0.1", 0, dispatcher, token);
    require(server.has_value(), "the engine binds a TCP port");
    require((*server)->port() != 0U, "and reports which one it got");
    (*server)->start();

    Client stranger{(*server)->port()};
    stranger.send("{\"id\":1,\"method\":\"playback.state\"}\n");
    const auto denied = protocol::Json::parse(stranger.line(), nullptr, false);
    require(denied.at("id") == 1, "an unauthenticated request is answered");
    require(denied.at("error").at("code") == "unauthorized",
            "with unauthorized, not the result it asked for");

    // Nor does it hear what is playing.
    (*server)->sink()(
        protocol::Event{.name = "playback.changed", .data = protocol::Json{{"status", "playing"}}});
    require(!stranger.maybe_line(std::chrono::milliseconds{200}).has_value(),
            "an unauthenticated peer receives no events");

    stranger.send("{\"id\":2,\"method\":\"session.authenticate\",\"params\":{\"token\":\"" + token +
                  "\"}}\n");
    const auto admitted = protocol::Json::parse(stranger.line(), nullptr, false);
    require(admitted.at("result").at("authenticated") == true, "the right token admits it");
    stranger.send("{\"id\":3,\"method\":\"playback.state\"}\n");
    const auto answered = protocol::Json::parse(stranger.line(), nullptr, false);
    require(answered.at("result").at("state") == "playing", "and then it is served");
    (*server)->sink()(
        protocol::Event{.name = "playback.changed", .data = protocol::Json{{"status", "playing"}}});
    require(stranger.maybe_line(std::chrono::seconds{2}).has_value(),
            "and hears events like any other client");

    Client guesser{(*server)->port()};
    guesser.send(
        "{\"id\":1,\"method\":\"session.authenticate\",\"params\":{\"token\":\"guess\"}}\n");
    const auto wrong = protocol::Json::parse(guesser.line(), nullptr, false);
    require(wrong.at("error").at("code") == "unauthorized", "a wrong token is refused");
    require(guesser.closed_by_peer(),
            "and the connection is closed, so each guess costs a reconnect");

    // The library client, which is what the workspace uses.
    trackknife::protocol::Endpoint endpoint;
    endpoint.host = "127.0.0.1";
    endpoint.port = (*server)->port();
    endpoint.token = token;
    auto connected = trackknife::protocol::Client::connect(endpoint);
    require(connected.has_value(), "the client library authenticates as it connects");
    auto state = (*connected)->call("playback.state");
    require(state.has_value() && state->at("state") == "playing", "and is served");

    endpoint.token = "wrong";
    auto rejected = trackknife::protocol::Client::connect(endpoint);
    require(!rejected.has_value() && rejected.error().code == core::ErrorCode::unauthorized,
            "a wrong token fails the connect itself, as unauthorized");

    (*connected)->close();
    (*server)->stop();
}

// A settings string names either kind of engine. A path always has a slash
// and an address never does, so no guessing is needed.
void an_endpoint_is_read_from_settings() {
    using trackknife::protocol::Endpoint;
    const auto unix_socket = Endpoint::parse("/run/user/1000/melodyd.sock", "ignored");
    require(unix_socket && !unix_socket->tcp(), "a path is a unix socket");
    require(unix_socket->token.empty(), "which needs no token");

    const auto bare = Endpoint::parse("nas.local:6601", "t");
    require(bare && bare->tcp() && bare->host == "nas.local" && bare->port == 6601,
            "host:port is TCP");
    require(bare->token == "t", "and carries its token");
    const auto schemed = Endpoint::parse("tcp://10.0.0.2:7000", "t");
    require(schemed && schemed->host == "10.0.0.2" && schemed->port == 7000,
            "tcp:// is accepted too");
    const auto v6 = Endpoint::parse("[::1]:6601", "t");
    require(v6 && v6->host == "::1" && v6->describe() == "[::1]:6601",
            "an IPv6 literal loses and regains its brackets");

    require(!Endpoint::parse("", "t"), "empty is no engine");
    require(!Endpoint::parse("nas.local", "t"), "an address needs a port");
    require(!Endpoint::parse("nas.local:99999", "t"), "a port must fit");
    require(!Endpoint::parse("nas.local:0", "t"), "and be a real one");
    const auto secret = Endpoint::parse("nas.local:6601", "SECRET-TOKEN");
    require(secret && secret->describe() == "nas.local:6601" &&
                secret->describe().find("SECRET") == std::string::npos,
            "describing an endpoint never includes its token");
}

// ADR-0223, second amendment: there is no open TCP listener. Open, anyone
// who could reach the port controlled the engine and could fetch any file it
// can read -- and 0.0.0.0 was the default.
void tcp_needs_a_password() {
    protocol::Dispatcher dispatcher;
    auto refused = engine::Server::listen_tcp("127.0.0.1", 0, dispatcher, "");
    require(!refused && refused.error().code == core::ErrorCode::invalid_argument,
            "a TCP listener without a password is refused");

    // Nor does a client try one without: it is told what is missing rather
    // than getting unauthorized from whatever it asks first.
    auto unasked = protocol::Client::connect(
        protocol::Endpoint{.socket = {}, .host = "127.0.0.1", .port = 1, .token = {}});
    require(!unasked && unasked.error().code == core::ErrorCode::unauthorized,
            "a client without a password for a TCP engine is refused before connecting");
}

// ADR-0228: an agent serves the engine it connected to. When that engine
// goes -- restarted, say -- the connection ends, so the agent reconnects.
// Kept open like a half-closed client, it stayed counted, and an idle agent
// that wrote nothing waited forever for an engine that had long come back.
void an_attached_connection_ends_with_its_engine() {
    protocol::Dispatcher dispatcher;
    auto agent_side = engine::Server::detached(dispatcher);
    std::array<int, 2> pair{-1, -1};
    require(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair.data()) == 0,
            "a connection is made");
    agent_side->attach(pair[0]);
    require(agent_side->connections() == 1U, "the agent serves the engine's connection");
    ::close(pair[1]);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (agent_side->connections() > 0U && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    require(agent_side->connections() == 0U,
            "and when the engine closes it, it is gone, so the agent knows to reconnect");
    agent_side->stop();
}

// An agent told to stop while it was still registering attached the new
// connection to a server that had stopped. Its worker was never joined, and
// destroying it ended the process -- the engine with the agent in it, which
// went offline everywhere. Attaching to a stopped server closes the
// connection instead, and attaching while it stops is joined like the rest.
void attaching_to_a_stopping_server_does_not_end_the_process() {
    protocol::Dispatcher dispatcher;
    {
        auto stopped = engine::Server::detached(dispatcher);
        stopped->stop();
        std::array<int, 2> pair{-1, -1};
        require(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair.data()) == 0,
                "a connection is made");
        stopped->attach(pair[0]);
        require(stopped->connections() == 0U, "a stopped server takes nothing on");
        char byte = 0;
        require(::recv(pair[1], &byte, 1, 0) == 0, "and the connection is closed, not left hanging");
        ::close(pair[1]);
    }
    // And racing: attaches from another thread while the server stops.
    for (int round = 0; round < 50; ++round) {
        auto server = engine::Server::detached(dispatcher);
        std::vector<int> peers;
        std::mutex peers_mutex;
        std::thread attaching{[&] {
            for (int count = 0; count < 20; ++count) {
                std::array<int, 2> pair{-1, -1};
                if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair.data()) != 0) {
                    return;
                }
                server->attach(pair[0]);
                const std::lock_guard guard{peers_mutex};
                peers.push_back(pair[1]);
            }
        }};
        server->stop();
        attaching.join();
        server.reset();
        for (const auto peer : peers) {
            ::close(peer);
        }
    }
}

// A request that takes a while does not stop the engine reading the same
// connection, and a cancel gets through while it runs. Handled on the reading
// thread, a slow request once held everything behind it -- the cancel for a
// scan included, which then arrived after the scan had finished.
void a_slow_request_does_not_hold_a_cancel(const std::filesystem::path& path) {
    std::mutex gate_mutex;
    std::condition_variable gate_changed;
    bool released = false;
    std::atomic_bool cancelled_while_waiting{false};
    protocol::Dispatcher dispatcher;
    dispatcher.on("slow", [&](const protocol::Json&) -> core::Result<protocol::Json> {
        std::unique_lock lock{gate_mutex};
        gate_changed.wait_for(lock, std::chrono::seconds{5}, [&] { return released; });
        return protocol::Json{{"slow", "done"}};
    });
    dispatcher.on("job.cancel", [&](const protocol::Json&) -> core::Result<protocol::Json> {
        const std::lock_guard lock{gate_mutex};
        cancelled_while_waiting.store(!released);
        return protocol::Json{{"accepted", true}};
    });
    dispatcher.on("quick", [](const protocol::Json&) -> core::Result<protocol::Json> {
        return protocol::Json{{"quick", true}};
    });

    auto server = engine::Server::listen(path, dispatcher);
    require(server.has_value(), "the engine must bind its socket");
    (*server)->start();

    Client client{path};
    client.send("{\"id\":1,\"method\":\"slow\"}\n{\"id\":2,\"method\":\"quick\"}\n"
                "{\"id\":3,\"method\":\"job.cancel\",\"params\":{\"job_id\":\"x\"}}\n");
    const auto first = protocol::Json::parse(client.line(), nullptr, false);
    require(first.at("id") == 3, "the cancel is answered while the slow request still runs");
    require(cancelled_while_waiting.load(), "and it reached the engine before the work finished");
    {
        const std::lock_guard lock{gate_mutex};
        released = true;
    }
    gate_changed.notify_all();
    // Everything else keeps its order: a client that sends two requests hears
    // them answered in the order it asked.
    const auto second = protocol::Json::parse(client.line(), nullptr, false);
    const auto third = protocol::Json::parse(client.line(), nullptr, false);
    require(second.at("id") == 1 && third.at("id") == 2, "the rest are answered in order");

    (*server)->stop();
}

// An answer carrying bytes that are not UTF-8 -- a file name, a tag -- cannot
// be written as JSON. It once threw out of the engine and ended the process
// for every client; it is now refused to the one caller that asked.
void an_unencodable_answer_does_not_end_the_engine(const std::filesystem::path& path) {
    protocol::Dispatcher dispatcher;
    dispatcher.on("raw", [](const protocol::Json&) -> core::Result<protocol::Json> {
        return protocol::Json{{"name", std::string{"raw-\xff.flac"}}};
    });
    dispatcher.on("quick", [](const protocol::Json&) -> core::Result<protocol::Json> {
        return protocol::Json{{"quick", true}};
    });
    auto server = engine::Server::listen(path, dispatcher);
    require(server.has_value(), "the engine must bind its socket");
    (*server)->start();

    Client client{path};
    client.send("{\"id\":1,\"method\":\"raw\"}\n");
    const auto refused = protocol::Json::parse(client.line(), nullptr, false);
    require(refused.at("id") == 1, "the caller is answered");
    require(refused.contains("error"), "with an error rather than a crash");
    client.send("{\"id\":2,\"method\":\"quick\"}\n");
    const auto after = protocol::Json::parse(client.line(), nullptr, false);
    require(after.at("id") == 2 && after.contains("result"), "and the engine keeps answering");

    (*server)->stop();
}


// Every connection had a thread of its own that was only joined when the
// engine stopped, so each client that came and went -- melody-cli from a key
// binding, a reconnecting agent -- left one behind for good.
void closed_clients_give_back_their_threads(const std::filesystem::path& path) {
    protocol::Dispatcher dispatcher;
    dispatcher.on("playback.state", [](const protocol::Json&) -> core::Result<protocol::Json> {
        return protocol::Json{{"state", "playing"}};
    });
    auto server = engine::Server::listen(path, dispatcher);
    require(server.has_value(), "the server listens");
    (*server)->start();
    for (int round = 0; round < 100; ++round) {
        Client client{path};
        client.send("{\"id\":1,\"method\":\"playback.state\"}\n");
        require(client.line().find("playing") != std::string::npos, "each client is answered");
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (((*server)->threads() > 0U || (*server)->connections() > 0U) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    require((*server)->connections() == 0U, "a client that closed is let go");
    require((*server)->threads() == 0U, "and its threads are joined, not kept until shutdown");
    (*server)->stop();
}

// Events were written to each client in turn, on the thread that raised them,
// so one client that stopped reading held up every other client's events --
// and, through the job registry, job.cancel.
void a_client_that_does_not_read_is_let_go(const std::filesystem::path& path) {
    protocol::Dispatcher dispatcher;
    auto server = engine::Server::listen(path, dispatcher);
    require(server.has_value(), "the server listens");
    (*server)->start();

    Client stuck{path};
    Client reading{path};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while ((*server)->connections() < 2U && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    std::atomic_bool heard_last{false};
    std::thread listener{[&] {
        while (!heard_last.load()) {
            const auto line = reading.maybe_line(std::chrono::seconds{10});
            require(line.has_value(), "a client that reads keeps hearing events");
            heard_last.store(line->find("\"last\"") != std::string::npos);
        }
    }};

    const std::string filler(64U * 1024U, 'x');
    const auto sink = (*server)->sink();
    for (int index = 0; index < 160; ++index) {
        const auto started = std::chrono::steady_clock::now();
        sink(protocol::Event{.name = "test.filler", .data = protocol::Json{{"filler", filler}}});
        require(std::chrono::steady_clock::now() - started < std::chrono::milliseconds{250},
                "raising an event never waits for a client");
        // At a pace a reading client keeps up with; the stuck one falls
        // further behind every time.
        if (index % 8 == 7) {
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
    }
    sink(protocol::Event{.name = "test.last", .data = protocol::Json{{"last", true}}});
    listener.join();
    require(heard_last.load(), "the reading client heard everything");
    const auto let_go = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while ((*server)->connections() > 1U && std::chrono::steady_clock::now() < let_go) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    require((*server)->connections() == 1U, "and the one that never read was let go");
    require(stuck.closed_by_peer(), "hung up on, not left waiting");
    (*server)->stop();
}

// Connections are bounded. One more is told why it is refused -- unless a
// half-closed connection can make room, which over TCP may well be a client
// that closed long ago.
void connections_are_bounded(const std::filesystem::path& path) {
    protocol::Dispatcher dispatcher;
    dispatcher.on("playback.state", [](const protocol::Json&) -> core::Result<protocol::Json> {
        return protocol::Json{{"state", "playing"}};
    });
    auto server = engine::Server::listen(path, dispatcher);
    require(server.has_value(), "the server listens");
    (*server)->start();
    std::vector<std::unique_ptr<Client>> clients;
    for (int index = 0; index < 64; ++index) {
        clients.push_back(std::make_unique<Client>(path));
        clients.back()->send("{\"id\":1,\"method\":\"playback.state\"}\n");
        require(clients.back()->line().find("playing") != std::string::npos,
                "every client up to the bound is served");
    }
    Client refused{path};
    const auto why = refused.line();
    require(why.find("too many connections") != std::string::npos,
            "one more is told why it is refused");
    require(refused.closed_by_peer(), "and hung up on");

    clients.front()->half_close();
    // A unix peer that half-closed is still listening, so it is kept -- until
    // room is wanted.
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    Client admitted{path};
    admitted.send("{\"id\":1,\"method\":\"playback.state\"}\n");
    require(admitted.line().find("playing") != std::string::npos,
            "a half-closed connection makes room for a new one");
    require(clients.front()->closed_by_peer(), "and is the one let go");
    (*server)->stop();
}

// ADR-0223: a TCP peer holds a connection from the moment it connects, so it
// has a few seconds to authenticate. Once in, an idle client is a normal one.
void a_stranger_has_seconds_to_authenticate() {
    protocol::Dispatcher dispatcher;
    dispatcher.on("playback.state", [](const protocol::Json&) -> core::Result<protocol::Json> {
        return protocol::Json{{"state", "playing"}};
    });
    auto server = engine::Server::listen_tcp("127.0.0.1", 0, dispatcher, "the-password");
    require(server.has_value(), "the engine binds a TCP port");
    (*server)->start();
    Client stranger{(*server)->port()};
    Client owner{(*server)->port()};
    owner.send("{\"id\":1,\"method\":\"session.authenticate\",\"params\":{\"password\":"
               "\"the-password\"}}\n");
    require(owner.line().find("\"authenticated\":true") != std::string::npos, "the owner is in");
    require(stranger.closed_by_peer(std::chrono::seconds{15}),
            "a peer that never authenticates is hung up on");
    owner.send("{\"id\":2,\"method\":\"playback.state\"}\n");
    require(owner.line().find("playing") != std::string::npos,
            "while an authenticated client that sat idle as long is still served");
    (*server)->stop();
}

// A stranger that asks, gets its refusal and leaves -- a probe without the
// password -- sent EOF, and a peer that has sent EOF was waited on until a
// write to it failed. A stranger is written nothing, so it was held for
// good: dozens of dead sockets on an engine others probe. It has nothing
// more to hear once answered.
void a_stranger_that_leaves_is_let_go() {
    protocol::Dispatcher dispatcher;
    auto server = engine::Server::listen_tcp("127.0.0.1", 0, dispatcher, "the-password");
    require(server.has_value(), "the engine binds a TCP port");
    (*server)->start();
    Client stranger{(*server)->port()};
    stranger.send("{\"id\":1,\"method\":\"playback.state\"}\n");
    stranger.half_close();
    require(stranger.line().find("error") != std::string::npos, "the stranger is still answered");
    require(stranger.closed_by_peer(std::chrono::seconds{3}),
            "and then hung up on, well before its time to authenticate is up");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while ((*server)->connections() > 0U && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    require((*server)->connections() == 0U, "and its place given back");
    (*server)->stop();
}

int main() {
    const auto directory = std::filesystem::temp_directory_path() /
                           ("trackknife-server-" + core::StableId::random().to_string());
    std::filesystem::create_directory(directory);
    the_socket_answers_plain_lines(directory / "a.sock");
    events_reach_every_client(directory / "b.sock");
    a_second_engine_refuses_an_occupied_socket(directory / "c.sock");
    tcp_admits_only_the_token_holder();
    an_endpoint_is_read_from_settings();
    tcp_needs_a_password();
    an_attached_connection_ends_with_its_engine();
    attaching_to_a_stopping_server_does_not_end_the_process();
    a_slow_request_does_not_hold_a_cancel(directory / "d.sock");
    an_unencodable_answer_does_not_end_the_engine(directory / "e.sock");
    closed_clients_give_back_their_threads(directory / "f.sock");
    a_client_that_does_not_read_is_let_go(directory / "g.sock");
    connections_are_bounded(directory / "h.sock");
    a_stranger_has_seconds_to_authenticate();
    a_stranger_that_leaves_is_let_go();
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    std::cout << "engine server: 15 scenarios\n";
    return EXIT_SUCCESS;
}

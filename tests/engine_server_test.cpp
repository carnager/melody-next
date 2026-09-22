// SPDX-License-Identifier: GPL-3.0-only

// ADR-0220 Phase 2 and ADR-0222: the unix socket, talked to as a real client
// would. The "nc and a shell script remain debugging tools" requirement is
// only meaningful if a plain socket and plain lines are enough, so this test
// uses neither the codec's framing helpers nor any library beyond POSIX when
// acting as the client.

#include "trackknife/engine/job_methods.hpp"
#include "trackknife/engine/server.hpp"
#include "trackknife/protocol/message.hpp"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

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

int main() {
    const auto directory = std::filesystem::temp_directory_path() /
                           ("trackknife-server-" + core::StableId::random().to_string());
    std::filesystem::create_directory(directory);
    the_socket_answers_plain_lines(directory / "a.sock");
    events_reach_every_client(directory / "b.sock");
    a_second_engine_refuses_an_occupied_socket(directory / "c.sock");
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    std::cout << "engine server: 3 scenarios\n";
    return EXIT_SUCCESS;
}

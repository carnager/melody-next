// SPDX-License-Identifier: GPL-3.0-only

// ADR-0222: the client and the server against each other. Up to now each half
// has been tested against a hand-written counterpart; this is the first time
// both real implementations meet, which is where a disagreement about the
// contract would actually show.

#include "trackknife/engine/server.hpp"
#include "trackknife/protocol/client.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
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

void calls_and_errors_survive_the_round_trip(const std::filesystem::path& path) {
    protocol::Dispatcher dispatcher;
    dispatcher.on("echo", [](const protocol::Json& params) -> core::Result<protocol::Json> {
        return params;
    });
    dispatcher.on("fails", [](const protocol::Json&) -> core::Result<protocol::Json> {
        return std::unexpected(core::Error{.code = core::ErrorCode::conflict,
                                           .message = "a stale revision",
                                           .context = {{.key = "revision", .value = "7"}}});
    });
    // Deliberately slow, to prove a later call can overtake it.
    dispatcher.on("slow", [](const protocol::Json&) -> core::Result<protocol::Json> {
        // Long enough that the margins below survive a loaded machine:
        // the suite runs these in parallel with everything else.
        std::this_thread::sleep_for(std::chrono::milliseconds{600});
        return protocol::Json{{"slow", true}};
    });

    auto server = engine::Server::listen(path, dispatcher);
    require(server.has_value(), "the engine binds");
    (*server)->start();

    auto client = protocol::Client::connect(path);
    require(client.has_value(), "the client connects");

    const auto echoed = (*client)->call("echo", protocol::Json{{"value", 41}});
    require(echoed.has_value(), "an ordinary call succeeds");
    require(echoed->at("value") == 41, "and carries its params back");

    // The error's code and context cross intact, so a client can act on the
    // code rather than parsing prose.
    const auto failed = (*client)->call("fails");
    require(!failed, "a failing call reports failure");
    require(failed.error().code == core::ErrorCode::conflict, "with the code the engine used");
    require(failed.error().context.size() == 1U, "and its context");
    require(failed.error().context[0].key == "revision", "naming what was stale");

    const auto unknown = (*client)->call("nope");
    require(!unknown, "an unknown method fails");
    require(unknown.error().code == core::ErrorCode::unsupported, "as unsupported");

    // One connection serves its requests in order, because the engine
    // dispatches on the connection's own thread. A slow handler therefore
    // delays the next request on that connection -- which is why ADR-0220
    // makes long work a job rather than a slow handler, and why this is a
    // constraint on handlers rather than a flaw in the framing.
    // Observed by how long the second call waits rather than by a flag: both
    // responses arrive back to back, so which thread sets a flag first is a
    // race even when the response order is not.
    std::thread slow{
        [&] { require((*client)->call("slow").has_value(), "the slow call answers"); }};
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    const auto behind_started = std::chrono::steady_clock::now();
    const auto fast = (*client)->call("echo", protocol::Json{{"value", 1}});
    const auto behind_waited = std::chrono::steady_clock::now() - behind_started;
    require(fast.has_value(), "a call behind a slow one still answers");
    require(behind_waited > std::chrono::milliseconds{300},
            "but waits for the slow one ahead of it on the same connection");
    slow.join();

    // A client that genuinely needs concurrency opens a second connection,
    // which is served by its own thread.
    auto second = protocol::Client::connect(path);
    require(second.has_value(), "a second connection is allowed");
    std::thread blocker{[&] { static_cast<void>((*client)->call("slow")); }};
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    const auto concurrent_started = std::chrono::steady_clock::now();
    const auto concurrent = (*second)->call("echo", protocol::Json{{"value", 2}});
    const auto concurrent_waited = std::chrono::steady_clock::now() - concurrent_started;
    require(concurrent.has_value(), "the second connection answers");
    require(concurrent_waited < std::chrono::milliseconds{250},
            "without waiting for the first connection's slow call");
    blocker.join();

    (*server)->stop();
}

void events_reach_the_client(const std::filesystem::path& path) {
    protocol::Dispatcher dispatcher;
    auto server = engine::Server::listen(path, dispatcher);
    require(server.has_value(), "the engine binds");
    (*server)->start();

    auto client = protocol::Client::connect(path);
    require(client.has_value(), "the client connects");

    std::mutex mutex;
    std::vector<protocol::Event> seen;
    (*client)->on_event([&](const protocol::Event& event) {
        const std::lock_guard guard{mutex};
        seen.push_back(event);
    });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while ((*server)->connections() == 0U && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    const auto sink = (*server)->sink();
    sink(
        protocol::Event{.name = "playback.changed", .data = protocol::Json{{"status", "playing"}}});

    bool arrived = false;
    while (!arrived && std::chrono::steady_clock::now() < deadline) {
        {
            const std::lock_guard guard{mutex};
            arrived = !seen.empty();
        }
        if (!arrived) {
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
        }
    }
    require(arrived, "an unsolicited event reaches the client");
    const std::lock_guard guard{mutex};
    require(seen.front().name == "playback.changed", "with its name");
    require(seen.front().data.at("status") == "playing", "and its data");

    (*server)->stop();
}

void a_vanished_engine_fails_rather_than_hangs(const std::filesystem::path& path) {
    protocol::Dispatcher dispatcher;
    dispatcher.on("never", [](const protocol::Json&) -> core::Result<protocol::Json> {
        std::this_thread::sleep_for(std::chrono::seconds{30});
        return protocol::Json{};
    });

    auto server = engine::Server::listen(path, dispatcher);
    require(server.has_value(), "the engine binds");
    (*server)->start();

    auto client = protocol::Client::connect(path);
    require(client.has_value(), "the client connects");

    // A call that outlives the engine's patience is an error, never a wedged
    // thread: a UI calling this must stay answerable.
    const auto started = std::chrono::steady_clock::now();
    const auto timed_out =
        (*client)->call("never", protocol::Json::object(), std::chrono::milliseconds{100});
    const auto waited = std::chrono::steady_clock::now() - started;
    require(!timed_out, "a call that is not answered fails");
    require(timed_out.error().code == core::ErrorCode::io, "as a transport failure");
    require(waited < std::chrono::seconds{5}, "and returns promptly rather than hanging");

    // An engine that goes away mid-call is reported too, rather than leaving
    // the caller waiting for a socket that will never speak again.
    (*server)->stop();
    const auto after =
        (*client)->call("never", protocol::Json::object(), std::chrono::milliseconds{500});
    require(!after, "calling a stopped engine fails");
}

} // namespace

int main() {
    const auto directory = std::filesystem::temp_directory_path() /
                           ("trackknife-roundtrip-" + core::StableId::random().to_string());
    std::filesystem::create_directory(directory);
    calls_and_errors_survive_the_round_trip(directory / "a.sock");
    events_reach_the_client(directory / "b.sock");
    a_vanished_engine_fails_rather_than_hangs(directory / "c.sock");
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    std::cout << "protocol round trip: 3 scenarios\n";
    return EXIT_SUCCESS;
}

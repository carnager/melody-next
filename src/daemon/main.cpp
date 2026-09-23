// SPDX-License-Identifier: GPL-3.0-only

// ADR-0220: the engine as a process, melodyd.
//
// It was built as tkengine while the Go daemon of that name was meant to keep
// running beside it. No install of the Go melodyd exists to be replaced, so
// the engine took the name early (ADR-0226).

#include "trackknife/engine/catalogue_methods.hpp"
#include "trackknife/engine/job_methods.hpp"
#include "trackknife/engine/playback_methods.hpp"
#include "trackknife/engine/playback_store.hpp"
#include "trackknife/engine/recorder.hpp"
#include "trackknife/engine/server.hpp"
#include "trackknife/engine/token.hpp"
#include "trackknife/engine/workspace.hpp"
#include "trackknife/protocol/client.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace {

std::atomic_bool stop_requested{false};

void request_stop(int) { stop_requested.store(true); }

// Where Trackknife has always kept its data (Qt's AppDataLocation for the
// "trackknife" organisation and application), so the engine a workspace
// starts adopts the library, ratings and history already there rather than
// beginning empty beside them.
[[nodiscard]] std::filesystem::path default_state_directory() {
    if (const auto* explicit_home = std::getenv("TRACKKNIFE_STATE_DIR")) {
        return explicit_home;
    }
    if (const auto* data_home = std::getenv("XDG_DATA_HOME")) {
        return std::filesystem::path{data_home} / "trackknife" / "trackknife";
    }
    if (const auto* home = std::getenv("HOME")) {
        return std::filesystem::path{home} / ".local" / "share" / "trackknife" / "trackknife";
    }
    return std::filesystem::current_path();
}

// One database: catalogue, ratings, history, lists and the operation journals
// share a schema and a file. The name is the one Trackknife gave it.
constexpr std::string_view database_filename{"lists.sqlite"};

// Held for the engine's lifetime. Two engines on one database would both
// play, both scan and both answer for the same ratings; and two started at
// once for one socket can each find it stale and take it from the other. The
// kernel drops a flock when the process dies, so a crash leaves nothing to
// clean up.
[[nodiscard]] bool hold_lock(const std::filesystem::path& path) {
    const auto descriptor = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (descriptor < 0) {
        return false;
    }
    if (::flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
        ::close(descriptor);
        return false;
    }
    return true; // Deliberately never closed.
}

[[nodiscard]] std::filesystem::path default_socket_path() {
    if (const auto* runtime = std::getenv("XDG_RUNTIME_DIR")) {
        return std::filesystem::path{runtime} / "melodyd.sock";
    }
    return std::filesystem::temp_directory_path() / "melodyd.sock";
}

void usage() {
    std::cerr << "usage: melodyd [--socket PATH] [--state DIR] [--listen HOST:PORT]\n"
              << "\n"
              << "  --socket PATH  where to listen (default $XDG_RUNTIME_DIR/melodyd.sock)\n"
              << "  --state DIR    where the database lives (default\n"
              << "                 $XDG_DATA_HOME/trackknife/trackknife, Trackknife's own)\n"
              << "  --listen HOST:PORT\n"
              << "                 also accept TCP connections. Every one must authenticate\n"
              << "                 with the token in DIR/engine.token (created on first use).\n"
              << "                 There is no TLS: use it on a home network or inside a\n"
              << "                 WireGuard tunnel, or put a TLS proxy in front (ADR-0223).\n"
              << "\n"
              << "Speaks protocol v1: one JSON object per line. Try:\n"
              << "  echo '{\"id\":1,\"method\":\"catalogue.roots\"}' | nc -UN -w2 "
              << default_socket_path().string() << "\n"
              << "\n"
              << "A half-closed connection stays open, because a client that has\n"
              << "finished sending is usually still listening -- that is how job\n"
              << "events reach the client that submitted the job. So give nc a read\n"
              << "timeout (-w) or it will wait for an engine that has nothing more\n"
              << "to say.\n";
}

} // namespace

int main(int argc, char** argv) {
    auto socket_path = default_socket_path();
    auto state_directory = default_state_directory();
    std::string listen_address;

    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        const auto value = [&]() -> std::string {
            return index + 1 < argc ? argv[++index] : std::string{};
        };
        if (argument == "--socket") {
            socket_path = value();
        } else if (argument == "--state") {
            state_directory = value();
        } else if (argument == "--listen") {
            listen_address = value();
        } else if (argument == "--help" || argument == "-h") {
            usage();
            return EXIT_SUCCESS;
        } else {
            std::cerr << "melodyd: unrecognised argument " << argument << "\n\n";
            usage();
            return EXIT_FAILURE;
        }
    }

    std::error_code ignored;
    std::filesystem::create_directories(state_directory, ignored);
    if (!hold_lock(state_directory / "engine.lock")) {
        std::cerr << "melodyd: another engine is using " << state_directory.string() << "\n";
        return EXIT_FAILURE;
    }
    if (!hold_lock(std::filesystem::path{socket_path.string() + ".lock"})) {
        std::cerr << "melodyd: another engine is starting on " << socket_path.string() << "\n";
        return EXIT_FAILURE;
    }

    // Both views of the one database are opened eagerly, so a migration
    // failure surfaces now rather than in the response to some client's first
    // request.
    const auto database = state_directory / database_filename;
    trackknife::engine::LocalCatalogue catalogue{database};
    if (const auto prepared = catalogue.prepare(); !prepared) {
        std::cerr << "melodyd: could not open the catalogue: " << prepared.error().message << "\n";
        return EXIT_FAILURE;
    }
    auto workspace = trackknife::engine::Workspace::open(database);
    if (!workspace) {
        std::cerr << "melodyd: could not open the workspace: " << workspace.error().message << "\n";
        return EXIT_FAILURE;
    }

    trackknife::protocol::Dispatcher dispatcher;
    trackknife::engine::register_catalogue_methods(dispatcher, catalogue);

    // Playback is optional at startup: a machine with no audio device can
    // still serve the catalogue, which is what a headless indexing host is.
    // Refusing to start would make the engine useless there for no gain.
    auto player = trackknife::engine::Player::create();
    if (player) {
        trackknife::engine::register_playback_methods(dispatcher, **player);
    } else {
        std::cerr << "melodyd: no audio output (" << player.error().message
                  << "); playback methods are unavailable\n";
    }

    auto server = trackknife::engine::Server::listen(socket_path, dispatcher);
    if (!server) {
        std::cerr << "melodyd: could not listen on " << socket_path.string() << ": "
                  << server.error().message << "\n";
        return EXIT_FAILURE;
    }

    // ADR-0223: TCP only when asked for, and never without a token.
    std::unique_ptr<trackknife::engine::Server> tcp_server;
    if (!listen_address.empty()) {
        const auto endpoint = trackknife::protocol::Endpoint::parse(listen_address, {});
        if (!endpoint || !endpoint->tcp()) {
            std::cerr << "melodyd: --listen wants HOST:PORT, got " << listen_address << "\n";
            return EXIT_FAILURE;
        }
        const auto token_path = state_directory / "engine.token";
        auto token = trackknife::engine::load_or_create_token(token_path);
        if (!token) {
            std::cerr << "melodyd: " << token.error().message << " (" << token_path.string()
                      << ")\n";
            return EXIT_FAILURE;
        }
        auto listening = trackknife::engine::Server::listen_tcp(endpoint->host, endpoint->port,
                                                                dispatcher, std::move(*token));
        if (!listening) {
            std::cerr << "melodyd: could not listen on " << listen_address << ": "
                      << listening.error().message << "\n";
            return EXIT_FAILURE;
        }
        tcp_server = std::move(*listening);
        std::cerr << "melodyd: listening on " << endpoint->describe() << " (token in "
                  << token_path.string() << ")\n";
    }

    // Every listener hears every event. A client on TCP is as much a client
    // as one on the socket, and a job started from one must report to both.
    const auto unix_sink = (*server)->sink();
    const auto tcp_sink = tcp_server ? tcp_server->sink() : trackknife::engine::EventSink{};
    const trackknife::engine::EventSink sink = [unix_sink, tcp_sink](const auto& event) {
        unix_sink(event);
        if (tcp_sink) {
            tcp_sink(event);
        }
    };

    // Jobs report through the sockets, so the registry is given its sink and
    // must be destroyed before the servers it writes to.
    trackknife::engine::JobRegistry jobs{sink};
    trackknife::engine::JobCatalog job_catalogue;
    trackknife::engine::register_catalogue_jobs(job_catalogue, catalogue);
    trackknife::engine::register_job_methods(dispatcher, jobs, job_catalogue);

    // Pushed state, so a client learns a track changed without asking.
    std::optional<trackknife::engine::PlaybackWatcher> watcher;
    // And the counters the player accumulates are written down, rather than
    // computed and discarded. Without this the engine plays but remembers
    // nothing -- no play counts, no resume.
    std::optional<trackknife::engine::Recorder> recorder;
    // ADR-0220: the queue is the engine's, so the engine brings it back. Without
    // this the first client to connect after a restart decides what the engine
    // is playing, which is the client owning the queue with extra steps.
    std::optional<trackknife::engine::PlaybackStore> playback_store;
    if (player) {
        watcher.emplace(**player, sink);
        watcher->start();
        recorder.emplace(**player, *workspace);
        recorder->start();
        playback_store.emplace(**player, *workspace);
        if (playback_store->restore()) {
            std::cerr << "melodyd: restored " << (*player)->queue().size()
                      << " queued entries, paused\n";
        }
        playback_store->start();
    }

    std::signal(SIGINT, request_stop);
    std::signal(SIGTERM, request_stop);
    // A client hanging up must not take the engine with it.
    std::signal(SIGPIPE, SIG_IGN);

    (*server)->start();
    if (tcp_server) {
        tcp_server->start();
    }
    std::cerr << "melodyd: listening on " << socket_path.string() << "\n"
              << "melodyd: database " << database.string() << "\n";

    while (!stop_requested.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }

    std::cerr << "melodyd: stopping\n";
    // Both sample the player, so they stop before it and before the server
    // the watcher writes to.
    if (recorder) {
        recorder->stop();
    }
    if (playback_store) {
        // Stopping writes one last time, so a clean shutdown does not lose the
        // seconds since the last tick.
        playback_store->stop();
    }
    if (watcher) {
        watcher->stop();
    }
    if (tcp_server) {
        tcp_server->stop();
    }
    (*server)->stop();
    return EXIT_SUCCESS;
}

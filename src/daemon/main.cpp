// SPDX-License-Identifier: GPL-3.0-only

// ADR-0220: the engine as a process.
//
// It is deliberately not called melodyd. The Go daemon owns that name, its
// service file and its config path, and an existing install must not be
// upgraded into a different program by accident; this takes the name only
// after the Phase 6 migration, as a major version bump. Until then the two
// can run side by side.

#include "trackknife/engine/catalogue_methods.hpp"
#include "trackknife/engine/job_methods.hpp"
#include "trackknife/engine/playback_methods.hpp"
#include "trackknife/engine/server.hpp"
#include "trackknife/engine/workspace.hpp"

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

[[nodiscard]] std::filesystem::path default_state_directory() {
    if (const auto* explicit_home = std::getenv("TRACKKNIFE_STATE_DIR")) {
        return explicit_home;
    }
    if (const auto* data_home = std::getenv("XDG_DATA_HOME")) {
        return std::filesystem::path{data_home} / "trackknife";
    }
    if (const auto* home = std::getenv("HOME")) {
        return std::filesystem::path{home} / ".local" / "share" / "trackknife";
    }
    return std::filesystem::current_path();
}

[[nodiscard]] std::filesystem::path default_socket_path() {
    if (const auto* runtime = std::getenv("XDG_RUNTIME_DIR")) {
        return std::filesystem::path{runtime} / "tkengine.sock";
    }
    return std::filesystem::temp_directory_path() / "tkengine.sock";
}

void usage() {
    std::cerr << "usage: tkengine [--socket PATH] [--state DIR]\n"
              << "\n"
              << "  --socket PATH  where to listen (default $XDG_RUNTIME_DIR/tkengine.sock)\n"
              << "  --state DIR    where the databases live (default $XDG_DATA_HOME/trackknife)\n"
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

    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        const auto value = [&]() -> std::string {
            return index + 1 < argc ? argv[++index] : std::string{};
        };
        if (argument == "--socket") {
            socket_path = value();
        } else if (argument == "--state") {
            state_directory = value();
        } else if (argument == "--help" || argument == "-h") {
            usage();
            return EXIT_SUCCESS;
        } else {
            std::cerr << "tkengine: unrecognised argument " << argument << "\n\n";
            usage();
            return EXIT_FAILURE;
        }
    }

    std::error_code ignored;
    std::filesystem::create_directories(state_directory, ignored);

    // The two stores the engine owns. Both are opened eagerly so a migration
    // failure surfaces now rather than in the response to some client's first
    // request, and so both exist on disk once the engine says it is listening.
    trackknife::engine::LocalCatalogue catalogue{state_directory / "library.sqlite3"};
    if (const auto prepared = catalogue.prepare(); !prepared) {
        std::cerr << "tkengine: could not open the catalogue: " << prepared.error().message << "\n";
        return EXIT_FAILURE;
    }
    auto workspace = trackknife::engine::Workspace::open(state_directory / "workspace.sqlite3");
    if (!workspace) {
        std::cerr << "tkengine: could not open the workspace: " << workspace.error().message
                  << "\n";
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
        std::cerr << "tkengine: no audio output (" << player.error().message
                  << "); playback methods are unavailable\n";
    }

    auto server = trackknife::engine::Server::listen(socket_path, dispatcher);
    if (!server) {
        std::cerr << "tkengine: could not listen on " << socket_path.string() << ": "
                  << server.error().message << "\n";
        return EXIT_FAILURE;
    }

    // Jobs report through the socket, so the registry is given its sink and
    // must be destroyed before the server it writes to.
    trackknife::engine::JobRegistry jobs{(*server)->sink()};
    trackknife::engine::JobCatalog job_catalogue;
    trackknife::engine::register_catalogue_jobs(job_catalogue, catalogue);
    trackknife::engine::register_job_methods(dispatcher, jobs, job_catalogue);

    // Pushed state, so a client learns a track changed without asking.
    std::optional<trackknife::engine::PlaybackWatcher> watcher;
    if (player) {
        watcher.emplace(**player, (*server)->sink());
        watcher->start();
    }

    std::signal(SIGINT, request_stop);
    std::signal(SIGTERM, request_stop);
    // A client hanging up must not take the engine with it.
    std::signal(SIGPIPE, SIG_IGN);

    (*server)->start();
    std::cerr << "tkengine: listening on " << socket_path.string() << "\n"
              << "tkengine: state in " << state_directory.string() << "\n";

    while (!stop_requested.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }

    std::cerr << "tkengine: stopping\n";
    // The watcher writes to the server's sink, so it stops first.
    if (watcher) {
        watcher->stop();
    }
    (*server)->stop();
    return EXIT_SUCCESS;
}

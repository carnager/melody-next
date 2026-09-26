// SPDX-License-Identifier: GPL-3.0-only

// ADR-0220: the engine as a process, melodyd.
//
// It was built as tkengine while the Go daemon of that name was meant to keep
// running beside it. No install of the Go melodyd exists to be replaced, so
// the engine took the name early (ADR-0226).

#include "agent/agent.hpp"
#include "agent/guests.hpp"
#include "agent/speaker_arbiter.hpp"
#include "trackknife/core/stable_id.hpp"
#include "trackknife/discovery/mdns.hpp"
#include "trackknife/engine/catalogue_methods.hpp"
#include "trackknife/engine/job_methods.hpp"
#include "trackknife/engine/outputs.hpp"
#include "trackknife/engine/playback_methods.hpp"
#include "trackknife/engine/playback_store.hpp"
#include "trackknife/engine/recorder.hpp"
#include "trackknife/engine/server.hpp"
#include "trackknife/engine/lastfm.hpp"
#include "trackknife/engine/media_streams.hpp"
#include "trackknife/engine/stream_server.hpp"
#include "trackknife/engine/transcode_cache.hpp"
#include "trackknife/engine/token.hpp"
#include "trackknife/engine/workspace.hpp"
#include "trackknife/protocol/client.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
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

constexpr const char* default_listen = "0.0.0.0:6603";
constexpr const char* default_http = "0.0.0.0:6604";

void usage() {
    std::cerr << "usage: melodyd [--socket PATH] [--state DIR] [--listen HOST:PORT | --local-only]\n"
              << "               [--name NAME] [--password PASS | --password-file FILE]\n"
              << "               [--http HOST:PORT] [--music-root DIR]\n"
              << "               [--play-for HOST:PORT [--play-for-name NAME]\n"
              << "                [--play-for-password PASS | --play-for-password-file FILE]\n"
              << "                [--play-for-music-root DIR]]\n"
              << "               [--agent [--agent-password PASS] [--agent-music-root DIR]]\n"
              << "\n"
              << "  --socket PATH  where to listen (default $XDG_RUNTIME_DIR/melodyd.sock)\n"
              << "  --state DIR    where the database lives (default\n"
              << "                 $XDG_DATA_HOME/trackknife/trackknife, Trackknife's own)\n"
              << "  --music-root DIR\n"
              << "                 where the music is: output agents with their own copy are\n"
              << "                 sent paths relative to it (ADR-0228)\n"
              << "  --name NAME    what clients call this engine (default: the host name)\n"
              << "  --listen HOST:PORT\n"
              << "                 where to accept TCP connections, from clients and output\n"
              << "                 agents (default 0.0.0.0:6603, streams on 0.0.0.0:6604, when\n"
              << "                 a password is set); the engine is announced there to be\n"
              << "                 found by name. Needs a password.\n"
              << "  --local-only   no TCP and no streams by default: this machine's socket\n"
              << "                 only, unless --listen or --http names them. The default\n"
              << "                 without a password.\n"
              << "  --password PASS, --password-file FILE\n"
              << "                 the password every TCP connection must give, the same on\n"
              << "                 every agent and client. There is no TLS: on an untrusted\n"
              << "                 network use WireGuard, or put a TLS proxy in front of a\n"
              << "                 loopback --listen (ADR-0223).\n"              << "  --http HOST:PORT\n"
              << "                 serve the music being played to output agents that have\n"
              << "                 no copy of their own (melody-agent --stream). Only what\n"
              << "                 the queue holds is served -- converted to Opus for an agent\n"
              << "                 that asks, and to clients with a ticket (offline copies).\n"
              << "  --transcode-cache MB\n"
              << "                 how much converted music to keep for streams and\n"
              << "                 downloads (default 2048)\n"
              << "  --play-for HOST:PORT\n"
              << "                 let another engine play on this machine's speakers, as an\n"
              << "                 output agent built in -- no melody-agent needed here. The\n"
              << "                 newest to start playing gets the speakers; the other pauses.\n"
              << "  --play-for-name NAME\n"
              << "                 what to call that engine when it takes them (default: its\n"
              << "                 address)\n"
              << "  --play-for-password PASS, --play-for-password-file FILE\n"
              << "                 its password (default: this engine's own)\n"
              << "  --play-for-music-root DIR\n"
              << "                 where its music is mounted here; without one it streams\n"
              << "  --agent        play for every other engine found on the network, on this\n"
              << "                 machine's speakers: no melody-agent needed. Engines listening\n"
              << "                 on the network (--listen) announce themselves to be found.\n"
              << "  --agent-password PASS, --agent-password-file FILE\n"
              << "                 for the engines it plays for (default: --play-for's, else\n"
              << "                 this engine's own)\n"
              << "  --agent-music-root DIR\n"
              << "                 where their music is mounted here; without one it streams\n"
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
    std::string http_address;
    std::uint64_t transcode_cache_mb = 2048;
    bool local_only = false;
    std::string password;
    std::string password_file;
    std::string engine_name;
    std::optional<std::filesystem::path> music_root;
    std::string play_for;
    std::string play_for_name;
    std::string play_for_password;
    std::string play_for_password_file;
    std::optional<std::filesystem::path> play_for_music_root;
    bool agent_for_all = false;
    std::string agent_password;
    std::optional<std::filesystem::path> agent_music_root;

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
        } else if (argument == "--local-only") {
            local_only = true;
        } else if (argument == "--name") {
            engine_name = value();
        } else if (argument == "--password") {
            password = value();
        } else if (argument == "--password-file") {
            password_file = value();
        } else if (argument == "--http") {
            http_address = value();
        } else if (argument == "--transcode-cache") {
            const auto text = value();
            try {
                transcode_cache_mb = std::stoull(text);
            } catch (const std::exception&) {
                std::cerr << "melodyd: --transcode-cache wants megabytes, got " << text << "\n";
                return EXIT_FAILURE;
            }
        } else if (argument == "--music-root") {
            music_root = std::filesystem::path{value()};
        } else if (argument == "--play-for") {
            play_for = value();
        } else if (argument == "--play-for-name") {
            play_for_name = value();
        } else if (argument == "--play-for-password") {
            play_for_password = value();
        } else if (argument == "--play-for-password-file") {
            play_for_password_file = value();
        } else if (argument == "--agent") {
            agent_for_all = true;
        } else if (argument == "--agent-password") {
            agent_password = value();
        } else if (argument == "--agent-password-file") {
            std::ifstream file{value()};
            std::getline(file, agent_password);
            while (!agent_password.empty() &&
                   (agent_password.back() == '\r' || agent_password.back() == ' ')) {
                agent_password.pop_back();
            }
        } else if (argument == "--agent-music-root") {
            agent_music_root = std::filesystem::path{value()};
        } else if (argument == "--play-for-music-root") {
            play_for_music_root = std::filesystem::path{value()};
        } else if (argument == "--help" || argument == "-h") {
            usage();
            return EXIT_SUCCESS;
        } else {
            std::cerr << "melodyd: unrecognised argument " << argument << "\n\n";
            usage();
            return EXIT_FAILURE;
        }
    }

    if (!password_file.empty()) {
        std::ifstream file{password_file};
        std::getline(file, password);
        while (!password.empty() && (password.back() == '\r' || password.back() == ' ')) {
            password.pop_back();
        }
        if (password.empty()) {
            std::cerr << "melodyd: no password in " << password_file << "\n";
            return EXIT_FAILURE;
        }
    }

    if (!play_for_password_file.empty()) {
        std::ifstream file{play_for_password_file};
        std::getline(file, play_for_password);
        while (!play_for_password.empty() &&
               (play_for_password.back() == '\r' || play_for_password.back() == ' ')) {
            play_for_password.pop_back();
        }
    }
    // ADR-0223: every TCP connection gives a password, so a listener asked
    // for without one is refused before anything is opened.
    if (!listen_address.empty() && password.empty()) {
        std::cerr << "melodyd: --listen needs --password or --password-file: every TCP "
                     "connection must give it (ADR-0223)\n";
        return EXIT_FAILURE;
    }
    // One password set everywhere is the usual case, as for --agent-password.
    if (play_for_password.empty()) {
        play_for_password = password;
    }
    std::optional<trackknife::protocol::Endpoint> guest_endpoint;
    if (!play_for.empty()) {
        guest_endpoint = trackknife::protocol::Endpoint::parse(play_for, play_for_password);
        if (!guest_endpoint) {
            std::cerr << "melodyd: --play-for wants HOST:PORT or a socket path\n";
            return EXIT_FAILURE;
        }
        if (guest_endpoint->tcp() && play_for_password.empty()) {
            // Not fatal: the engine is still this machine's player. But the
            // other one will refuse it, and that should be said here.
            std::cerr << "melodyd: --play-for " << play_for
                      << " has no password; that engine will refuse this one\n";
        }
    }

    if (engine_name.empty()) {
        // What clients call this engine: its machine, unless told otherwise.
        std::array<char, 256> host{};
        engine_name = ::gethostname(host.data(), host.size()) == 0 ? host.data() : "melodyd";
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
    // A rating is told to every client, through the listeners -- which are
    // made further down, once the dispatcher has its methods. Until then
    // there is no one to tell, and the relay drops it.
    struct EventRelay {
        std::mutex lock;
        trackknife::engine::EventSink sink;
    };
    const auto relay = std::make_shared<EventRelay>();
    trackknife::engine::register_catalogue_methods(
        dispatcher, catalogue, [relay](const trackknife::protocol::Event& event) {
            const std::scoped_lock held{relay->lock};
            if (relay->sink) {
                relay->sink(event);
            }
        });

    // A machine with no audio device still plays: through output agents
    // (ADR-0228). Its player keeps the queue and plays nothing until one is
    // chosen, rather than the engine going without playback at all.
    std::unique_ptr<trackknife::engine::Player> player;
    if (auto local = trackknife::engine::Player::create()) {
        player = std::move(*local);
    } else {
        std::cerr << "melodyd: no audio output here (" << local.error().message
                  << "); playing through output agents only\n";
        player = trackknife::engine::Player::create_without_audio();
    }
    trackknife::engine::register_playback_methods(dispatcher, *player);
    // Who this is, for a client to show rather than an address -- and its
    // id, the one it is announced with, so a client that reaches it both
    // here and over the network knows it is one engine.
    const auto engine_id = trackknife::core::StableId::random().to_string();
    dispatcher.on("engine.info", [&engine_name, engine_id](const trackknife::protocol::Json&)
                                     -> trackknife::core::Result<trackknife::protocol::Json> {
        return trackknife::protocol::Json{
            {"name", engine_name}, {"id", engine_id}, {"protocol", 1}};
    });

    auto server = trackknife::engine::Server::listen(socket_path, dispatcher);
    if (!server) {
        std::cerr << "melodyd: could not listen on " << socket_path.string() << ": "
                  << server.error().message << "\n";
        return EXIT_FAILURE;
    }

    // ADR-0223: TCP always wants a password.
    std::unique_ptr<trackknife::engine::Server> tcp_server;
    // On the network unless told otherwise, when it has a password: an engine
    // is there to be played from and on, and found by name. Without one it
    // stays on this machine rather than refusing to start, and says why; a
    // --listen asked for without one is refused below. The ports asked for
    // must be had; the default ones are given up quietly for local-only when
    // taken.
    if (!local_only && listen_address.empty() && password.empty()) {
        std::cerr << "melodyd: no --password, so not on the network; this machine only\n";
        local_only = true;
    }
    const bool listen_by_default = !local_only && listen_address.empty();
    if (listen_by_default) {
        listen_address = default_listen;
        if (http_address.empty()) {
            http_address = default_http;
        }
    }
    if (!listen_address.empty()) {
        const auto endpoint = trackknife::protocol::Endpoint::parse(listen_address, {});
        if (!endpoint || !endpoint->tcp()) {
            std::cerr << "melodyd: --listen wants HOST:PORT, got " << listen_address << "\n";
            return EXIT_FAILURE;
        }
        auto listening = trackknife::engine::Server::listen_tcp(endpoint->host, endpoint->port,
                                                                dispatcher, password);
        if (!listening && listen_by_default) {
            std::cerr << "melodyd: " << listen_address << " is taken ("
                      << listening.error().message << "); this machine only\n";
            http_address.clear();
        } else if (!listening) {
            std::cerr << "melodyd: could not listen on " << listen_address << ": "
                      << listening.error().message << "\n";
            return EXIT_FAILURE;
        } else {
            tcp_server = std::move(*listening);
            std::cerr << "melodyd: listening on " << endpoint->describe() << " (with a password)\n";
        }
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

    {
        const std::scoped_lock held{relay->lock};
        relay->sink = sink;
    }

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
    {
        watcher.emplace(*player, sink);
        watcher->start();
        recorder.emplace(*player, *workspace);
        recorder->start();
        playback_store.emplace(*player, *workspace);
        if (playback_store->restore()) {
            std::cerr << "melodyd: restored " << player->queue().size()
                      << " queued entries, paused\n";
        }
        playback_store->start();
    }

    // ADR-0228: the files an agent without its own copy fetches -- only
    // what the player holds, with a token that lives as long as this run and
    // reaches agents only in the URLs the engine gives them.
    std::unique_ptr<trackknife::engine::TranscodeCache> transcodes;
    std::unique_ptr<trackknife::engine::MediaStreams> media;
    std::unique_ptr<trackknife::engine::StreamServer> streams;
    trackknife::output::AgentPaths agent_paths{
        .music_root = music_root, .stream_port = 0U, .stream_host = {}, .stream_token = {}};
    if (!http_address.empty()) {
        const auto endpoint = trackknife::protocol::Endpoint::parse(http_address, {});
        auto token = trackknife::engine::random_token();
        if (!endpoint || !endpoint->tcp() || !token) {
            std::cerr << "melodyd: --http wants HOST:PORT, got " << http_address << "\n";
            return EXIT_FAILURE;
        }
        transcodes = std::make_unique<trackknife::engine::TranscodeCache>(
            state_directory / "transcodes", transcode_cache_mb * 1024U * 1024U);
        media = std::make_unique<trackknife::engine::MediaStreams>(
            *token, [&player](const std::string& raw_path) { return player->holds(raw_path); },
            transcodes.get());
        auto listening = trackknife::engine::StreamServer::listen(
            endpoint->host, endpoint->port,
            [&media](const std::string_view query) { return media->resolve(query); });
        if (!listening && listen_by_default) {
            std::cerr << "melodyd: " << http_address << " is taken ("
                      << listening.error().message << "); agents with no copy of the music "
                      << "cannot be streamed to\n";
        } else if (!listening) {
            std::cerr << "melodyd: could not serve streams on " << http_address << ": "
                      << listening.error().message << "\n";
            return EXIT_FAILURE;
        } else {
            streams = std::move(*listening);
            agent_paths.stream_port = streams->port();
            trackknife::engine::register_stream_methods(dispatcher, *media, catalogue,
                                                        streams->port());
            agent_paths.stream_token = std::move(*token);
            // Served on every address, each agent fetches from the one it
            // reached the engine at; on one address, from that one.
            if (endpoint->host != "0.0.0.0" && endpoint->host != "::" && !endpoint->host.empty()) {
                agent_paths.stream_host = endpoint->host;
            }
            std::cerr << "melodyd: serving streams to agents on " << endpoint->describe() << "\n";
        }
    }

    // ADR-0228: what the engine plays on -- its own audio and any output
    // agents. After the queue is restored, so the chosen output takes it up.
    trackknife::engine::Outputs outputs{*player, std::move(agent_paths), &*workspace, sink,
                                        engine_name};
    trackknife::engine::register_output_methods(dispatcher, outputs);
    const auto admit = [&outputs](const trackknife::protocol::Json& params, const int descriptor) {
        outputs.admit(params, descriptor);
    };
    (*server)->on_agent(admit);
    if (tcp_server) {
        tcp_server->on_agent(admit);
    }
    outputs.restore();

    // Last.fm for what this engine plays, with its session handed over by a
    // client (lastfm.set_session); it scrobbles with every window closed.
    trackknife::engine::LastFm lastfm{*player, catalogue, state_directory / "lastfm.json"};
    trackknife::engine::register_lastfm_methods(dispatcher, lastfm);
    lastfm.start();

    // Another engine on these speakers, through an agent built in: newest
    // wins them (ADR-0228).
    std::unique_ptr<trackknife::agent::Agent> guest;
    std::unique_ptr<trackknife::agent::SpeakerArbiter> arbiter;
    if (guest_endpoint) {
        auto audition = trackknife::audio::LocalAuditionService::create();
        if (!audition) {
            std::cerr << "melodyd: no audio here to play " << play_for << " on: "
                      << audition.error().message << "\n";
        } else {
            static_cast<void>((*audition)->refresh_output_devices());
            auto made = trackknife::agent::Agent::create(
                trackknife::agent::AgentConfig{.server = *guest_endpoint,
                                               .name = engine_name,
                                               .music_root = play_for_music_root,
                                               .stream_only = !play_for_music_root.has_value()},
                std::move(*audition));
            if (!made) {
                std::cerr << "melodyd: cannot play for " << play_for << ": "
                          << made.error().message << "\n";
            } else {
                guest = std::move(*made);
                arbiter = std::make_unique<trackknife::agent::SpeakerArbiter>(&*player);
                arbiter->add_guest(
                    play_for_name.empty() ? guest_endpoint->describe() : play_for_name, *guest);
            }
        }
    }

    // This run's identity among engines on the network: what an engine that
    // plays for others looks for, so it does not play for itself.
    std::unique_ptr<trackknife::agent::Guests> guests;
    if (agent_for_all) {
        if (!arbiter) {
            arbiter = std::make_unique<trackknife::agent::SpeakerArbiter>(player.get());
        }
        guests = std::make_unique<trackknife::agent::Guests>(
            trackknife::agent::Guests::Config{
                .name = engine_name,
                // Asked for, else the one given for --play-for, else this
                // engine's own: one password set everywhere is the usual case.
                .password = !agent_password.empty()      ? agent_password
                            : !play_for_password.empty() ? play_for_password
                                                         : password,
                .music_root = agent_music_root,
                .own_id = engine_id,
                .already = play_for.empty() ? std::vector<std::string>{}
                                            : std::vector<std::string>{play_for}},
            *arbiter);
    }
    // Listening on the network, it says so there: agents and clients find it
    // by name without being told where it is.
    std::unique_ptr<trackknife::discovery::Announcer> announcer;
    if (tcp_server) {
        auto announced = trackknife::discovery::Announcer::start(trackknife::discovery::Advertisement{
            .instance = engine_name,
            .port = tcp_server->port(),
            .txt = {{"id", engine_id},
                    {"proto", "1"},
                    // Always wanted now; kept for clients that read it.
                    {"auth", "1"},
                    {"http", streams ? std::to_string(streams->port()) : std::string{}}}});
        if (announced) {
            announcer = std::move(*announced);
        } else {
            std::cerr << "melodyd: not announced on the network: " << announced.error().message
                      << "\n";
        }
    }

    std::signal(SIGINT, request_stop);
    std::signal(SIGTERM, request_stop);
    // A client hanging up must not take the engine with it.
    std::signal(SIGPIPE, SIG_IGN);

    (*server)->start();
    if (tcp_server) {
        tcp_server->start();
    }
    if (streams) {
        streams->start();
    }
    if (guests && guests->start()) {
        std::cerr << "melodyd: playing for the engines on the network as \"" << engine_name
                  << "\"\n";
    }
    if (arbiter) {
        arbiter->start();
    }
    if (guest) {
        guest->start();
        std::cerr << "melodyd: playing for " << guest_endpoint->describe() << " as \""
                  << engine_name << "\"\n";
    }
    std::cerr << "melodyd: listening on " << socket_path.string() << "\n"
              << "melodyd: database " << database.string() << "\n";

    while (!stop_requested.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }

    std::cerr << "melodyd: stopping\n";
    announcer.reset();
    if (arbiter) {
        arbiter->stop();
    }
    if (guests) {
        guests->stop();
    }
    if (guest) {
        guest->stop();
    }
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
    lastfm.stop();
    if (streams) {
        streams->stop();
    }
    if (tcp_server) {
        tcp_server->stop();
    }
    (*server)->stop();
    return EXIT_SUCCESS;
}

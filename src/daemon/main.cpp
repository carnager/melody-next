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
#if TRACKKNIFE_ENABLE_UPNP
#include "trackknife/discovery/upnp.hpp"
#endif
#include "trackknife/engine/catalogue_methods.hpp"
#include "trackknife/engine/file_work_methods.hpp"
#include "trackknife/engine/job_methods.hpp"
#include "trackknife/engine/lastfm.hpp"
#include "trackknife/engine/list_methods.hpp"
#include "trackknife/engine/media_streams.hpp"
#include "trackknife/engine/metadata_services.hpp"
#include "trackknife/engine/naming_methods.hpp"
#include "trackknife/engine/now_playing_methods.hpp"
#include "trackknife/engine/outputs.hpp"
#include "trackknife/engine/playback_methods.hpp"
#include "trackknife/engine/playback_store.hpp"
#include "trackknife/engine/rating_tags.hpp"
#include "trackknife/engine/recorder.hpp"
#include "trackknife/engine/server.hpp"
#include "trackknife/engine/stream_server.hpp"
#include "trackknife/engine/token.hpp"
#include "trackknife/engine/transcode_cache.hpp"
#include "trackknife/engine/workspace.hpp"
#include "trackknife/formats/probe.hpp"
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
#ifdef __APPLE__
        return std::filesystem::path{home} / "Library" / "Application Support" / "trackknife";
#else
        return std::filesystem::path{home} / ".local" / "share" / "trackknife" / "trackknife";
#endif
    }
    return std::filesystem::current_path();
}

// ADR-0234: who this engine is, across restarts -- minted once and kept in
// its state directory, beside the library and lists it identifies. An id
// that cannot be kept still names the engine for this run, and says so.
[[nodiscard]] std::string stable_engine_id(const std::filesystem::path& state_directory) {
    const auto path = state_directory / "engine-id";
    if (std::ifstream in{path}; in) {
        std::string text;
        std::getline(in, text);
        if (auto kept = trackknife::core::StableId::parse(text); kept) {
            return kept->to_string();
        }
    }
    auto minted = trackknife::core::StableId::random().to_string();
    const auto partial = state_directory / "engine-id.partial";
    std::error_code error;
    if (std::ofstream out{partial, std::ios::trunc};
        out && (out << minted << '\n') && out.flush()) {
        out.close();
        std::filesystem::rename(partial, path, error);
    } else {
        error = std::make_error_code(std::errc::io_error);
    }
    if (error) {
        std::cerr << "melodyd: could not keep this engine's id in " << path.string()
                  << "; clients will see a new engine next time\n";
    }
    return minted;
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

// ADR-0239: the rates given, each else its default.
[[nodiscard]] trackknife::agent::StreamChoice stream_choice(const std::optional<int> nearby_kbps,
                                                            const std::optional<int> away_kbps) {
    trackknife::agent::StreamChoice choice;
    if (nearby_kbps) {
        choice.nearby = trackknife::agent::format_for_kbps(*nearby_kbps);
    }
    if (away_kbps) {
        choice.away = trackknife::agent::format_for_kbps(*away_kbps);
    }
    return choice;
}

void usage() {
    std::cerr
        << "usage: melodyd [--socket PATH] [--state DIR] [--listen HOST:PORT | --local-only]\n"
        << "               [--name NAME] [--password PASS | --password-file FILE]\n"
        << "               [--http HOST:PORT] [--music-root DIR]\n"
        << "               [--play-for HOST:PORT [--play-for-name NAME]\n"
        << "                [--play-for-password PASS | --play-for-password-file FILE]\n"
        << "                [--play-for-music-root DIR]\n"
        << "                [--play-for-bitrate-nearby KBPS] [--play-for-bitrate-away KBPS]]\n"
        << "               [--agent [--agent-password PASS] [--agent-music-root DIR]\n"
        << "                [--agent-bitrate-nearby KBPS] [--agent-bitrate-away KBPS]]\n"
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
        << "                 loopback --listen (ADR-0223).\n"
#if TRACKKNIFE_ENABLE_UPNP
        << "  --upnp [--upnp-interface NAME]\n"
        << "      Discover UPnP renderers (requires a LAN-reachable --http listener).\n"
#endif
        << "  --http HOST:PORT\n"
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
        << "                 Given again, for each engine; the --play-for-* options after\n"
        << "                 one are its, those before any are every one's.\n"
        << "  --play-for-name NAME\n"
        << "                 what to call that engine when it takes them (default: its\n"
        << "                 address)\n"
        << "  --play-for-password PASS, --play-for-password-file FILE\n"
        << "                 its password (default: this engine's own)\n"
        << "  --play-for-music-root DIR\n"
        << "                 where its music is mounted here; without one it streams\n"
        << "  --play-for-bitrate-nearby KBPS, --play-for-bitrate-away KBPS\n"
        << "                 what it streams here when it is on this machine's own\n"
        << "                 network, and when it is reached through a VPN or a router:\n"
        << "                 0 for the original files, else Opus at 16 to 512 kbps\n"
        << "                 (default: 0 nearby, 128 away; ADR-0239)\n"
        << "  --agent        play for every other engine found on the network, on this\n"
        << "                 machine's speakers: no melody-agent needed. Engines listening\n"
        << "                 on the network (--listen) announce themselves to be found.\n"
        << "  --agent-password PASS, --agent-password-file FILE\n"
        << "                 for the engines it plays for (default: --play-for's, else\n"
        << "                 this engine's own)\n"
        << "  --agent-music-root DIR\n"
        << "                 where their music is mounted here; without one it streams\n"
        << "  --agent-bitrate-nearby KBPS, --agent-bitrate-away KBPS\n"
        << "                 as --play-for-bitrate-*, for the engines found\n"
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
#if TRACKKNIFE_ENABLE_UPNP
    bool upnp_enabled = false;
    std::string upnp_interface;
#endif
    std::string password;
    std::string password_file;
    std::string engine_name;
    std::optional<std::filesystem::path> music_root;
    // ADR-0234: engines this one plays for by address, each on these
    // speakers; `--play-for-*` after one applies to it, before any to all.
    struct PlayFor {
        std::string address;
        std::string name;
        std::string password;
        std::string password_file;
        std::optional<std::filesystem::path> music_root;
        // ADR-0239: kbps asked streamed nearby and away; 0 the original.
        std::optional<int> nearby_kbps;
        std::optional<int> away_kbps;
    };
    std::vector<PlayFor> play_fors;
    PlayFor play_for_defaults;
    const auto play_for_option = [&play_fors, &play_for_defaults]() -> PlayFor& {
        return play_fors.empty() ? play_for_defaults : play_fors.back();
    };
    bool agent_for_all = false;
    std::string agent_password;
    std::string agent_password_file;
    std::optional<std::filesystem::path> agent_music_root;
    std::optional<int> agent_nearby_kbps;
    std::optional<int> agent_away_kbps;

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
#if TRACKKNIFE_ENABLE_UPNP
        } else if (argument == "--upnp") {
            upnp_enabled = true;
        } else if (argument == "--upnp-interface") {
            upnp_interface = value();
            upnp_enabled = true;
#endif
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
            play_fors.push_back(PlayFor{.address = value(),
                                        .name = {},
                                        .password = {},
                                        .password_file = {},
                                        .music_root = {},
                                        .nearby_kbps = {},
                                        .away_kbps = {}});
        } else if (argument == "--play-for-name") {
            play_for_option().name = value();
        } else if (argument == "--play-for-password") {
            play_for_option().password = value();
        } else if (argument == "--play-for-password-file") {
            play_for_option().password_file = value();
        } else if (argument == "--agent") {
            agent_for_all = true;
        } else if (argument == "--agent-password") {
            agent_password = value();
        } else if (argument == "--agent-password-file") {
            agent_password_file = value();
        } else if (argument == "--agent-music-root") {
            agent_music_root = std::filesystem::path{value()};
        } else if (argument == "--play-for-music-root") {
            play_for_option().music_root = std::filesystem::path{value()};
        } else if (argument == "--play-for-bitrate-nearby" || argument == "--play-for-bitrate-away" ||
                   argument == "--agent-bitrate-nearby" || argument == "--agent-bitrate-away") {
            const auto text = value();
            const auto kbps = trackknife::agent::parse_kbps(text);
            if (!kbps) {
                std::cerr << "melodyd: " << argument
                          << " wants 0 (the original files) or 16 to 512 kbps, got " << text << "\n";
                return EXIT_FAILURE;
            }
            if (argument == "--play-for-bitrate-nearby") {
                play_for_option().nearby_kbps = *kbps;
            } else if (argument == "--play-for-bitrate-away") {
                play_for_option().away_kbps = *kbps;
            } else if (argument == "--agent-bitrate-nearby") {
                agent_nearby_kbps = *kbps;
            } else {
                agent_away_kbps = *kbps;
            }
        } else if (argument == "--help" || argument == "-h") {
            usage();
            return EXIT_SUCCESS;
        } else {
            std::cerr << "melodyd: unrecognised argument " << argument << "\n\n";
            usage();
            return EXIT_FAILURE;
        }
    }

    std::vector<std::pair<std::string*, std::string*>> password_files{
        {&password_file, &password},
        {&play_for_defaults.password_file, &play_for_defaults.password},
        {&agent_password_file, &agent_password}};
    for (auto& target : play_fors) {
        password_files.emplace_back(&target.password_file, &target.password);
    }
    for (const auto& [file, password_read] : password_files) {
        if (file->empty()) {
            continue;
        }
        auto read = trackknife::protocol::read_password_file(*file);
        if (!read) {
            std::cerr << "melodyd: " << read.error().message << "\n";
            return EXIT_FAILURE;
        }
        *password_read = std::move(*read);
    }
    // ADR-0223: every TCP connection gives a password, so a listener asked
    // for without one is refused before anything is opened.
    if (!listen_address.empty() && password.empty()) {
        std::cerr << "melodyd: --listen needs --password or --password-file: every TCP "
                     "connection must give it (ADR-0223)\n";
        return EXIT_FAILURE;
    }
    // One password set everywhere is the usual case, as for --agent-password.
    if (play_for_defaults.password.empty()) {
        play_for_defaults.password = password;
    }
    std::vector<std::pair<PlayFor, trackknife::protocol::Endpoint>> guest_endpoints;
    for (auto target : play_fors) {
        if (target.name.empty()) {
            target.name = play_for_defaults.name;
        }
        if (target.password.empty()) {
            target.password = play_for_defaults.password;
        }
        if (!target.music_root) {
            target.music_root = play_for_defaults.music_root;
        }
        if (!target.nearby_kbps) {
            target.nearby_kbps = play_for_defaults.nearby_kbps;
        }
        if (!target.away_kbps) {
            target.away_kbps = play_for_defaults.away_kbps;
        }
        auto endpoint = trackknife::protocol::Endpoint::parse(target.address, target.password);
        if (!endpoint) {
            std::cerr << "melodyd: --play-for wants HOST:PORT or a socket path\n";
            return EXIT_FAILURE;
        }
        if (endpoint->tcp() && target.password.empty()) {
            // Not fatal: the engine is still this machine's player. But the
            // other one will refuse it, and that should be said here.
            std::cerr << "melodyd: --play-for " << target.address
                      << " has no password; that engine will refuse this one\n";
        }
        guest_endpoints.emplace_back(std::move(target), std::move(*endpoint));
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
    // A cover is given for what this engine may stream -- what it holds to
    // play -- as well as for its library: a list of files it does not index
    // shows its covers too, as does an engine with no library at all.
    // ADR-0237 stage 2: ratings copied into the files, when that is on.
    trackknife::engine::RatingTags rating_tags{database, catalogue, *workspace};
    trackknife::engine::register_rating_tag_methods(dispatcher, rating_tags);
    trackknife::engine::register_catalogue_methods(
        dispatcher, catalogue,
        [relay](const trackknife::protocol::Event& event) {
            const std::scoped_lock held{relay->lock};
            if (relay->sink) {
                relay->sink(event);
            }
        },
        [&player](const std::string& raw_path) { return player->holds(raw_path); },
        [&rating_tags](const std::string& hash, const bool album, const unsigned rating) {
            rating_tags.rated(hash, album, rating);
        });
    trackknife::engine::register_playback_methods(dispatcher, *player);
    trackknife::engine::register_now_playing_methods(dispatcher, catalogue, *player);
    // Who this is, for a client to show rather than an address -- and its
    // id, the one it is announced with, so a client that reaches it both
    // here and over the network knows it is one engine, and one that stores
    // the id finds it again after a restart (ADR-0234).
    const auto engine_id = stable_engine_id(state_directory);
    dispatcher.on("engine.info",
                  [&engine_name, engine_id](const trackknife::protocol::Json&)
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
            std::cerr << "melodyd: " << listen_address << " is taken (" << listening.error().message
                      << "); this machine only\n";
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
    // ADR-0237: a file moved here is followed, in the same commit, by the
    // workspace, the library, the lists and the queue.
    const auto follow_moves =
        trackknife::engine::follow_moves(*workspace, catalogue, player.get(), sink);
    trackknife::engine::register_file_work_jobs(job_catalogue, database, catalogue, follow_moves);
    // File work a crash interrupted is finished or rolled back before anyone
    // connects (ADR-0237), and what could not be is kept for Trackknife to show.
    auto file_work_recovery =
        trackknife::engine::recover_file_work(database, catalogue, follow_moves);
    if (file_work_recovery.error) {
        std::cerr << "melodyd: could not recover interrupted file work: "
                  << file_work_recovery.error->message << "\n";
    } else if (file_work_recovery.recovered > 0U) {
        std::cerr << "melodyd: recovered " << file_work_recovery.recovered
                  << " interrupted file operation(s)\n";
    }
    // ADR-0237: artwork a client hands over is kept here until written.
    trackknife::engine::clean_artwork_staging(state_directory / "artwork-staging");
    trackknife::engine::register_artwork_methods(dispatcher, state_directory / "artwork-staging");
    // ADR-0237: the tagger's online lookups, made here for this engine's files.
    trackknife::engine::MetadataServices metadata_services{database, state_directory};
    trackknife::engine::register_metadata_service_jobs(job_catalogue, metadata_services);
    trackknife::engine::register_metadata_service_methods(dispatcher, metadata_services);
    trackknife::engine::register_file_work_methods(dispatcher, database,
                                                   std::move(file_work_recovery));
    trackknife::engine::register_job_methods(dispatcher, jobs, job_catalogue);
    // ADR-0233: the engine's lists, working and saved, for every client.
    trackknife::engine::register_list_methods(dispatcher, *workspace, sink, *player);
    // ADR-0237: naming layouts (Trackknife's, copied here) and this engine's
    // move destinations.
    trackknife::engine::register_naming_methods(dispatcher, *workspace, sink);

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
            std::cerr << "melodyd: " << http_address << " is taken (" << listening.error().message
                      << "); agents with no copy of the music "
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
#if TRACKKNIFE_ENABLE_UPNP
    std::shared_ptr<trackknife::discovery::UpnpDiscovery> upnp;
    if (upnp_enabled) {
        if (!streams) {
            std::cerr << "melodyd: --upnp needs --http bound to a LAN address\n";
            return EXIT_FAILURE;
        }
        auto found = trackknife::discovery::UpnpDiscovery::start(
            [&outputs](const auto& renderer) { outputs.renderer_changed(renderer); },
            upnp_interface);
        if (!found) {
            std::cerr << found.error().message << "\n";
            return EXIT_FAILURE;
        }
        upnp = std::move(*found);
        outputs.configure_upnp(
            upnp,
            [&media, &transcodes, &streams](const trackknife::output::StreamRequest& request,
                                            const trackknife::discovery::UpnpRenderer& renderer)
                -> trackknife::core::Result<trackknife::output::RendererTrack> {
                using namespace trackknife;
                if (renderer.address.empty() || renderer.address.starts_with("127.")) {
                    return std::unexpected(
                        core::Error{.code = core::ErrorCode::unsupported,
                                    .message = "renderer has no reachable engine address",
                                    .context = {}});
                }
                auto probe = formats::probe_local_media(request.raw_path);
                if (!probe) {
                    return std::unexpected(probe.error());
                }
                std::int64_t duration = probe->duration_ms.value_or(0);
                auto mime = output::stream_content_type(request.raw_path);
                int source_sample_rate = 0;
                const auto selected_stream = request.selection.stream_index
                                                 ? request.selection.stream_index
                                                 : probe->best_audio_stream;
                if (selected_stream && *selected_stream >= 0) {
                    const auto found =
                        std::ranges::find(probe->audio_streams, *selected_stream,
                                          &formats::AudioStreamInfo::stream_index);
                    if (found != probe->audio_streams.end()) {
                        source_sample_rate = found->sample_rate;
                    }
                }
                const auto delivery =
                    output::renderer_compatible_request(request, renderer, source_sample_rate);
                if (delivery.format) {
                    auto converted =
                        transcodes->ensure(engine::TranscodeSource{.raw_path = delivery.raw_path,
                                                                   .selection = delivery.selection,
                                                                   .segment = delivery.segment},
                                           *delivery.format);
                    if (!converted) {
                        return std::unexpected(converted.error());
                    }
                    mime = output::stream_content_type(converted->native());
                    if (auto converted_probe = formats::probe_local_media(converted->native())) {
                        duration = converted_probe->duration_ms.value_or(duration);
                    }
                }
                const auto base = "http://" + renderer.address + ":" +
                                  std::to_string(streams->port()) + "/stream?";
                // Distinct URLs also distinguish two consecutive occurrences of the same song.
                const auto url = base + media->ticket(delivery, std::chrono::hours{1}) +
                                 "&occurrence=" + core::StableId::random().to_string();
                std::string title = std::filesystem::path{request.raw_path}.stem().string(), artist,
                            album, artwork;
                for (const auto& tag : probe->tags) {
                    auto name = tag.name;
                    std::ranges::transform(name, name.begin(), [](unsigned char c) {
                        return static_cast<char>(std::tolower(c));
                    });
                    if (name == "title") {
                        title = tag.value;
                    }
                    if (name == "artist") {
                        artist = tag.value;
                    }
                    if (name == "album") {
                        album = tag.value;
                    }
                }
                if (transcodes->artwork(request.raw_path)) {
                    output::StreamRequest cover{.raw_path = request.raw_path,
                                                .format = {},
                                                .selection = {},
                                                .segment = {},
                                                .artwork = true};
                    artwork = base + media->ticket(cover, std::chrono::hours{1});
                }
                return output::RendererTrack{
                    .source = request,
                    .url = url,
                    .metadata =
                        output::renderer_didl(url, mime, duration, title, artist, album, artwork),
                    .duration_ms = duration};
            });
    }
#endif
    outputs.restore();

    // Last.fm for what this engine plays, with its session handed over by a
    // client (lastfm.set_session); it scrobbles with every window closed.
    trackknife::engine::LastFm lastfm{*player, catalogue, state_directory / "lastfm.json"};
    trackknife::engine::register_lastfm_methods(dispatcher, lastfm);
    lastfm.start();

    // Another engine on these speakers, through an agent built in: newest
    // wins them (ADR-0228).
    // One agent each, with audio of its own; they share the speakers
    // through the arbiter as agents found on the network do.
    std::vector<std::pair<std::string, std::unique_ptr<trackknife::agent::Agent>>> guest_agents;
    std::unique_ptr<trackknife::agent::SpeakerArbiter> arbiter;
    for (const auto& [target, endpoint] : guest_endpoints) {
        auto audition = trackknife::audio::LocalAuditionService::create();
        if (!audition) {
            std::cerr << "melodyd: no audio here to play " << target.address
                      << " on: " << audition.error().message << "\n";
            continue;
        }
        static_cast<void>((*audition)->refresh_output_devices());
        auto made = trackknife::agent::Agent::create(
            trackknife::agent::AgentConfig{.server = endpoint,
                                           .name = engine_name,
                                           .music_root = target.music_root,
                                           .stream_only = !target.music_root.has_value(),
                                           .stream = stream_choice(target.nearby_kbps,
                                                                   target.away_kbps)},
            std::move(*audition));
        if (!made) {
            std::cerr << "melodyd: cannot play for " << target.address << ": "
                      << made.error().message << "\n";
            continue;
        }
        if (!arbiter) {
            arbiter = std::make_unique<trackknife::agent::SpeakerArbiter>(&*player);
        }
        arbiter->add_guest(target.name.empty() ? endpoint.describe() : target.name, **made);
        guest_agents.emplace_back(endpoint.describe(), std::move(*made));
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
                .password = !agent_password.empty()               ? agent_password
                            : !play_for_defaults.password.empty() ? play_for_defaults.password
                                                                  : password,
                .music_root = agent_music_root,
                .own_id = engine_id,
                .already =
                    [&play_fors] {
                        std::vector<std::string> addresses;
                        for (const auto& target : play_fors) {
                            addresses.push_back(target.address);
                        }
                        return addresses;
                    }(),
                .stream = stream_choice(agent_nearby_kbps, agent_away_kbps)},
            *arbiter);
    }
    // Listening on the network, it says so there: agents and clients find it
    // by name without being told where it is.
    std::unique_ptr<trackknife::discovery::Announcer> announcer;
    if (tcp_server) {
        auto announced =
            trackknife::discovery::Announcer::start(trackknife::discovery::Advertisement{
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
#if TRACKKNIFE_ENABLE_UPNP
    if (upnp) {
        upnp->begin();
    }
#endif
    if (guests && guests->start()) {
        std::cerr << "melodyd: playing for the engines on the network as \"" << engine_name
                  << "\"\n";
    }
    if (arbiter) {
        arbiter->start();
    }
    for (const auto& [described, agent] : guest_agents) {
        agent->start();
        std::cerr << "melodyd: playing for " << described << " as \"" << engine_name << "\"\n";
    }
    std::cerr << "melodyd: listening on " << socket_path.string() << "\n"
              << "melodyd: database " << database.string() << "\n";

    while (!stop_requested.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }

    std::cerr << "melodyd: stopping\n";
#if TRACKKNIFE_ENABLE_UPNP
    if (upnp) {
        upnp->stop();
    }
#endif
    announcer.reset();
    if (arbiter) {
        arbiter->stop();
    }
    if (guests) {
        guests->stop();
    }
    for (const auto& [described, agent] : guest_agents) {
        static_cast<void>(described);
        agent->stop();
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

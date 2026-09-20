// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/core/stable_id.hpp"
#include "trackknife/mpd/session.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

void require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::abort();
    }
}

void write_all(int socket, std::string_view text) {
    std::size_t offset = 0U;
    while (offset < text.size()) {
        const auto written =
            ::send(socket, text.data() + offset, text.size() - offset, MSG_NOSIGNAL);
        if (written <= 0) {
            return;
        }
        offset += static_cast<std::size_t>(written);
    }
}

class SessionServer final {
  public:
    SessionServer() {
        listen_socket_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        require(listen_socket_ >= 0, "session server socket must open");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        require(::bind(listen_socket_, reinterpret_cast<const sockaddr*>(&address),
                       sizeof(address)) == 0,
                "session server must bind");
        require(::listen(listen_socket_, 2) == 0, "session server must listen");
        socklen_t size = sizeof(address);
        require(::getsockname(listen_socket_, reinterpret_cast<sockaddr*>(&address), &size) == 0,
                "session server must report its port");
        port_ = ntohs(address.sin_port);
        accept_worker_ = std::thread([this] { accept_clients(); });
    }

    SessionServer(const SessionServer&) = delete;
    SessionServer& operator=(const SessionServer&) = delete;

    ~SessionServer() {
        stopping_.store(true, std::memory_order_release);
        static_cast<void>(::shutdown(listen_socket_, SHUT_RDWR));
        static_cast<void>(::close(listen_socket_));
        {
            std::lock_guard lock{sockets_mutex_};
            for (const auto socket : sockets_) {
                static_cast<void>(::shutdown(socket, SHUT_RDWR));
            }
        }
        if (accept_worker_.joinable()) {
            accept_worker_.join();
        }
        for (auto& worker : client_workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    [[nodiscard]] unsigned port() const noexcept { return port_; }
    [[nodiscard]] std::size_t transportCommandCount() const noexcept {
        return transport_commands_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::size_t queuePlayCommandCount() const noexcept {
        return queue_play_commands_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::size_t queueDeleteCommandCount() const noexcept {
        return queue_delete_commands_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::size_t queueClearCommandCount() const noexcept {
        return queue_clear_commands_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::size_t queueAddCommandCount() const noexcept {
        return queue_add_commands_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::size_t queueMoveCommandCount() const noexcept {
        return queue_move_commands_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::size_t queuePriorityCommandCount() const noexcept {
        return queue_priority_commands_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::size_t queueChangesCommandCount() const noexcept {
        return queue_changes_commands_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::size_t queueSnapshotCommandCount() const noexcept {
        return queue_snapshot_commands_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::size_t optionCommandCount() const noexcept {
        return option_commands_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::size_t searchCommandCount() const noexcept {
        return search_commands_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::size_t ratingExpressionSearchCount() const noexcept {
        return rating_expression_searches_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::size_t storedPlaylistMutationCount() const noexcept {
        return stored_playlist_mutations_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::size_t stickerSetCommandCount() const noexcept {
        return sticker_set_commands_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::size_t melodyRateCommandCount() const noexcept {
        return rate_commands_.load(std::memory_order_acquire);
    }
    void dropNextQueueAddResponse() {
        drop_next_queue_add_response_.store(true, std::memory_order_release);
    }
    void publishPausedState() {
        paused_.store(true, std::memory_order_release);
        notifyChanged("player");
    }
    void publishQueueAppend() {
        queue_version_.store(2U, std::memory_order_release);
        queue_length_.store(3U, std::memory_order_release);
        notifyChanged("playlist");
    }
    void publishMalformedQueueChange() {
        queue_version_.store(3U, std::memory_order_release);
        queue_length_.store(4U, std::memory_order_release);
        notifyChanged("playlist");
    }
    void disconnectClients() {
        std::lock_guard lock{sockets_mutex_};
        for (const auto socket : sockets_) {
            static_cast<void>(::shutdown(socket, SHUT_RDWR));
        }
    }

  private:
    void notifyChanged(std::string_view subsystem) {
        std::lock_guard lock{idle_mutex_};
        if (idle_waiting_) {
            write_all(idle_socket_, "changed: " + std::string{subsystem} + "\nOK\n");
            idle_waiting_ = false;
        } else {
            pending_subsystem_ = subsystem;
        }
    }

    void accept_clients() {
        while (!stopping_.load(std::memory_order_acquire)) {
            const int client = ::accept4(listen_socket_, nullptr, nullptr, SOCK_CLOEXEC);
            if (client < 0) {
                break;
            }
            {
                std::lock_guard lock{sockets_mutex_};
                sockets_.push_back(client);
            }
            client_workers_.emplace_back([this, client] { serve(client); });
        }
    }

    void serve(int client) {
        write_all(client, "OK MPD 0.24.2\n");
        std::string pending;
        bool command_list = false;
        std::array<char, 256> buffer{};
        while (!stopping_.load(std::memory_order_acquire)) {
            const auto count = ::recv(client, buffer.data(), buffer.size(), 0);
            if (count <= 0) {
                break;
            }
            pending.append(buffer.data(), static_cast<std::size_t>(count));
            auto newline = pending.find('\n');
            while (newline != std::string::npos) {
                const auto command = pending.substr(0, newline);
                pending.erase(0, newline + 1U);
                if (command == "idle") {
                    std::lock_guard lock{idle_mutex_};
                    if (!pending_subsystem_.empty()) {
                        write_all(client, "changed: " + pending_subsystem_ + "\nOK\n");
                        pending_subsystem_.clear();
                    } else {
                        idle_socket_ = client;
                        idle_waiting_ = true;
                    }
                } else if (command == "noidle") {
                    std::lock_guard lock{idle_mutex_};
                    if (idle_waiting_ && idle_socket_ == client) {
                        write_all(client, "OK\n");
                        idle_waiting_ = false;
                    }
                } else {
                    respond(client, command, command_list);
                }
                newline = pending.find('\n');
            }
        }
        static_cast<void>(::close(client));
    }

    void respond(int client, std::string_view command, bool& command_list) {
        if (command == "command_list_begin") {
            command_list = true;
        } else if (command == "command_list_end") {
            command_list = false;
            write_all(client, "OK\n");
        } else if (command == "commands") {
            write_all(client, "command: currentsong\ncommand: playlistinfo\ncommand: outputs\n"
                              "command: plchanges\ncommand: lsinfo\ncommand: search\n"
                              "command: listplaylists\ncommand: listplaylistinfo\n"
                              "command: replay_gain_status\ncommand: replay_gain_mode\n"
                              "command: prioid\n"
                              "command: sticker\ncommand: rate\ncommand: getrating\n"
                              "command: albumrate\ncommand: getalbumrating\n"
                              "command: idle\ncommand: noidle\nOK\n");
        } else if (command == "tagtypes") {
            write_all(client, "tagtype: Artist\ntagtype: MusicBrainzTrackId\nOK\n");
        } else if (command == "status") {
            write_all(client, "volume: 72\nrepeat: 0\nrandom: 0\nsingle: 0\nconsume: 0\n"
                              "playlist: ");
            write_all(client,
                      std::to_string(queue_version_.load(std::memory_order_acquire)) + "\n");
            write_all(client, "playlistlength: " +
                                  std::to_string(queue_length_.load(std::memory_order_acquire)) +
                                  "\nstate: ");
            write_all(client, paused_.load(std::memory_order_acquire) ? "pause\n" : "play\n");
            write_all(client, "songid: 7\nelapsed: 4.5\nduration: 220.9\nOK\n");
        } else if (command == "currentsong") {
            write_all(client, "file: Slayer/Divine Intervention/01.flac\nArtist: Slayer\n"
                              "Title: Killing Fields\nPos: 0\nId: 7\nOK\n");
        } else if (command == "playlistinfo") {
            queue_snapshot_commands_.fetch_add(1U, std::memory_order_release);
            write_all(client, "file: Slayer/Divine Intervention/01.flac\nTitle: Killing Fields\n"
                              "Pos: 0\nId: 7\nX-SongId: 501\nX-Rating: " +
                                  std::to_string(melody_rating_.load(std::memory_order_acquire)) +
                                  "\nfile: Slayer/Divine Intervention/02.flac\n"
                                  "Title: Sex. Murder. Art.\nPos: 1\nId: 9\n");
            if (queue_version_.load(std::memory_order_acquire) >= 3U) {
                write_all(client, "file: Slayer/Divine Intervention/03.flac\n"
                                  "Title: Fictional Reality\nPos: 2\nId: 11\n"
                                  "file: Slayer/Divine Intervention/04.flac\n"
                                  "Title: Dittohead\nPos: 3\nId: 12\n");
            }
            write_all(client, "OK\n");
        } else if (command == "plchanges \"1\"") {
            queue_changes_commands_.fetch_add(1U, std::memory_order_release);
            write_all(client, "file: Slayer/Divine Intervention/03.flac\n"
                              "Title: Fictional Reality\nPos: 2\nId: 11\nOK\n");
        } else if (command == "plchanges \"2\"") {
            queue_changes_commands_.fetch_add(1U, std::memory_order_release);
            write_all(client, "OK\n");
        } else if (command == "outputs") {
            write_all(client, "outputid: 0\noutputname: caprica\noutputenabled: 1\n"
                              "outputonline: 1\noutputprimary: 1\nplugin: agent\nOK\n");
        } else if (command == "lsinfo" || command == "lsinfo \"slow\"") {
            if (command != "lsinfo") {
                std::this_thread::sleep_for(std::chrono::milliseconds{150});
            }
            write_all(client, "directory: Slayer\n"
                              "file: loose.flac\nArtist: Slayer\nTitle: Loose track\n"
                              "playlist: Road mix\nOK\n");
        } else if (command.starts_with("search ")) {
            search_commands_.fetch_add(1U, std::memory_order_release);
            if (command.find("(rating >= 8)") != std::string_view::npos) {
                rating_expression_searches_.fetch_add(1U, std::memory_order_release);
            }
            write_all(client, "file: Slayer/Divine Intervention/01.flac\nArtist: Slayer\n"
                              "AlbumArtist: Slayer\nAlbum: Divine Intervention\nDate: 1994\n"
                              "MusicBrainzAlbumId: release-divine-intervention\n"
                              "Title: Killing Fields\nOK\n");
        } else if (command == "listplaylists") {
            write_all(client, "playlist: Road mix\nplaylist: Quiet mix\nOK\n");
        } else if (command == "listplaylistinfo \"Road mix\"") {
            write_all(client, "file: Slayer/Divine Intervention/02.flac\nArtist: Slayer\n"
                              "Title: Sex. Murder. Art.\nOK\n");
        } else if (command == "replay_gain_status") {
            write_all(client, "replay_gain_mode: track\nOK\n");
        } else if (command.starts_with("repeat ") || command.starts_with("random ") ||
                   command.starts_with("single ") || command.starts_with("consume ") ||
                   command.starts_with("replay_gain_mode ")) {
            option_commands_.fetch_add(1U, std::memory_order_release);
            write_all(client, "OK\n");
        } else if (command == "next") {
            transport_commands_.fetch_add(1U, std::memory_order_release);
            write_all(client, "OK\n");
        } else if (command == "playid \"9\"") {
            queue_play_commands_.fetch_add(1U, std::memory_order_release);
            write_all(client, "OK\n");
        } else if (command.starts_with("deleteid ")) {
            queue_delete_commands_.fetch_add(1U, std::memory_order_release);
            if (!command_list) {
                write_all(client, "OK\n");
            }
        } else if (command == "clear") {
            queue_clear_commands_.fetch_add(1U, std::memory_order_release);
            write_all(client, "OK\n");
        } else if (command.starts_with("addid ")) {
            queue_add_commands_.fetch_add(1U, std::memory_order_release);
            if (command == "addid \"ambiguous.flac\"" &&
                drop_next_queue_add_response_.exchange(false, std::memory_order_acq_rel)) {
                static_cast<void>(::shutdown(client, SHUT_RDWR));
                return;
            }
            if (!command_list) {
                write_all(client, "Id: 11\nOK\n");
            }
        } else if (command.starts_with("moveid \"999\"")) {
            queue_move_commands_.fetch_add(1U, std::memory_order_release);
            write_all(client, "ACK [50@0] {moveid} No such song\n");
        } else if (command.starts_with("moveid ")) {
            queue_move_commands_.fetch_add(1U, std::memory_order_release);
            if (!command_list) {
                write_all(client, "OK\n");
            }
        } else if (command.starts_with("prioid ")) {
            queue_priority_commands_.fetch_add(1U, std::memory_order_release);
            if (!command_list) {
                write_all(client, "OK\n");
            }
        } else if (command.starts_with("sticker \"set\" \"song\" ")) {
            sticker_set_commands_.fetch_add(1U, std::memory_order_release);
            const auto args = quoted_args(command);
            if (args.size() == 5U) {
                sticker_rating_.store(
                    static_cast<unsigned>(std::strtoul(args.back().c_str(), nullptr, 10)),
                    std::memory_order_release);
            }
            if (!command_list) {
                write_all(client, "OK\n");
            }
        } else if (command == "sticker \"find\" \"song\" \"\" \"rating\"") {
            write_all(client, "file: Slayer/Divine Intervention/01.flac\nsticker: rating=" +
                                  std::to_string(sticker_rating_.load(std::memory_order_acquire)) +
                                  "\nOK\n");
        } else if (command.starts_with("rate ")) {
            rate_commands_.fetch_add(1U, std::memory_order_release);
            const auto args = quoted_args(command);
            if (args.size() == 2U && args.front() == "501") {
                melody_rating_.store(
                    static_cast<unsigned>(std::strtoul(args.back().c_str(), nullptr, 10)),
                    std::memory_order_release);
            }
            if (!command_list) {
                write_all(client, "OK\n");
            }
        } else if (command.starts_with("albumrate ")) {
            write_all(client, "OK\n");
        } else if (command.starts_with("getalbumrating ")) {
            write_all(client, "rating: 8\ncomputed: 7.5\nOK\n");
        } else if (command.starts_with("save ") || command.starts_with("load ") ||
                   command.starts_with("playlistadd ") || command.starts_with("playlistdelete ") ||
                   command.starts_with("playlistmove ") || command.starts_with("playlistclear ") ||
                   command.starts_with("rename ") || command.starts_with("rm ")) {
            stored_playlist_mutations_.fetch_add(1U, std::memory_order_release);
            if (!command_list) {
                write_all(client, "OK\n");
            }
        } else {
            write_all(client, "ACK [5@0] {} unsupported fixture command\n");
        }
    }

    [[nodiscard]] static std::vector<std::string> quoted_args(std::string_view command) {
        std::vector<std::string> args;
        std::size_t position = 0U;
        while ((position = command.find('"', position)) != std::string_view::npos) {
            const auto end = command.find('"', position + 1U);
            if (end == std::string_view::npos) {
                break;
            }
            args.emplace_back(command.substr(position + 1U, end - position - 1U));
            position = end + 1U;
        }
        return args;
    }

    int listen_socket_{-1};
    unsigned port_{0U};
    std::atomic_bool stopping_{false};
    std::atomic_bool paused_{false};
    std::atomic_uint32_t queue_version_{1U};
    std::atomic_uint32_t queue_length_{2U};
    std::atomic_size_t transport_commands_{0U};
    std::atomic_size_t queue_play_commands_{0U};
    std::atomic_size_t queue_delete_commands_{0U};
    std::atomic_size_t queue_clear_commands_{0U};
    std::atomic_size_t queue_add_commands_{0U};
    std::atomic_size_t queue_move_commands_{0U};
    std::atomic_size_t queue_priority_commands_{0U};
    std::atomic_size_t queue_changes_commands_{0U};
    std::atomic_size_t queue_snapshot_commands_{0U};
    std::atomic_size_t option_commands_{0U};
    std::atomic_size_t search_commands_{0U};
    std::atomic_size_t rating_expression_searches_{0U};
    std::atomic_size_t stored_playlist_mutations_{0U};
    std::atomic_size_t sticker_set_commands_{0U};
    std::atomic_size_t rate_commands_{0U};
    std::atomic_uint sticker_rating_{6U};
    std::atomic_uint melody_rating_{4U};
    std::atomic_bool drop_next_queue_add_response_{false};
    std::mutex sockets_mutex_;
    std::vector<int> sockets_;
    std::mutex idle_mutex_;
    int idle_socket_{-1};
    bool idle_waiting_{false};
    std::string pending_subsystem_;
    std::thread accept_worker_;
    std::vector<std::thread> client_workers_;
};

void session_publishes_initial_and_idle_refreshed_snapshots() {
    SessionServer server;
    trackknife::mpd::Profile profile{
        .id = trackknife::core::StableId::random(),
        .name = "session fixture",
        .host = "127.0.0.1",
        .port = server.port(),
        .password = std::nullopt,
        .local_music_root = std::nullopt,
        .connect_timeout = std::chrono::milliseconds{1'000},
        .command_timeout = std::chrono::milliseconds{1'000},
    };

    std::mutex mutex;
    std::condition_variable changed;
    std::size_t snapshots = 0U;
    bool saw_connected = false;
    bool saw_player_idle = false;
    bool saw_queue_idle = false;
    bool command_finished = false;
    bool queue_play_finished = false;
    bool queue_delete_finished = false;
    bool queue_delete_batch_finished = false;
    bool queue_clear_finished = false;
    bool queue_add_finished = false;
    bool queue_add_batch_finished = false;
    bool ambiguous_add_failed = false;
    bool queue_move_finished = false;
    bool queue_move_batch_finished = false;
    bool queue_priority_finished = false;
    bool queue_conflict_finished = false;
    bool sticker_rating_finished = false;
    bool melody_rating_finished = false;
    bool melody_album_rate_finished = false;
    std::optional<trackknife::mpd::MelodyAlbumRating> melody_album_rating_payload;
    bool browse_finished = false;
    bool search_finished = false;
    bool invalid_search_failed = false;
    bool cancelled_search_finished = false;
    bool playlists_finished = false;
    bool playlist_finished = false;
    std::size_t stored_playlist_mutations_finished = 0U;
    std::size_t option_commands_finished = 0U;
    bool saw_reconnecting_error = false;
    trackknife::mpd::SessionSnapshot latest;

    {
        trackknife::mpd::Session session{
            std::move(profile),
            trackknife::mpd::SessionCallbacks{
                .state_changed =
                    [&](const trackknife::mpd::SessionState& state) {
                        std::lock_guard lock{mutex};
                        saw_connected = saw_connected ||
                                        state.phase == trackknife::mpd::SessionPhase::connected;
                        saw_reconnecting_error =
                            saw_reconnecting_error ||
                            (state.phase == trackknife::mpd::SessionPhase::reconnecting &&
                             state.error.has_value());
                        changed.notify_all();
                    },
                .snapshot_changed =
                    [&](const trackknife::mpd::SessionSnapshot& snapshot) {
                        std::lock_guard lock{mutex};
                        latest = snapshot;
                        ++snapshots;
                        changed.notify_all();
                    },
                .idle_received =
                    [&](trackknife::mpd::IdleEvents events) {
                        std::lock_guard lock{mutex};
                        saw_player_idle =
                            saw_player_idle || events.contains(trackknife::mpd::IdleEvent::player);
                        saw_queue_idle =
                            saw_queue_idle || events.contains(trackknife::mpd::IdleEvent::queue);
                        changed.notify_all();
                    },
                .command_finished =
                    [&](const trackknife::mpd::SessionCommandResult& result) {
                        std::lock_guard lock{mutex};
                        if (result.kind == trackknife::mpd::SessionCommandKind::transport) {
                            command_finished =
                                result.action == trackknife::mpd::TransportAction::next &&
                                !result.error;
                        } else if (result.kind == trackknife::mpd::SessionCommandKind::queue_play) {
                            queue_play_finished = !result.error;
                        } else if (result.kind ==
                                   trackknife::mpd::SessionCommandKind::queue_delete) {
                            queue_delete_finished = !result.error;
                        } else if (result.kind ==
                                   trackknife::mpd::SessionCommandKind::queue_delete_batch) {
                            queue_delete_batch_finished = !result.error;
                        } else if (result.kind ==
                                   trackknife::mpd::SessionCommandKind::queue_clear) {
                            queue_clear_finished = !result.error;
                        } else if (result.kind == trackknife::mpd::SessionCommandKind::queue_add) {
                            if (result.error) {
                                ambiguous_add_failed = true;
                            } else {
                                queue_add_finished = true;
                            }
                        } else if (result.kind ==
                                   trackknife::mpd::SessionCommandKind::queue_add_batch) {
                            queue_add_batch_finished = !result.error;
                        } else if (result.kind == trackknife::mpd::SessionCommandKind::queue_move) {
                            queue_move_finished = !result.error;
                            if (result.error &&
                                result.error->code == trackknife::core::ErrorCode::conflict) {
                                queue_conflict_finished = true;
                            }
                        } else if (result.kind ==
                                   trackknife::mpd::SessionCommandKind::queue_move_batch) {
                            queue_move_batch_finished = !result.error;
                        } else if (result.kind ==
                                   trackknife::mpd::SessionCommandKind::queue_priority) {
                            queue_priority_finished = !result.error;
                        } else if (result.kind ==
                                   trackknife::mpd::SessionCommandKind::sticker_rating) {
                            sticker_rating_finished = !result.error;
                        } else if (result.kind ==
                                   trackknife::mpd::SessionCommandKind::melody_rating) {
                            melody_rating_finished = !result.error;
                        } else if (result.kind ==
                                   trackknife::mpd::SessionCommandKind::melody_album_rate) {
                            melody_album_rate_finished = !result.error;
                        } else if (result.kind ==
                                   trackknife::mpd::SessionCommandKind::melody_album_rating) {
                            if (const auto* rating =
                                    std::get_if<trackknife::mpd::MelodyAlbumRating>(
                                        &result.payload);
                                rating != nullptr && !result.error) {
                                melody_album_rating_payload = *rating;
                            }
                        } else if (result.kind ==
                                   trackknife::mpd::SessionCommandKind::database_browse) {
                            const auto* entries =
                                std::get_if<std::vector<trackknife::mpd::DatabaseEntry>>(
                                    &result.payload);
                            browse_finished = !result.error && entries != nullptr &&
                                              entries->size() == 3U && result.generation > 0U;
                        } else if (result.kind ==
                                   trackknife::mpd::SessionCommandKind::database_search) {
                            if (result.error) {
                                invalid_search_failed =
                                    result.error->code ==
                                    trackknife::core::ErrorCode::invalid_argument;
                                cancelled_search_finished =
                                    cancelled_search_finished ||
                                    result.error->code == trackknife::core::ErrorCode::cancelled;
                            } else {
                                const auto* search =
                                    std::get_if<trackknife::mpd::LibrarySearchResult>(
                                        &result.payload);
                                search_finished =
                                    search != nullptr && search->tracks.size() == 1U &&
                                    search->albums.size() == 1U && result.generation > 0U;
                            }
                        } else if (result.kind ==
                                   trackknife::mpd::SessionCommandKind::stored_playlists) {
                            const auto* playlists =
                                std::get_if<std::vector<trackknife::mpd::StoredPlaylist>>(
                                    &result.payload);
                            playlists_finished = !result.error && playlists != nullptr &&
                                                 playlists->size() == 2U && result.generation > 0U;
                        } else if (result.kind ==
                                   trackknife::mpd::SessionCommandKind::stored_playlist) {
                            const auto* tracks =
                                std::get_if<std::vector<trackknife::mpd::Track>>(&result.payload);
                            playlist_finished = !result.error && tracks != nullptr &&
                                                tracks->size() == 1U && result.generation > 0U;
                        } else if (
                            result.kind ==
                                trackknife::mpd::SessionCommandKind::stored_playlist_save ||
                            result.kind ==
                                trackknife::mpd::SessionCommandKind::stored_playlist_load ||
                            result.kind ==
                                trackknife::mpd::SessionCommandKind::stored_playlist_add ||
                            result.kind ==
                                trackknife::mpd::SessionCommandKind::stored_playlist_add_batch ||
                            result.kind ==
                                trackknife::mpd::SessionCommandKind::stored_playlist_delete_item ||
                            result.kind ==
                                trackknife::mpd::SessionCommandKind::stored_playlist_delete_batch ||
                            result.kind ==
                                trackknife::mpd::SessionCommandKind::stored_playlist_move_item ||
                            result.kind ==
                                trackknife::mpd::SessionCommandKind::stored_playlist_clear ||
                            result.kind ==
                                trackknife::mpd::SessionCommandKind::stored_playlist_rename ||
                            result.kind ==
                                trackknife::mpd::SessionCommandKind::stored_playlist_remove) {
                            if (!result.error) {
                                ++stored_playlist_mutations_finished;
                            }
                        } else if (result.kind == trackknife::mpd::SessionCommandKind::repeat ||
                                   result.kind == trackknife::mpd::SessionCommandKind::random ||
                                   result.kind == trackknife::mpd::SessionCommandKind::single ||
                                   result.kind == trackknife::mpd::SessionCommandKind::consume ||
                                   result.kind ==
                                       trackknife::mpd::SessionCommandKind::replay_gain) {
                            if (!result.error) {
                                ++option_commands_finished;
                            }
                        }
                        changed.notify_all();
                    },
            }};

        std::unique_lock lock{mutex};
        const auto ready = changed.wait_for(lock, std::chrono::seconds{2}, [&] {
            return saw_connected && snapshots >= 1U && latest.queue.size() == 2U &&
                   latest.outputs.size() == 1U;
        });
        require(ready, "session must publish a connected snapshot and react to idle events");
        require(latest.generation > 0U, "session snapshots must carry a generation");
        require(latest.status_sample_time.has_value(),
                "playback status must carry its sampling time for elapsed projection");
        require(latest.status.state == trackknife::mpd::PlaybackState::playing &&
                    latest.status.song_id == 7U,
                "session snapshot must retain typed playback status");
        require(latest.outputs.front().primary == true,
                "session snapshot must retain Melody output state");
        require(latest.replay_gain_mode == trackknife::mpd::ReplayGainMode::track,
                "session snapshots must include advertised ReplayGain state");
        lock.unlock();

        const auto idle_change_started = std::chrono::steady_clock::now();
        server.publishPausedState();
        lock.lock();
        const auto idle_refreshed = changed.wait_for(lock, std::chrono::seconds{2}, [&] {
            return saw_player_idle && latest.status.state == trackknife::mpd::PlaybackState::paused;
        });
        const auto idle_latency = std::chrono::steady_clock::now() - idle_change_started;
        require(idle_refreshed, "player idle must publish an authoritative status snapshot");
        require(idle_latency < std::chrono::milliseconds{250},
                "player idle refresh must not wait for a polling interval");
        lock.unlock();

        server.publishQueueAppend();
        lock.lock();
        const auto queue_diffed = changed.wait_for(lock, std::chrono::seconds{2}, [&] {
            return saw_queue_idle && latest.queue.size() == 3U &&
                   latest.queue.back().queue_id == 11U;
        });
        require(queue_diffed, "queue idle must apply a compatible versioned plchanges response");
        require(server.queueChangesCommandCount() == 1U && server.queueSnapshotCommandCount() == 1U,
                "compatible queue changes must avoid a second full playlist snapshot");
        lock.unlock();

        server.publishMalformedQueueChange();
        lock.lock();
        const auto queue_fell_back = changed.wait_for(lock, std::chrono::seconds{2}, [&] {
            return latest.queue.size() == 4U && latest.queue.back().queue_id == 12U;
        });
        require(queue_fell_back, "an incomplete queue diff must fall back to playlistinfo");
        require(server.queueChangesCommandCount() == 2U && server.queueSnapshotCommandCount() == 2U,
                "a malformed queue change must perform exactly one full fallback");
        lock.unlock();

        static_cast<void>(session.run_transport(trackknife::mpd::TransportAction::next));
        lock.lock();
        const auto transported = changed.wait_for(lock, std::chrono::seconds{2}, [&] {
            return command_finished && server.transportCommandCount() == 1U;
        });
        require(transported, "session must serialize transport commands exactly once");
        lock.unlock();

        static_cast<void>(session.play_queue_id(9U));
        static_cast<void>(session.add_queue_uri("Slayer/Divine Intervention/03.flac"));
        static_cast<void>(session.delete_queue_id(9U));
        static_cast<void>(session.move_queue_id(9U, 0U));
        lock.lock();
        const auto queue_mutated = changed.wait_for(lock, std::chrono::seconds{2}, [&] {
            return queue_play_finished && queue_add_finished && queue_delete_finished &&
                   queue_move_finished && server.queuePlayCommandCount() == 1U &&
                   server.queueAddCommandCount() == 1U && server.queueDeleteCommandCount() == 1U &&
                   server.queueMoveCommandCount() == 1U;
        });
        require(queue_mutated, "session must serialize stable-ID queue commands exactly once");
        lock.unlock();

        static_cast<void>(session.move_queue_ids({
            trackknife::mpd::QueueMove{.song_id = 7U, .position = 1U},
            trackknife::mpd::QueueMove{.song_id = 9U, .position = 0U},
        }));
        lock.lock();
        const auto queue_batch_moved = changed.wait_for(lock, std::chrono::seconds{2}, [&] {
            return queue_move_batch_finished && server.queueMoveCommandCount() == 3U;
        });
        require(queue_batch_moved,
                "multi-selection reorder must serialize one stable-ID command list exactly once");
        lock.unlock();

        const auto snapshots_before_conflict = snapshots;
        static_cast<void>(session.move_queue_id(999U, 0U));
        lock.lock();
        const auto queue_conflict_refreshed = changed.wait_for(lock, std::chrono::seconds{2}, [&] {
            return queue_conflict_finished && snapshots > snapshots_before_conflict;
        });
        require(queue_conflict_refreshed,
                "a stale queue ID must report a conflict and publish an authoritative refresh");
        lock.unlock();

        static_cast<void>(session.set_queue_priority({7U, 9U}, 192U));
        lock.lock();
        const auto queue_priority_set = changed.wait_for(lock, std::chrono::seconds{2}, [&] {
            return queue_priority_finished && server.queuePriorityCommandCount() == 2U;
        });
        require(queue_priority_set,
                "multi-selection priority must serialize one stable-ID command list exactly once");
        require(latest.sticker_ratings ==
                    std::vector<trackknife::mpd::TrackRating>{
                        {.uri = "Slayer/Divine Intervention/01.flac", .rating = 6U}},
                "a sticker-capable snapshot must include the bulk rating load");
        require(latest.queue.front().rating == 4U && latest.queue.front().melody_song_id == 501U,
                "queue snapshots must retain Melody rating lines");
        lock.unlock();

        static_cast<void>(session.set_sticker_ratings({"Slayer/Divine Intervention/01.flac"}, 9U));
        lock.lock();
        const auto sticker_rated = changed.wait_for(lock, std::chrono::seconds{2}, [&] {
            return sticker_rating_finished && server.stickerSetCommandCount() == 1U &&
                   !latest.sticker_ratings.empty() && latest.sticker_ratings.front().rating == 9U;
        });
        require(sticker_rated,
                "a sticker rating must serialize once and refresh the bulk rating snapshot");
        lock.unlock();

        static_cast<void>(session.set_melody_track_ratings({501U}, 7U));
        lock.lock();
        const auto melody_rated = changed.wait_for(lock, std::chrono::seconds{2}, [&] {
            return melody_rating_finished && server.melodyRateCommandCount() == 1U &&
                   latest.queue.front().rating == 7U;
        });
        require(melody_rated, "a Melody rating must bypass the queue-version shortcut so the "
                              "snapshot reflects the new listing rating");
        lock.unlock();

        static_cast<void>(session.set_melody_album_rating(
            trackknife::mpd::MelodyAlbumKey{
                .album_artist = "Slayer", .album = "Divine Intervention", .date = "1994"},
            8U));
        static_cast<void>(session.melody_album_rating(trackknife::mpd::MelodyAlbumKey{
            .album_artist = "Slayer", .album = "Divine Intervention", .date = "1994"}));
        lock.lock();
        const auto album_rated = changed.wait_for(lock, std::chrono::seconds{2}, [&] {
            return melody_album_rate_finished && melody_album_rating_payload.has_value();
        });
        require(album_rated && melody_album_rating_payload->rating == 8U &&
                    melody_album_rating_payload->computed == 7.5,
                "Melody album ratings must round-trip stored and computed values");
        lock.unlock();

        static_cast<void>(session.add_queue_uris({
            trackknife::mpd::QueueAddition{.uri = "batch-1.flac", .position = 1U},
            trackknife::mpd::QueueAddition{.uri = "batch-2.flac", .position = 2U},
        }));
        lock.lock();
        const auto queue_batch_added = changed.wait_for(lock, std::chrono::seconds{2}, [&] {
            return queue_add_batch_finished && server.queueAddCommandCount() == 3U;
        });
        require(queue_batch_added,
                "multi-selection addition must serialize one ordered command list exactly once");
        lock.unlock();

        static_cast<void>(session.delete_queue_ids({7U, 9U}));
        lock.lock();
        const auto queue_batch_deleted = changed.wait_for(lock, std::chrono::seconds{2}, [&] {
            return queue_delete_batch_finished && server.queueDeleteCommandCount() == 3U;
        });
        require(queue_batch_deleted,
                "multi-selection deletion must serialize one stable-ID command list exactly once");
        lock.unlock();

        static_cast<void>(session.clear_queue());
        lock.lock();
        const auto queue_cleared = changed.wait_for(lock, std::chrono::seconds{2}, [&] {
            return queue_clear_finished && server.queueClearCommandCount() == 1U;
        });
        require(queue_cleared, "queue clear must serialize exactly once");
        lock.unlock();

        static_cast<void>(session.set_repeat(true));
        static_cast<void>(session.set_random(true));
        static_cast<void>(session.set_single(trackknife::mpd::PlaybackModeState::oneshot));
        static_cast<void>(session.set_consume(trackknife::mpd::PlaybackModeState::on));
        static_cast<void>(session.set_replay_gain_mode(trackknife::mpd::ReplayGainMode::album));
        lock.lock();
        const auto options_changed = changed.wait_for(lock, std::chrono::seconds{2}, [&] {
            return option_commands_finished == 5U && server.optionCommandCount() == 5U;
        });
        require(options_changed,
                "playback and ReplayGain modes must serialize exactly once on the session worker");
        lock.unlock();

        static_cast<void>(session.browse());
        static_cast<void>(session.search_any("Slayer", 0U, 50U));
        static_cast<void>(session.list_stored_playlists());
        static_cast<void>(session.load_stored_playlist("Road mix"));
        lock.lock();
        const auto queried = changed.wait_for(lock, std::chrono::seconds{2}, [&] {
            return browse_finished && search_finished && playlists_finished && playlist_finished;
        });
        require(
            queried,
            "browse, bounded search, and stored-playlist reads must stay on the session worker");
        lock.unlock();

        static_cast<void>(session.save_queue_as_playlist("New mix"));
        static_cast<void>(session.load_stored_playlist_into_queue("Road mix"));
        static_cast<void>(session.add_to_stored_playlist("Road mix", "Slayer/Seasons/01.flac", 1U));
        static_cast<void>(session.add_to_stored_playlist(
            "Road mix",
            std::vector<std::string>{"Slayer/Seasons/02.flac", "Slayer/Seasons/03.flac"}, 2U));
        static_cast<void>(session.delete_from_stored_playlist("Road mix", 1U));
        static_cast<void>(
            session.delete_from_stored_playlist("Road mix", std::vector<unsigned>{4U, 2U}));
        static_cast<void>(session.move_in_stored_playlist("Road mix", 0U, 1U));
        static_cast<void>(session.clear_stored_playlist("Quiet mix"));
        static_cast<void>(session.rename_stored_playlist("New mix", "Renamed mix"));
        static_cast<void>(session.delete_stored_playlist("Renamed mix"));
        lock.lock();
        const auto playlists_mutated = changed.wait_for(lock, std::chrono::seconds{2}, [&] {
            return stored_playlist_mutations_finished == 10U &&
                   server.storedPlaylistMutationCount() == 12U;
        });
        require(playlists_mutated,
                "stored-playlist mutations must be serialized exactly once on the session worker");
        lock.unlock();

        static_cast<void>(session.browse("slow"));
        const auto cancelled_search = session.search_any("superseded", 0U, 50U);
        session.cancel_pending(cancelled_search);
        lock.lock();
        const auto cancellation_reported = changed.wait_for(
            lock, std::chrono::seconds{2}, [&] { return cancelled_search_finished; });
        require(cancellation_reported && server.searchCommandCount() == 1U,
                "a superseded pending search must finish as cancelled without reaching MPD");
        const auto generation_before_invalid_query = latest.generation;
        lock.unlock();

        static_cast<void>(session.search_any("", 0U, 50U));
        lock.lock();
        const auto invalid_query_rejected =
            changed.wait_for(lock, std::chrono::seconds{2}, [&] { return invalid_search_failed; });
        require(invalid_query_rejected, "invalid asynchronous queries must report typed errors");
        lock.unlock();

        static_cast<void>(session.search_any("Slayer rating>=8", 0U, 50U));
        lock.lock();
        const auto rating_searched = changed.wait_for(lock, std::chrono::seconds{2}, [&] {
            return server.ratingExpressionSearchCount() == 1U;
        });
        require(rating_searched, "a server advertising Melody ratings must receive rating "
                                 "search terms as a filter expression");
        lock.unlock();

        static_cast<void>(session.run_transport(trackknife::mpd::TransportAction::next));
        lock.lock();
        const auto connection_remained_usable = changed.wait_for(
            lock, std::chrono::seconds{2}, [&] { return server.transportCommandCount() == 2U; });
        require(connection_remained_usable && latest.generation == generation_before_invalid_query,
                "a local query validation error must not reconnect a healthy MPD session");
        const auto generation_before_idle_disconnect = latest.generation;
        lock.unlock();

        server.disconnectClients();
        lock.lock();
        const auto autonomously_reconnected = changed.wait_for(lock, std::chrono::seconds{3}, [&] {
            return latest.generation > generation_before_idle_disconnect;
        });
        require(autonomously_reconnected,
                "losing an idle connection must wake and reconnect the sleeping command session");
        const auto generation_before_disconnect = latest.generation;
        saw_reconnecting_error = false;
        lock.unlock();

        const auto additions_before_ambiguous_command = server.queueAddCommandCount();
        server.dropNextQueueAddResponse();
        static_cast<void>(session.add_queue_uri("ambiguous.flac"));
        lock.lock();
        const auto ambiguous_failed = changed.wait_for(lock, std::chrono::seconds{4}, [&] {
            return ambiguous_add_failed && saw_reconnecting_error;
        });
        if (!ambiguous_failed) {
            std::cerr << "diagnostic: add_failed=" << ambiguous_add_failed
                      << " reconnecting_error=" << saw_reconnecting_error << " adds_sent="
                      << (server.queueAddCommandCount() - additions_before_ambiguous_command)
                      << '\n';
        }
        require(ambiguous_failed,
                "a disconnected non-idempotent queue command must report an ambiguous failure");
        require(server.queueAddCommandCount() == additions_before_ambiguous_command + 1U,
                "an ambiguous queue add must have been sent only once");

        const auto reconnected = changed.wait_for(lock, std::chrono::seconds{4}, [&] {
            return latest.generation > generation_before_disconnect;
        });
        require(reconnected, "the read session must publish a newer snapshot after reconnecting");
        require(server.queueAddCommandCount() == additions_before_ambiguous_command + 1U,
                "reconnect must never replay the ambiguous queue add");
        lock.unlock();

        static_cast<void>(session.run_transport(trackknife::mpd::TransportAction::next));
        lock.lock();
        const auto post_reconnect_command = changed.wait_for(
            lock, std::chrono::seconds{2}, [&] { return server.transportCommandCount() == 3U; });
        require(post_reconnect_command,
                "the session must accept fresh commands after an authoritative reconnect snapshot");
    }
}

} // namespace

int main() {
    session_publishes_initial_and_idle_refreshed_snapshots();
    return EXIT_SUCCESS;
}

// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/stable_id.hpp"
#include "trackknife/mpd/client.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>

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

class FakeMpdServer final {
  public:
    FakeMpdServer() {
        listen_socket_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        require(listen_socket_ >= 0, "fake server socket must open");

        const int reuse = 1;
        require(::setsockopt(listen_socket_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == 0,
                "fake server must set SO_REUSEADDR");

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        require(::bind(listen_socket_, reinterpret_cast<const sockaddr*>(&address),
                       sizeof(address)) == 0,
                "fake server must bind");
        require(::listen(listen_socket_, 1) == 0, "fake server must listen");

        socklen_t size = sizeof(address);
        require(::getsockname(listen_socket_, reinterpret_cast<sockaddr*>(&address), &size) == 0,
                "fake server must report its port");
        port_ = ntohs(address.sin_port);
        worker_ = std::thread([this] { serve(); });
    }

    FakeMpdServer(const FakeMpdServer&) = delete;
    FakeMpdServer& operator=(const FakeMpdServer&) = delete;

    ~FakeMpdServer() {
        if (client_socket_.load() >= 0) {
            static_cast<void>(::shutdown(client_socket_.load(), SHUT_RDWR));
        }
        static_cast<void>(::shutdown(listen_socket_, SHUT_RDWR));
        static_cast<void>(::close(listen_socket_));
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    [[nodiscard]] unsigned port() const noexcept { return port_; }

  private:
    void serve() {
        const int client = ::accept4(listen_socket_, nullptr, nullptr, SOCK_CLOEXEC);
        if (client < 0) {
            return;
        }
        client_socket_.store(client);
        write_all(client, "OK MPD 0.24.2\n");

        std::string pending;
        bool command_list = false;
        std::array<char, 256> buffer{};
        while (true) {
            const auto count = ::recv(client, buffer.data(), buffer.size(), 0);
            if (count <= 0) {
                break;
            }
            pending.append(buffer.data(), static_cast<std::size_t>(count));
            auto newline = pending.find('\n');
            while (newline != std::string::npos) {
                const auto command = pending.substr(0, newline);
                pending.erase(0, newline + 1U);
                respond(client, command, command_list);
                newline = pending.find('\n');
            }
        }
        static_cast<void>(::close(client));
        client_socket_.store(-1);
    }

    static void respond(int client, std::string_view command, bool& command_list) {
        if (command == "command_list_begin") {
            command_list = true;
        } else if (command == "command_list_end") {
            command_list = false;
            write_all(client, "OK\n");
        } else if (command == "commands") {
            write_all(client, "command: status\ncommand: currentsong\ncommand: playlistinfo\n"
                              "command: plchanges\ncommand: outputs\ncommand: switchoutput\n"
                              "command: prioid\n"
                              "command: sticker\ncommand: rate\ncommand: getrating\n"
                              "command: albumrate\ncommand: getalbumrating\n"
                              "command: lsinfo\ncommand: search\ncommand: listplaylists\n"
                              "command: listplaylistinfo\ncommand: replay_gain_status\n"
                              "command: replay_gain_mode\ncommand: ping\nOK\n");
        } else if (command == "tagtypes") {
            write_all(client, "tagtype: Artist\ntagtype: MusicBrainzTrackId\n"
                              "tagtype: MusicBrainzReleaseGroupId\nOK\n");
        } else if (command == "status") {
            write_all(client, "volume: 72\nrepeat: 0\nrandom: 0\nsingle: 0\nconsume: 0\n"
                              "playlist: 43\nplaylistlength: 2\nstate: play\nsongid: 7\n"
                              "elapsed: 12.5\nduration: 123.5\nOK\n");
        } else if (command == "currentsong") {
            write_all(client,
                      "file: Artist/Release/01.flac\nArtist: Credited Artist\n"
                      "MusicBrainzTrackId: recording-id\nPos: 0\nId: 7\nduration: 123.5\nOK\n");
        } else if (command == "playlistinfo") {
            write_all(client, "file: Artist/Release/01.flac\nTitle: One\nPos: 0\nId: 7\nPrio: 192\n"
                              "file: Artist/Release/02.flac\nTitle: Two\nPos: 1\nId: 9\nOK\n");
        } else if (command == "plchanges \"43\"") {
            write_all(client, "file: Artist/Release/02.flac\nTitle: Two\nPos: 0\nId: 9\nOK\n");
        } else if (command == "outputs") {
            write_all(client, "outputid: 0\noutputname: local\noutputenabled: 1\n"
                              "outputprimary: 1\noutputonline: 1\nplugin: server\n"
                              "outputformat: flac\nOK\n");
        } else if (command == "lsinfo") {
            write_all(client, "directory: Artist\nLast-Modified: 2026-08-24T10:00:00Z\n"
                              "file: loose.flac\nArtist: Loose Artist\nTitle: Loose track\n"
                              "playlist: Road mix\nLast-Modified: 2026-08-24T11:00:00Z\nOK\n");
        } else if (command.starts_with("list ") &&
                   command.find("group") != std::string_view::npos) {
            // Nested ordering captured from stock MPD 0.24: the last group
            // is outermost. Repeated release IDs count once across dates.
            write_all(client,
                      "MUSICBRAINZ_ALBUMID: \nDate: 2020\nAlbumArtist: A\nAlbum: Same\n"
                      "Date: 2021\nAlbumArtist: A\nAlbum: Same\n"
                      "MUSICBRAINZ_ALBUMID: release-b\nDate: 2022\nAlbumArtist: B\nAlbum: Second\n"
                      "Date: 2023\nAlbumArtist: B\nAlbum: Alternate title\nOK\n");
        } else if (command.starts_with("list ")) {
            write_all(client, "AlbumArtist: Credited Artist\nAlbumArtist: Various Artists\nOK\n");
        } else if (command.starts_with("search ") &&
                   command.find("genre contains") != std::string_view::npos) {
            // Server-translated structured queries arrive as one expression
            // with the requested sort and window.
            if (command.find("sort -date") == std::string_view::npos ||
                command.find("window 0:500") == std::string_view::npos) {
                write_all(client, "ACK [2@0] {search} expected sorted windowed expression\n");
                return;
            }
            write_all(client, "file: Jazz/A/01.flac\nArtist: Jazz Artist\n"
                              "Title: Structured hit\nOK\n");
        } else if (command.starts_with("searchalbums ")) {
            if (command.find("albumrating >= 8") == std::string_view::npos) {
                write_all(client, "ACK [2@0] {searchalbums} expected album rating filter\n");
                return;
            }
            write_all(client, "AlbumArtist: Rated Artist\nAlbum: Rated Release\nDate: 1994\n"
                              "X-AlbumId: 12\nX-TrackCount: 10\nX-Duration: 2400\nX-Rating: 8\n"
                              "X-ComputedRating: 7.5\nX-ArtworkUri: Rated/A/01.flac\n"
                              "AlbumArtist: Sparse Artist\nAlbum: Undated\nDate: 0000\n"
                              "X-AlbumId: 13\nX-TrackCount: 1\nX-Duration: 60\nOK\n");
        } else if (command.starts_with("search ") &&
                   command.find("albumrating >= 8") != std::string_view::npos) {
            write_all(client, "file: Rated/A/01.flac\nAlbumArtist: Rated Artist\n"
                              "Album: Rated Release\nTitle: Rated track\nOK\n");
        } else if (command.starts_with("search ") &&
                   command.find("rating >= 8") != std::string_view::npos) {
            // ADR-0179: rating search terms must arrive as a Melody filter
            // expression combined with the residual free-text words.
            if (command.find("any contains") == std::string_view::npos ||
                command.find("beatles") == std::string_view::npos) {
                write_all(client, "ACK [2@0] {search} expected combined rating filter\n");
                return;
            }
            write_all(client, "file: Rated/A/01.flac\nAlbumArtist: Rated Artist\n"
                              "Album: Rated Release\nTitle: Rated track\nX-Rating: 9\nOK\n");
        } else if (command == R"(melody_lastfm love "Björk & A" "A + B")") {
            write_all(client, "lastfm: {\"loved\":true}\nOK\n");
        } else if (command == "melody_upnext") {
            write_all(client, "revision: 42\nactive: 12\ncontext: Road\nfile: A.flac\nTitle: "
                              "A\nPos: 0\nId: 20\nfile: A.flac\nTitle: A\nPos: 1\nId: 21\nOK\n");
        } else if (command == "melody_context stage" ||
                   command == "melody_context stage \"A.flac\"") {
            write_all(client, "OK\n");
        } else if (command == "melody_upnext append 42" ||
                   command == "melody_upnext move 42 21 0" ||
                   command == "melody_upnext return 42" ||
                   command == "melody_upnext_edit 42 21 20") {
            write_all(client, "OK\n");
        } else if (command == "melody_upnext clear 41") {
            write_all(client,
                      "ACK [2@0] {melody_upnext} request queue changed; refresh before retrying\n");
        } else if (command == "melody_albums_latest") {
            write_all(client, "X-Album: 3\tFresh Artist\tNew album\t2026\n"
                              "X-Album: 2\tFresh Artist\tAnother album\t2025\n"
                              "X-Album: 1\tOld Artist\tOld album\t1990\nOK\n");
        } else if (command.starts_with("search ")) {
            if (command.find("sort") != std::string_view::npos) {
                // The banner advertises MPD 0.24, so the newest lookup must
                // sort by database insertion time, newest first.
                if (command.find("-Added") == std::string_view::npos) {
                    write_all(client, "ACK [2@0] {search} expected sort -Added\n");
                    return;
                }
                write_all(client, "file: Fresh/A/01.flac\nAlbumArtist: Fresh Artist\n"
                                  "file: Fresh/A/02.flac\nAlbumArtist: Fresh Artist\n"
                                  "file: Mid/B/01.flac\nAlbumArtist: Middle Artist\n"
                                  "file: Old/C/01.flac\nAlbumArtist: Old Artist\nOK\n");
            } else if (command.find("window") != std::string_view::npos) {
                write_all(client, "file: Artist/Release/01.flac\nArtist: Credited Artist\n"
                                  "Title: Search one\nMusicBrainzTrackId: recording-id\n"
                                  "file: Artist/Release/02.flac\nArtist: Credited Artist\n"
                                  "Title: Search two\nOK\n");
            } else {
                write_all(client, "file: Artist/Early/01.flac\nAlbumArtist: Credited Artist\n"
                                  "Album: Early Release\nDate: 1998\n"
                                  "MusicBrainzAlbumId: release-early\nTitle: First\n"
                                  "file: Artist/Early/02.flac\nAlbumArtist: Credited Artist\n"
                                  "Album: Early Release\nDate: 1998\n"
                                  "MusicBrainzAlbumId: release-early\nTitle: Second\n"
                                  "file: Artist/Later/01.flac\nAlbumArtist: Credited Artist\n"
                                  "Album: Later Release\nDate: 2001\n"
                                  "MusicBrainzAlbumId: release-later\nTitle: Third\nOK\n");
            }
        } else if (command.starts_with("find ")) {
            write_all(client, "file: Artist/Release/01.flac\nAlbumArtist: Credited Artist\n"
                              "Album: Complete Release\nTrack: 1\nTitle: First\n"
                              "file: Artist/Release/02.flac\nAlbumArtist: Credited Artist\n"
                              "Album: Complete Release\nTrack: 2\nTitle: Second\nOK\n");
        } else if (command.starts_with("albumart ") || command.starts_with("readpicture ")) {
            constexpr std::string_view artwork{"0123456789ABCDEFGHIJ"};
            constexpr std::size_t binary_limit = 8U;
            const auto separator = command.find_last_of(' ');
            require(separator != std::string_view::npos, "artwork command must include an offset");
            auto offset_text = command.substr(separator + 1U);
            if (offset_text.size() >= 2U && offset_text.front() == '"' &&
                offset_text.back() == '"') {
                offset_text.remove_prefix(1U);
                offset_text.remove_suffix(1U);
            }
            const std::string offset_storage{offset_text};
            char* end = nullptr;
            const auto offset = std::strtoul(offset_storage.c_str(), &end, 10);
            require(end != nullptr && *end == '\0' && offset <= artwork.size(),
                    "artwork offset must advance within the fixture");
            const auto count = std::min(binary_limit, artwork.size() - offset);
            write_all(client, "size: " + std::to_string(artwork.size()) +
                                  "\nbinary: " + std::to_string(count) + "\n");
            write_all(client, artwork.substr(offset, count));
            write_all(client, "\nOK\n");
        } else if (command == "listplaylists") {
            write_all(client, "playlist: Road mix\nLast-Modified: 2026-08-24T11:00:00Z\n"
                              "playlist: Quiet mix\nOK\n");
        } else if (command == "listplaylistinfo \"Road mix\"") {
            write_all(client, "file: Artist/Release/02.flac\nArtist: Credited Artist\n"
                              "Title: Search two\nOK\n");
        } else if (command == "replay_gain_status") {
            write_all(client, "replay_gain_mode: track\nOK\n");
        } else if (command.starts_with("addid ")) {
            if (!command_list) {
                write_all(client, "Id: 11\nOK\n");
            }
        } else if (command == "deleteid \"999\"") {
            write_all(client, "ACK [50@0] {deleteid} No such song\n");
        } else if (command.starts_with("deleteid ") && command_list) {
            return;
        } else if (command.starts_with("moveid ") && command_list) {
            return;
        } else if (command.starts_with("prioid ") && command_list) {
            return;
        } else if (command.starts_with("playlistdelete ") && command_list) {
            return;
        } else if (command.starts_with("playlistadd ") && command_list) {
            return;
        } else if (command.starts_with("play ") || command.starts_with("playid ") ||
                   command.starts_with("deleteid ") || command.starts_with("moveid ") ||
                   command.starts_with("seekid ") || command.starts_with("prioid ") ||
                   command.starts_with("setvol ") || command.starts_with("enableoutput ") ||
                   command.starts_with("switchoutput ") || command.starts_with("repeat ") ||
                   command.starts_with("random ") || command.starts_with("single ") ||
                   command.starts_with("consume ") || command.starts_with("replay_gain_mode ") ||
                   command.starts_with("save ") || command.starts_with("load ") ||
                   command.starts_with("playlistadd ") || command.starts_with("playlistdelete ") ||
                   command.starts_with("playlistmove ") || command.starts_with("playlistclear ") ||
                   command.starts_with("rename ") || command.starts_with("rm ") ||
                   command == "clear") {
            write_all(client, "OK\n");
        } else if (command == "sticker \"delete\" \"song\" \"unrated.flac\" \"rating\"") {
            write_all(client, "ACK [50@0] {sticker} no such sticker\n");
        } else if (command.starts_with("sticker \"set\" \"song\" ") && command_list) {
            return;
        } else if (command.starts_with("sticker \"set\" \"song\" ") ||
                   command.starts_with("sticker \"delete\" \"song\" ")) {
            write_all(client, "OK\n");
        } else if (command == "sticker \"find\" \"song\" \"\" \"rating\"") {
            write_all(client, "file: Artist/Release/01.flac\nsticker: rating=8\n"
                              "file: foreign.flac\nsticker: rating=4.5\nOK\n");
        } else if (command.starts_with("rate ") && command_list) {
            return;
        } else if (command.starts_with("rate ") || command.starts_with("albumrate ")) {
            write_all(client, "OK\n");
        } else if (command.starts_with("getalbumrating ")) {
            write_all(client, "rating: 8\ncomputed: 7.5\nOK\n");
        } else if (command.starts_with("update")) {
            write_all(client, "updating_db: 7\nOK\n");
        } else if (command == "ping") {
            write_all(client, "OK\n");
        } else if (command == "idle") {
            write_all(client, "changed: playlist\nchanged: output\nOK\n");
        } else {
            write_all(client, "ACK [5@0] {} unknown command\n");
        }
    }

    int listen_socket_{-1};
    std::atomic_int client_socket_{-1};
    unsigned port_{0};
    std::thread worker_;
};

void client_negotiates_and_preserves_extensions() {
    FakeMpdServer server;
    trackknife::mpd::Profile profile{
        .id = trackknife::core::StableId::random(),
        .name = "fixture",
        .host = "127.0.0.1",
        .port = server.port(),
        .password = std::nullopt,
        .local_music_root = std::filesystem::path{"/srv/music"},
        .connect_timeout = std::chrono::milliseconds{1'000},
        .command_timeout = std::chrono::milliseconds{1'000},
    };

    auto connected = trackknife::mpd::Client::connect(profile);
    require(connected.has_value(), "client must connect to a valid MPD greeting");
    auto client = std::move(*connected);
    require(client.protocol_version() == trackknife::mpd::ProtocolVersion{0, 24, 2},
            "server protocol version must project");

    const auto capabilities = client.capabilities();
    require(capabilities.has_value(), "capability discovery must succeed");
    require(capabilities->supports_command("SWITCHOUTPUT"),
            "advertised Melody command must be case-insensitively discoverable");
    require(capabilities->exposes_tag("musicbrainzreleasegroupid"),
            "newer/unknown tag types must survive capability discovery");

    const auto status = client.status();
    require(status.has_value() && status->state == trackknife::mpd::PlaybackState::playing &&
                status->song_id == 7U && status->volume == 72U,
            "typed playback status must cross the adapter boundary");

    const auto current = client.current_song();
    require(current.has_value() && current->size() == 1U,
            "current song must project from generic pairs");
    require(current->front().queue_id == 7U && current->front().musicbrainz.recording_ids ==
                                                   std::vector<std::string>{"recording-id"},
            "queue and MusicBrainz identity must survive adapter boundary");

    const auto queue = client.queue_snapshot();
    require(queue.has_value() && queue->size() == 2U && queue->front().priority == 192U &&
                queue->at(1).queue_id == 9U,
            "queue duplicates/occurrences must retain server IDs");
    const auto changes = client.queue_changes(43U);
    require(changes && changes->size() == 1U && changes->front().queue_id == 9U &&
                changes->front().queue_position == 0U,
            "versioned queue changes must preserve their stable ID and new position");

    const auto outputs = client.outputs();
    require(outputs.has_value() && outputs->size() == 1U,
            "outputs must project from generic pairs");
    require(outputs->front().primary == true && outputs->front().online == true &&
                outputs->front().stream_format == "flac",
            "Melody output extensions must survive libmpdclient transport");
    const auto database = client.browse();
    require(database && database->size() == 3U &&
                std::holds_alternative<trackknife::mpd::DatabaseDirectory>(database->at(0)) &&
                std::holds_alternative<trackknife::mpd::Track>(database->at(1)) &&
                std::holds_alternative<trackknife::mpd::StoredPlaylist>(database->at(2)),
            "non-recursive database browse must preserve heterogeneous entry order");
    const auto artists = client.list_tag("AlbumArtist");
    require(artists && *artists == std::vector<std::string>{"Credited Artist", "Various Artists"},
            "server-side tag browse must preserve typed values");
    const auto album_counts = client.album_counts("AlbumArtist");
    require(album_counts && album_counts->size() == 2U && album_counts->at(0).artist == "A" &&
                album_counts->at(0).albums == 2U && album_counts->at(1).artist == "B" &&
                album_counts->at(1).albums == 1U,
            "grouped counts separate editions and deduplicate releases without loading tracks");
    require(!client.album_counts("Genre"), "album counts reject non-artist groupings");
    const auto artist_tracks = client.find_tag_tracks("AlbumArtist", "Credited Artist", 10'000U);
    require(artist_tracks && artist_tracks->size() == 2U &&
                artist_tracks->back().uri == "Artist/Release/02.flac",
            "a lazy library-tree branch must use one bounded exact tag lookup");
    require(!client.find_tag_tracks("AlbumArtist", "Credited Artist", 20'001U),
            "a library-tree branch cannot exceed the adapter bound");
    require(!client.list_tag("not a real tag") && !client.artwork("", false),
            "invalid tag and artwork requests must fail before protocol I/O");
    constexpr std::string_view expected_artwork{"0123456789ABCDEFGHIJ"};
    const auto album_artwork = client.artwork("Artist/Release/01.flac", false);
    require(album_artwork && album_artwork->size() == expected_artwork.size() &&
                std::memcmp(album_artwork->data(), expected_artwork.data(),
                            expected_artwork.size()) == 0,
            "folder artwork must assemble every MPD binary-response chunk");
    const auto embedded_artwork = client.artwork("Artist/Release/01.flac", true);
    require(embedded_artwork && embedded_artwork->size() == expected_artwork.size() &&
                std::memcmp(embedded_artwork->data(), expected_artwork.data(),
                            expected_artwork.size()) == 0,
            "embedded artwork must assemble every MPD binary-response chunk");
    const auto searched = client.search_any("Credited Artist", 0U, 2U);
    require(searched && searched->size() == 2U &&
                searched->front().musicbrainz.recording_ids ==
                    std::vector<std::string>{"recording-id"},
            "bounded any-tag search must return typed metadata-rich tracks");
    require(!client.search_any("", 0U, 2U) && !client.search_any("too broad", 0U, 501U),
            "unbounded or empty searches must fail before protocol I/O");
    const auto library_search = client.search_library("Credited Artist", 2U, 100U);
    require(library_search && library_search->tracks.size() == 2U &&
                library_search->albums.size() == 2U &&
                library_search->albums.back().filter.release_id == "release-later" &&
                std::ranges::all_of(library_search->albums,
                                    [](const auto& album) { return !album.artwork_uri.empty(); }),
            "library search must derive real albums before bounding the displayed track page");
    const auto next_page = client.search_library("Credited Artist", 2U, 100U, 2U);
    require(next_page && next_page->albums.empty() && next_page->tracks.size() == 2U,
            "continued search must use a bounded server-side window without rebuilding albums");
    const auto album = client.find_album(trackknife::mpd::AlbumFilter{
        .release_id = std::string{"release-id"},
        .artist = "Credited Artist",
        .album = "Complete Release",
        .date = std::string{"2000"},
        .artist_is_album_artist = true,
    });
    require(album && album->size() == 2U && album->back().uri == "Artist/Release/02.flac",
            "exact album lookup must return every track in the release");
    const auto playlists = client.stored_playlists();
    require(playlists && playlists->size() == 2U && playlists->front().name == "Road mix",
            "stored playlist discovery must retain server order and identity");
    const auto playlist = client.stored_playlist("Road mix");
    require(playlist && playlist->size() == 1U &&
                playlist->front().metadata.first("Title") == "Search two",
            "stored playlist contents must retain full track metadata");
    require(
        client.save_queue_as_playlist("New mix").has_value() &&
            client.load_stored_playlist_into_queue("Road mix").has_value() &&
            client.add_to_stored_playlist("Road mix", "Artist/Release/03.flac").has_value() &&
            client.add_to_stored_playlist("Road mix", "Artist/Release/04.flac", 1U).has_value() &&
            client
                .add_to_stored_playlist(
                    "Road mix",
                    std::vector<std::string>{"Artist/Release/05.flac", "Artist/Release/06.flac"})
                .has_value() &&
            client
                .add_to_stored_playlist(
                    "Road mix",
                    std::vector<std::string>{"Artist/Release/07.flac", "Artist/Release/08.flac"},
                    2U)
                .has_value() &&
            client.delete_from_stored_playlist("Road mix", 1U).has_value() &&
            client.delete_from_stored_playlist("Road mix", std::array{4U, 2U}).has_value() &&
            client.move_in_stored_playlist("Road mix", 0U, 1U).has_value() &&
            client.clear_stored_playlist("Quiet mix").has_value() &&
            client.rename_stored_playlist("New mix", "Renamed mix").has_value() &&
            client.delete_stored_playlist("Renamed mix").has_value(),
        "stored-playlist mutations must cross the adapter boundary");
    require(!client.save_queue_as_playlist("") && !client.add_to_stored_playlist("Road mix", "") &&
                !client.add_to_stored_playlist("Road mix", std::vector<std::string>{}) &&
                !client.add_to_stored_playlist("Road mix", std::vector<std::string>{"ok", ""}) &&
                !client.delete_from_stored_playlist("Road mix", std::array{1U, 1U}) &&
                !client.move_in_stored_playlist("Road mix", 1U, 1U) &&
                !client.rename_stored_playlist("same", "same"),
            "invalid stored-playlist mutations must fail before protocol I/O");
    require(client.seek_id(7U, std::chrono::milliseconds{12'500}).has_value(),
            "ID-based fractional seek must use the command connection");
    require(client.set_volume(31U).has_value(), "absolute volume must use the command connection");
    require(client.set_repeat(true).has_value() && client.set_random(true).has_value(),
            "repeat and random modes must use typed command methods");
    require(client.set_single(trackknife::mpd::PlaybackModeState::oneshot).has_value() &&
                client.set_consume(trackknife::mpd::PlaybackModeState::on).has_value(),
            "single and consume must preserve extended mode states");
    require(!client.set_single(trackknife::mpd::PlaybackModeState::unknown),
            "an unknown playback mode must fail before protocol I/O");
    require(client.replay_gain_mode() == trackknife::mpd::ReplayGainMode::track &&
                client.set_replay_gain_mode(trackknife::mpd::ReplayGainMode::album).has_value(),
            "ReplayGain status and changes must stay typed across the adapter boundary");
    require(client.set_output_enabled(0U, true).has_value(),
            "additive MPD output enable must use the command connection");
    require(client.play_id(9U).has_value(), "queue playback must address the stable song ID");
    require(client.play_position(3U).has_value(),
            "queue playback must also address a plain position for freshly replaced queues");
    const auto appended = client.add_id("Artist/Release/03.flac");
    require(appended == 11U, "queue append must return the server-assigned stable song ID");
    const auto inserted = client.add_id("Artist/Release/03.flac", 1U);
    require(inserted == 11U, "queue insertion must retain the server-assigned song ID");
    const std::array additions{
        trackknife::mpd::QueueAddition{.uri = "Artist/Release/04.flac", .position = 1U},
        trackknife::mpd::QueueAddition{.uri = "Artist/Release/05.flac", .position = 2U},
    };
    require(client.add_ids(additions).has_value(),
            "multi-selection addition must use one ordered command list");
    const std::array invalid_additions{
        trackknife::mpd::QueueAddition{.uri = "", .position = std::nullopt},
        trackknife::mpd::QueueAddition{.uri = "valid.flac", .position = std::nullopt},
    };
    require(!client.add_ids(invalid_additions),
            "an invalid URI must reject the whole addition batch before protocol I/O");
    require(client.delete_id(9U).has_value(), "queue deletion must address the stable song ID");
    const std::array delete_batch{7U, 9U};
    require(client.delete_ids(delete_batch).has_value(),
            "multi-selection deletion must use one stable-ID command list");
    const std::array duplicate_delete_batch{7U, 7U};
    require(!client.delete_ids(duplicate_delete_batch),
            "duplicate IDs must fail before a deletion command list is sent");
    const auto missing_delete = client.delete_id(999U);
    require(!missing_delete &&
                missing_delete.error().code == trackknife::core::ErrorCode::not_found,
            "MPD no-exist ACKs must retain a recoverable typed error");
    require(client.clear_queue().has_value(), "queue clear must use the command connection");
    const auto loved = client.lastfm({"love", {"Björk & A", "A + B"}});
    require(loved && loved->json == R"({"loved":true})",
            "Last.fm command quotes metadata and returns typed state");
    require(!client.lastfm({"love", {"bad\nmetadata", "song"}}),
            "Last.fm rejects command injection");
    const auto requests = client.request_queue();
    require(requests && requests->revision == 42 && requests->active_id == 12 &&
                requests->context == "Road" && requests->pending.size() == 2,
            "Up Next snapshot must retain revision and ordered duplicates");
    require(requests->pending[0].queue_id == 20 && requests->pending[1].queue_id == 21,
            "request identity is occurrence-based");
    trackknife::mpd::RequestQueueCommand request;
    request.revision = 42;
    request.uris = {"A.flac"};
    require(client.edit_request_queue(request).has_value(),
            "request batch is staged then committed");
    request.operation = trackknife::mpd::RequestQueueOperation::move;
    request.id = 21;
    request.position = 0;
    require(client.edit_request_queue(request).has_value(),
            "request moves carry identity and revision");
    request.operation = trackknife::mpd::RequestQueueOperation::retain;
    request.ids = {21, 20};
    require(client.edit_request_queue(request).has_value(),
            "batch request edits send ordered occurrence IDs in one revision");
    request.operation = trackknife::mpd::RequestQueueOperation::resume;
    require(client.edit_request_queue(request).has_value(), "explicit return reaches daemon");
    request.operation = trackknife::mpd::RequestQueueOperation::clear;
    request.revision = 41;
    require(!client.edit_request_queue(request), "stale request edits must surface conflicts");
    const auto melody_newest = client.newest_tag_values("AlbumArtist", 2'000U, true);
    require(melody_newest &&
                *melody_newest == std::vector<std::string>{"Fresh Artist", "Old Artist"},
            "Melody latest albums must provide complete, deduplicated artist order");
    const auto newest = client.newest_tag_values("AlbumArtist", 2'000U);
    require(newest &&
                *newest == std::vector<std::string>{"Fresh Artist", "Middle Artist", "Old Artist"},
            "newest lookup must sort by Added and dedupe in arrival order");
    require(client.update_database("Some Artist").has_value(),
            "a scoped database update must reach the server");
    require(client.update_database({}).has_value(),
            "an unscoped database update must reach the server");
    require(client.move_id(9U, 0U).has_value(), "queue move must address the stable song ID");
    const std::array moves{
        trackknife::mpd::QueueMove{.song_id = 7U, .position = 1U},
        trackknife::mpd::QueueMove{.song_id = 9U, .position = 0U},
    };
    require(client.move_ids(moves).has_value(),
            "multi-selection queue reorder must use one stable-ID command list");
    const std::array duplicate_moves{
        trackknife::mpd::QueueMove{.song_id = 7U, .position = 1U},
        trackknife::mpd::QueueMove{.song_id = 7U, .position = 0U},
    };
    require(!client.move_ids(duplicate_moves),
            "queue reorder batches must reject duplicate stable IDs before protocol I/O");
    require(client.set_priority_id(7U, 192U).has_value(),
            "queue priority must address the stable song ID");
    require(client.set_priority_ids(std::array{7U, 9U}, 255U).has_value(),
            "multi-selection priority must use one stable-ID command list");
    require(!client.set_priority_ids(std::array{7U, 7U}, 128U) && !client.set_priority_id(7U, 256U),
            "invalid queue priority requests must fail before protocol I/O");
    require(!client.add_id(""), "an empty queue URI must fail before protocol I/O");
    require(!client.set_volume(101U), "out-of-range volume must fail before protocol I/O");

    const auto rating_search = client.search_library(
        "beatles rating>=8", 200U, 1'000U, 0U, {.rating_filters = true, .album_search = false});
    require(rating_search.has_value() && rating_search->tracks.size() == 1U &&
                rating_search->tracks.front().rating == 9U,
            "rating search terms must become a Melody filter expression");
    const auto plain_rating_search = client.search_library("beatles rating>=8");
    require(plain_rating_search.has_value() && plain_rating_search->tracks.size() == 3U,
            "without the Melody gate, rating words stay ordinary search text");
    const auto album_search = client.search_library("albumrating>=8", 200U, 1'000U, 0U,
                                                    {.rating_filters = true, .album_search = true});
    require(album_search.has_value() && album_search->albums.size() == 2U,
            "album search must take the album section from searchalbums records");
    const auto& rated_album = album_search->albums.front();
    require(rated_album.artist == "Rated Artist" && rated_album.album == "Rated Release" &&
                rated_album.date == "1994" && rated_album.rating == 8U &&
                rated_album.computed_rating == 7.5 && rated_album.track_count == 10U &&
                rated_album.duration_seconds == 2'400U &&
                rated_album.artwork_uri == "Rated/A/01.flac",
            "searchalbums records must project with their rating and identity data");
    require(album_search->albums.back().date.empty() &&
                !album_search->albums.back().filter.date.has_value(),
            "the 0000 identity placeholder must not surface as a display date");

    const auto expression_search =
        client.search_expression("(genre contains \"jazz\")", "-date", 500U);
    require(expression_search.has_value() && expression_search->size() == 1U &&
                expression_search->front().metadata.first("Title") == "Structured hit",
            "expression search must send the filter with sort and window");

    require(client.set_sticker_rating("Artist/Release/01.flac", 8U).has_value(),
            "track rating must store the interoperable rating sticker");
    require(client.set_sticker_rating("unrated.flac", 0U).has_value(),
            "unrating an unrated song must succeed instead of surfacing no-exist");
    const std::array rating_uris{std::string{"Artist/Release/01.flac"},
                                 std::string{"Artist/Release/02.flac"}};
    require(client.set_sticker_ratings(rating_uris, 10U).has_value(),
            "multi-selection rating must use one sticker command list");
    require(!client.set_sticker_rating("Artist/Release/01.flac", 11U),
            "a rating above 10 must fail before protocol I/O");
    require(
        !client.set_sticker_ratings(std::array{std::string{"a.flac"}, std::string{"a.flac"}}, 5U),
        "rating batches must reject duplicate URIs before protocol I/O");
    const auto sticker_map = client.sticker_ratings();
    require(sticker_map.has_value() && sticker_map->size() == 1U &&
                sticker_map->front().uri == "Artist/Release/01.flac" &&
                sticker_map->front().rating == 8U,
            "bulk rating load must project interoperable stickers and skip foreign values");
    require(client.set_melody_track_rating(4'711U, 8U).has_value(),
            "Melody track rating must use the database song id");
    require(
        client
            .set_melody_track_ratings(std::array{std::uint64_t{4'711U}, std::uint64_t{4'712U}}, 6U)
            .has_value(),
        "multi-selection Melody rating must use one command list");
    const trackknife::mpd::MelodyAlbumKey album_key{
        .album_artist = "Credited Artist", .album = "Early Release", .date = "1998"};
    require(client.set_melody_album_rating(album_key, 8U).has_value(),
            "Melody album rating must send the literal album identity");
    const auto album_rating = client.melody_album_rating(album_key);
    require(album_rating.has_value() && album_rating->rating == 8U && album_rating->computed == 7.5,
            "Melody album rating reads must project both stored and computed values");
    require(!client.set_melody_album_rating(
                trackknife::mpd::MelodyAlbumKey{.album_artist = "", .album = "X", .date = ""}, 5U),
            "Melody album ratings must fail without an album artist before protocol I/O");
    require(client.ping().has_value(), "connection must remain usable after all responses");

    const trackknife::core::CancellationSource idle_cancellation;
    const auto idle = client.wait_for_idle(idle_cancellation.token());
    require(idle.has_value() && idle->contains(trackknife::mpd::IdleEvent::queue) &&
                idle->contains(trackknife::mpd::IdleEvent::output),
            "idle subsystem changes must project without polling the whole server");
}

} // namespace

int main() {
    client_negotiates_and_preserves_extensions();
    return EXIT_SUCCESS;
}

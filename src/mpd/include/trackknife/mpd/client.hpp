// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/mpd/model.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace trackknife::mpd {

enum class IdleEvent : std::uint32_t {
    database = 1U << 0U,
    stored_playlist = 1U << 1U,
    queue = 1U << 2U,
    player = 1U << 3U,
    mixer = 1U << 4U,
    output = 1U << 5U,
    options = 1U << 6U,
    update = 1U << 7U,
    sticker = 1U << 8U,
    partition = 1U << 9U,
};

struct IdleEvents {
    std::uint32_t mask{0U};

    [[nodiscard]] bool contains(IdleEvent event) const noexcept {
        return (mask & static_cast<std::uint32_t>(event)) != 0U;
    }
};

enum class TransportAction { play, pause, resume, stop, next, previous };

// ADR-0179: with rating_filters, `rating>=8` / `albumrating==10` words in a
// search become Melody filter conditions and the remaining words stay an
// any-contains constraint; off, they search as plain text. With
// album_search, the album section of a library search comes from the
// server's searchalbums records instead of being derived from songs.
struct MelodySearchFeatures {
    bool rating_filters{false};
    bool album_search{false};
};

struct QueueAddition {
    std::string uri;
    std::optional<unsigned> position;

    friend bool operator==(const QueueAddition&, const QueueAddition&) = default;
};

struct QueueMove {
    std::uint32_t song_id{0U};
    unsigned position{0U};

    friend bool operator==(const QueueMove&, const QueueMove&) = default;
};

// A blocking, single-owner protocol adapter. Application code must run it on a
// session worker, never the Qt UI thread. Client serializes one complete MPD
// request/response at a time by construction.
class Client final {
  public:
    Client(Client&&) noexcept;
    Client& operator=(Client&&) noexcept;
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    ~Client();

    [[nodiscard]] static core::Result<Client> connect(const Profile& profile);

    [[nodiscard]] ProtocolVersion protocol_version() const noexcept;
    [[nodiscard]] core::Result<Capabilities> capabilities();
    [[nodiscard]] core::Result<std::vector<Pair>> command_pairs(std::string_view command);
    [[nodiscard]] core::Result<PlaybackStatus> status();
    [[nodiscard]] core::Result<std::vector<Track>> current_song();
    [[nodiscard]] core::Result<std::vector<Track>> queue_snapshot();
    [[nodiscard]] core::Result<std::vector<Track>> queue_changes(std::uint32_t from_version);
    [[nodiscard]] core::Result<std::vector<DatabaseEntry>> browse(std::string_view uri = {});
    [[nodiscard]] core::Result<std::vector<std::string>> list_tag(std::string_view tag);
    [[nodiscard]] core::Result<std::vector<ArtistAlbumCount>>
    album_counts(std::string_view artist_tag);
    [[nodiscard]] core::Result<std::vector<Track>>
    find_tag_tracks(std::string_view tag, std::string_view value, unsigned limit = 10'000U);
    [[nodiscard]] core::Result<std::vector<std::byte>> artwork(std::string_view uri, bool embedded);
    [[nodiscard]] core::Result<std::vector<Track>>
    search_any(std::string_view query, unsigned offset = 0U, unsigned limit = 200U,
               bool melody_rating_filters = false);
    [[nodiscard]] core::Result<LibrarySearchResult>
    search_library(std::string_view query, unsigned track_limit = 200U,
                   unsigned album_limit = 1'000U, unsigned offset = 0U,
                   MelodySearchFeatures melody = {});
    // Raw filter-expression search for server-translated structured queries
    // (ADR-0150 via tkq_melody.hpp): sort is a Melody sort argument like
    // "-date"; limit bounds the window. Callers gate on server capability.
    [[nodiscard]] core::Result<std::vector<Track>>
    search_expression(std::string_view filter_expression, std::string_view sort = {},
                      unsigned limit = 0U);
    // Raw Melody album search with an already-built filter expression
    // (docs in the melody repo: docs/protocol.md). Callers gate on the
    // advertised `searchalbums` command.
    [[nodiscard]] core::Result<std::vector<MelodyAlbum>>
    search_melody_albums(std::string_view filter_expression, std::string_view sort = {},
                         unsigned limit = 0U);
    [[nodiscard]] core::Result<std::vector<Track>> find_album(const AlbumFilter& album);
    [[nodiscard]] core::Result<std::vector<StoredPlaylist>> stored_playlists();
    [[nodiscard]] core::Result<std::vector<Track>> stored_playlist(std::string_view name);
    [[nodiscard]] core::Result<void> save_queue_as_playlist(std::string_view name);
    [[nodiscard]] core::Result<void> load_stored_playlist_into_queue(std::string_view name);
    [[nodiscard]] core::Result<void>
    add_to_stored_playlist(std::string_view name, std::string_view uri,
                           std::optional<unsigned> position = std::nullopt);
    [[nodiscard]] core::Result<void>
    add_to_stored_playlist(std::string_view name, std::span<const std::string> uris,
                           std::optional<unsigned> first_position = std::nullopt);
    [[nodiscard]] core::Result<void> delete_from_stored_playlist(std::string_view name,
                                                                 unsigned position);
    [[nodiscard]] core::Result<void>
    delete_from_stored_playlist(std::string_view name, std::span<const unsigned> positions);
    [[nodiscard]] core::Result<void> move_in_stored_playlist(std::string_view name, unsigned from,
                                                             unsigned to);
    [[nodiscard]] core::Result<void> clear_stored_playlist(std::string_view name);
    [[nodiscard]] core::Result<void> rename_stored_playlist(std::string_view from,
                                                            std::string_view to);
    [[nodiscard]] core::Result<void> delete_stored_playlist(std::string_view name);
    [[nodiscard]] core::Result<std::vector<Output>> outputs();
    [[nodiscard]] core::Result<void> run_transport(TransportAction action);
    [[nodiscard]] core::Result<void> play_id(std::uint32_t song_id);
    [[nodiscard]] core::Result<void> play_position(unsigned position);
    [[nodiscard]] core::Result<std::uint32_t>
    add_id(std::string_view uri, std::optional<unsigned> position = std::nullopt);
    [[nodiscard]] core::Result<void> add_ids(std::span<const QueueAddition> additions);
    [[nodiscard]] core::Result<void> delete_id(std::uint32_t song_id);
    [[nodiscard]] core::Result<void> delete_ids(std::span<const std::uint32_t> song_ids);
    [[nodiscard]] core::Result<void> clear_queue();
    // Requests a database update scoped to the URI; empty updates everything.
    [[nodiscard]] core::Result<void> update_database(const std::string& uri);
    // Distinct values of tag over the library's most recently modified
    // tracks, newest first — the ordering source for "Latest" browsing.
    [[nodiscard]] core::Result<std::vector<std::string>> newest_tag_values(std::string_view tag,
                                                                           unsigned track_limit);
    [[nodiscard]] core::Result<void> move_id(std::uint32_t song_id, unsigned position);
    [[nodiscard]] core::Result<void> move_ids(std::span<const QueueMove> moves);
    [[nodiscard]] core::Result<void> set_priority_id(std::uint32_t song_id, unsigned priority);
    [[nodiscard]] core::Result<void> set_priority_ids(std::span<const std::uint32_t> song_ids,
                                                      unsigned priority);
    // Track ratings on the interoperable 0-10 scale (ADR-0179). Stock MPD
    // stores the "rating" song sticker; 0 deletes it and unrating an unrated
    // song succeeds. Callers gate on the advertised `sticker` command.
    [[nodiscard]] core::Result<void> set_sticker_rating(std::string_view uri, unsigned rating);
    [[nodiscard]] core::Result<void> set_sticker_ratings(std::span<const std::string> uris,
                                                         unsigned rating);
    [[nodiscard]] core::Result<std::vector<TrackRating>> sticker_ratings();
    // Melody's native rating extension, gated on the advertised `getrating`
    // command. Track identity is the X-SongId database id, never the queue id.
    [[nodiscard]] core::Result<void> set_melody_track_rating(std::uint64_t song_id,
                                                             unsigned rating);
    [[nodiscard]] core::Result<void>
    set_melody_track_ratings(std::span<const std::uint64_t> song_ids, unsigned rating);
    [[nodiscard]] core::Result<void> melody_context_play(std::string_view name,
                                                         std::optional<unsigned> row);
    [[nodiscard]] core::Result<void> melody_context_queue(std::optional<unsigned> row);
    [[nodiscard]] core::Result<std::string> melody_active_context();
    [[nodiscard]] core::Result<void> set_melody_album_rating(const MelodyAlbumKey& key,
                                                             unsigned rating);
    [[nodiscard]] core::Result<MelodyAlbumRating> melody_album_rating(const MelodyAlbumKey& key);
    [[nodiscard]] core::Result<void> seek_id(std::uint32_t song_id,
                                             std::chrono::milliseconds position);
    [[nodiscard]] core::Result<void> set_volume(unsigned volume);
    [[nodiscard]] core::Result<void> set_repeat(bool enabled);
    [[nodiscard]] core::Result<void> set_random(bool enabled);
    [[nodiscard]] core::Result<void> set_single(PlaybackModeState state);
    [[nodiscard]] core::Result<void> set_consume(PlaybackModeState state);
    [[nodiscard]] core::Result<ReplayGainMode> replay_gain_mode();
    [[nodiscard]] core::Result<void> set_replay_gain_mode(ReplayGainMode mode);
    [[nodiscard]] core::Result<void> set_output_enabled(std::uint32_t output_id, bool enabled);
    [[nodiscard]] core::Result<void> ping();
    [[nodiscard]] core::Result<IdleEvents>
    wait_for_idle(const core::CancellationToken& cancellation);

  private:
    struct Impl;
    explicit Client(std::unique_ptr<Impl> implementation);

    std::unique_ptr<Impl> implementation_;
};

} // namespace trackknife::mpd

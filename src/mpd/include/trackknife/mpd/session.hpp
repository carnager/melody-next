// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/error.hpp"
#include "trackknife/mpd/client.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace trackknife::mpd {

enum class SessionPhase { connecting, connected, reconnecting, stopped };

struct SessionState {
    SessionPhase phase{SessionPhase::stopped};
    std::uint64_t generation{0U};
    std::optional<core::Error> error;
};

struct SessionSnapshot {
    std::uint64_t generation{0U};
    Capabilities capabilities;
    PlaybackStatus status;
    ReplayGainMode replay_gain_mode{ReplayGainMode::unknown};
    std::vector<Track> current_song;
    std::vector<Track> queue;
    std::vector<Output> outputs;
    // Per-URI 0-10 track ratings from the server's sticker database; empty
    // when the server does not advertise the sticker command.
    std::vector<TrackRating> sticker_ratings;
    // ADR-0187: the stored playlist currently materialized as the playback
    // context, empty for the live queue. Absent when the server does not
    // advertise melody_context.
    std::optional<MelodyContextState> context;
    // The queue context's own contents: the stashed queue while another
    // list is materialized, so the Queue tab keeps showing its own list.
    std::vector<Track> queue_context_tracks;
};

enum class SessionCommandKind {
    transport,
    queue_play,
    queue_add,
    queue_add_batch,
    queue_delete,
    queue_delete_batch,
    queue_clear,
    queue_move,
    queue_move_batch,
    queue_priority,
    database_update,
    database_newest,
    database_browse,
    database_tag,
    database_album_counts,
    database_tag_tracks,
    artwork,
    database_search,
    database_album,
    stored_playlists,
    stored_playlist,
    stored_playlist_save,
    stored_playlist_load,
    stored_playlist_add,
    stored_playlist_add_batch,
    stored_playlist_delete_item,
    stored_playlist_delete_batch,
    stored_playlist_move_item,
    stored_playlist_clear,
    stored_playlist_rename,
    stored_playlist_remove,
    repeat,
    random,
    single,
    consume,
    replay_gain,
    seek,
    volume,
    output_enabled,
    sticker_rating,
    melody_rating,
    melody_album_rate,
    melody_album_rating,
    melody_context_play,
    melody_context_queue,
    melody_context_tracks,
    melody_context_resync,
    melody_context_queue_add,
    melody_context_queue_replace,
    melody_context_queue_delete,
    melody_context_queue_move,
    database_expression_search,
};

using SessionCommandPayload =
    std::variant<std::monostate, std::vector<DatabaseEntry>, std::vector<Track>,
                 std::vector<StoredPlaylist>, std::vector<std::string>, std::vector<std::byte>,
                 LibrarySearchResult, std::vector<ArtistAlbumCount>, MelodyAlbumRating>;

struct SessionCommandResult {
    std::uint64_t id{0U};
    std::uint64_t generation{0U};
    SessionCommandKind kind{SessionCommandKind::transport};
    TransportAction action{TransportAction::play};
    SessionCommandPayload payload;
    std::optional<core::Error> error;
};

struct SessionCallbacks {
    // Callbacks run on session workers. A GUI adapter must marshal them to the
    // UI thread and reject generations older than its current snapshot.
    std::function<void(const SessionState&)> state_changed;
    std::function<void(const SessionSnapshot&)> snapshot_changed;
    std::function<void(IdleEvents)> idle_received;
    std::function<void(const SessionCommandResult&)> command_finished;
};

// Owns one serialized command connection and one cancellable idle connection.
// Read refreshes reconnect; mutations report ambiguous failures and are never
// automatically replayed. Pending interactive commands take priority over
// background browse/search reads.
class Session final {
  public:
    Session(Profile profile, SessionCallbacks callbacks);
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&&) = delete;
    Session& operator=(Session&&) = delete;

    void request_full_refresh();
    void cancel_pending(std::uint64_t command_id);
    [[nodiscard]] std::uint64_t run_transport(TransportAction action);
    [[nodiscard]] std::uint64_t play_queue_id(std::uint32_t song_id);
    [[nodiscard]] std::uint64_t play_queue_position(unsigned position);
    [[nodiscard]] std::uint64_t add_queue_uri(std::string uri,
                                              std::optional<unsigned> position = std::nullopt);
    [[nodiscard]] std::uint64_t add_queue_uris(std::vector<QueueAddition> additions);
    [[nodiscard]] std::uint64_t delete_queue_id(std::uint32_t song_id);
    [[nodiscard]] std::uint64_t delete_queue_ids(std::vector<std::uint32_t> song_ids);
    [[nodiscard]] std::uint64_t clear_queue();
    [[nodiscard]] std::uint64_t move_queue_id(std::uint32_t song_id, unsigned position);
    [[nodiscard]] std::uint64_t move_queue_ids(std::vector<QueueMove> moves);
    [[nodiscard]] std::uint64_t set_queue_priority(std::vector<std::uint32_t> song_ids,
                                                   unsigned priority);
    // Ratings on the interoperable 0-10 scale (ADR-0179). The adapter picks
    // the backend from advertised capabilities: sticker for stock MPD,
    // Melody's native commands when `getrating` is advertised.
    [[nodiscard]] std::uint64_t set_sticker_ratings(std::vector<std::string> uris,
                                                    unsigned rating);
    [[nodiscard]] std::uint64_t set_melody_track_ratings(std::vector<std::uint64_t> song_ids,
                                                         unsigned rating);
    [[nodiscard]] std::uint64_t set_melody_album_rating(MelodyAlbumKey key, unsigned rating);
    [[nodiscard]] std::uint64_t melody_album_rating(MelodyAlbumKey key);
    // ADR-0187: play a stored playlist as the active playback context, or
    // switch back to the stashed queue. row = std::nullopt resumes.
    [[nodiscard]] std::uint64_t melody_context_play(std::string name, std::optional<unsigned> row);
    [[nodiscard]] std::uint64_t melody_context_queue(std::optional<unsigned> row);
    [[nodiscard]] std::uint64_t melody_context_tracks(std::vector<std::string> uris, unsigned row,
                                                      std::string label);
    [[nodiscard]] std::uint64_t melody_context_resync(std::vector<std::string> uris);
    // Queue-context edits: the Queue tab's list is the server's stash while
    // another list is the active queue, so its edits go there.
    [[nodiscard]] std::uint64_t melody_context_queue_add(std::vector<std::string> uris,
                                                         int position);
    [[nodiscard]] std::uint64_t melody_context_queue_replace(std::vector<std::string> uris,
                                                             unsigned position);
    [[nodiscard]] std::uint64_t melody_context_queue_delete(std::vector<unsigned> rows);
    [[nodiscard]] std::uint64_t melody_context_queue_move(unsigned from, unsigned to);
    // Server-translated structured query: a raw filter expression plus an
    // optional Melody sort argument; the payload is the bounded track list.
    [[nodiscard]] std::uint64_t search_expression(std::string filter_expression, std::string sort,
                                                  unsigned limit = 500U);
    [[nodiscard]] std::uint64_t update_database(std::string uri);
    [[nodiscard]] std::uint64_t newest_root_values(std::string tag, unsigned track_limit);
    [[nodiscard]] std::uint64_t browse(std::string uri = {});
    [[nodiscard]] std::uint64_t list_tag(std::string tag);
    [[nodiscard]] std::uint64_t album_counts(std::string artist_tag);
    [[nodiscard]] std::uint64_t find_tag_tracks(std::string tag, std::string value,
                                                unsigned limit = 10'000U);
    [[nodiscard]] std::uint64_t load_artwork(std::string uri, bool embedded);
    [[nodiscard]] std::uint64_t search_any(std::string query, unsigned offset = 0U,
                                           unsigned limit = 200U);
    [[nodiscard]] std::uint64_t find_album(AlbumFilter album);
    [[nodiscard]] std::uint64_t list_stored_playlists();
    [[nodiscard]] std::uint64_t load_stored_playlist(std::string name);
    [[nodiscard]] std::uint64_t save_queue_as_playlist(std::string name);
    [[nodiscard]] std::uint64_t load_stored_playlist_into_queue(std::string name);
    [[nodiscard]] std::uint64_t
    add_to_stored_playlist(std::string name, std::string uri,
                           std::optional<unsigned> position = std::nullopt);
    [[nodiscard]] std::uint64_t
    add_to_stored_playlist(std::string name, std::vector<std::string> uris,
                           std::optional<unsigned> first_position = std::nullopt);
    [[nodiscard]] std::uint64_t delete_from_stored_playlist(std::string name, unsigned position);
    [[nodiscard]] std::uint64_t delete_from_stored_playlist(std::string name,
                                                            std::vector<unsigned> positions);
    [[nodiscard]] std::uint64_t move_in_stored_playlist(std::string name, unsigned from,
                                                        unsigned to);
    [[nodiscard]] std::uint64_t clear_stored_playlist(std::string name);
    [[nodiscard]] std::uint64_t rename_stored_playlist(std::string from, std::string to);
    [[nodiscard]] std::uint64_t delete_stored_playlist(std::string name);
    [[nodiscard]] std::uint64_t seek(std::uint32_t song_id, std::chrono::milliseconds position);
    [[nodiscard]] std::uint64_t set_volume(unsigned volume);
    [[nodiscard]] std::uint64_t set_repeat(bool enabled);
    [[nodiscard]] std::uint64_t set_random(bool enabled);
    [[nodiscard]] std::uint64_t set_single(PlaybackModeState state);
    [[nodiscard]] std::uint64_t set_consume(PlaybackModeState state);
    [[nodiscard]] std::uint64_t set_replay_gain_mode(ReplayGainMode mode);
    [[nodiscard]] std::uint64_t set_output_enabled(std::uint32_t output_id, bool enabled);

  private:
    struct Impl;
    std::unique_ptr<Impl> implementation_;
};

} // namespace trackknife::mpd

// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/stable_id.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace trackknife::mpd {

struct ArtistAlbumCount {
    std::string artist;
    std::size_t albums{0};
};

// Interoperable rating vocabulary (ADR-0179): an integer 0-10 shows as five
// stars in half-star steps; 0 means unrated and deletes the stored value.
inline constexpr unsigned maximum_rating = 10U;

struct TrackRating {
    std::string uri;
    unsigned rating{0U};

    friend bool operator==(const TrackRating&, const TrackRating&) = default;
};

// Melody album rating identity: the literal AlbumArtist/Album/Date strings.
// An empty date is part of the identity and must be sent verbatim.
struct MelodyAlbumKey {
    std::string album_artist;
    std::string album;
    std::string date;

    friend bool operator==(const MelodyAlbumKey&, const MelodyAlbumKey&) = default;
};

struct MelodyAlbumRating {
    // The user-set album rating; 0 when unset.
    unsigned rating{0U};
    // Mean of the album's track ratings, reported only once the server's
    // rated-track threshold is met; 0.0 otherwise.
    double computed{0.0};

    friend bool operator==(const MelodyAlbumRating&, const MelodyAlbumRating&) = default;
};

// One record of Melody's album-shaped search (searchalbums). The date is the
// server's stored identity including its 0000 placeholder.
struct MelodyAlbum {
    std::string album_artist;
    std::string album;
    std::string date;
    std::uint64_t album_id{0U};
    unsigned track_count{0U};
    std::uint64_t duration_seconds{0U};
    unsigned rating{0U};
    double computed_rating{0.0};
    std::string artwork_uri;

    friend bool operator==(const MelodyAlbum&, const MelodyAlbum&) = default;
};

struct LastFmCommand {
    std::string operation;
    std::vector<std::string> arguments;
};
struct LastFmReply {
    std::string json;
};

struct Pair {
    std::string name;
    std::string value;

    friend bool operator==(const Pair&, const Pair&) = default;
};

class Metadata final {
  public:
    Metadata() = default;
    explicit Metadata(std::vector<Pair> fields);

    [[nodiscard]] const std::vector<Pair>& fields() const noexcept { return fields_; }
    [[nodiscard]] std::vector<std::string_view> values(std::string_view name) const;
    [[nodiscard]] std::optional<std::string_view> first(std::string_view name) const;

    friend bool operator==(const Metadata&, const Metadata&) = default;

  private:
    std::vector<Pair> fields_;
};

struct MusicBrainzIdentity {
    std::vector<std::string> artist_ids;
    std::vector<std::string> album_artist_ids;
    std::vector<std::string> recording_ids;
    std::vector<std::string> release_track_ids;
    std::vector<std::string> release_ids;
    std::vector<std::string> release_group_ids;
    std::vector<std::string> work_ids;
    std::vector<std::string> artist_sort_names;
    std::vector<std::string> album_artist_sort_names;

    friend bool operator==(const MusicBrainzIdentity&, const MusicBrainzIdentity&) = default;
};

[[nodiscard]] MusicBrainzIdentity project_musicbrainz(const Metadata& metadata);

struct ProtocolVersion {
    unsigned major{0};
    unsigned minor{0};
    unsigned patch{0};

    friend auto operator<=>(const ProtocolVersion&, const ProtocolVersion&) = default;
};

struct Capabilities {
    ProtocolVersion protocol;
    std::vector<std::string> commands;
    std::vector<std::string> tag_types;

    [[nodiscard]] bool supports_command(std::string_view command) const;
    [[nodiscard]] bool exposes_tag(std::string_view tag) const;
};

struct Profile {
    core::StableId id;
    std::string name;
    std::string host;
    unsigned port{6600};
    std::optional<std::string> password;
    std::optional<std::filesystem::path> local_music_root;
    std::chrono::milliseconds connect_timeout{5'000};
    std::chrono::milliseconds command_timeout{10'000};

    friend bool operator==(const Profile&, const Profile&) = default;
};

struct Track {
    std::string uri;
    Metadata metadata;
    MusicBrainzIdentity musicbrainz;
    std::optional<std::uint32_t> queue_id;
    std::optional<std::uint32_t> queue_position;
    std::optional<std::chrono::milliseconds> duration;
    std::optional<std::string> last_modified;
    std::optional<std::string> audio_format;
    std::optional<unsigned> priority;
    // Melody extension projection: the 0-10 track rating and the server
    // database song id that the rate command addresses. Absent on stock MPD.
    std::optional<unsigned> rating;
    std::optional<std::uint64_t> melody_song_id;
    std::vector<Pair> unknown_structural_pairs;

    friend bool operator==(const Track&, const Track&) = default;
};

// ADR-0188: which list is materialized as the playback context, and
// whether a displaced queue is waiting to be restored.
enum class RequestQueueOperation {
    append,
    prepend,
    remove,
    move,
    clear,
    resume,
    insert,
    play,
    undo
};
struct RequestQueueCommand {
    RequestQueueOperation operation{RequestQueueOperation::append};
    unsigned revision{0};
    unsigned id{0};
    unsigned position{0};
    std::vector<std::string> uris;
};
struct RequestQueueState {
    unsigned revision{0};
    unsigned active_id{0};
    bool can_undo{false};
    std::string context;
    std::vector<Track> pending;
};

struct MelodyContextState {
    std::string name;
    bool queue_stashed{false};

    friend bool operator==(const MelodyContextState&, const MelodyContextState&) = default;
};

struct AlbumFilter {
    std::optional<std::string> release_id;
    std::string artist;
    std::string album;
    std::optional<std::string> date;
    bool artist_is_album_artist{true};

    friend bool operator==(const AlbumFilter&, const AlbumFilter&) = default;
};

struct AlbumSummary {
    AlbumFilter filter;
    std::string artist;
    std::string album;
    std::string date;
    std::string artwork_uri;
    // Melody searchalbums enrichment (ADR-0179): zero when the server only
    // returned songs and the summary was derived client-side.
    unsigned rating{0U};
    double computed_rating{0.0};
    unsigned track_count{0U};
    std::uint64_t duration_seconds{0U};

    friend bool operator==(const AlbumSummary&, const AlbumSummary&) = default;
};

struct LibrarySearchResult {
    std::vector<AlbumSummary> albums;
    std::vector<Track> tracks;

    friend bool operator==(const LibrarySearchResult&, const LibrarySearchResult&) = default;
};

void sort_search_results(std::vector<Track>& tracks);

struct DatabaseDirectory {
    std::string uri;
    std::optional<std::string> last_modified;
    std::vector<Pair> unknown_pairs;

    friend bool operator==(const DatabaseDirectory&, const DatabaseDirectory&) = default;
};

struct StoredPlaylist {
    std::string name;
    std::optional<std::string> last_modified;
    std::vector<Pair> unknown_pairs;

    friend bool operator==(const StoredPlaylist&, const StoredPlaylist&) = default;
};

using DatabaseEntry = std::variant<DatabaseDirectory, Track, StoredPlaylist>;

enum class PlaybackState { stopped, playing, paused, unknown };
enum class PlaybackModeState { off, on, oneshot, unknown };
enum class ReplayGainMode { off, track, album, automatic, unknown };

struct PlaybackStatus {
    PlaybackState state{PlaybackState::unknown};
    std::optional<unsigned> volume;
    std::optional<std::chrono::milliseconds> elapsed;
    std::optional<std::chrono::milliseconds> duration;
    std::optional<std::uint32_t> queue_version;
    std::optional<std::uint32_t> queue_length;
    std::optional<std::uint32_t> song_id;
    // Queue position of the playing song (MPD's "song"), for surfaces that
    // mark rows by position rather than queue id (ADR-0187).
    std::optional<std::uint32_t> song_position;
    std::optional<std::uint32_t> next_song_id;
    bool repeat{false};
    bool random{false};
    PlaybackModeState single{PlaybackModeState::unknown};
    PlaybackModeState consume{PlaybackModeState::unknown};
    std::optional<unsigned> crossfade_seconds;
    std::optional<std::string> error;
    std::vector<Pair> unknown_pairs;

    friend bool operator==(const PlaybackStatus&, const PlaybackStatus&) = default;
};

struct Output {
    std::uint32_t id{0};
    std::string name;
    bool enabled{false};
    std::optional<std::string> plugin;
    std::vector<Pair> attributes;

    // Melody extension projection. Absence means the server did not advertise
    // the field, which is different from false/offline.
    std::optional<bool> primary;
    std::optional<bool> online;
    std::optional<std::string> stream_format;
    std::optional<std::uint32_t> maximum_bitrate;

    friend bool operator==(const Output&, const Output&) = default;
};

[[nodiscard]] bool ascii_case_equal(std::string_view left, std::string_view right) noexcept;

} // namespace trackknife::mpd

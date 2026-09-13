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
    std::vector<Pair> unknown_structural_pairs;

    friend bool operator==(const Track&, const Track&) = default;
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

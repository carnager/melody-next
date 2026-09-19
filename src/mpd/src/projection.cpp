// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/mpd/projection.hpp"

#include <charconv>
#include <limits>
#include <string_view>
#include <unordered_set>

namespace trackknife::mpd {
namespace {

template <typename Value> [[nodiscard]] std::optional<Value> parse_unsigned(std::string_view text) {
    Value value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::optional<std::chrono::milliseconds> parse_seconds(std::string_view text) {
    double seconds = 0.0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), seconds);
    if (error != std::errc{} || end != text.data() + text.size() || seconds < 0.0 ||
        seconds > static_cast<double>(std::numeric_limits<std::int64_t>::max()) / 1000.0) {
        return std::nullopt;
    }
    return std::chrono::milliseconds{static_cast<std::int64_t>(seconds * 1000.0)};
}

[[nodiscard]] core::Error malformed(std::string message) {
    return core::Error{
        .code = core::ErrorCode::backend, .message = std::move(message), .context = {}};
}

[[nodiscard]] bool parse_boolean(std::string_view value, bool& output) {
    if (value == "1") {
        output = true;
        return true;
    }
    if (value == "0") {
        output = false;
        return true;
    }
    return false;
}

void finish_track(Track& track, std::vector<Pair>& metadata, std::vector<Track>& tracks) {
    track.metadata = Metadata{std::move(metadata)};
    track.musicbrainz = project_musicbrainz(track.metadata);
    tracks.push_back(std::move(track));
    track = Track{};
    metadata.clear();
}

[[nodiscard]] bool begins_database_entry(const Pair& pair) {
    return ascii_case_equal(pair.name, "file") || ascii_case_equal(pair.name, "directory") ||
           ascii_case_equal(pair.name, "playlist");
}

[[nodiscard]] core::Result<DatabaseEntry>
project_database_entry(const std::span<const Pair> pairs) {
    if (pairs.empty() || pairs.front().value.empty()) {
        return std::unexpected(malformed("MPD returned an empty database entry"));
    }
    if (ascii_case_equal(pairs.front().name, "file")) {
        auto tracks = project_tracks(pairs);
        if (!tracks || tracks->size() != 1U) {
            return std::unexpected(tracks ? malformed("MPD database file entry was not singular")
                                          : std::move(tracks.error()));
        }
        return DatabaseEntry{std::move(tracks->front())};
    }

    const auto project_container = [&pairs](auto entry) -> DatabaseEntry {
        for (const auto& pair : pairs.subspan(1U)) {
            if (ascii_case_equal(pair.name, "Last-Modified")) {
                entry.last_modified = pair.value;
            } else {
                entry.unknown_pairs.push_back(pair);
            }
        }
        return entry;
    };
    if (ascii_case_equal(pairs.front().name, "directory")) {
        return project_container(DatabaseDirectory{
            .uri = pairs.front().value, .last_modified = std::nullopt, .unknown_pairs = {}});
    }
    if (ascii_case_equal(pairs.front().name, "playlist")) {
        return project_container(StoredPlaylist{
            .name = pairs.front().value, .last_modified = std::nullopt, .unknown_pairs = {}});
    }
    return std::unexpected(malformed("MPD database response began with an unknown entry type"));
}

} // namespace

core::Result<std::vector<Track>> project_tracks(std::span<const Pair> pairs) {
    std::vector<Track> tracks;
    std::vector<Pair> metadata;
    Track current;
    bool has_current = false;

    for (const auto& pair : pairs) {
        if (ascii_case_equal(pair.name, "file")) {
            if (has_current) {
                finish_track(current, metadata, tracks);
            }
            if (pair.value.empty()) {
                return std::unexpected(malformed("MPD returned a song with an empty file URI"));
            }
            current.uri = pair.value;
            has_current = true;
            continue;
        }
        if (!has_current) {
            return std::unexpected(malformed("MPD song response did not begin with a file pair"));
        }

        if (ascii_case_equal(pair.name, "Id")) {
            current.queue_id = parse_unsigned<std::uint32_t>(pair.value);
            if (!current.queue_id) {
                return std::unexpected(malformed("MPD returned an invalid queue song ID"));
            }
        } else if (ascii_case_equal(pair.name, "Pos")) {
            current.queue_position = parse_unsigned<std::uint32_t>(pair.value);
            if (!current.queue_position) {
                return std::unexpected(malformed("MPD returned an invalid queue position"));
            }
        } else if (ascii_case_equal(pair.name, "duration")) {
            current.duration = parse_seconds(pair.value);
            if (!current.duration) {
                return std::unexpected(malformed("MPD returned an invalid song duration"));
            }
        } else if (ascii_case_equal(pair.name, "Time")) {
            if (!current.duration) {
                const auto seconds = parse_unsigned<std::uint64_t>(pair.value);
                if (!seconds || *seconds > static_cast<std::uint64_t>(
                                               std::numeric_limits<std::int64_t>::max()) /
                                               1000U) {
                    return std::unexpected(malformed("MPD returned an invalid song time"));
                }
                current.duration =
                    std::chrono::milliseconds{static_cast<std::int64_t>(*seconds * 1000U)};
            }
        } else if (ascii_case_equal(pair.name, "Last-Modified")) {
            current.last_modified = pair.value;
        } else if (ascii_case_equal(pair.name, "Format")) {
            current.audio_format = pair.value;
        } else if (ascii_case_equal(pair.name, "Prio")) {
            current.priority = parse_unsigned<unsigned>(pair.value);
            if (!current.priority || *current.priority > 255U) {
                return std::unexpected(malformed("MPD returned an invalid queue priority"));
            }
        } else if (ascii_case_equal(pair.name, "X-Rating")) {
            current.rating = parse_unsigned<unsigned>(pair.value);
            if (!current.rating || *current.rating > 10U) {
                return std::unexpected(malformed("Melody returned an invalid track rating"));
            }
        } else if (ascii_case_equal(pair.name, "X-SongId")) {
            current.melody_song_id = parse_unsigned<std::uint64_t>(pair.value);
            if (!current.melody_song_id) {
                return std::unexpected(malformed("Melody returned an invalid database song ID"));
            }
        } else if (ascii_case_equal(pair.name, "Range") || ascii_case_equal(pair.name, "Added")) {
            current.unknown_structural_pairs.push_back(pair);
        } else {
            metadata.push_back(pair);
        }
    }

    if (has_current) {
        finish_track(current, metadata, tracks);
    }
    return tracks;
}

core::Result<std::vector<MelodyAlbum>> project_melody_albums(std::span<const Pair> pairs) {
    std::vector<MelodyAlbum> albums;
    bool has_current = false;
    MelodyAlbum current;
    const auto finish = [&] {
        if (has_current) {
            albums.push_back(std::move(current));
            current = MelodyAlbum{};
        }
    };
    for (const auto& pair : pairs) {
        if (ascii_case_equal(pair.name, "AlbumArtist")) {
            finish();
            current.album_artist = pair.value;
            has_current = true;
            continue;
        }
        if (!has_current) {
            return std::unexpected(
                malformed("Melody album response did not begin with an AlbumArtist pair"));
        }
        if (ascii_case_equal(pair.name, "Album")) {
            current.album = pair.value;
        } else if (ascii_case_equal(pair.name, "Date")) {
            current.date = pair.value;
        } else if (ascii_case_equal(pair.name, "X-AlbumId")) {
            const auto id = parse_unsigned<std::uint64_t>(pair.value);
            if (!id) {
                return std::unexpected(malformed("Melody returned an invalid album ID"));
            }
            current.album_id = *id;
        } else if (ascii_case_equal(pair.name, "X-TrackCount")) {
            current.track_count = parse_unsigned<unsigned>(pair.value).value_or(0U);
        } else if (ascii_case_equal(pair.name, "X-Duration")) {
            current.duration_seconds = parse_unsigned<std::uint64_t>(pair.value).value_or(0U);
        } else if (ascii_case_equal(pair.name, "X-Rating")) {
            const auto rating = parse_unsigned<unsigned>(pair.value);
            if (!rating || *rating > 10U) {
                return std::unexpected(malformed("Melody returned an invalid album rating"));
            }
            current.rating = *rating;
        } else if (ascii_case_equal(pair.name, "X-ComputedRating")) {
            double value = 0.0;
            const auto* begin = pair.value.data();
            const auto* end = begin + pair.value.size();
            const auto [parsed, error] = std::from_chars(begin, end, value);
            if (error != std::errc{} || parsed != end || value < 0.0 || value > 10.0) {
                return std::unexpected(
                    malformed("Melody returned an invalid computed album rating"));
            }
            current.computed_rating = value;
        } else if (ascii_case_equal(pair.name, "X-ArtworkUri")) {
            current.artwork_uri = pair.value;
        }
        // Unknown pairs are future extension lines; skip them.
    }
    finish();
    return albums;
}

core::Result<std::vector<TrackRating>> project_sticker_ratings(std::span<const Pair> pairs) {
    std::vector<TrackRating> ratings;
    std::string current_uri;
    for (const auto& pair : pairs) {
        if (ascii_case_equal(pair.name, "file")) {
            if (pair.value.empty()) {
                return std::unexpected(
                    malformed("MPD returned a sticker entry with an empty file URI"));
            }
            current_uri = pair.value;
            continue;
        }
        if (!ascii_case_equal(pair.name, "sticker")) {
            continue;
        }
        if (current_uri.empty()) {
            return std::unexpected(
                malformed("MPD sticker response did not begin with a file pair"));
        }
        const std::string_view value = pair.value;
        constexpr std::string_view prefix = "rating=";
        if (!value.starts_with(prefix)) {
            continue;
        }
        // Foreign clients own their sticker values; anything outside the
        // interoperable 0-10 integer scale is skipped, not a failure.
        const auto rating = parse_unsigned<unsigned>(value.substr(prefix.size()));
        if (rating && *rating >= 1U && *rating <= 10U) {
            ratings.push_back({.uri = current_uri, .rating = *rating});
        }
    }
    return ratings;
}

core::Result<std::vector<DatabaseEntry>>
project_database_entries(const std::span<const Pair> pairs) {
    std::vector<DatabaseEntry> entries;
    std::size_t begin = 0U;
    while (begin < pairs.size()) {
        if (!begins_database_entry(pairs[begin])) {
            return std::unexpected(malformed("MPD database response did not begin with an entry"));
        }
        std::size_t end = begin + 1U;
        while (end < pairs.size() && !begins_database_entry(pairs[end])) {
            ++end;
        }
        auto entry = project_database_entry(pairs.subspan(begin, end - begin));
        if (!entry) {
            return std::unexpected(std::move(entry.error()));
        }
        entries.push_back(std::move(*entry));
        begin = end;
    }
    return entries;
}

core::Result<std::vector<Track>> apply_queue_changes(const std::span<const Track> current,
                                                     const std::span<const Track> changed,
                                                     const std::size_t new_length) {
    if (new_length > current.size() && new_length - current.size() > changed.size()) {
        return std::unexpected(
            malformed("MPD queue diff length exceeds the records needed to construct it"));
    }
    std::vector<std::optional<Track>> slots(new_length);
    std::unordered_set<std::uint32_t> changed_ids;

    for (const auto& track : changed) {
        if (!track.queue_id || !track.queue_position) {
            return std::unexpected(malformed("MPD queue change omitted its song ID or position"));
        }
        if (*track.queue_position >= new_length || slots[*track.queue_position] ||
            !changed_ids.insert(*track.queue_id).second) {
            return std::unexpected(malformed("MPD queue changes have an inconsistent shape"));
        }
        slots[*track.queue_position] = track;
    }

    for (const auto& track : current) {
        if (!track.queue_id || !track.queue_position) {
            return std::unexpected(malformed("Current MPD queue omitted its song ID or position"));
        }
        if (changed_ids.contains(*track.queue_id) || *track.queue_position >= new_length ||
            slots[*track.queue_position]) {
            continue;
        }
        slots[*track.queue_position] = track;
    }

    std::vector<Track> result;
    result.reserve(new_length);
    std::unordered_set<std::uint32_t> result_ids;
    for (std::size_t position = 0U; position < slots.size(); ++position) {
        if (!slots[position] || !slots[position]->queue_id ||
            !result_ids.insert(*slots[position]->queue_id).second) {
            return std::unexpected(malformed("MPD queue diff cannot form a complete unique queue"));
        }
        slots[position]->queue_position = static_cast<std::uint32_t>(position);
        result.push_back(std::move(*slots[position]));
    }
    return result;
}

core::Result<PlaybackStatus> project_status(std::span<const Pair> pairs) {
    PlaybackStatus status;
    for (const auto& pair : pairs) {
        if (ascii_case_equal(pair.name, "state")) {
            if (pair.value == "stop") {
                status.state = PlaybackState::stopped;
            } else if (pair.value == "play") {
                status.state = PlaybackState::playing;
            } else if (pair.value == "pause") {
                status.state = PlaybackState::paused;
            } else {
                return std::unexpected(malformed("MPD returned an invalid playback state"));
            }
        } else if (ascii_case_equal(pair.name, "volume")) {
            if (pair.value == "-1") {
                status.volume.reset();
            } else {
                status.volume = parse_unsigned<unsigned>(pair.value);
                if (!status.volume || *status.volume > 100U) {
                    return std::unexpected(malformed("MPD returned an invalid volume"));
                }
            }
        } else if (ascii_case_equal(pair.name, "elapsed")) {
            status.elapsed = parse_seconds(pair.value);
            if (!status.elapsed) {
                return std::unexpected(malformed("MPD returned an invalid elapsed time"));
            }
        } else if (ascii_case_equal(pair.name, "duration")) {
            status.duration = parse_seconds(pair.value);
            if (!status.duration) {
                return std::unexpected(malformed("MPD returned an invalid playback duration"));
            }
        } else if (ascii_case_equal(pair.name, "playlist")) {
            status.queue_version = parse_unsigned<std::uint32_t>(pair.value);
            if (!status.queue_version) {
                return std::unexpected(malformed("MPD returned an invalid queue version"));
            }
        } else if (ascii_case_equal(pair.name, "playlistlength")) {
            status.queue_length = parse_unsigned<std::uint32_t>(pair.value);
            if (!status.queue_length) {
                return std::unexpected(malformed("MPD returned an invalid queue length"));
            }
        } else if (ascii_case_equal(pair.name, "song")) {
            status.song_position = parse_unsigned<std::uint32_t>(pair.value);
            if (!status.song_position) {
                return std::unexpected(malformed("MPD returned an invalid current song position"));
            }
        } else if (ascii_case_equal(pair.name, "songid")) {
            status.song_id = parse_unsigned<std::uint32_t>(pair.value);
            if (!status.song_id) {
                return std::unexpected(malformed("MPD returned an invalid current song ID"));
            }
        } else if (ascii_case_equal(pair.name, "nextsongid")) {
            status.next_song_id = parse_unsigned<std::uint32_t>(pair.value);
            if (!status.next_song_id) {
                return std::unexpected(malformed("MPD returned an invalid next song ID"));
            }
        } else if (ascii_case_equal(pair.name, "repeat")) {
            if (!parse_boolean(pair.value, status.repeat)) {
                return std::unexpected(malformed("MPD returned an invalid repeat state"));
            }
        } else if (ascii_case_equal(pair.name, "random")) {
            if (!parse_boolean(pair.value, status.random)) {
                return std::unexpected(malformed("MPD returned an invalid random state"));
            }
        } else if (ascii_case_equal(pair.name, "single") ||
                   ascii_case_equal(pair.name, "consume")) {
            auto mode = PlaybackModeState::unknown;
            if (pair.value == "0") {
                mode = PlaybackModeState::off;
            } else if (pair.value == "1") {
                mode = PlaybackModeState::on;
            } else if (pair.value == "oneshot") {
                mode = PlaybackModeState::oneshot;
            } else {
                return std::unexpected(malformed("MPD returned an invalid playback mode"));
            }
            if (ascii_case_equal(pair.name, "single")) {
                status.single = mode;
            } else {
                status.consume = mode;
            }
        } else if (ascii_case_equal(pair.name, "xfade")) {
            status.crossfade_seconds = parse_unsigned<unsigned>(pair.value);
            if (!status.crossfade_seconds) {
                return std::unexpected(malformed("MPD returned an invalid crossfade"));
            }
        } else if (ascii_case_equal(pair.name, "error")) {
            status.error = pair.value;
        } else {
            status.unknown_pairs.push_back(pair);
        }
    }
    return status;
}

core::Result<std::vector<Output>> project_outputs(std::span<const Pair> pairs) {
    std::vector<Output> outputs;
    Output current;
    bool has_current = false;

    for (const auto& pair : pairs) {
        if (ascii_case_equal(pair.name, "outputid")) {
            if (has_current) {
                outputs.push_back(std::move(current));
                current = Output{};
            }
            const auto id = parse_unsigned<std::uint32_t>(pair.value);
            if (!id) {
                return std::unexpected(malformed("MPD returned an invalid output ID"));
            }
            current.id = *id;
            has_current = true;
            continue;
        }
        if (!has_current) {
            return std::unexpected(malformed("MPD output response did not begin with outputid"));
        }

        if (ascii_case_equal(pair.name, "outputname")) {
            current.name = pair.value;
        } else if (ascii_case_equal(pair.name, "outputenabled")) {
            if (!parse_boolean(pair.value, current.enabled)) {
                return std::unexpected(malformed("MPD returned an invalid output enabled state"));
            }
        } else if (ascii_case_equal(pair.name, "plugin")) {
            current.plugin = pair.value;
        } else if (ascii_case_equal(pair.name, "outputprimary")) {
            bool value = false;
            if (!parse_boolean(pair.value, value)) {
                return std::unexpected(
                    malformed("Melody returned an invalid output primary state"));
            }
            current.primary = value;
        } else if (ascii_case_equal(pair.name, "outputonline")) {
            bool value = false;
            if (!parse_boolean(pair.value, value)) {
                return std::unexpected(malformed("Melody returned an invalid output online state"));
            }
            current.online = value;
        } else if (ascii_case_equal(pair.name, "outputformat")) {
            current.stream_format = pair.value;
        } else if (ascii_case_equal(pair.name, "outputmaxbitrate")) {
            current.maximum_bitrate = parse_unsigned<std::uint32_t>(pair.value);
            if (!current.maximum_bitrate) {
                return std::unexpected(malformed("Melody returned an invalid output bitrate"));
            }
        } else {
            current.attributes.push_back(pair);
        }
    }

    if (has_current) {
        outputs.push_back(std::move(current));
    }
    return outputs;
}

} // namespace trackknife::mpd

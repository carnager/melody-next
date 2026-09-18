// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/mpd/model.hpp"
#include "trackknife/mpd/music_root.hpp"
#include "trackknife/mpd/projection.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

void require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::abort();
    }
}

void metadata_and_musicbrainz_are_ordered() {
    using trackknife::mpd::Metadata;
    using trackknife::mpd::Pair;

    const Metadata metadata{{
        Pair{"Artist", "Credited Artist"},
        Pair{"MUSICBRAINZARTISTID", "artist-one"},
        Pair{"MusicBrainzArtistId", "artist-two"},
        Pair{"MusicBrainzAlbumId", "release"},
        Pair{"MusicBrainzReleaseGroupId", "release-group"},
        Pair{"MusicBrainzTrackId", "recording"},
        Pair{"MusicBrainzReleaseTrackId", "release-track"},
        Pair{"MusicBrainzWorkId", "work"},
        Pair{"ArtistSort", "Artist, Credited"},
        Pair{"X-Custom", "first"},
        Pair{"X-Custom", "second"},
    }};

    const auto artists = metadata.values("musicbrainzartistid");
    require(artists.size() == 2U && artists[0] == "artist-one" && artists[1] == "artist-two",
            "metadata lookup must be ASCII-case-insensitive and ordered");
    const auto custom = metadata.values("x-custom");
    require(custom.size() == 2U && custom[0] == "first" && custom[1] == "second",
            "unknown repeated fields must survive");

    const auto identity = trackknife::mpd::project_musicbrainz(metadata);
    require(identity.artist_ids == std::vector<std::string>{"artist-one", "artist-two"},
            "artist IDs must retain order");
    require(identity.release_ids == std::vector<std::string>{"release"}, "release ID must project");
    require(identity.release_group_ids == std::vector<std::string>{"release-group"},
            "release-group ID must project even beyond the baseline libmpdclient enum");
    require(identity.recording_ids == std::vector<std::string>{"recording"},
            "recording ID must project");
    require(identity.release_track_ids == std::vector<std::string>{"release-track"},
            "release-track ID must project");
    require(identity.work_ids == std::vector<std::string>{"work"}, "work ID must project");
}

void song_pairs_project_without_flattening() {
    using trackknife::mpd::Pair;
    const std::vector<Pair> pairs{
        {"file", "Artist/Album/01 - Song.flac"},
        {"Last-Modified", "2026-08-24T10:00:00Z"},
        {"Format", "96000:24:2"},
        {"Time", "181"},
        {"duration", "180.125"},
        {"Pos", "3"},
        {"Id", "42"},
        {"Prio", "192"},
        {"Artist", "First"},
        {"Artist", "Second"},
        {"MusicBrainzTrackId", "recording"},
        {"TotallyNewTag", "preserved"},
        {"file", "Artist/Album/02 - Other.flac"},
        {"Title", "Other"},
    };

    const auto result = trackknife::mpd::project_tracks(pairs);
    require(result.has_value(), "valid song pairs must project");
    require(result->size() == 2U, "each file pair must start a song");
    const auto& first = result->front();
    require(first.queue_id == 42U && first.queue_position == 3U, "queue identity must be parsed");
    require(first.priority == 192U, "queue priority must remain typed");
    require(first.duration == std::chrono::milliseconds{180125},
            "fractional duration must take precedence over whole Time");
    require(first.metadata.values("Artist").size() == 2U, "repeated tags must not flatten");
    require(first.metadata.first("TotallyNewTag") == "preserved",
            "unknown metadata must survive projection");
    require(first.musicbrainz.recording_ids == std::vector<std::string>{"recording"},
            "MusicBrainz identity must be attached to the track");
}

void database_entries_keep_kind_order_and_extensions() {
    using trackknife::mpd::Pair;
    const std::vector<Pair> pairs{
        {"directory", "Slayer"},
        {"Last-Modified", "2026-08-24T10:00:00Z"},
        {"future-directory-field", "preserved"},
        {"file", "Slayer/Divine Intervention/01.flac"},
        {"Artist", "Slayer"},
        {"Title", "Killing Fields"},
        {"playlist", "Road mix"},
        {"Last-Modified", "2026-08-24T11:00:00Z"},
        {"future-playlist-field", "preserved"},
    };

    const auto result = trackknife::mpd::project_database_entries(pairs);
    require(result && result->size() == 3U,
            "lsinfo entries must retain their heterogeneous server order");
    const auto* directory = std::get_if<trackknife::mpd::DatabaseDirectory>(&result->at(0));
    const auto* track = std::get_if<trackknife::mpd::Track>(&result->at(1));
    const auto* playlist = std::get_if<trackknife::mpd::StoredPlaylist>(&result->at(2));
    const std::vector<Pair> expected_directory_unknown{{"future-directory-field", "preserved"}};
    const std::vector<Pair> expected_playlist_unknown{{"future-playlist-field", "preserved"}};
    require(directory != nullptr && directory->uri == "Slayer" &&
                directory->unknown_pairs == expected_directory_unknown,
            "directory identity and unknown fields must survive");
    require(track != nullptr && track->metadata.first("Title") == "Killing Fields",
            "database songs must retain arbitrary metadata");
    require(playlist != nullptr && playlist->name == "Road mix" &&
                playlist->unknown_pairs == expected_playlist_unknown,
            "stored-playlist identity and unknown fields must survive");
}

void search_results_sort_by_release_track_order() {
    const auto make_track = [](std::string artist, std::string album, std::string disc,
                               std::string track_number, std::string title,
                               std::string album_artist_sort = {}) {
        std::vector<trackknife::mpd::Pair> fields{
            {"Artist", std::move(artist)},
            {"Album", std::move(album)},
            {"Disc", std::move(disc)},
            {"Track", std::move(track_number)},
            {"Title", title},
        };
        if (!album_artist_sort.empty()) {
            fields.push_back({"AlbumArtistSort", std::move(album_artist_sort)});
        }
        trackknife::mpd::Metadata metadata{std::move(fields)};
        return trackknife::mpd::Track{
            .uri = title + ".flac",
            .metadata = metadata,
            .musicbrainz = trackknife::mpd::project_musicbrainz(metadata),
            .queue_id = std::nullopt,
            .queue_position = std::nullopt,
            .duration = std::nullopt,
            .last_modified = std::nullopt,
            .audio_format = std::nullopt,
            .priority = std::nullopt,
            .rating = std::nullopt,
            .melody_song_id = std::nullopt,
            .unknown_structural_pairs = {},
        };
    };

    std::vector tracks{
        make_track("The Alpha", "Second", "1/1", "1/1", "second album", "Alpha, The"),
        make_track("The Alpha", "First", "2/2", "1/2", "disc two", "Alpha, The"),
        make_track("Beta", "Only", "1", "1", "beta"),
        make_track("The Alpha", "First", "1/2", "10/10", "track ten", "Alpha, The"),
        make_track("Aardvark", "Only", "1", "1", "fallback artist"),
        make_track("The Alpha", "First", "1/2", "2/10", "Zulu", "Alpha, The"),
        make_track("The Alpha", "First", "1/2", "2/10", "alpha", "Alpha, The"),
    };

    trackknife::mpd::sort_search_results(tracks);

    std::vector<std::string_view> titles;
    titles.reserve(tracks.size());
    for (const auto& track : tracks) {
        titles.push_back(*track.metadata.first("Title"));
    }
    require(titles == std::vector<std::string_view>{"fallback artist", "alpha", "Zulu", "track ten",
                                                    "disc two", "second album", "beta"},
            "search results must sort by sort-aware album artist, album, numeric disc/track, and "
            "title");
}

void output_pairs_keep_stock_and_melody_state() {
    using trackknife::mpd::Pair;
    const std::vector<Pair> pairs{
        {"outputid", "0"},
        {"outputname", "PipeWire"},
        {"outputenabled", "1"},
        {"plugin", "pipewire"},
        {"attribute", "allowed_formats=96000:24:2"},
        {"outputid", "1"},
        {"outputname", "living-room"},
        {"outputenabled", "1"},
        {"outputprimary", "0"},
        {"outputonline", "0"},
        {"plugin", "agent"},
        {"outputformat", "opus"},
        {"outputmaxbitrate", "192"},
        {"futuremelodyfield", "keep-me"},
    };

    const auto result = trackknife::mpd::project_outputs(pairs);
    require(result.has_value() && result->size() == 2U, "output records must project");
    require(result->at(0).enabled && !result->at(0).online.has_value(),
            "stock output must not invent Melody online state");
    const auto& melody = result->at(1);
    require(melody.enabled && melody.online == false && melody.primary == false,
            "offline Melody output must remain enabled and non-primary");
    require(melody.stream_format == "opus" && melody.maximum_bitrate == 192U,
            "Melody stream preferences must project");
    require(melody.attributes == std::vector<Pair>{{"futuremelodyfield", "keep-me"}},
            "unknown output extensions must survive");
}

void status_pairs_project_typed_state_and_preserve_extensions() {
    using trackknife::mpd::Pair;
    const std::vector<Pair> pairs{
        {"volume", "72"},        {"repeat", "1"},
        {"random", "0"},         {"single", "oneshot"},
        {"consume", "0"},        {"playlist", "43"},
        {"playlistlength", "8"}, {"state", "play"},
        {"songid", "73"},        {"nextsongid", "74"},
        {"elapsed", "12.125"},   {"duration", "220.9"},
        {"xfade", "5"},          {"future-status", "preserved"},
    };

    const auto result = trackknife::mpd::project_status(pairs);
    require(result.has_value(), "valid status pairs must project");
    require(result->state == trackknife::mpd::PlaybackState::playing && result->volume == 72U,
            "playback and volume state must project");
    require(result->elapsed == std::chrono::milliseconds{12125} &&
                result->duration == std::chrono::milliseconds{220900},
            "fractional playback times must project to milliseconds");
    require(result->single == trackknife::mpd::PlaybackModeState::oneshot && result->repeat,
            "extended playback modes must retain their semantics");
    require(result->unknown_pairs == std::vector<Pair>{{"future-status", "preserved"}},
            "unknown status fields must survive projection");

    require(!trackknife::mpd::project_status(std::vector<Pair>{{"volume", "101"}}),
            "out-of-range volume must fail");
}

trackknife::mpd::Track queue_track(const std::uint32_t id, const std::uint32_t position) {
    trackknife::mpd::Track track;
    track.uri = "track-" + std::to_string(id) + ".flac";
    track.queue_id = id;
    track.queue_position = position;
    return track;
}

void queue_changes_reconstruct_or_reject_the_new_shape() {
    const std::vector current{queue_track(1U, 0U), queue_track(2U, 1U), queue_track(3U, 2U)};

    const std::vector appended_change{queue_track(4U, 3U)};
    const auto appended = trackknife::mpd::apply_queue_changes(current, appended_change, 4U);
    require(appended && appended->at(3).queue_id == 4U,
            "an appended plchanges record must retain unchanged occurrences");

    const std::vector deletion_change{queue_track(3U, 1U)};
    const auto deleted = trackknife::mpd::apply_queue_changes(current, deletion_change, 2U);
    require(deleted && deleted->at(0).queue_id == 1U && deleted->at(1).queue_id == 3U,
            "a deletion diff must remove the displaced old occurrence");

    const std::vector moved_changes{queue_track(3U, 0U), queue_track(1U, 1U), queue_track(2U, 2U)};
    const auto moved = trackknife::mpd::apply_queue_changes(current, moved_changes, 3U);
    require(moved && moved->front().queue_id == 3U && moved->back().queue_id == 2U,
            "a move diff must reconstruct order by stable IDs and reported positions");

    require(!trackknife::mpd::apply_queue_changes(current, {}, 4U),
            "an incomplete queue diff must request a full-snapshot fallback");
    require(!trackknife::mpd::apply_queue_changes(
                current, std::vector{queue_track(3U, 0U), queue_track(3U, 1U)}, 3U),
            "duplicate IDs in a queue diff must be rejected");
}

void local_root_mapping_is_literal_and_contained() {
    const std::filesystem::path root{"/srv/music"};

    const auto ordinary =
        trackknife::mpd::resolve_below_music_root(root, "Artist/Album/01 - Song.flac");
    require(ordinary == std::filesystem::path{"/srv/music/Artist/Album/01 - Song.flac"},
            "relative MPD path must map below root");

    const auto unicode = trackknife::mpd::resolve_below_music_root(root, "Björk/Debut.flac");
    require(unicode == std::filesystem::path{"/srv/music/Björk/Debut.flac"},
            "valid UTF-8 bytes must map losslessly on Linux");

    const auto percent = trackknife::mpd::resolve_below_music_root(root, "100%25/Track.flac");
    require(percent == std::filesystem::path{"/srv/music/100%25/Track.flac"},
            "MPD file URIs must not be URL-percent-decoded");

    require(!trackknife::mpd::resolve_below_music_root(root, "../outside.flac"),
            "parent traversal must fail");
    require(!trackknife::mpd::resolve_below_music_root(root, "Artist//Track.flac"),
            "empty components must fail");
    require(!trackknife::mpd::resolve_below_music_root(root, "/etc/passwd"),
            "absolute paths must fail");
    require(!trackknife::mpd::resolve_below_music_root(root, "https://example.test/a.flac"),
            "remote URLs must not map locally");
    require(!trackknife::mpd::resolve_below_music_root("relative", "Track.flac"),
            "relative roots must fail");

    const std::string invalid_utf8{"bad\xFFname.flac", 13U};
    require(!trackknife::mpd::resolve_below_music_root(root, invalid_utf8),
            "invalid protocol UTF-8 must not become a guessed local path");
}

void ratings_project_from_melody_lines_and_sticker_entries() {
    using trackknife::mpd::Pair;
    const std::vector<Pair> song_pairs{
        {"file", "Artist/Album/01 - Song.flac"},
        {"Title", "Song"},
        {"X-Rating", "8"},
        {"X-SongId", "4711"},
        {"X-AlbumId", "12"},
        {"file", "Artist/Album/02 - Other.flac"},
        {"Title", "Other"},
    };
    const auto songs = trackknife::mpd::project_tracks(song_pairs);
    require(songs.has_value() && songs->size() == 2U, "Melody song lines must project");
    require(songs->front().rating == 8U && songs->front().melody_song_id == 4711U,
            "Melody rating and database song id must be typed");
    require(!songs->back().rating && !songs->back().melody_song_id,
            "unrated songs must stay unrated");
    require(songs->front().metadata.first("X-Rating") == std::nullopt,
            "projected extension lines must not leak into tag metadata");
    require(!trackknife::mpd::project_tracks(
                std::vector<Pair>{{"file", "a.flac"}, {"X-Rating", "11"}}),
            "a rating outside 0-10 must fail projection");

    const std::vector<Pair> sticker_pairs{
        {"file", "a.flac"},          {"sticker", "rating=10"}, {"file", "b.flac"},
        {"sticker", "rating=text"},  {"file", "c.flac"},       {"sticker", "rating=99"},
        {"file", "d.flac"},          {"sticker", "playcount=4"},
    };
    const auto ratings = trackknife::mpd::project_sticker_ratings(sticker_pairs);
    require(ratings.has_value() && ratings->size() == 1U,
            "only interoperable rating stickers may project");
    require(ratings->front().uri == "a.flac" && ratings->front().rating == 10U,
            "sticker ratings must keep their URI and value");
    require(!trackknife::mpd::project_sticker_ratings(
                std::vector<Pair>{{"sticker", "rating=5"}}),
            "sticker entries without a file identity must fail");
}

void malformed_responses_fail() {
    using trackknife::mpd::Pair;
    require(!trackknife::mpd::project_tracks(std::vector<Pair>{{"Artist", "orphan"}}),
            "song response without file must fail");
    require(!trackknife::mpd::project_tracks(
                std::vector<Pair>{{"file", "a.flac"}, {"Id", "not-an-id"}}),
            "invalid song ID must fail");
    require(!trackknife::mpd::project_outputs(
                std::vector<Pair>{{"outputid", "1"}, {"outputenabled", "yes"}}),
            "invalid output boolean must fail");
    require(
        !trackknife::mpd::project_database_entries(std::vector<Pair>{{"Last-Modified", "orphan"}}),
        "database fields without an entry identity must fail");
}

} // namespace

int main() {
    metadata_and_musicbrainz_are_ordered();
    song_pairs_project_without_flattening();
    database_entries_keep_kind_order_and_extensions();
    search_results_sort_by_release_track_order();
    status_pairs_project_typed_state_and_preserve_extensions();
    queue_changes_reconstruct_or_reject_the_new_shape();
    output_pairs_keep_stock_and_melody_state();
    local_root_mapping_is_literal_and_contained();
    ratings_project_from_melody_lines_and_sticker_entries();
    malformed_responses_fail();
    return EXIT_SUCCESS;
}

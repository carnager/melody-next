// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QString>
#include <Qt>

#include <array>

namespace trackknife::ui {

// Optional QTableView properties used by the shared grouped delegate. Models
// with a different physical column projection set these to their logical
// metadata columns. The default projection is the shared seven-column
// Trackknife contract.
inline constexpr auto track_artwork_column_property = "trackknife-track-artwork-column";
inline constexpr auto track_artist_column_property = "trackknife-track-artist-column";
inline constexpr auto track_number_column_property = "trackknife-track-number-column";
inline constexpr auto track_title_column_property = "trackknife-track-title-column";
inline constexpr auto track_album_column_property = "trackknife-track-album-column";
inline constexpr auto track_date_column_property = "trackknife-track-date-column";
inline constexpr auto track_length_column_property = "trackknife-track-length-column";
inline constexpr auto track_separate_number_property = "trackknife-track-separate-number";
inline constexpr auto track_side_artwork_property = "trackknife-track-side-artwork";

// Shared role and column contract for the album-grouped track presentation
// (QueueItemDelegate/QueueTableView). Any model rendered by the shared
// delegate provides these roles and the seven-column layout below; the MPD
// queue model and Trackknife's local list model both implement it.
enum TrackRowRole : int {
    track_source_role = Qt::UserRole + 1, // MPD URI or raw local path bytes
    track_id_role,                        // stable per-occurrence identity
    track_position_role,                  // display position
    track_duration_ms_role,               // qint64 duration in milliseconds
    track_current_role,                   // bool: the playing occurrence
    track_album_artist_role,              // grouping artist with fallbacks
    track_priority_role,                  // uint MPD priority; 0/absent hides
    track_album_artwork_role,             // QImage cover for the group header
    track_album_artwork_key_role,         // artwork cache identity
    // Optional fast path: bool, true only for the first row of a group with
    // at least two members. Views fall back to adjacent group-key comparison.
    track_album_group_start_role,
    track_rating_role,       // uint 0-10 track rating (ADR-0179); 0/absent unrated
    track_album_rating_role, // uint 0-10 album rating painted over covers
};

// Complete physical column layout used by both authority-bound queues:
// artwork/status, artist, track number, title, album, date, duration, rating.
enum TrackRowColumn : int {
    track_artwork_column = 0,
    track_marker_column = track_artwork_column,
    track_artist_column = 1,
    track_number_column = 2,
    track_title_column = 3,
    track_album_column = 4,
    track_date_column = 5,
    track_length_column = 6,
    track_rating_column = 7,
    track_play_count_column = 8,
    track_last_played_column = 9,
    track_column_count = 10,
};

// Column labels are part of the same shared contract as their positions. Keep
// authority-specific models from drifting into near-equivalent names.
inline constexpr std::array<const char*, track_column_count> track_column_headers{
    "", "Artist", "#", "Title", "Album", "Date", "Length", "Rating", "Play count", "Last played"};

// The shared 0-10 rating rendered as star text: full stars in half-star
// steps, nothing for unrated so the column stays quiet.
[[nodiscard]] inline QString track_rating_stars(const unsigned rating) {
    if (rating == 0U || rating > 10U) {
        return {};
    }
    QString stars;
    for (unsigned star = 0U; star < rating / 2U; ++star) {
        stars += QChar{0x2605}; // BLACK STAR
    }
    if (rating % 2U != 0U) {
        stars += QChar{0x00BD}; // VULGAR FRACTION ONE HALF
    }
    return stars;
}

} // namespace trackknife::ui

// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "uicommon/track_row_roles.hpp"

#include <QHash>
#include <QList>
#include <QString>

#include <functional>
#include <utility>

class QAbstractItemModel;

namespace trackknife::ui {

// How a track list groups its rows into albums, and what each group adds
// above its first row -- the rules both windows draw by, so an album reads
// the same in each. A group is two or more consecutive rows with the same
// album artist, album and date; a row in none is a loose track.

// The measures of a grouped list.
inline constexpr int track_row_height = 22;
inline constexpr int album_header_height = 34;
// Space above a run of loose tracks that follows an album, where its
// hairline goes.
inline constexpr int loose_run_gap = 10;
// Where a group's hairline is drawn, below the top of its header or gap, so
// it does not touch the row above.
inline constexpr int hairline_offset = 5;
// Above the first track of each disc, in an album of several.
inline constexpr int disc_header_height = 26;
// The cover in an album's header strip, when covers are not beside the rows.
inline constexpr int album_cover_extent = 22;
// A cover beside its album's rows: at most this, inside this padding.
inline constexpr int maximum_side_artwork_extent = 160;
inline constexpr int side_artwork_padding = 6;

// Which of a model's columns hold the album and date its groups are made of.
// And whether a lone track is an album of its own -- its header and cover
// as any album's -- rather than a loose track with its cover in place of
// its number (the Qt Quick window's choice).
struct TrackGroupColumns {
    int album{track_album_column};
    int date{track_date_column};
    bool lone_tracks_grouped{false};
};

[[nodiscard]] QString trackGroupKey(const QAbstractItemModel& model, int row,
                                    TrackGroupColumns columns = {});
// The row is one of a group of two or more.
[[nodiscard]] bool inTrackGroup(const QAbstractItemModel& model, int row,
                                TrackGroupColumns columns = {});
// The row begins a group of two or more.
[[nodiscard]] bool beginsTrackGroup(const QAbstractItemModel& model, int row,
                                    TrackGroupColumns columns = {});
// A loose track whose row above belongs to a group: a gap and a hairline.
[[nodiscard]] bool beginsLooseRun(const QAbstractItemModel& model, int row,
                                  TrackGroupColumns columns = {});
// "Disc 2" where a disc begins in an album of several, else empty.
[[nodiscard]] QString trackDiscStart(const QAbstractItemModel& model, int row);
// What a row adds above its track: an album's header, a run's gap, or none
// -- and a disc's name where one of several begins.
[[nodiscard]] int trackGroupSpacing(const QAbstractItemModel& model, int row,
                                    TrackGroupColumns columns = {});
// The first and last rows of the group `row` is in.
[[nodiscard]] std::pair<int, int> trackGroupRange(const QAbstractItemModel& model, int row,
                                                  TrackGroupColumns columns = {});

// What an album group's header says: the album, and a quieter line of
// "artist · year · N tracks · length" beside it.
struct AlbumHeaderText {
    QString album;
    QString details;
};

// The header text of the group starting at `first_row`, read from its rows.
[[nodiscard]] AlbumHeaderText albumHeaderText(const QAbstractItemModel& model, int first_row,
                                              int album_column, int date_column);

// The widths a list's `visible` columns take in `available` pixels: each
// column not `expanding` its preferred width; the expanding ones -- the
// artist, title and album -- share the rest in proportion to how far their
// preferred width is above their minimum, the last taking what is left. Too
// narrow for that, every column takes part.
[[nodiscard]] QHash<int, int> fitTrackColumns(const QList<int>& visible, QList<int> expanding,
                                              const std::function<int(int)>& minimum,
                                              const std::function<int(int)>& preferred,
                                              int available);

// What a grouped list's cell shows, by the rules of the album view: inside
// an album what its header says is not repeated -- album and date go, and
// the artist stays only where it differs, quietly; a loose track's cover
// stands where its number would; what a hidden column would have said
// follows the title, quieter.
struct TrackCellContext {
    TrackGroupColumns groups;
    int artwork{track_artwork_column};
    int artist{track_artist_column};
    int number{track_number_column};
    int title{track_title_column};
    // Covers beside the rows rather than in the album's header strip.
    bool side_artwork{false};
    // The number has a column of its own and is not put before the title.
    bool separate_number{true};
    // Whether a column is hidden now.
    std::function<bool(int)> hidden;
};

struct TrackCell {
    QString text;
    // After the title, quieter: " — " is put before it when drawn.
    QString suffix;
    // Drawn in the quiet (placeholder) colour.
    bool quiet{false};
    bool right_aligned{false};
    // The playing track's row: its text bold in the accent.
    bool current{false};
    // The playing mark goes in this cell.
    bool playing_icon{false};
    // A loose track's small cover goes at this cell's right end.
    bool inline_cover{false};
    // The cover gutter: never selected, never text.
    bool artwork_cell{false};
    bool in_group{false};
};

[[nodiscard]] TrackCell trackCell(const QAbstractItemModel& model, int row, int column,
                                  const TrackCellContext& context);

// m:ss, or h:mm:ss with hours.
[[nodiscard]] QString formatTrackDuration(qint64 milliseconds);

} // namespace trackknife::ui

// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "bench/local_list_model.hpp"
#include "trackknife/persistence/local_library.hpp"

#include <QIcon>
#include <QObject>
#include <QPalette>
#include <QString>
#include <QStringList>

#include <array>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>

namespace trackknife::bench {

[[nodiscard]] QIcon albumShuffleIcon(const QPalette& palette);

// Whether a track view's rows are files on the remote engine's machine
// (ADR-0227): a remote tab's, or dynamic results drawn from its library.
[[nodiscard]] inline bool isRemoteView(const QObject* view) {
    return view != nullptr && view->property("bench-remote-list").toBool();
}

struct TrackColumnSpec {
    int logical;
    const char* id;
    const char* label;
    int default_width;
    int minimum_width;
};

// What a new list is called until someone names it -- the one a window opens
// with, or starts again with after its last tab closes. Not "Local Queue":
// the queue is Up Next, and "local" was the MPD era's word for this computer.
inline constexpr auto untitled_list_name = "Untitled";

// The workspace's standalone commands: what the command palette offers, and
// what the shortcut editor lets a key be given to whether or not it has one
// by default. Device names, rating values, column names and transient
// context-menu choices are parameters, not commands.
inline constexpr std::array workspace_command_ids{"action-open-files",
                                                  "action-open-folder",
                                                  "action-open-list",
                                                  "action-lists-panel",
                                                  "action-new-list",
                                                  "action-import-m3u8",
                                                  "action-export-m3u8",
                                                  "action-dynamic-playlists",
                                                  "action-settings",
                                                  "action-search-dialog",
                                                  "action-focus-library-search",
                                                  "action-quick-album",
                                                  "action-quick-track",
                                                  "action-find-in-list",
                                                  "action-jump-to-playing",
                                                  "action-follow-playback",
                                                  "action-show-up-next",
                                                  "action-play-pause",
                                                  "action-stop",
                                                  "action-next-track",
                                                  "action-previous-track",
                                                  "action-save-list",
                                                  "action-rename-tab",
                                                  "action-duplicate-tab",
                                                  "action-close-tab",
                                                  "action-track-properties",
                                                  "action-replaygain-dialog",
                                                  "action-convert-files",
                                                  "action-reverse-list",
                                                  "action-shuffle-albums",
                                                  "action-local-album-random",
                                                  "action-deduplicate-list",
                                                  "action-backup-workspace",
                                                  "action-restore-workspace"};

inline constexpr std::array<TrackColumnSpec, local_column_count> track_column_specs{{
    {local_artwork_column, "artwork", "Artwork", 110, 72},
    {local_artist_column, "artist", "Artist", 150, 72},
    {local_track_number_column, "track-number", "Track number", 46, 36},
    {local_title_column, "title", "Title", 220, 96},
    {local_album_column, "album", "Album", 160, 72},
    {local_date_column, "date", "Date", 64, 52},
    {local_length_column, "length", "Length", 68, 56},
    {local_rating_column, "rating", "Rating", 84, 56},
    {local_play_count_column, "play-count", "Play count", 88, 60},
    {local_last_played_column, "last-played", "Last played", 170, 100},
}};

[[nodiscard]] QString trackColumnId(int logical);
[[nodiscard]] int trackColumnLogical(const QString& id);
[[nodiscard]] QStringList trackColumnIds();
[[nodiscard]] QString displayText(const std::string& utf8);
[[nodiscard]] std::string utf8Bytes(const QString& text);
[[nodiscard]] QString formatTime(qint64 milliseconds);
[[nodiscard]] std::string lowercased_ascii(std::string name);
[[nodiscard]] std::optional<std::string_view> probed_semantic_alias(std::string_view native_name);
void remove_shadowed_probed_metadata(metadata::MetadataDocument& document);
[[nodiscard]] std::string metadata_value(const metadata::MetadataDocument& document,
                                         std::initializer_list<std::string_view> candidate_names);
void project_display_metadata(LocalTrackRow& row);
[[nodiscard]] LocalTrackRow cached_library_row(persistence::LibraryTrackSnapshot snapshot);

// ADR-0139: the stable CUE logical identity ("cue-v1" NUL sheet path NUL
// file index NUL track index), shared by ingest, apply capture, and
// committed-sheet view refresh.
[[nodiscard]] std::string cue_track_logical_reference(const std::string& raw_cue_path,
                                                      std::size_t file_index,
                                                      std::size_t track_index);

struct CueLogicalReferenceParts {
    std::string raw_cue_path;
    std::size_t file_index{0U};
    std::size_t track_index{0U};
};

[[nodiscard]] std::optional<CueLogicalReferenceParts>
parse_cue_logical_reference(const std::string& reference);

} // namespace trackknife::bench

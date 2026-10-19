// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// ADR-0296: a playlist file in an engine's playlist folder, as read and as
// written -- M3U and M3U8, and PLS. Lenient where the strict importer
// (m3u8.hpp) is not: a folder holds what other players wrote, and a line
// that cannot be used is passed over and counted, not a reason to refuse
// the file. Pure: what exists on disk is asked of the caller.

#include "trackknife/core/result.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace trackknife::lists {

// How a file names its tracks, kept when it is written again: relative to
// the playlist's own folder (the M3U convention), relative to the music
// folder the track is in (MPD's), or whole paths.
enum class PlaylistPathStyle : std::uint8_t { playlist_relative, root_relative, absolute };

struct PlaylistFileEntry {
    std::string raw_path;
    std::string title;
    std::optional<std::int64_t> duration_ms;
    // What Melody keeps of the entry that other players do not read, as the
    // "#MELODY:" line before it said it: a JSON object, empty for none.
    std::string melody;
};

struct PlaylistFile {
    std::vector<PlaylistFileEntry> entries;
    PlaylistPathStyle style{PlaylistPathStyle::playlist_relative};
    // The list it is, as its "#MELODY-LIST:" line said; else none.
    std::string list_id;
    // Lines naming no file this engine can read -- a web address, a Windows
    // drive -- passed over.
    std::size_t skipped{0};
};

enum class PlaylistFormat : std::uint8_t { m3u, pls };
// By the file's name: .m3u, .m3u8 or .pls; else none.
[[nodiscard]] std::optional<PlaylistFormat> playlist_format(std::string_view file_name);

// Reads a playlist at `absolute_path`. A relative line is the file under the
// playlist's folder, else under each of `music_roots` -- the first of them
// that `exists` says is there, else the playlist's folder.
[[nodiscard]] core::Result<PlaylistFile>
parse_playlist_file(std::string_view bytes, const std::string& absolute_path,
                    std::span<const std::string> music_roots,
                    const std::function<bool(const std::string&)>& exists);

// The file again, in `style` -- a track outside every music root, or outside
// the playlist's folder's reach, named whole -- with its "#MELODY:" lines and
// `list_id`'s "#MELODY-LIST:" line. PLS keeps neither.
[[nodiscard]] std::string render_playlist_file(std::span<const PlaylistFileEntry> entries,
                                               const std::string& absolute_path,
                                               PlaylistPathStyle style,
                                               std::span<const std::string> music_roots,
                                               std::string_view list_id);

} // namespace trackknife::lists

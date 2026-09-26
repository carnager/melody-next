// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace trackknife::audio {

// The fields that decide whether two rows belong to the same album.
struct AlbumRowKey final {
    std::string album_artist;
    std::string artist;
    std::string album;
    std::string date;

    friend bool operator==(const AlbumRowKey&, const AlbumRowKey&) = default;
};

// Album shuffle builds one key per row and holds them all at once, so a
// pathological list -- a million rows, or tags carrying megabytes of text --
// has to be refused rather than allowed to exhaust memory.
struct AlbumGroupingLimits final {
    std::size_t max_row_key_bytes{64U * 1024U};
    std::size_t max_total_key_bytes{64U * 1024U * 1024U};
    std::size_t max_rows{1'000'000U};

    friend bool operator==(const AlbumGroupingLimits&, const AlbumGroupingLimits&) = default;
};

// Groups row indexes by album, in list order within each group.
//
// Two rules that are easy to miss when they are inline: the album artist falls
// back to the track artist, so a release tagged only per-track still groups;
// and a row with no album at all becomes its own group rather than joining a
// shared "untitled" one, because unrelated untagged files are not an album.
class AlbumGrouper final {
  public:
    AlbumGrouper() = default;
    explicit AlbumGrouper(const AlbumGroupingLimits& limits) : limits_(limits) {}

    // Whether a list of this size may be grouped at all.
    [[nodiscard]] bool admits(std::size_t row_count) const noexcept {
        return row_count <= limits_.max_rows;
    }

    // Adds one row. Returns false once the budget is exhausted, at which point
    // the grouping is abandoned: a truncated album order would silently play
    // the tail of a list in the wrong order, which is worse than refusing.
    [[nodiscard]] bool add(const AlbumRowKey& key, int row);

    [[nodiscard]] std::vector<std::vector<int>> take() noexcept {
        keys_.clear();
        return std::move(groups_);
    }

  private:
    AlbumGroupingLimits limits_;
    std::map<std::tuple<std::string, std::string, std::string>, std::size_t> keys_;
    std::vector<std::vector<int>> groups_;
    std::size_t key_bytes_{0};
};

} // namespace trackknife::audio

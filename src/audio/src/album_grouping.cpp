// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/audio/album_grouping.hpp"

namespace trackknife::audio {

bool AlbumGrouper::add(const AlbumRowKey& key, const int row) {
    const auto& artist = key.album_artist.empty() ? key.artist : key.album_artist;
    const auto bytes = artist.size() + key.album.size() + key.date.size();
    key_bytes_ += bytes;
    if (bytes > limits_.max_row_key_bytes || key_bytes_ > limits_.max_total_key_bytes) {
        return false;
    }

    auto group = groups_.size();
    // An untitled row is its own album: unrelated untagged files sharing an
    // empty album name are not a release.
    if (!key.album.empty()) {
        const auto [found, inserted] =
            keys_.try_emplace(std::tuple{artist, key.album, key.date}, group);
        group = found->second;
        static_cast<void>(inserted);
    }
    if (group == groups_.size()) {
        groups_.emplace_back();
    }
    groups_[group].push_back(row);
    return true;
}

} // namespace trackknife::audio

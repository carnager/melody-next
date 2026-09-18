// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/metadata/document.hpp"

#include <string>
#include <string_view>

namespace trackknife::persistence {

// ADR-0179: content-identity keys for stored ratings, byte-compatible with
// the Melody server's hashes so a future rating synchronization is a pure
// key join. Identity follows tags, never paths, so ratings survive rescans,
// renames, and moves; retagging the identity fields detaches a rating.
[[nodiscard]] std::string track_rating_hash(std::string_view album_artist, std::string_view album,
                                            std::string_view title, int track_number);
[[nodiscard]] std::string album_rating_hash(std::string_view album_artist, std::string_view album,
                                            std::string_view date);

struct RatingIdentity {
    std::string track_hash;
    std::string album_hash;
};

// Applies Melody's normalization before hashing: album artist falls back to
// artist and then "Unknown Artist", album to the parent directory name,
// title to the file stem, and the date is the four-digit year or "0000".
[[nodiscard]] RatingIdentity rating_identity(const metadata::MetadataDocument& document,
                                             const std::string& raw_path);

} // namespace trackknife::persistence

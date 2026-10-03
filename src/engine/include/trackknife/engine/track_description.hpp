// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "trackknife/persistence/local_library.hpp"
#include "trackknife/protocol/message.hpp"

#include <optional>
#include <string>

namespace trackknife::engine {

// ADR-0259: an indexed track as `list.get {describe}` tells a client of it --
// its tags, technical facts, ratings and the file's revision -- leaner than
// `catalogue.cached_tracks`, which also carries what a query matches on:
//
//   {"fields": {name: [value]}, "duration_ms", "codec", "sample_rate",
//    "bits", "channels", "rating", "album_rating",
//    "revision": {device, inode, size, seconds, nanoseconds} | null}
[[nodiscard]] protocol::Json describe_track(const persistence::LibraryTrackSnapshot& track);

// The track `description` describes, as the file at `raw_path`; nothing for
// one that is not a description.
[[nodiscard]] std::optional<persistence::LibraryTrackSnapshot>
described_track(const protocol::Json& description, std::string raw_path);

} // namespace trackknife::engine

// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/cancellation.hpp"

#include <QImage>

#include <cstdint>

#include <string>

namespace trackknife::ui {

// The largest cover, in pixels, that artwork is decoded from -- everywhere,
// so the list, the library and the tagger agree about whether an album has a
// cover. A guard against decompression bombs, not a quality bar: real front
// cover scans run past 16 megapixels (4320x4000 is ordinary for a vinyl
// scan), and a lower limit in one place than another shows a cover in the
// tagger and none in the list for the same file.
inline constexpr std::int64_t maximum_artwork_pixels = 32'000'000;

// Worker-only local thumbnail reader shared by lists and the library. Loads
// embedded art first, then conventional sibling images; never uses the network.
[[nodiscard]] QImage loadLocalArtwork(const std::string& raw_path,
                                      const core::CancellationToken& cancellation = {});

} // namespace trackknife::ui

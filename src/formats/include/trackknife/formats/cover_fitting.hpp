// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "trackknife/core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace trackknife::formats {

// A cover made to be written, with its size in pixels.
struct FittedCover {
    std::vector<std::uint8_t> bytes;
    int width{0};
    int height{0};
};

// A cover no longer than `longest_edge` pixels on either side, for a client
// that shows it small and pays for every byte -- a phone on mobile data,
// scrolling a grid of hundreds. Scaled down smoothly, aspect kept, and sent
// as JPEG within a budget of 0.3 bytes a pixel -- coarser only where a
// grainy cover needs it. One already within the size is never made larger;
// within the budget too, it comes back as it was, byte for byte. An image
// embedded at a megabyte is never sent to a phone whole.
[[nodiscard]] core::Result<std::vector<std::uint8_t>> fit_cover(std::span<const std::uint8_t> image,
                                                                int longest_edge);
// ADR-0317, ADR-0335: a cover as Melody writes it into files -- decoded from
// JPEG, PNG, WebP, GIF, BMP or TIFF, at most `longest_edge` on its longer side
// (0: as large as it is), transparency over white, a JPEG at quality 90.
// Nothing where it is a JPEG that fits: that one is written as it is.
[[nodiscard]] core::Result<std::optional<FittedCover>>
fit_cover_for_writing(std::span<const std::uint8_t> image, std::uint32_t longest_edge);

// Pictures larger than any cover: not decoded.
inline constexpr std::int64_t maximum_cover_pixels = 32'000'000;

} // namespace trackknife::formats

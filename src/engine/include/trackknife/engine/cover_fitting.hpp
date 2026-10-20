// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "trackknife/core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace trackknife::engine {

// A cover no longer than `longest_edge` pixels on either side, for a client
// that shows it small and pays for every byte -- a phone on mobile data,
// scrolling a grid of hundreds. Scaled down smoothly, aspect kept, and sent
// as JPEG within a budget of 0.3 bytes a pixel -- coarser only where a
// grainy cover needs it. One already within the size is never made larger;
// within the budget too, it comes back as it was, byte for byte. An image
// embedded at a megabyte is never sent to a phone whole.
[[nodiscard]] core::Result<std::vector<std::uint8_t>> fit_cover(std::span<const std::uint8_t> image,
                                                                int longest_edge);

} // namespace trackknife::engine

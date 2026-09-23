// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/result.hpp"

#include <string>
#include <vector>

namespace trackknife::formats {

// Returns the encoded bytes (PNG/JPEG as stored) of the first attached
// picture in the container, exactly as the muxer carries them. A media file
// without an attached picture reports ErrorCode::not_found so callers can
// fall back to folder images; decoding the bytes into a pixmap is the UI
// layer's job. Encoded pictures larger than 16 MiB report limit_exceeded.
[[nodiscard]] core::Result<std::vector<unsigned char>>
load_embedded_artwork(const std::string& raw_path,
                      const core::CancellationToken& cancellation = {});

// A track's cover as encoded bytes: the embedded picture, else the first of
// the usual folder images beside it (cover, folder, front; jpg/jpeg/png).
// Empty when there is none, or it is over 16 MiB. What Trackknife shows and
// what an engine sends a client come from here, so they agree.
[[nodiscard]] std::vector<unsigned char>
load_track_artwork(const std::string& raw_path, const core::CancellationToken& cancellation = {});

} // namespace trackknife::formats

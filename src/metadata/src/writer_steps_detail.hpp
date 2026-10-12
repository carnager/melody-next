// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/result.hpp"
#include "trackknife/metadata/write_plan.hpp"

#include <mp4file.h>
#include <mpegfile.h>

#include <optional>
#include <string>

namespace trackknife::metadata {

// The steps a format's writer takes after the generic property write, on a
// file already open -- a prepared copy, or the overlay of an in-place write
// (ADR-0281) -- so both paths write exactly the same.
namespace mp3_detail {
// ADR-0237: the FMPS rating's TXXX spelled as specified, and Windows Media
// Player's POPM carrying the same rating.
[[nodiscard]] core::Result<void> apply_rating_frames_to(const MetadataWritePlanSource& source_plan,
                                                        TagLib::MPEG::File& file,
                                                        const std::string& prepared_raw_path);
// The rating a plan sets: none planned, cleared, or a 0-10 value.
[[nodiscard]] std::optional<std::optional<unsigned>>
planned_rating_of(const MetadataWritePlanSource& plan);
} // namespace mp3_detail

namespace mp4_detail {
// ADR-0237: the FMPS rating atom spelled as specified.
[[nodiscard]] core::Result<void> spell_rating_atom_in(const MetadataWritePlanSource& source_plan,
                                                      TagLib::MP4::File& file,
                                                      const std::string& prepared_raw_path);
} // namespace mp4_detail

} // namespace trackknife::metadata

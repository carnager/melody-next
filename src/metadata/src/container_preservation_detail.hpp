// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/metadata/artwork.hpp"
#include "trackknife/metadata/write_plan.hpp"

#include <flacpicture.h>

#include <string>

// Internal seams shared between the text and artwork writers of one
// container (ADR-0137): the preservation proof of a format must be one
// implementation, whichever kind of metadata was rewritten. Defined in the
// respective writer translation units.
namespace trackknife::metadata::preservation_detail {
[[nodiscard]] core::Result<void> rewrite_flac_composed(const MetadataWritePlanSource&,
                                                       const std::string&,
                                                       const core::CancellationToken&);
[[nodiscard]] core::Result<void> verify_flac_composed(const std::string&, const std::string&, bool,
                                                      const core::CancellationToken&);

// ADR-0103: MPEG audio region byte-identical between the leading ID3v2 tag
// and any ID3v1/APEv2 trailers, with every APEv2 binary item preserved.
[[nodiscard]] core::Result<void>
verify_mp3_binary_preservation(const std::string& source_raw_path,
                               const std::string& prepared_raw_path,
                               const core::CancellationToken& cancellation);

// ADR-0136: ftyp, every mdat, and every other non-moov/free top-level box
// byte-identical and in unchanged order.
[[nodiscard]] core::Result<void>
verify_mp4_box_preservation(const std::string& source_raw_path,
                            const std::string& prepared_raw_path,
                            const core::CancellationToken& cancellation);

} // namespace trackknife::metadata::preservation_detail

namespace trackknife::metadata::artwork_detail {

// The shared FLAC/ID3v2 picture-type vocabulary for added pictures
// (ADR-0078); APIC types share FLAC's numbering.
[[nodiscard]] TagLib::FLAC::Picture::Type canonical_picture_type(ArtworkRole role);

} // namespace trackknife::metadata::artwork_detail

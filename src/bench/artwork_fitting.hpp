// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/metadata/artwork.hpp"
#include "trackknife/operations/artwork_apply.hpp"

#include <QString>

#include <cstdint>

namespace trackknife::bench {

// Converts a cover to one whose longer edge is at most max_edge, saved as a
// draft in directory and named by its content, so the same conversion is one
// file. JPEG unless the image has transparency, which JPEG would lose. An
// image that already fits is returned as it is.
[[nodiscard]] core::Result<metadata::ArtworkImageFile>
fitArtworkImage(const metadata::ArtworkImageFile& image, std::uint32_t max_edge,
                const QString& directory, const core::CancellationToken& cancellation = {});

// The fitter the tagger plans with: drafts beside the other cover drafts.
[[nodiscard]] operations::ArtworkImageFitter artworkFitter();
[[nodiscard]] QString coverDraftDirectory();

} // namespace trackknife::bench

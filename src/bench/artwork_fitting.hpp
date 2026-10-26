// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/metadata/artwork.hpp"
#include "trackknife/operations/artwork_apply.hpp"

#include <QByteArray>
#include <QString>

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace trackknife::bench {

// ADR-0317: a cover as Melody writes it -- decoded from whatever it is that
// Qt's image plugins read (JPEG, PNG, WebP, BMP, GIF, TIFF, …), at most
// `max_edge` on its longer side (0: as large as it is), flattened onto white,
// and encoded as JPEG at quality 90. A JPEG is shrunk as it is read.
[[nodiscard]] core::Result<QByteArray> coverJpeg(const QByteArray& encoded, std::uint32_t max_edge);

// The resize at writing: a cover that is no JPEG, or larger than max_edge,
// made one by coverJpeg. Nothing when it is a JPEG that fits.
struct FittedArtwork {
    QByteArray bytes;
    bool png{false};
};
[[nodiscard]] core::Result<std::optional<FittedArtwork>>
fitArtworkBytes(const std::vector<unsigned char>& encoded, std::uint32_t max_edge,
                const core::CancellationToken& cancellation = {});

// ADR-0237: the same resize for an engine's files -- the cover read through
// the engine, resized here, and handed to the engine (`stage`) to be written.
[[nodiscard]] operations::ArtworkImageFitter engineArtworkFitter(
    operations::ArtworkFileAccess access,
    std::function<core::Result<metadata::ArtworkImageFile>(std::span<const unsigned char>)> stage);

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

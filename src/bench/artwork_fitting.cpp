// SPDX-License-Identifier: GPL-3.0-only

#include "bench/artwork_fitting.hpp"

#include "uicommon/local_artwork.hpp"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QImageReader>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>
#include <utility>

namespace trackknife::bench {
namespace {

[[nodiscard]] core::Error fitting_error(const core::ErrorCode code, std::string message) {
    return core::Error{.code = code, .message = std::move(message), .context = {}};
}

// JPEG would flatten a cover that uses transparency; one that merely has an
// opaque alpha channel (a PNG screenshot, a paste) is a photo like any other.
[[nodiscard]] bool hasTransparency(const QImage& image) {
    if (!image.hasAlphaChannel())
        return false;
    const auto argb = image.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < argb.height(); ++y) {
        const auto* line = reinterpret_cast<const QRgb*>(argb.constScanLine(y));
        if (std::any_of(line, line + argb.width(),
                        [](const QRgb pixel) { return qAlpha(pixel) != 255; }))
            return true;
    }
    return false;
}

} // namespace

core::Result<metadata::ArtworkImageFile>
fitArtworkImage(const metadata::ArtworkImageFile& image, const std::uint32_t max_edge,
                const QString& directory, const core::CancellationToken& cancellation) {
    if (max_edge == 0U)
        return image;
    auto encoded = metadata::read_artwork_image_bytes(
        image, operations::maximum_fittable_artwork_bytes, cancellation);
    if (!encoded)
        return std::unexpected(encoded.error());
    QByteArray source(reinterpret_cast<const char*>(encoded->data()),
                      static_cast<qsizetype>(encoded->size()));
    QBuffer buffer(&source);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    const auto size = reader.size();
    if (!size.isValid() ||
        static_cast<std::int64_t>(size.width()) * size.height() > ui::maximum_artwork_pixels)
        return std::unexpected(fitting_error(core::ErrorCode::unsupported,
                                             "The cover cannot be decoded to resize it"));
    const auto edge = static_cast<int>(max_edge);
    if (std::max(size.width(), size.height()) <= edge)
        return image;
    reader.setAutoTransform(true);
    auto decoded = reader.read();
    if (decoded.isNull())
        return std::unexpected(fitting_error(core::ErrorCode::unsupported,
                                             "The cover cannot be decoded to resize it"));
    if (cancellation.is_cancellation_requested())
        return std::unexpected(fitting_error(core::ErrorCode::cancelled, "Cover review cancelled"));
    const auto scaled = decoded.scaled(edge, edge, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    const auto transparent = hasTransparency(scaled);
    QByteArray bytes;
    QBuffer output(&bytes);
    const auto saved =
        output.open(QIODevice::WriteOnly) &&
        (transparent ? scaled.save(&output, "PNG")
                     : scaled.convertToFormat(QImage::Format_RGB32).save(&output, "JPEG", 90));
    if (!saved)
        return std::unexpected(fitting_error(core::ErrorCode::io, "Could not convert the cover"));
    const auto path =
        directory + QLatin1Char('/') +
        QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()) +
        (transparent ? QStringLiteral(".png") : QStringLiteral(".jpg"));
    QSaveFile file(path);
    if (!QDir{}.mkpath(directory) || !file.open(QIODevice::WriteOnly) ||
        file.write(bytes) != bytes.size() || !file.commit())
        return std::unexpected(
            fitting_error(core::ErrorCode::io, "Could not save the converted cover"));
    return metadata::read_artwork_image_file(QFile::encodeName(path).toStdString(),
                                             16U * 1024U * 1024U, cancellation);
}

QString coverDraftDirectory() {
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) +
           QStringLiteral("/cover-drafts");
}

operations::ArtworkImageFitter artworkFitter() {
    return [directory = coverDraftDirectory()](const metadata::ArtworkImageFile& image,
                                               const std::uint32_t max_edge,
                                               const core::CancellationToken& cancellation) {
        return fitArtworkImage(image, max_edge, directory, cancellation);
    };
}

} // namespace trackknife::bench

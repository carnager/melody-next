// SPDX-License-Identifier: GPL-3.0-only

#include "bench/artwork_fitting.hpp"

#include "uicommon/local_artwork.hpp"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QImageReader>
#include <QPainter>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>
#include <utility>

namespace trackknife::bench {
namespace {

[[nodiscard]] core::Error fitting_error(const core::ErrorCode code, std::string message) {
    return core::Error{.code = code, .message = std::move(message), .context = {}};
}

} // namespace

core::Result<QByteArray> coverJpeg(const QByteArray& encoded, const std::uint32_t max_edge) {
    QByteArray source = encoded;
    QBuffer buffer(&source);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    reader.setAutoTransform(true);
    const auto size = reader.size();
    if (!size.isValid())
        return std::unexpected(
            fitting_error(core::ErrorCode::unsupported,
                          "The image cannot be read: " + reader.errorString().toStdString()));
    if (static_cast<std::int64_t>(size.width()) * size.height() > ui::maximum_artwork_pixels)
        return std::unexpected(
            fitting_error(core::ErrorCode::unsupported, "The image is too large to read"));
    const auto edge = static_cast<int>(max_edge);
    if (max_edge > 0U && std::max(size.width(), size.height()) > edge)
        reader.setScaledSize(size.scaled(edge, edge, Qt::KeepAspectRatio));
    const auto decoded = reader.read();
    if (decoded.isNull())
        return std::unexpected(
            fitting_error(core::ErrorCode::unsupported,
                          "The image cannot be read: " + reader.errorString().toStdString()));
    // Transparency, where there is any, over white: a cover is a JPEG.
    QImage flat(decoded.size(), QImage::Format_RGB32);
    flat.fill(Qt::white);
    {
        QPainter painter(&flat);
        painter.drawImage(0, 0, decoded);
    }
    QByteArray bytes;
    QBuffer output(&bytes);
    if (!output.open(QIODevice::WriteOnly) || !flat.save(&output, "JPEG", 90))
        return std::unexpected(fitting_error(core::ErrorCode::io, "Could not convert the cover"));
    return bytes;
}

core::Result<std::optional<FittedArtwork>>
fitArtworkBytes(const std::vector<unsigned char>& encoded, const std::uint32_t max_edge,
                const core::CancellationToken& cancellation) {
    QByteArray source(reinterpret_cast<const char*>(encoded.data()),
                      static_cast<qsizetype>(encoded.size()));
    QBuffer buffer(&source);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    const auto size = reader.size();
    const auto jpeg = source.size() > 3 && static_cast<unsigned char>(source.at(0)) == 0xFFU &&
                      static_cast<unsigned char>(source.at(1)) == 0xD8U;
    // A JPEG that fits stays as it is.
    if (jpeg && size.isValid() &&
        (max_edge == 0U || std::max(size.width(), size.height()) <= static_cast<int>(max_edge)))
        return std::optional<FittedArtwork>{};
    if (cancellation.is_cancellation_requested())
        return std::unexpected(fitting_error(core::ErrorCode::cancelled, "Cover review cancelled"));
    auto converted = coverJpeg(source, max_edge);
    if (!converted)
        return std::unexpected(converted.error());
    return std::optional{FittedArtwork{.bytes = std::move(*converted), .png = false}};
}

core::Result<metadata::ArtworkImageFile>
fitArtworkImage(const metadata::ArtworkImageFile& image, const std::uint32_t max_edge,
                const QString& directory, const core::CancellationToken& cancellation) {
    auto encoded = metadata::read_artwork_image_bytes(
        image, operations::maximum_fittable_artwork_bytes, cancellation);
    if (!encoded)
        return std::unexpected(encoded.error());
    auto fitted = fitArtworkBytes(*encoded, max_edge, cancellation);
    if (!fitted)
        return std::unexpected(fitted.error());
    if (!*fitted)
        return image;
    const auto& bytes = (*fitted)->bytes;
    const auto path =
        directory + QLatin1Char('/') +
        QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()) +
        ((*fitted)->png ? QStringLiteral(".png") : QStringLiteral(".jpg"));
    QSaveFile file(path);
    if (!QDir{}.mkpath(directory) || !file.open(QIODevice::WriteOnly) ||
        file.write(bytes) != bytes.size() || !file.commit())
        return std::unexpected(
            fitting_error(core::ErrorCode::io, "Could not save the converted cover"));
    return metadata::read_artwork_image_file(QFile::encodeName(path).toStdString(),
                                             16U * 1024U * 1024U, cancellation);
}

operations::ArtworkImageFitter engineArtworkFitter(
    operations::ArtworkFileAccess access,
    std::function<core::Result<metadata::ArtworkImageFile>(std::span<const unsigned char>)> stage) {
    return [access = std::move(access), stage = std::move(stage)](
               const metadata::ArtworkImageFile& image, const std::uint32_t max_edge,
               const core::CancellationToken& cancellation)
               -> core::Result<metadata::ArtworkImageFile> {
        auto encoded =
            access.image_bytes(image, operations::maximum_fittable_artwork_bytes, cancellation);
        if (!encoded)
            return std::unexpected(encoded.error());
        auto fitted = fitArtworkBytes(*encoded, max_edge, cancellation);
        if (!fitted)
            return std::unexpected(fitted.error());
        if (!*fitted)
            return image;
        const auto& bytes = (*fitted)->bytes;
        return stage(std::span{reinterpret_cast<const unsigned char*>(bytes.constData()),
                               static_cast<std::size_t>(bytes.size())});
    };
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

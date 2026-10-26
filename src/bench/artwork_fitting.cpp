// SPDX-License-Identifier: GPL-3.0-only

#include "bench/artwork_fitting.hpp"

#include "trackknife/formats/cover_fitting.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
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
    const std::span bytes{reinterpret_cast<const std::uint8_t*>(encoded.constData()),
                          static_cast<std::size_t>(encoded.size())};
    auto fitted = formats::fit_cover_for_writing(bytes, max_edge);
    if (!fitted) {
        return std::unexpected(fitting_error(
            core::ErrorCode::unsupported, "The image cannot be read: " + fitted.error().message));
    }
    if (!*fitted) {
        return encoded;
    }
    return QByteArray(reinterpret_cast<const char*>((*fitted)->bytes.data()),
                      static_cast<qsizetype>((*fitted)->bytes.size()));
}

core::Result<std::optional<FittedArtwork>>
fitArtworkBytes(const std::vector<unsigned char>& encoded, const std::uint32_t max_edge,
                const core::CancellationToken& cancellation) {
    if (cancellation.is_cancellation_requested())
        return std::unexpected(fitting_error(core::ErrorCode::cancelled, "Cover review cancelled"));
    auto fitted = formats::fit_cover_for_writing(encoded, max_edge);
    if (!fitted)
        return std::unexpected(fitting_error(
            core::ErrorCode::unsupported, "The image cannot be read: " + fitted.error().message));
    if (!*fitted)
        return std::optional<FittedArtwork>{};
    return std::optional{
        FittedArtwork{.bytes = QByteArray(reinterpret_cast<const char*>((*fitted)->bytes.data()),
                                          static_cast<qsizetype>((*fitted)->bytes.size())),
                      .png = false}};
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

// SPDX-License-Identifier: GPL-3.0-only

#include "uicommon/local_artwork.hpp"

#include "trackknife/formats/artwork.hpp"

#include <QBuffer>
#include <QImageReader>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

namespace trackknife::ui {
namespace {

constexpr std::size_t artwork_file_limit = 16U * 1024U * 1024U;
constexpr int thumbnail_extent = 128;

} // namespace

QImage artworkThumbnail(const std::vector<unsigned char>& bytes) {
    if (bytes.empty() || bytes.size() > artwork_file_limit) {
        return {};
    }
    QBuffer buffer;
    buffer.setData(QByteArray::fromRawData(reinterpret_cast<const char*>(bytes.data()),
                                           static_cast<qsizetype>(bytes.size())));
    if (!buffer.open(QIODevice::ReadOnly)) {
        return {};
    }
    QImageReader reader{&buffer};
    const auto size = reader.size();
    if (!size.isValid() || size.isEmpty() || size.width() > 32'768 || size.height() > 32'768 ||
        static_cast<std::int64_t>(size.width()) * size.height() > maximum_artwork_pixels) {
        return {};
    }
    if (size.width() > thumbnail_extent || size.height() > thumbnail_extent) {
        reader.setScaledSize(size.scaled(thumbnail_extent, thumbnail_extent, Qt::KeepAspectRatio));
    }
    return reader.read();
}

QImage loadLocalArtwork(const std::string& raw_path, const core::CancellationToken& cancellation) {
    if (cancellation.is_cancellation_requested()) {
        return {};
    }
    auto image = artworkThumbnail(formats::load_track_artwork(raw_path, cancellation));
    return cancellation.is_cancellation_requested() ? QImage{} : image;
}

} // namespace trackknife::ui

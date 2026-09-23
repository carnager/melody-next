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

constexpr std::uintmax_t artwork_file_limit = 16U * 1024U * 1024U;
constexpr int thumbnail_extent = 128;

std::vector<unsigned char> folderArtwork(const std::string& raw_path,
                                         const core::CancellationToken& cancellation) {
    const auto directory = std::filesystem::path{raw_path}.parent_path();
    static constexpr std::array names{"cover.jpg",  "cover.jpeg",  "cover.png",
                                      "folder.jpg", "folder.jpeg", "folder.png",
                                      "front.jpg",  "front.jpeg",  "front.png"};
    std::error_code error;
    std::filesystem::directory_iterator iterator{
        directory, std::filesystem::directory_options::skip_permission_denied, error};
    if (error) {
        return {};
    }
    std::size_t visited = 0;
    for (; iterator != std::filesystem::directory_iterator{}; iterator.increment(error)) {
        if (error || cancellation.is_cancellation_requested() || ++visited > 10'000U) {
            return {};
        }
        auto name = iterator->path().filename().native();
        for (auto& byte : name) {
            if (byte >= 'A' && byte <= 'Z') {
                byte = static_cast<char>(byte - 'A' + 'a');
            }
        }
        if (std::ranges::find(names, name) == names.end() || !iterator->is_regular_file(error)) {
            error.clear();
            continue;
        }
        const auto size = iterator->file_size(error);
        if (error || size == 0U || size > artwork_file_limit) {
            return {};
        }
        std::ifstream input{iterator->path(), std::ios::binary};
        std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
        input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
        if (input.gcount() != static_cast<std::streamsize>(size)) {
            return {};
        }
        return bytes;
    }
    return {};
}

QImage thumbnail(const std::vector<unsigned char>& bytes) {
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

} // namespace

QImage loadLocalArtwork(const std::string& raw_path, const core::CancellationToken& cancellation) {
    if (cancellation.is_cancellation_requested()) {
        return {};
    }
    QImage image;
    if (const auto embedded = formats::load_embedded_artwork(raw_path, cancellation); embedded) {
        image = thumbnail(*embedded);
    }
    if (image.isNull() && !cancellation.is_cancellation_requested()) {
        image = thumbnail(folderArtwork(raw_path, cancellation));
    }
    return cancellation.is_cancellation_requested() ? QImage{} : image;
}

} // namespace trackknife::ui

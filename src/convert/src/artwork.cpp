// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/convert/artwork.hpp"

#include "trackknife/formats/artwork.hpp"
#include "trackknife/metadata/artwork.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace trackknife::convert {
namespace {

// The shared FLAC/ID3v2 picture-type numbering at the role granularity the
// inventory preserves (ADR-0076); finer native types collapse onto these.
[[nodiscard]] std::uint32_t picture_type_for_role(const metadata::ArtworkRole role) {
    switch (role) {
    case metadata::ArtworkRole::front:
        return 3U;
    case metadata::ArtworkRole::back:
        return 4U;
    case metadata::ArtworkRole::artist:
        return 8U;
    case metadata::ArtworkRole::disc:
        return 6U;
    case metadata::ArtworkRole::icon:
        return 1U;
    case metadata::ArtworkRole::other:
        break;
    }
    return 0U;
}

[[nodiscard]] std::optional<ConversionArtwork> artwork_from_bytes(std::vector<unsigned char> bytes,
                                                                  const std::uint32_t picture_type,
                                                                  std::string description) {
    const auto inspected = metadata::inspect_encoded_image_bytes(bytes);
    if (!inspected) {
        return std::nullopt;
    }
    return ConversionArtwork{
        .bytes = std::move(bytes),
        .mime_type = inspected->mime_type,
        .width = inspected->width,
        .height = inspected->height,
        .picture_type = picture_type,
        .description = std::move(description),
    };
}

[[nodiscard]] std::optional<ConversionArtwork>
artwork_from_inventory_item(const metadata::ArtworkInventoryItem& item,
                            const core::CancellationToken& cancellation) {
    const metadata::ArtworkImageFile image{
        .raw_path = item.raw_source_path,
        .source_revision = item.source_revision,
        .mime_type = item.mime_type,
        .width = item.width,
        .height = item.height,
        .byte_size = item.byte_size,
        .content_fingerprint = item.content_fingerprint,
        .embedded_source_ordinal = item.provenance == metadata::ArtworkProvenance::embedded
                                       ? std::optional<std::size_t>{item.source_ordinal}
                                       : std::nullopt,
    };
    auto bytes = metadata::read_artwork_image_bytes(image, 16U * 1024U * 1024U, cancellation);
    if (!bytes) {
        return std::nullopt;
    }
    const auto embedded = item.provenance == metadata::ArtworkProvenance::embedded;
    return artwork_from_bytes(std::move(*bytes), embedded ? picture_type_for_role(item.role) : 3U,
                              embedded ? item.description : std::string{});
}

} // namespace

std::optional<ConversionArtwork>
resolve_conversion_artwork(const std::string& source_raw_path,
                           const core::CancellationToken& cancellation) {
    std::vector<metadata::ArtworkInventoryItem> embedded_items;
    std::vector<metadata::ArtworkInventoryItem> external_items;
    if (auto inventory = metadata::read_local_artwork_inventory(
            source_raw_path, metadata::default_artwork_inventory_policy(), cancellation)) {
        for (auto& item : inventory->items) {
            (item.provenance == metadata::ArtworkProvenance::embedded ? embedded_items
                                                                      : external_items)
                .push_back(std::move(item));
        }
    }
    const auto front_first = [](const metadata::ArtworkInventoryItem& left,
                                const metadata::ArtworkInventoryItem& right) {
        const auto left_front = left.role == metadata::ArtworkRole::front;
        const auto right_front = right.role == metadata::ArtworkRole::front;
        if (left_front != right_front) {
            return left_front;
        }
        return left.source_ordinal < right.source_ordinal;
    };
    std::ranges::sort(embedded_items, front_first);
    std::ranges::sort(external_items, front_first);

    // Pictures the inventory reads first -- FLAC, and Opus and Vorbis
    // (ADR-0337) -- with their role-level type and description intact.
    for (const auto& item : embedded_items) {
        if (auto resolved = artwork_from_inventory_item(item, cancellation)) {
            return resolved;
        }
    }
    // Other containers only expose their first embedded picture through the
    // container-agnostic reader; it is normalized to a front cover.
    if (embedded_items.empty()) {
        if (auto bytes = formats::load_embedded_artwork(source_raw_path, cancellation)) {
            if (auto resolved = artwork_from_bytes(std::move(*bytes), 3U, {})) {
                return resolved;
            }
        }
    }
    for (const auto& item : external_items) {
        if (auto resolved = artwork_from_inventory_item(item, cancellation)) {
            return resolved;
        }
    }
    return std::nullopt;
}

} // namespace trackknife::convert

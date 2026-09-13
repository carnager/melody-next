// SPDX-License-Identifier: GPL-3.0-only

#include "container_preservation_detail.hpp"
#include "trackknife/metadata/artwork_write_plan.hpp"

#include <algorithm>
#include <set>

namespace trackknife::metadata {
namespace {
core::Error conflict(const std::string& path, const std::string& message) {
    return {.code = core::ErrorCode::conflict, .message = message, .context = {{"source", path}}};
}
} // namespace

std::vector<ArtworkWritePlanChange> artwork_changes(const ArtworkWritePlanSource& source) {
    std::vector<ArtworkWritePlanChange> changes{source.change};
    changes.insert(changes.end(), source.additional_changes.begin(),
                   source.additional_changes.end());
    return changes;
}

core::Result<std::vector<ArtworkInventoryItem>>
project_artwork_inventory(const LocalArtworkInventory& inventory,
                          const ArtworkWritePlanSource& source) {
    auto projected = inventory.items;
    std::set<std::size_t> targets;
    std::optional<std::size_t> previous_target;
    bool adding = false;
    for (const auto& change : artwork_changes(source)) {
        const auto ordinal = change.target_ordinal;
        if (change.kind == ArtworkWritePlanIntentKind::add) {
            adding = true;
            if (!change.replacement || ordinal != projected.size() || projected.size() >= 64U) {
                return std::unexpected(
                    conflict(source.raw_media_path,
                             "invalid artwork insertion point or artwork count limit"));
            }
            const auto& replacement = *change.replacement;
            if (std::ranges::any_of(projected, [&](const auto& item) {
                    return item.content_fingerprint == replacement.content_fingerprint;
                })) {
                return std::unexpected(
                    conflict(source.raw_media_path, "added image already exists"));
            }
            const auto covr = inventory.embedded_adapter_name == "taglib-mp4-covr-v1";
            if (covr && !change.added_description.empty()) {
                return std::unexpected(
                    conflict(source.raw_media_path, "MP4 covers cannot carry descriptions"));
            }
            projected.push_back(ArtworkInventoryItem{
                .role = covr ? ArtworkRole::front : change.added_role,
                .native_type = covr ? std::string{}
                                    : TagLib::FLAC::Picture::typeToString(
                                          artwork_detail::canonical_picture_type(change.added_role))
                                          .to8Bit(true),
                .mime_type = replacement.mime_type,
                .description = covr ? std::string{} : change.added_description,
                .width = replacement.width,
                .height = replacement.height,
                .byte_size = replacement.byte_size,
                .content_fingerprint = replacement.content_fingerprint,
                .provenance = ArtworkProvenance::embedded,
                .raw_source_path = source.raw_media_path,
                .source_revision = inventory.media_revision,
                .source_ordinal = ordinal,
                .duplicate_of = std::nullopt});
            continue;
        }
        if (adding || (previous_target && ordinal >= *previous_target)) {
            return std::unexpected(
                conflict(source.raw_media_path,
                         "picture edits must use descending original ordinals before additions"));
        }
        previous_target = ordinal;
        if (!targets.insert(ordinal).second || ordinal >= inventory.items.size() ||
            ordinal >= projected.size() || !change.original ||
            inventory.items[ordinal] != *change.original ||
            inventory.items[ordinal].content_fingerprint != change.expected_target_fingerprint) {
            return std::unexpected(
                conflict(source.raw_media_path, "artwork differs from the reviewed original"));
        }
        if (change.kind == ArtworkWritePlanIntentKind::remove) {
            projected.erase(projected.begin() + static_cast<std::ptrdiff_t>(ordinal));
        } else if (change.kind == ArtworkWritePlanIntentKind::replace && change.replacement) {
            auto& target = projected[ordinal];
            const auto& replacement = *change.replacement;
            target.mime_type = replacement.mime_type;
            target.width = replacement.width;
            target.height = replacement.height;
            target.byte_size = replacement.byte_size;
            target.content_fingerprint = replacement.content_fingerprint;
        } else {
            return std::unexpected(conflict(source.raw_media_path, "invalid artwork change"));
        }
    }
    for (std::size_t index = 0; index < projected.size(); ++index) {
        projected[index].source_ordinal = index;
        projected[index].duplicate_of.reset();
    }
    return projected;
}

core::Result<MetadataWritePlan> merge_artwork_write_plan(MetadataWritePlan metadata,
                                                         ArtworkWritePlan artwork) {
    for (auto& source : artwork.sources) {
        if (!source.ready()) {
            for (const auto& issue : source.issues) {
                if (issue.blocking) {
                    return std::unexpected(issue.error);
                }
            }
            return std::unexpected(conflict(source.raw_media_path, "artwork plan is incomplete"));
        }
        auto found = std::ranges::find(metadata.sources, source.raw_media_path,
                                       &MetadataWritePlanSource::raw_path);
        if (found == metadata.sources.end()) {
            metadata.sources.push_back(MetadataWritePlanSource{
                .raw_path = source.raw_media_path,
                .occurrence_indexes = source.occurrence_indexes,
                .expected_revision = source.expected_media_revision,
                .observed_revision = source.observed_media_revision,
                .adapter_name = source.adapter_name == "taglib-flac-picture-v1" ? "taglib-flac-v1"
                                : source.adapter_name == "taglib-id3v2-apic-v1" ? "taglib-mpeg-v1"
                                                                                : "taglib-mp4-v1",
                .changes = {},
                .issues = {}});
            found = std::prev(metadata.sources.end());
        }
        if (found->expected_revision != source.expected_media_revision ||
            found->observed_revision != source.observed_media_revision) {
            return std::unexpected(conflict(source.raw_media_path,
                                            "tags and artwork have different source revisions"));
        }
        for (const auto occurrence : source.occurrence_indexes) {
            if (!std::ranges::contains(found->occurrence_indexes, occurrence)) {
                found->occurrence_indexes.push_back(occurrence);
            }
        }
        found->artwork = std::make_shared<const ArtworkWritePlanSource>(std::move(source));
    }
    return metadata;
}
} // namespace trackknife::metadata

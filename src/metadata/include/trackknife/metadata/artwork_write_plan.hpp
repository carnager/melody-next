// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/metadata/artwork.hpp"
#include "trackknife/metadata/write_plan.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace trackknife::metadata {

enum class ArtworkWritePlanIntentKind : std::uint8_t {
    replace = 0,
    remove = 1,
    add = 2,
    batch = 3, // Journal summary only; never an individual edit intent.
};

[[nodiscard]] std::string_view artwork_write_plan_intent_kind_name(ArtworkWritePlanIntentKind kind);

// One logical Properties occurrence. Equal intents for repeated occurrences
// of one raw media path and picture collapse into one physical change.
// Multiple pictures in a file are ordered steps: descending originals, then additions.
struct ArtworkWritePlanIntent {
    std::size_t occurrence_index{0U};
    std::string raw_media_path;
    std::optional<core::LocalSourceRevision> expected_media_revision;
    std::size_t target_ordinal{0U};
    core::ContentFingerprint expected_target_fingerprint;
    ArtworkWritePlanIntentKind kind{ArtworkWritePlanIntentKind::replace};
    std::optional<std::string> replacement_raw_path;
    // Add has no original target. The plan derives its resulting ordinal from
    // the freshly revalidated embedded-picture count and uses this role and
    // description to construct the new native FLAC picture.
    ArtworkRole added_role{ArtworkRole::front};
    std::string added_description;
    // Copy uses one already inventoried embedded donor. The plan revalidates
    // this compact evidence; it never carries the encoded image bytes.
    std::optional<ArtworkInventoryItem> replacement_embedded_source;

    friend bool operator==(const ArtworkWritePlanIntent&, const ArtworkWritePlanIntent&) = default;
};

enum class ArtworkWritePlanIssueKind : std::uint8_t {
    missing_baseline_revision,
    inconsistent_baseline_revision,
    conflicting_logical_intents,
    source_revalidation_failed,
    source_changed,
    physical_source_alias,
    writer_unavailable,
    target_not_found,
    target_changed,
    replacement_unavailable,
    replacement_unsupported,
    replacement_unchanged,
};

[[nodiscard]] std::string_view artwork_write_plan_issue_kind_name(ArtworkWritePlanIssueKind kind);

struct ArtworkWritePlanIssue {
    ArtworkWritePlanIssueKind kind{ArtworkWritePlanIssueKind::source_revalidation_failed};
    core::Error error;
    std::vector<std::size_t> occurrence_indexes;
    bool blocking{true};

    friend bool operator==(const ArtworkWritePlanIssue&, const ArtworkWritePlanIssue&) = default;
};

struct ArtworkWritePlanChange {
    ArtworkWritePlanIntentKind kind{ArtworkWritePlanIntentKind::replace};
    std::size_t target_ordinal{0U};
    core::ContentFingerprint expected_target_fingerprint;
    std::optional<ArtworkInventoryItem> original;
    std::optional<ArtworkImageFile> replacement;
    ArtworkRole added_role{ArtworkRole::front};
    std::string added_description;

    friend bool operator==(const ArtworkWritePlanChange&, const ArtworkWritePlanChange&) = default;
};

// Captured during review; the basename follows the image's encoded format.
struct FolderImageWritePlan {
    std::string raw_path;
    ArtworkImageFile image;
    std::optional<ArtworkImageFile> original;
    friend bool operator==(const FolderImageWritePlan&, const FolderImageWritePlan&) = default;
};

struct ArtworkStoragePolicy {
    bool embed{true};
    bool write_folder_image{false};
    std::string folder_image_name{"cover.jpg"};
    std::string fetch_source{"coverartarchive"};
    friend bool operator==(const ArtworkStoragePolicy&, const ArtworkStoragePolicy&) = default;
};

struct ArtworkWritePlanSource {
    std::string raw_media_path;
    std::vector<std::size_t> occurrence_indexes;
    std::optional<core::LocalSourceRevision> expected_media_revision;
    std::optional<core::LocalSourceRevision> observed_media_revision;
    std::string adapter_name;
    ArtworkWritePlanChange change;
    std::vector<ArtworkWritePlanIssue> issues;
    std::vector<ArtworkWritePlanChange> additional_changes{};
    bool embed{true};
    std::optional<FolderImageWritePlan> folder_image{};

    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] std::size_t blocking_issue_count() const noexcept;

    friend bool operator==(const ArtworkWritePlanSource&, const ArtworkWritePlanSource&) = default;
};

struct ArtworkWritePlan {
    std::vector<ArtworkWritePlanSource> sources;
    std::size_t logical_intent_count{0U};

    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] std::size_t ready_source_count() const noexcept;
    [[nodiscard]] std::size_t blocking_issue_count() const noexcept;

    friend bool operator==(const ArtworkWritePlan&, const ArtworkWritePlan&) = default;
};

using ArtworkWritePlanInventoryReader = std::function<core::Result<LocalArtworkInventory>(
    const std::string&, const core::CancellationToken&)>;
using ArtworkWritePlanImageReader = std::function<core::Result<ArtworkImageFile>(
    const std::string&, const core::CancellationToken&)>;

// Builds one immutable batch preview. The readers run synchronously once per
// distinct physical input on the caller's bounded worker. Encoded image bytes
// are never retained in the returned plan.
[[nodiscard]] core::Result<ArtworkWritePlan>
build_artwork_write_plan(const std::vector<ArtworkWritePlanIntent>& intents,
                         const ArtworkWritePlanInventoryReader& inventory_reader,
                         const ArtworkWritePlanImageReader& image_reader,
                         const core::CancellationToken& cancellation = {});

// Production convenience using the embedded inventory and bounded PNG/JPEG
// replacement reader. Callers dispatch this filesystem work off the UI thread.
[[nodiscard]] core::Result<ArtworkWritePlan>
revalidate_artwork_write_plan(const std::vector<ArtworkWritePlanIntent>& intents,
                              const core::CancellationToken& cancellation = {});

// Atomic per-file composition, retaining independent typed tag and picture evidence.
[[nodiscard]] std::vector<ArtworkWritePlanChange>
artwork_changes(const ArtworkWritePlanSource& source);
[[nodiscard]] core::Result<std::vector<ArtworkInventoryItem>>
project_artwork_inventory(const LocalArtworkInventory& inventory,
                          const ArtworkWritePlanSource& source);
[[nodiscard]] core::Result<MetadataWritePlan> merge_artwork_write_plan(MetadataWritePlan metadata,
                                                                       ArtworkWritePlan artwork);

// The embedded adapters with a qualified prepared-copy artwork writer
// (ADR-0137): native FLAC pictures, ID3v2 APIC frames, and MP4 covr entries.
[[nodiscard]] bool is_qualified_artwork_adapter(std::string_view adapter_name);

} // namespace trackknife::metadata

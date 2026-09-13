// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/local_sources.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/metadata/document.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace trackknife::metadata {

enum class MetadataSelectionFieldState : std::uint8_t {
    common,
    mixed,
    missing,
    partial,
};

[[nodiscard]] std::string_view
metadata_selection_field_state_name(MetadataSelectionFieldState state);

// ADR-0139: binds one logical occurrence to the CUE sheet carrying its
// per-track metadata. ReplayGain drafts on such occurrences resolve to a
// sheet rewrite instead of whole-file tags.
struct StagedCueSheetBinding {
    std::string raw_cue_path;
    // The sheet revision observed when the draft was captured; commit is
    // gated on it so results never reach a changed sheet.
    std::optional<core::LocalSourceRevision> cue_revision;
    std::size_t file_index{0U};
    std::size_t track_index{0U};

    friend bool operator==(const StagedCueSheetBinding&, const StagedCueSheetBinding&) = default;
};

// ADR-0141: the logical source's position inside its physical file
// (decoder selection plus sample segment). It keys the loudness sidecar
// entry a non-CUE logical track's ReplayGain resolves to.
struct StagedLogicalIdentity {
    std::optional<int> stream_index;
    std::optional<int> subsong_index;
    std::optional<std::int64_t> start_sample;
    std::optional<std::int64_t> end_sample;

    friend bool operator==(const StagedLogicalIdentity&, const StagedLogicalIdentity&) = default;
};

// Immutable baseline captured when a properties workspace opens. Later staged
// patches remain sparse and refer back to this document/revision; the list
// cache is never sufficient authority for commit.
struct StagedMetadataSource {
    std::string raw_path;
    std::optional<core::LocalSourceRevision> source_revision;
    MetadataDocument baseline;
    // Segment/subsong/selected-stream measurements cannot become whole-file
    // loudness tags. Ordinary physical metadata editing remains independent.
    bool logical_track{false};
    std::optional<StagedCueSheetBinding> cue_sheet{};
    std::optional<StagedLogicalIdentity> logical_identity{};
    // A cache-only physical occurrence needs a worker-side native read before
    // becoming an editable baseline. Other revisionless previews stay read-only.
    bool needs_metadata_capture{false};

    friend bool operator==(const StagedMetadataSource&, const StagedMetadataSource&) = default;
};

struct StagedMetadataCell {
    std::string native_name;
    std::vector<std::string> values;
    FieldProvenance provenance{FieldProvenance::embedded};

    friend bool operator==(const StagedMetadataCell&, const StagedMetadataCell&) = default;
};

struct StagedMetadataField {
    std::string canonical_name;
    std::string display_name;
    // Present for a freeform field or synthetic exact-native address rather
    // than an explicitly mapped semantic field.
    std::optional<std::string> exact_native_name;
    MetadataSelectionFieldState state{MetadataSelectionFieldState::missing};
    std::size_t present_item_count{0U};
    // The first occurrence containing this field. Aggregate presentations can
    // use it to display a common value without scanning the complete selection.
    std::optional<std::size_t> representative_item_index;

    friend bool operator==(const StagedMetadataField&, const StagedMetadataField&) = default;
};

struct StagedMetadataSelectionLimits {
    std::size_t items{100'000U};
    std::size_t fields{4'096U};
};

struct StagedMetadataSubsetField {
    MetadataSelectionFieldState state{MetadataSelectionFieldState::missing};
    std::size_t present_item_count{0U};
    std::optional<std::size_t> representative_item_index;

    friend bool operator==(const StagedMetadataSubsetField&,
                           const StagedMetadataSubsetField&) = default;
};

// Sparse multi-item metadata projection. Preferred fields appear first even
// when absent everywhere; arbitrary fields follow in first-seen selection and
// document order. Cells retain exact ordered values and winning provenance.
class StagedMetadataSelection final {
  public:
    [[nodiscard]] static core::Result<StagedMetadataSelection>
    create(std::vector<StagedMetadataSource> sources,
           std::span<const std::string_view> preferred_fields = {},
           const StagedMetadataSelectionLimits& limits = {});

    [[nodiscard]] std::size_t item_count() const noexcept { return items_->size(); }
    [[nodiscard]] std::size_t field_count() const noexcept { return fields_.size(); }
    [[nodiscard]] std::size_t distinct_source_count() const noexcept {
        return distinct_source_count_;
    }
    [[nodiscard]] std::size_t item_revision_count() const noexcept { return item_revision_count_; }
    [[nodiscard]] const StagedMetadataSource& source(std::size_t item_index) const;
    // Advance all occurrences after a verified text-preserving commit (such
    // as embedded artwork). Reject a broken revision chain atomically. Copy
    // a published selection first; text baselines and field addresses stay fixed.
    [[nodiscard]] core::Result<std::size_t>
    advance_source_revision(std::string_view raw_path,
                            const core::LocalSourceRevision& previous_revision,
                            const core::LocalSourceRevision& published_revision);
    [[nodiscard]] const StagedMetadataField& field(std::size_t field_index) const;
    [[nodiscard]] const StagedMetadataCell* cell(std::size_t item_index,
                                                 std::size_t field_index) const;
    [[nodiscard]] std::span<const std::size_t> present_field_indexes(std::size_t item_index) const;
    [[nodiscard]] std::optional<std::size_t> field_index(std::string_view name) const;
    // Extends only the field vocabulary; item baselines remain shared and
    // immutable. Copy a published selection before calling this method.
    [[nodiscard]] core::Result<std::size_t>
    ensure_missing_field(std::string_view name, std::string_view display_name,
                         std::size_t maximum_fields = StagedMetadataSelectionLimits{}.fields);
    [[nodiscard]] core::Result<std::size_t>
    ensure_exact_native_field(std::string_view native_name, std::string_view display_name,
                              std::size_t maximum_fields = StagedMetadataSelectionLimits{}.fields);
    [[nodiscard]] std::optional<std::size_t>
    exact_native_field_index(std::string_view native_name) const;
    [[nodiscard]] core::Result<std::vector<StagedMetadataSubsetField>>
    summarize_items(std::span<const std::size_t> item_indexes) const;

  private:
    struct Item {
        StagedMetadataSource source;
        std::unordered_map<std::size_t, StagedMetadataCell> cells;
        std::vector<std::size_t> present_field_indexes;
    };

    std::shared_ptr<const std::vector<Item>> items_{std::make_shared<const std::vector<Item>>()};
    // Only changed sources are copied; large baseline/cell projections and
    // already published selection snapshots remain shared and immutable.
    std::unordered_map<std::size_t, std::shared_ptr<const StagedMetadataSource>> revised_sources_;
    std::vector<StagedMetadataField> fields_;
    std::unordered_map<std::string, std::size_t> field_positions_;
    std::unordered_map<std::string, std::size_t> exact_native_field_positions_;
    std::size_t distinct_source_count_{0U};
    std::size_t item_revision_count_{0U};
};

} // namespace trackknife::metadata

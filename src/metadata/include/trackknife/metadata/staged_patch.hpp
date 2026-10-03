// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/metadata/staged_selection.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace trackknife::metadata {

enum class StagedMetadataPatchKind : std::uint8_t {
    replace_values,
    remove_field,
};

// A patch addresses one occurrence and one field in an immutable staged
// selection. An empty replacement is deliberately not removal: callers must
// use remove_field so previews never infer destructive intent from a value.
struct StagedMetadataPatch {
    std::size_t item_index{0U};
    std::size_t field_index{0U};
    StagedMetadataPatchKind kind{StagedMetadataPatchKind::replace_values};
    std::vector<std::string> values;

    friend bool operator==(const StagedMetadataPatch&, const StagedMetadataPatch&) = default;
};

struct StagedMetadataPatchLimits {
    std::size_t patches{100'000U};
    std::size_t values_per_patch{16'384U};
    std::size_t total_text_bytes{64U * 1'024U * 1'024U};
};

struct StagedMetadataFieldProjection {
    MetadataSelectionFieldState state{MetadataSelectionFieldState::missing};
    std::size_t present_item_count{0U};
    std::size_t staged_item_count{0U};
    // Populated only when state is common. This is the exact ordered result,
    // including duplicates and explicit empty strings.
    std::vector<std::string> common_values;

    friend bool operator==(const StagedMetadataFieldProjection&,
                           const StagedMetadataFieldProjection&) = default;
};

// Sparse in-memory draft over a StagedMetadataSelection. It does not mutate
// the baseline document and has no filesystem/write behavior.
class StagedMetadataPatchSet final {
  public:
    explicit StagedMetadataPatchSet(StagedMetadataPatchLimits limits = {});

    [[nodiscard]] core::Result<bool> replace_values(const StagedMetadataSelection& selection,
                                                    std::size_t item_index, std::size_t field_index,
                                                    std::vector<std::string> values);
    [[nodiscard]] core::Result<bool> remove_field(const StagedMetadataSelection& selection,
                                                  std::size_t item_index, std::size_t field_index);
    [[nodiscard]] core::Result<bool> revert(const StagedMetadataSelection& selection,
                                            std::size_t item_index, std::size_t field_index);
    void clear();

    [[nodiscard]] const StagedMetadataPatch* patch(std::size_t item_index,
                                                   std::size_t field_index) const noexcept;
    [[nodiscard]] std::vector<StagedMetadataPatch> patches() const;
    [[nodiscard]] StagedMetadataFieldProjection
    project_field(const StagedMetadataSelection& selection, std::size_t field_index) const;
    [[nodiscard]] core::Result<std::vector<StagedMetadataFieldProjection>>
    project_items(const StagedMetadataSelection& selection,
                  std::span<const std::size_t> item_indexes,
                  const core::CancellationToken& cancellation = {}) const;

    [[nodiscard]] std::size_t patch_count() const noexcept { return state_->patches.size(); }
    [[nodiscard]] std::size_t field_patch_count(std::size_t field_index) const noexcept;
    [[nodiscard]] std::size_t total_text_bytes() const noexcept { return state_->total_text_bytes; }
    [[nodiscard]] bool empty() const noexcept { return state_->patches.empty(); }

  private:
    using Key = std::pair<std::size_t, std::size_t>;

    struct State {
        std::map<Key, StagedMetadataPatch> patches;
        std::map<std::size_t, std::size_t> field_patch_counts;
        std::size_t total_text_bytes{0U};
    };

    [[nodiscard]] core::Result<void> validate_cell(const StagedMetadataSelection& selection,
                                                   std::size_t item_index,
                                                   std::size_t field_index) const;
    [[nodiscard]] core::Result<bool> store_patch(const StagedMetadataSelection& selection,
                                                 StagedMetadataPatch patch);
    [[nodiscard]] static std::size_t text_bytes(const StagedMetadataPatch& patch) noexcept;
    void detach();

    StagedMetadataPatchLimits limits_;
    std::shared_ptr<State> state_;
};

// ADR-0257: drafts staged on a provisional selection -- built from a list's
// cached tags while the files are read -- carried onto the selection built
// from the files' own. Rows are matched by position and must name the same
// file; fields by name, `onto` gaining any a draft names that it lacks. A
// value edit is the same edit whatever the old value was. A draft that is no
// change against the real tags, or removes a field the file does not have,
// is dropped and counted.
struct CarriedMetadataPatches {
    StagedMetadataPatchSet patches;
    std::size_t carried{0U};
    std::size_t dropped{0U};
};
[[nodiscard]] core::Result<CarriedMetadataPatches>
carry_staged_patches(const StagedMetadataSelection& from, const StagedMetadataPatchSet& patches,
                     StagedMetadataSelection& onto);

} // namespace trackknife::metadata

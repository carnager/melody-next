// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/metadata/staged_patch.hpp"

#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace trackknife::metadata {
namespace {

[[nodiscard]] core::Error invalid_cell_error(const std::size_t item_index,
                                             const std::size_t field_index) {
    return core::Error{
        .code = core::ErrorCode::invalid_argument,
        .message = "staged metadata patch cell is out of range",
        .context = {{.key = "item", .value = std::to_string(item_index)},
                    {.key = "field", .value = std::to_string(field_index)}},
    };
}

[[nodiscard]] core::Error limit_error(const std::string_view subject, const std::size_t limit) {
    return core::Error{
        .code = core::ErrorCode::limit_exceeded,
        .message = "staged metadata patch " + std::string{subject} + " limit was exceeded",
        .context = {{.key = "limit", .value = std::to_string(limit)}},
    };
}

} // namespace

StagedMetadataPatchSet::StagedMetadataPatchSet(const StagedMetadataPatchLimits limits)
    : limits_(limits), state_(std::make_shared<State>()) {}

core::Result<bool> StagedMetadataPatchSet::replace_values(const StagedMetadataSelection& selection,
                                                          const std::size_t item_index,
                                                          const std::size_t field_index,
                                                          std::vector<std::string> values) {
    if (values.empty()) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::invalid_argument,
            .message = "a staged replacement must contain at least one value",
            .context = {},
        });
    }
    return store_patch(selection, StagedMetadataPatch{
                                      .item_index = item_index,
                                      .field_index = field_index,
                                      .kind = StagedMetadataPatchKind::replace_values,
                                      .values = std::move(values),
                                  });
}

core::Result<bool> StagedMetadataPatchSet::remove_field(const StagedMetadataSelection& selection,
                                                        const std::size_t item_index,
                                                        const std::size_t field_index) {
    if (const auto valid = validate_cell(selection, item_index, field_index); !valid) {
        return std::unexpected(valid.error());
    }
    if (selection.cell(item_index, field_index) == nullptr) {
        return revert(selection, item_index, field_index);
    }
    return store_patch(selection, StagedMetadataPatch{
                                      .item_index = item_index,
                                      .field_index = field_index,
                                      .kind = StagedMetadataPatchKind::remove_field,
                                      .values = {},
                                  });
}

core::Result<bool> StagedMetadataPatchSet::revert(const StagedMetadataSelection& selection,
                                                  const std::size_t item_index,
                                                  const std::size_t field_index) {
    if (const auto valid = validate_cell(selection, item_index, field_index); !valid) {
        return std::unexpected(valid.error());
    }
    const auto found = state_->patches.find({item_index, field_index});
    if (found == state_->patches.end()) {
        return false;
    }
    detach();
    const auto mutable_found = state_->patches.find({item_index, field_index});
    state_->total_text_bytes -= text_bytes(mutable_found->second);
    const auto field_count = state_->field_patch_counts.find(field_index);
    if (field_count != state_->field_patch_counts.end() && --field_count->second == 0U) {
        state_->field_patch_counts.erase(field_count);
    }
    state_->patches.erase(mutable_found);
    return true;
}

void StagedMetadataPatchSet::clear() {
    if (state_->patches.empty()) {
        return;
    }
    if (state_.use_count() != 1) {
        state_ = std::make_shared<State>();
        return;
    }
    state_->patches.clear();
    state_->field_patch_counts.clear();
    state_->total_text_bytes = 0U;
}

const StagedMetadataPatch*
StagedMetadataPatchSet::patch(const std::size_t item_index,
                              const std::size_t field_index) const noexcept {
    const auto found = state_->patches.find({item_index, field_index});
    return found == state_->patches.end() ? nullptr : &found->second;
}

std::vector<StagedMetadataPatch> StagedMetadataPatchSet::patches() const {
    std::vector<StagedMetadataPatch> result;
    result.reserve(state_->patches.size());
    for (const auto& [key, patch] : state_->patches) {
        static_cast<void>(key);
        result.push_back(patch);
    }
    return result;
}

StagedMetadataFieldProjection
StagedMetadataPatchSet::project_field(const StagedMetadataSelection& selection,
                                      const std::size_t field_index) const {
    if (field_index >= selection.field_count()) {
        throw std::out_of_range{"staged metadata field index is out of range"};
    }
    const std::vector<std::string>* first_values = nullptr;
    std::size_t present_count = 0U;
    std::size_t staged_count = 0U;
    bool values_differ = false;
    for (std::size_t item_index = 0U; item_index < selection.item_count(); ++item_index) {
        const auto* staged = patch(item_index, field_index);
        staged_count += staged == nullptr ? 0U : 1U;
        const std::vector<std::string>* values = nullptr;
        if (staged != nullptr) {
            if (staged->kind == StagedMetadataPatchKind::replace_values) {
                values = &staged->values;
            }
        } else if (const auto* baseline = selection.cell(item_index, field_index)) {
            values = &baseline->values;
        }
        if (values == nullptr) {
            continue;
        }
        ++present_count;
        if (first_values == nullptr) {
            first_values = values;
        } else if (*first_values != *values) {
            values_differ = true;
        }
    }

    auto state = MetadataSelectionFieldState::missing;
    if (present_count == 0U) {
        state = MetadataSelectionFieldState::missing;
    } else if (present_count != selection.item_count()) {
        state = MetadataSelectionFieldState::partial;
    } else if (values_differ) {
        state = MetadataSelectionFieldState::mixed;
    } else {
        state = MetadataSelectionFieldState::common;
    }
    return {
        .state = state,
        .present_item_count = present_count,
        .staged_item_count = staged_count,
        .common_values = state == MetadataSelectionFieldState::common && first_values != nullptr
                             ? *first_values
                             : std::vector<std::string>{},
    };
}

core::Result<std::vector<StagedMetadataFieldProjection>>
StagedMetadataPatchSet::project_items(const StagedMetadataSelection& selection,
                                      const std::span<const std::size_t> item_indexes,
                                      const core::CancellationToken& cancellation) const {
    const auto cancelled = [] {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::cancelled,
            .message = "staged metadata projection was cancelled",
            .context = {},
        });
    };
    if (cancellation.is_cancellation_requested()) {
        return cancelled();
    }
    for (std::size_t position = 0U; position < item_indexes.size(); ++position) {
        if (item_indexes[position] >= selection.item_count()) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::invalid_argument,
                .message = "staged metadata projection item is out of range",
                .context = {{.key = "item", .value = std::to_string(item_indexes[position])}},
            });
        }
        if (position > 0U && item_indexes[position - 1U] >= item_indexes[position]) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::invalid_argument,
                .message = "staged metadata projection items must be sorted and unique",
                .context = {{.key = "position", .value = std::to_string(position)}},
            });
        }
    }

    struct Accumulator {
        std::optional<std::vector<std::string>> first_values;
        std::size_t present_item_count{0U};
        std::size_t staged_item_count{0U};
        bool values_differ{false};
    };
    std::vector<Accumulator> accumulators(selection.field_count());
    const auto add_values = [&accumulators](const std::size_t field_index,
                                            const std::vector<std::string>& values) {
        auto& accumulator = accumulators[field_index];
        ++accumulator.present_item_count;
        if (!accumulator.first_values) {
            accumulator.first_values = values;
        } else if (*accumulator.first_values != values) {
            accumulator.values_differ = true;
        }
    };

    for (const auto item_index : item_indexes) {
        if (cancellation.is_cancellation_requested()) {
            return cancelled();
        }
        for (const auto field_index : selection.present_field_indexes(item_index)) {
            const auto* staged = patch(item_index, field_index);
            if (staged != nullptr) {
                ++accumulators[field_index].staged_item_count;
                if (staged->kind == StagedMetadataPatchKind::replace_values) {
                    add_values(field_index, staged->values);
                }
                continue;
            }
            add_values(field_index, selection.cell(item_index, field_index)->values);
        }

        const auto first_patch = state_->patches.lower_bound({item_index, 0U});
        const auto after_item =
            state_->patches.upper_bound({item_index, std::numeric_limits<std::size_t>::max()});
        for (auto staged = first_patch; staged != after_item; ++staged) {
            const auto& patch_value = staged->second;
            if (selection.cell(item_index, patch_value.field_index) != nullptr) {
                continue;
            }
            ++accumulators[patch_value.field_index].staged_item_count;
            if (patch_value.kind == StagedMetadataPatchKind::replace_values) {
                add_values(patch_value.field_index, patch_value.values);
            }
        }
    }

    std::vector<StagedMetadataFieldProjection> projections;
    projections.reserve(accumulators.size());
    for (auto& accumulator : accumulators) {
        auto state = MetadataSelectionFieldState::missing;
        if (accumulator.present_item_count == 0U) {
            state = MetadataSelectionFieldState::missing;
        } else if (accumulator.present_item_count != item_indexes.size()) {
            state = MetadataSelectionFieldState::partial;
        } else if (accumulator.values_differ) {
            state = MetadataSelectionFieldState::mixed;
        } else {
            state = MetadataSelectionFieldState::common;
        }
        projections.push_back(StagedMetadataFieldProjection{
            .state = state,
            .present_item_count = accumulator.present_item_count,
            .staged_item_count = accumulator.staged_item_count,
            .common_values =
                state == MetadataSelectionFieldState::common && accumulator.first_values
                    ? std::move(*accumulator.first_values)
                    : std::vector<std::string>{},
        });
    }
    return projections;
}

std::size_t
StagedMetadataPatchSet::field_patch_count(const std::size_t field_index) const noexcept {
    const auto found = state_->field_patch_counts.find(field_index);
    return found == state_->field_patch_counts.end() ? 0U : found->second;
}

core::Result<void> StagedMetadataPatchSet::validate_cell(const StagedMetadataSelection& selection,
                                                         const std::size_t item_index,
                                                         const std::size_t field_index) const {
    if (item_index >= selection.item_count() || field_index >= selection.field_count()) {
        return std::unexpected(invalid_cell_error(item_index, field_index));
    }
    return {};
}

core::Result<bool> StagedMetadataPatchSet::store_patch(const StagedMetadataSelection& selection,
                                                       StagedMetadataPatch patch_value) {
    if (const auto valid =
            validate_cell(selection, patch_value.item_index, patch_value.field_index);
        !valid) {
        return std::unexpected(valid.error());
    }
    if (patch_value.kind == StagedMetadataPatchKind::replace_values &&
        patch_value.values.size() > limits_.values_per_patch) {
        return std::unexpected(limit_error("value", limits_.values_per_patch));
    }
    const auto key = Key{patch_value.item_index, patch_value.field_index};
    const auto existing = state_->patches.find(key);
    const auto existing_bytes =
        existing == state_->patches.end() ? 0U : text_bytes(existing->second);
    const auto replacement_bytes = text_bytes(patch_value);
    if (replacement_bytes > limits_.total_text_bytes -
                                std::min(state_->total_text_bytes, limits_.total_text_bytes) +
                                existing_bytes) {
        return std::unexpected(limit_error("text", limits_.total_text_bytes));
    }

    const auto* baseline = selection.cell(patch_value.item_index, patch_value.field_index);
    if (patch_value.kind == StagedMetadataPatchKind::replace_values && baseline != nullptr &&
        patch_value.values == baseline->values) {
        return revert(selection, patch_value.item_index, patch_value.field_index);
    }
    if (existing == state_->patches.end() && state_->patches.size() == limits_.patches) {
        return std::unexpected(limit_error("count", limits_.patches));
    }
    if (existing != state_->patches.end() && existing->second == patch_value) {
        return false;
    }

    const auto inserting = existing == state_->patches.end();
    detach();
    state_->total_text_bytes = state_->total_text_bytes - existing_bytes + replacement_bytes;
    if (inserting) {
        state_->patches.emplace(key, std::move(patch_value));
        ++state_->field_patch_counts[key.second];
    } else {
        state_->patches.find(key)->second = std::move(patch_value);
    }
    return true;
}

std::size_t StagedMetadataPatchSet::text_bytes(const StagedMetadataPatch& patch_value) noexcept {
    std::size_t result = 0U;
    for (const auto& value : patch_value.values) {
        if (value.size() > std::numeric_limits<std::size_t>::max() - result) {
            return std::numeric_limits<std::size_t>::max();
        }
        result += value.size();
    }
    return result;
}

void StagedMetadataPatchSet::detach() {
    if (state_.use_count() != 1) {
        state_ = std::make_shared<State>(*state_);
    }
}

core::Result<CarriedMetadataPatches>
carry_staged_patches(const StagedMetadataSelection& from, const StagedMetadataPatchSet& patches,
                     StagedMetadataSelection& onto) {
    if (from.item_count() != onto.item_count()) {
        return std::unexpected(core::Error{.code = core::ErrorCode::invariant,
                                           .message = "a selection read anew has other rows",
                                           .context = {}});
    }
    CarriedMetadataPatches carried;
    for (const auto& patch : patches.patches()) {
        if (from.source(patch.item_index).raw_path != onto.source(patch.item_index).raw_path) {
            return std::unexpected(core::Error{.code = core::ErrorCode::invariant,
                                               .message = "a selection read anew has other rows",
                                               .context = {}});
        }
        const auto& field = from.field(patch.field_index);
        auto target = field.exact_native_name
                          ? onto.exact_native_field_index(*field.exact_native_name)
                          : onto.field_index(field.canonical_name);
        if (!target) {
            auto added = field.exact_native_name
                             ? onto.ensure_exact_native_field(*field.exact_native_name,
                                                              field.display_name)
                             : onto.ensure_missing_field(field.canonical_name, field.display_name);
            if (!added) {
                return std::unexpected(std::move(added.error()));
            }
            target = *added;
        }
        const auto stored =
            patch.kind == StagedMetadataPatchKind::remove_field
                ? (onto.cell(patch.item_index, *target) == nullptr
                       ? core::Result<bool>{false}
                       : carried.patches.remove_field(onto, patch.item_index, *target))
                : carried.patches.replace_values(onto, patch.item_index, *target, patch.values);
        if (!stored) {
            return std::unexpected(std::move(stored.error()));
        }
        if (*stored) {
            ++carried.carried;
        } else {
            ++carried.dropped;
        }
    }
    return carried;
}

} // namespace trackknife::metadata

// SPDX-License-Identifier: GPL-3.0-only
#include "workspace/preparation_planning.hpp"

#include "bench/settings_keys.hpp"
#include "trackknife/metadata/draft_document.hpp"
#include "trackknife/operations/output_path_preflight.hpp"

#include <QSettings>

#include <algorithm>
#include <string>
#include <utility>

namespace trackknife::bench {

core::Result<operations::PreparationPlan> planPreparation(PreparationRequest request,
                                                          const core::CancellationToken& cancellation) {
    const auto& operation_selection = request.operations;
    const auto& tools = request.tools;
    const auto& draft = request.draft;
    const auto& items = request.items;
    // Automatic scripts already staged their edits into the draft.
    const auto metadata_context_change_count =
        (operation_selection.save_tags ? draft.patch_count() : 0U) + request.artwork.size();
    std::optional<metadata::MetadataWritePlan> metadata_plan;
    if (operation_selection.save_tags && !draft.empty()) {
        auto revalidated = metadata::build_metadata_write_plan(*request.selection, draft,
                                                               tools.access, cancellation,
                                                               request.options);
        if (!revalidated) {
            return std::unexpected(std::move(revalidated.error()));
        }
        metadata_plan = std::move(*revalidated);
    }

    if (!request.artwork.empty()) {
        // ADR-0237: images of this computer handed over first when the
        // engine writes; planned against the files where they are.
        auto staged = stageReplacements(std::move(request.artwork), tools, cancellation,
                                        request.cover_policy);
        if (!staged) {
            return std::unexpected(staged.error());
        }
        auto art = operations::plan_artwork_storage(*staged, request.cover_policy, cancellation,
                                                    artworkFitterFor(tools), tools.artwork);
        if (!art) {
            return std::unexpected(art.error());
        }
        auto merged = metadata::merge_artwork_write_plan(
            metadata_plan.value_or(metadata::MetadataWritePlan{}), std::move(*art));
        if (!merged) {
            return std::unexpected(merged.error());
        }
        metadata_plan = std::move(*merged);
    }
    std::optional<operations::OutputPathPlan> path_plan;
    std::optional<operations::OutputPathPreflight> path_preflight;
    if (operation_selection.rename_files || operation_selection.move_files) {
        const metadata::StagedMetadataPatchSet actual_source_tags;
        const auto& naming_selection = *request.selection;
        const auto& naming_context = operation_selection.save_tags ? draft : actual_source_tags;
        auto documents = metadata::materialize_metadata_draft(naming_selection, naming_context,
                                                              items, cancellation);
        if (!documents) {
            return std::unexpected(std::move(documents.error()));
        }
        std::vector<operations::OutputPathPlanningItem> planning_items;
        planning_items.reserve(items.size());
        for (std::size_t position = 0U; position < items.size(); ++position) {
            const auto item_index = items[position];
            const auto& source = naming_selection.source(item_index);
            if (!source.source_revision) {
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::conflict,
                    .message = "File path planning requires a fresh source revision "
                               "for every selected track",
                    .context = {{.key = "item", .value = std::to_string(item_index)}},
                });
            }
            planning_items.push_back(operations::OutputPathPlanningItem{
                .item_index = item_index,
                .source_raw_path = source.raw_path,
                .source_revision = *source.source_revision,
                .final_metadata = std::move((*documents)[position]),
            });
        }
        auto planned = operations::plan_output_paths(
            planning_items,
            operations::OutputPathOperationSelection{
                .rename_files = operation_selection.rename_files,
                .move_files = operation_selection.move_files,
            },
            std::move(*request.layout), std::move(request.destination), {}, cancellation);
        if (!planned) {
            return std::unexpected(std::move(planned.error()));
        }
        path_plan = std::move(*planned);
        if (path_plan->ready()) {
            auto checked = tools.preflight
                               ? tools.preflight(*path_plan, cancellation)
                               : operations::preflight_output_paths(*path_plan, cancellation);
            if (!checked) {
                return std::unexpected(std::move(checked.error()));
            }
            path_preflight = std::move(*checked);
        }
    }
    return operations::assemble_preparation_plan(operation_selection,
                                                 metadata_context_change_count,
                                                 std::move(metadata_plan), std::move(path_plan),
                                                 std::move(path_preflight));
}

metadata::MetadataWritePlanOptions writePlanOptions() {
    const QSettings stored;
    return metadata::MetadataWritePlanOptions{
        .sidecar_loudness =
            stored.value(QLatin1String(SettingsKeys::replaygain_sidecar_only_key), false).toBool(),
        .true_peak_loudness =
            stored.value(QLatin1String(SettingsKeys::replaygain_true_peak_key), false).toBool()};
}

core::Result<metadata::StagedMetadataPatchSet>
draftOf(const metadata::StagedMetadataSelection& selection,
        const metadata::StagedMetadataPatchSet& draft, const std::span<const std::size_t> items) {
    metadata::StagedMetadataPatchSet subset;
    for (const auto& patch : draft.patches()) {
        if (std::ranges::find(items, patch.item_index) == items.end()) {
            continue;
        }
        auto stored =
            patch.kind == metadata::StagedMetadataPatchKind::remove_field
                ? subset.remove_field(selection, patch.item_index, patch.field_index)
                : subset.replace_values(selection, patch.item_index, patch.field_index,
                                        patch.values);
        if (!stored) {
            return std::unexpected(std::move(stored.error()));
        }
    }
    return subset;
}

} // namespace trackknife::bench

// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/operations/undo.hpp"

#include <utility>

namespace trackknife::operations {

core::Result<std::vector<UndoOutcome>>
undo_operations(const std::span<const UndoRequest> requests,
                MetadataOperationJournal& metadata_journal,
                FilePublicationJournal& publication_journal,
                const MetadataDependentStateCommitter& metadata_dependent,
                const FilePublicationDependentStateCommitter& publication_dependent,
                const UndoProgressCallback& progress, const core::CancellationToken& cancellation) {
    std::vector<UndoOutcome> outcomes;
    outcomes.reserve(requests.size());
    for (const auto& request : requests) {
        if (cancellation.is_cancellation_requested()) {
            return std::unexpected(core::Error{.code = core::ErrorCode::cancelled,
                                               .message = "Undo stopped",
                                               .context = {}});
        }
        UndoOutcome outcome{.request = request, .issue = std::nullopt, .from_raw_path = {},
                            .to_raw_path = {}};
        if (request.kind == UndoKind::metadata) {
            auto undone = undo_metadata_operation(request.journal_id, metadata_journal,
                                                  metadata_dependent, cancellation);
            if (undone) {
                outcome.from_raw_path = undone->source_raw_path;
                outcome.to_raw_path = undone->source_raw_path;
            } else {
                outcome.issue = std::move(undone.error());
            }
        } else {
            auto undone = undo_file_publication(request.journal_id, publication_journal,
                                                publication_dependent, cancellation);
            if (undone) {
                outcome.from_raw_path = undone->source_raw_path;
                outcome.to_raw_path = undone->target_raw_path;
            } else {
                outcome.issue = std::move(undone.error());
            }
        }
        outcomes.push_back(std::move(outcome));
        if (progress) {
            progress(outcomes.size(), requests.size());
        }
    }
    return outcomes;
}

} // namespace trackknife::operations

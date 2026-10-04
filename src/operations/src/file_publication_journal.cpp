// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/operations/file_publication_journal.hpp"

#include <utility>

namespace trackknife::operations {

std::filesystem::path file_publication_prepared_path(const std::filesystem::path& target,
                                                     const core::StableId& journal_id) {
    return target.parent_path() / (".trackknife-" + journal_id.to_string() + ".prepared");
}

bool publication_retains_source(const FilePublicationJournalRecord& record) noexcept {
    return record.publication == OutputPathPublicationKind::cross_filesystem_copy ||
           record.content == FilePublicationContentKind::prepared_destination_artifact;
}

std::filesystem::path file_publication_retained_path(const std::filesystem::path& source,
                                                     const core::StableId& journal_id) {
    return source.parent_path() / (".trackknife-" + journal_id.to_string() + ".retained");
}

core::Result<FilePublicationJournalRecord>
make_file_publication_journal_record(const OutputPathPreflight& preflight,
                                     const std::size_t source_index,
                                     const core::StableId& journal_id) {
    if (!preflight.ready() || journal_id.is_nil() || source_index >= preflight.sources.size()) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::invalid_argument,
            .message = "File publication requires a ready preflight, source, and operation ID",
            .context = {},
        });
    }
    const auto& source = preflight.sources[source_index];
    if (source.publication == OutputPathPublicationKind::no_change) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::invalid_argument,
            .message = "A no-change path does not create file-publication journal evidence",
            .context = {},
        });
    }
    const auto prepared =
        source.publication == OutputPathPublicationKind::cross_filesystem_copy
            ? file_publication_prepared_path(source.planned.target_raw_path, journal_id).native()
            : std::string{};
    return FilePublicationJournalRecord{
        .id = journal_id,
        .state = FilePublicationJournalState::planned,
        .publication = source.publication,
        .content = FilePublicationContentKind::preserve_source_bytes,
        .source_raw_path = source.planned.source_raw_path,
        .target_raw_path = source.planned.target_raw_path,
        .prepared_raw_path = prepared,
        .expected_source_revision = source.observed_revision,
        .prepared_revision = std::nullopt,
        .target_revision = std::nullopt,
        .occurrence_indexes = source.planned.item_indexes,
        .planned_missing_directory_raw_paths = source.missing_directory_raw_paths,
        .reverses_journal_id = std::nullopt,
        .failure = std::nullopt,
    };
}

core::Result<FilePublicationJournalRecord>
make_destination_artifact_journal_record(const OutputPathPreflight& preflight,
                                         const std::size_t source_index,
                                         const core::StableId& journal_id) {
    auto record = make_file_publication_journal_record(preflight, source_index, journal_id);
    if (!record) {
        return std::unexpected(std::move(record.error()));
    }
    record->content = FilePublicationContentKind::prepared_destination_artifact;
    record->prepared_raw_path =
        file_publication_prepared_path(record->target_raw_path, journal_id).native();
    return record;
}

} // namespace trackknife::operations

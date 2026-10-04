// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/error.hpp"
#include "trackknife/core/local_sources.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/core/stable_id.hpp"
#include "trackknife/operations/metadata_journal.hpp"
#include "trackknife/operations/output_path_preflight.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace trackknife::operations {

enum class FilePublicationJournalState : std::uint8_t {
    planned = 0,
    target_prepared = 1,
    target_published = 2,
    dependent_state_committed = 3,
    source_removed = 4,
    complete = 5,
    rolled_back = 6,
    needs_reconciliation = 7,
};

enum class FilePublicationContentKind : std::uint8_t {
    preserve_source_bytes = 0,
    prepared_destination_artifact = 1,
};

struct FilePublicationJournalRecord {
    core::StableId id;
    FilePublicationJournalState state{FilePublicationJournalState::planned};
    OutputPathPublicationKind publication{OutputPathPublicationKind::same_filesystem_rename};
    FilePublicationContentKind content{FilePublicationContentKind::preserve_source_bytes};
    std::string source_raw_path;
    std::string target_raw_path;
    // Empty only for a byte-preserving same-filesystem rename. Prepared-copy
    // and changed-artifact lifecycles use the exact executor-owned sibling
    // returned by file_publication_prepared_path.
    std::string prepared_raw_path;
    core::LocalSourceRevision expected_source_revision;
    std::optional<core::LocalSourceRevision> prepared_revision;
    std::optional<core::LocalSourceRevision> target_revision;
    std::vector<std::size_t> occurrence_indexes;
    std::vector<std::string> planned_missing_directory_raw_paths;
    // A reverse publication is an undo attempt for this completed record.
    // Rolled-back attempts may be followed by another reverse record.
    std::optional<core::StableId> reverses_journal_id;
    std::optional<core::Error> failure;

    friend bool operator==(const FilePublicationJournalRecord&,
                           const FilePublicationJournalRecord&) = default;
};

struct FilePublicationJournalTransition {
    FilePublicationJournalState expected_state{FilePublicationJournalState::planned};
    FilePublicationJournalState state{FilePublicationJournalState::planned};
    std::optional<core::LocalSourceRevision> prepared_revision;
    std::optional<core::LocalSourceRevision> target_revision;
    std::optional<core::Error> failure;
};

// ADR-0263: the retained source of a completed publication, with the same
// lifecycle as a metadata operation's backup -- retained, undone or
// released, or left for reconciliation.
struct FilePublicationBackupRecord {
    FilePublicationJournalRecord publication;
    MetadataOperationBackupState state{MetadataOperationBackupState::retained};
    std::optional<core::StableId> undo_id;
    std::int64_t completed_at_unix_seconds{0};
    std::int64_t updated_at_unix_seconds{0};
    std::optional<core::Error> failure;
    // ADR-0266: where the retained source is kept, when not beside the
    // source it was (empty), and its identity there when it is a copy.
    std::string kept_raw_path;
    std::optional<core::LocalSourceRevision> kept_revision;

    friend bool operator==(const FilePublicationBackupRecord&,
                           const FilePublicationBackupRecord&) = default;
};
using FilePublicationBackupTransition = MetadataOperationBackupTransition;

[[nodiscard]] std::filesystem::path
file_publication_prepared_path(const std::filesystem::path& target,
                               const core::StableId& journal_id);

// ADR-0263: a publication that writes a new file at the target -- copied to
// another filesystem, or written with its tags -- keeps the source it
// replaces, renamed beside it, for undo; same-filesystem renames keep the
// file itself and need nothing.
[[nodiscard]] bool publication_retains_source(const FilePublicationJournalRecord& record) noexcept;
[[nodiscard]] std::filesystem::path
file_publication_retained_path(const std::filesystem::path& source,
                               const core::StableId& journal_id);

[[nodiscard]] core::Result<FilePublicationJournalRecord>
make_file_publication_journal_record(const OutputPathPreflight& preflight, std::size_t source_index,
                                     const core::StableId& journal_id);

// Selects the prepared-artifact lifecycle on either filesystem topology. The
// artifact itself is created only after this record has been durably stored.
[[nodiscard]] core::Result<FilePublicationJournalRecord>
make_destination_artifact_journal_record(const OutputPathPreflight& preflight,
                                         std::size_t source_index,
                                         const core::StableId& journal_id);

class FilePublicationJournal {
  public:
    FilePublicationJournal() = default;
    FilePublicationJournal(FilePublicationJournal&&) noexcept = default;
    FilePublicationJournal& operator=(FilePublicationJournal&&) noexcept = default;
    FilePublicationJournal(const FilePublicationJournal&) = delete;
    FilePublicationJournal& operator=(const FilePublicationJournal&) = delete;
    virtual ~FilePublicationJournal() = default;

    [[nodiscard]] virtual core::Result<void> create(const FilePublicationJournalRecord& record) = 0;
    [[nodiscard]] virtual core::Result<void>
    transition(const core::StableId& id, const FilePublicationJournalTransition& transition) = 0;
    [[nodiscard]] virtual core::Result<std::optional<FilePublicationJournalRecord>>
    load(const core::StableId& id) const = 0;
    [[nodiscard]] virtual core::Result<std::vector<FilePublicationJournalRecord>>
    load_incomplete() const = 0;
    [[nodiscard]] virtual core::Result<std::vector<FilePublicationJournalRecord>>
    load_reversals(const core::StableId& journal_id) const = 0;
    // A publication that retains its source records it as retained when it
    // completes; newest first.
    [[nodiscard]] virtual core::Result<std::optional<FilePublicationBackupRecord>>
    load_backup(const core::StableId& id) const = 0;
    [[nodiscard]] virtual core::Result<std::vector<FilePublicationBackupRecord>>
    load_backups() const = 0;
    [[nodiscard]] virtual core::Result<void>
    transition_backup(const core::StableId& id, const FilePublicationBackupTransition& transition) = 0;
    // ADR-0266: a retained source moved where undo copies are kept.
    [[nodiscard]] virtual core::Result<void>
    relocate_kept(const core::StableId& id, const std::string& raw_path,
                  const core::LocalSourceRevision& revision) {
        static_cast<void>(id);
        static_cast<void>(raw_path);
        static_cast<void>(revision);
        return std::unexpected(core::Error{.code = core::ErrorCode::unsupported,
                                           .message = "This journal cannot move retained sources",
                                           .context = {}});
    }
};

} // namespace trackknife::operations

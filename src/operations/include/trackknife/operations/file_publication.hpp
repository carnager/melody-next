// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/local_sources.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/core/stable_id.hpp"
#include "trackknife/operations/file_publication_journal.hpp"
#include "trackknife/operations/metadata_commit.hpp"
#include "trackknife/operations/output_path_preflight.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace trackknife::operations {

struct FilePublicationCommitResult {
    core::StableId journal_id;
    FilePublicationContentKind content{FilePublicationContentKind::preserve_source_bytes};
    std::string source_raw_path;
    std::string target_raw_path;
    core::LocalSourceRevision source_revision;
    core::LocalSourceRevision target_revision;
    std::vector<std::size_t> occurrence_indexes;
    // Non-blocking degradations on limited destination filesystems
    // (ADR-0111) — preservation that could not happen; the publication
    // itself succeeded and verified.
    std::vector<std::string> notes;

    friend bool operator==(const FilePublicationCommitResult&,
                           const FilePublicationCommitResult&) = default;
};

using FilePublicationDependentStateCommitter =
    std::function<core::Result<void>(const FilePublicationCommitResult&)>;
using FilePublicationDirectoryCreationObserver =
    std::function<void(std::span<const std::string> created_raw_paths)>;

// Creates and fully verifies one exclusive regular file at prepared_raw_path.
// Failure must leave no path; success returns its final observed revision. The
// executor reapplies source filesystem metadata, locks and syncs the artifact,
// and revalidates both source and artifact before publication.
using FilePublicationDestinationArtifactPreparer =
    std::function<core::Result<core::LocalSourceRevision>(
        const std::string& prepared_raw_path, const core::CancellationToken& cancellation)>;

// Executes one ready same-filesystem source preflight. The caller runs this
// synchronous operation on a bounded mutation worker. The dependent-state
// callback must be idempotent and all-or-nothing because recovery may replay
// it after a crash. Its failure restores the original path before returning.
[[nodiscard]] core::Result<FilePublicationCommitResult> commit_same_filesystem_publication(
    const OutputPathPreflight& preflight, std::size_t source_index, FilePublicationJournal& journal,
    const FilePublicationDependentStateCommitter& dependent_state_committer,
    const core::CancellationToken& cancellation = {},
    const FilePublicationDirectoryCreationObserver& directory_creation_observer = {});

// Reverses one completed same-filesystem publication as a second journaled
// no-replace rename. The reverse record has its own dependent-state identity,
// is recovered by the normal publication recovery path, and makes repeated
// successful undo requests idempotent.
[[nodiscard]] core::Result<FilePublicationCommitResult> undo_same_filesystem_publication(
    const core::StableId& journal_id, FilePublicationJournal& journal,
    const FilePublicationDependentStateCommitter& dependent_state_committer,
    const core::CancellationToken& cancellation = {});

// Copies one ready cross-filesystem source into an executor-owned sibling,
// verifies exact bytes, publishes without replacement, advances dependent
// state, and only then removes the still-locked original source.
[[nodiscard]] core::Result<FilePublicationCommitResult> commit_cross_filesystem_publication(
    const OutputPathPreflight& preflight, std::size_t source_index, FilePublicationJournal& journal,
    const FilePublicationDependentStateCommitter& dependent_state_committer,
    const core::CancellationToken& cancellation = {},
    const FilePublicationDirectoryCreationObserver& directory_creation_observer = {});

// Publishes one changed-content artifact directly at the reviewed destination.
// This never rewrites the source and then renames it: publication, dependent
// state, and exact source removal share the prepared-artifact journal lifecycle
// on both same- and cross-filesystem paths.
[[nodiscard]] core::Result<FilePublicationCommitResult> commit_destination_artifact_publication(
    const OutputPathPreflight& preflight, std::size_t source_index, FilePublicationJournal& journal,
    const FilePublicationDestinationArtifactPreparer& artifact_preparer,
    const FilePublicationDependentStateCommitter& dependent_state_committer,
    const core::CancellationToken& cancellation = {},
    const FilePublicationDirectoryCreationObserver& directory_creation_observer = {});

enum class FilePublicationRecoveryOutcome : std::uint8_t {
    completed,
    rolled_back,
    needs_reconciliation,
};

struct FilePublicationRecoveryResult {
    core::StableId journal_id;
    FilePublicationRecoveryOutcome outcome{FilePublicationRecoveryOutcome::needs_reconciliation};
    std::optional<core::Error> issue;

    friend bool operator==(const FilePublicationRecoveryResult&,
                           const FilePublicationRecoveryResult&) = default;
};

// Recovers incomplete byte-preserving same-filesystem renames and leaves every
// prepared-copy/artifact record for its distinct source-removal state machine.
[[nodiscard]] core::Result<std::vector<FilePublicationRecoveryResult>>
recover_same_filesystem_publications(
    FilePublicationJournal& journal,
    const FilePublicationDependentStateCommitter& dependent_state_committer,
    const core::CancellationToken& cancellation = {});

// Recovers exact prepared, published, dependent-committed, and source-removed
// copy/artifact boundaries. Ambiguous evidence is retained for manual
// reconciliation and is never deleted by inference.
[[nodiscard]] core::Result<std::vector<FilePublicationRecoveryResult>>
recover_cross_filesystem_publications(
    FilePublicationJournal& journal,
    const FilePublicationDependentStateCommitter& dependent_state_committer,
    const core::CancellationToken& cancellation = {});

// ADR-0263: undoes one completed publication that retained its source --
// the source renamed back where it was, the target removed, the directories
// the publication made removed when empty -- through the retained source's
// lifecycle: undoing, then undone, resumed by recover_publication_undos after
// a crash. The result is the reverse publication: from the target back to
// the source.
[[nodiscard]] core::Result<FilePublicationCommitResult>
undo_retained_publication(const core::StableId& journal_id, FilePublicationJournal& journal,
                          const FilePublicationDependentStateCommitter& dependent_state_committer,
                          const core::CancellationToken& cancellation = {});

// Any completed publication undone as its kind is: a same-filesystem rename
// renamed back, a retained source restored.
[[nodiscard]] core::Result<FilePublicationCommitResult>
undo_file_publication(const core::StableId& journal_id, FilePublicationJournal& journal,
                      const FilePublicationDependentStateCommitter& dependent_state_committer,
                      const core::CancellationToken& cancellation = {});

// Removes one verified retained source; one already gone is released.
[[nodiscard]] core::Result<void>
release_publication_backup(const core::StableId& journal_id, FilePublicationJournal& journal,
                           const core::CancellationToken& cancellation = {});

// Keeps the newest retained sources within the policy's age, count and
// bytes and releases the rest, as maintain_metadata_backups does.
[[nodiscard]] core::Result<std::vector<MetadataBackupMaintenanceResult>>
maintain_publication_backups(FilePublicationJournal& journal,
                             const MetadataBackupRetentionPolicy& policy,
                             std::int64_t now_unix_seconds,
                             const core::CancellationToken& cancellation = {});

// Finishes every undo a crash interrupted, or leaves it for reconciliation.
[[nodiscard]] core::Result<std::vector<FilePublicationRecoveryResult>>
recover_publication_undos(FilePublicationJournal& journal,
                          const FilePublicationDependentStateCommitter& dependent_state_committer,
                          const core::CancellationToken& cancellation = {});

} // namespace trackknife::operations

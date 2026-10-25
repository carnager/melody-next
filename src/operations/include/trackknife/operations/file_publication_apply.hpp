// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/error.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/metadata/document.hpp"
#include "trackknife/operations/file_publication.hpp"
#include "trackknife/operations/file_publication_journal.hpp"
#include "trackknife/operations/metadata_commit.hpp"
#include "trackknife/operations/metadata_journal.hpp"
#include "trackknife/operations/output_path_preflight.hpp"
#include "trackknife/operations/preparation_plan.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace trackknife::operations {

enum class FilePublicationApplySourceState : std::uint8_t {
    pending,
    running,
    unchanged,
    committed,
    failed,
    cancelled,
};

struct FilePublicationApplySourceResult {
    std::size_t source_index{0U};
    std::string source_raw_path;
    std::string target_raw_path;
    OutputPathPublicationKind publication{OutputPathPublicationKind::no_change};
    FilePublicationApplySourceState state{FilePublicationApplySourceState::pending};
    std::optional<FilePublicationCommitResult> commit;
    std::optional<MetadataCommitResult> metadata_commit;
    // ADR-0318: the folder cover of a file that moved, written in the folder
    // it moved to once every file is there.
    std::optional<MetadataCommitResult> folder_image_commit;
    std::optional<metadata::MetadataDocument> published_metadata;
    std::optional<core::Error> issue;

    friend bool operator==(const FilePublicationApplySourceResult&,
                           const FilePublicationApplySourceResult&) = default;
};

struct FilePublicationApplyProgress {
    std::size_t source_index{0U};
    std::string source_raw_path;
    std::string target_raw_path;
    OutputPathPublicationKind publication{OutputPathPublicationKind::no_change};
    FilePublicationApplySourceState state{FilePublicationApplySourceState::pending};
    std::size_t completed_sources{0U};
    std::size_t total_sources{0U};
    std::optional<core::Error> issue;

    friend bool operator==(const FilePublicationApplyProgress&,
                           const FilePublicationApplyProgress&) = default;
};

struct FilePublicationApplyResult {
    std::vector<FilePublicationApplySourceResult> sources;
    bool cancellation_requested{false};

    [[nodiscard]] std::size_t committed_source_count() const noexcept;
    [[nodiscard]] std::size_t unchanged_source_count() const noexcept;
    [[nodiscard]] std::size_t failed_source_count() const noexcept;
    [[nodiscard]] std::size_t cancelled_source_count() const noexcept;

    friend bool operator==(const FilePublicationApplyResult&,
                           const FilePublicationApplyResult&) = default;
};

struct FilePublicationApplyOptions {
    // Independent sources may publish in parallel. Sources whose reviewed
    // targets share a missing directory prefix serialize only until each
    // required directory has been created by a successful batch member.
    std::size_t maximum_parallelism{2U};
};

using FilePublicationApplyProgressCallback =
    std::function<void(const FilePublicationApplyProgress&)>;
using DestinationArtifactDependentStateCommitter = std::function<core::Result<void>(
    const FilePublicationCommitResult&, const metadata::MetadataDocument&)>;

// Applies one entirely ready immutable filesystem preflight on a bounded
// mutation pool. Each admitted changed source receives a fresh single-source
// preflight before the same/cross-filesystem executor is selected. Results
// retain source order and unrelated successes remain committed after a partial
// failure. Cancellation stops new admission while in-flight executors reach a
// safe journaled boundary. The dependent-state callback may run concurrently
// and must remain idempotent; progress delivery is serialized.
[[nodiscard]] core::Result<FilePublicationApplyResult>
apply_file_publications(const OutputPathPreflight& preflight, FilePublicationJournal& journal,
                        const FilePublicationDependentStateCommitter& dependent_state_committer,
                        const FilePublicationApplyProgressCallback& progress = {},
                        const core::CancellationToken& cancellation = {},
                        const FilePublicationApplyOptions& options = {});

// Applies every physical source in one ready preparation plan that contains a
// path review. A source may be path-only, metadata-only because its reviewed
// path is unchanged, or one changed destination artifact. The latter is
// prepared by the qualified native-FLAC writer and invokes one composed
// dependent callback carrying the exact published document. All three paths
// retain bounded admission, ordered partial results, and fresh revalidation.
[[nodiscard]] core::Result<FilePublicationApplyResult> apply_preparation_publications(
    const PreparationPlan& plan, FilePublicationJournal& file_journal,
    MetadataOperationJournal& metadata_journal,
    const MetadataDependentStateCommitter& metadata_dependent_state_committer,
    const FilePublicationDependentStateCommitter& file_dependent_state_committer,
    const DestinationArtifactDependentStateCommitter& artifact_dependent_state_committer,
    const FilePublicationApplyProgressCallback& progress = {},
    const core::CancellationToken& cancellation = {},
    const FilePublicationApplyOptions& options = {});

} // namespace trackknife::operations

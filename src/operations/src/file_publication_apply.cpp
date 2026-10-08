// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/operations/file_publication_apply.hpp"

#include "trackknife/core/local_sources.hpp"
#include "trackknife/metadata/flac_writer.hpp"
#include "trackknife/metadata/mp3_writer.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <ranges>
#include <span>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace trackknife::operations {
namespace {

constexpr std::size_t maximum_apply_parallelism = 8U;

[[nodiscard]] core::Error apply_error(const core::ErrorCode code, std::string message,
                                      const std::string& source_raw_path = {},
                                      const std::string& target_raw_path = {}) {
    core::Error result{.code = code, .message = std::move(message), .context = {}};
    if (!source_raw_path.empty()) {
        result.context.push_back(
            {.key = "source", .value = core::escape_raw_path(source_raw_path)});
    }
    if (!target_raw_path.empty()) {
        result.context.push_back(
            {.key = "target", .value = core::escape_raw_path(target_raw_path)});
    }
    return result;
}

class SynchronizedJournal final : public FilePublicationJournal {
  public:
    explicit SynchronizedJournal(FilePublicationJournal& journal) : journal_{journal} {}

    [[nodiscard]] core::Result<void> create(const FilePublicationJournalRecord& record) override {
        std::scoped_lock lock{mutex_};
        return journal_.create(record);
    }

    [[nodiscard]] core::Result<void>
    transition(const core::StableId& id,
               const FilePublicationJournalTransition& transition) override {
        std::scoped_lock lock{mutex_};
        return journal_.transition(id, transition);
    }

    [[nodiscard]] core::Result<std::optional<FilePublicationJournalRecord>>
    load(const core::StableId& id) const override {
        std::scoped_lock lock{mutex_};
        return journal_.load(id);
    }

    [[nodiscard]] core::Result<std::vector<FilePublicationJournalRecord>>
    load_incomplete() const override {
        std::scoped_lock lock{mutex_};
        return journal_.load_incomplete();
    }

    [[nodiscard]] core::Result<std::vector<FilePublicationJournalRecord>>
    load_reversals(const core::StableId& journal_id) const override {
        std::scoped_lock lock{mutex_};
        return journal_.load_reversals(journal_id);
    }

    [[nodiscard]] core::Result<std::optional<FilePublicationBackupRecord>>
    load_backup(const core::StableId& id) const override {
        std::scoped_lock lock{mutex_};
        return journal_.load_backup(id);
    }

    [[nodiscard]] core::Result<std::vector<FilePublicationBackupRecord>>
    load_backups() const override {
        std::scoped_lock lock{mutex_};
        return journal_.load_backups();
    }

    [[nodiscard]] core::Result<void>
    transition_backup(const core::StableId& id,
                      const FilePublicationBackupTransition& transition) override {
        std::scoped_lock lock{mutex_};
        return journal_.transition_backup(id, transition);
    }

  private:
    FilePublicationJournal& journal_;
    mutable std::mutex mutex_;
};

class SynchronizedMetadataJournal final : public MetadataOperationJournal {
  public:
    explicit SynchronizedMetadataJournal(MetadataOperationJournal& journal) : journal_{journal} {}

    [[nodiscard]] core::Result<void> create(const MetadataOperationJournalRecord& record) override {
        std::scoped_lock lock{mutex_};
        return journal_.create(record);
    }

    [[nodiscard]] core::Result<void>
    transition(const core::StableId& id,
               const MetadataOperationJournalTransition& transition) override {
        std::scoped_lock lock{mutex_};
        return journal_.transition(id, transition);
    }

    [[nodiscard]] core::Result<std::optional<MetadataOperationJournalRecord>>
    load(const core::StableId& id) const override {
        std::scoped_lock lock{mutex_};
        return journal_.load(id);
    }

    [[nodiscard]] core::Result<std::vector<MetadataOperationJournalRecord>>
    load_incomplete() const override {
        std::scoped_lock lock{mutex_};
        return journal_.load_incomplete();
    }

    [[nodiscard]] core::Result<std::optional<MetadataOperationBackupRecord>>
    load_backup(const core::StableId& id) const override {
        std::scoped_lock lock{mutex_};
        return journal_.load_backup(id);
    }

    [[nodiscard]] core::Result<std::vector<MetadataOperationBackupRecord>>
    load_backups() const override {
        std::scoped_lock lock{mutex_};
        return journal_.load_backups();
    }

    [[nodiscard]] core::Result<void>
    transition_backup(const core::StableId& id,
                      const MetadataOperationBackupTransition& transition) override {
        std::scoped_lock lock{mutex_};
        return journal_.transition_backup(id, transition);
    }

  private:
    MetadataOperationJournal& journal_;
    mutable std::mutex mutex_;
};

struct DirectoryTopologyGroup {
    std::mutex mutex;
    std::unordered_set<std::string> created_raw_paths;
};

[[nodiscard]] core::Result<void>
require_reviewed_topology(const OutputPathPreflightSource& reviewed,
                          const OutputPathPreflightSource& refreshed,
                          const DirectoryTopologyGroup* group) {
    if (refreshed.publication != reviewed.publication ||
        refreshed.target_filesystem_device != reviewed.target_filesystem_device) {
        return std::unexpected(
            apply_error(core::ErrorCode::conflict,
                        "Filesystem publication kind changed after the reviewed preflight",
                        reviewed.planned.source_raw_path, reviewed.planned.target_raw_path));
    }
    const auto& expected = reviewed.missing_directory_raw_paths;
    const auto& current = refreshed.missing_directory_raw_paths;
    if (current.size() > expected.size() ||
        !std::ranges::equal(current, std::span{expected}.last(current.size()))) {
        return std::unexpected(
            apply_error(core::ErrorCode::conflict,
                        "Target directory topology changed after the reviewed preflight",
                        reviewed.planned.source_raw_path, reviewed.planned.target_raw_path));
    }
    const auto appeared_count = expected.size() - current.size();
    for (std::size_t index = 0U; index < appeared_count; ++index) {
        if (group == nullptr || !group->created_raw_paths.contains(expected[index])) {
            return std::unexpected(apply_error(
                core::ErrorCode::conflict,
                "A reviewed missing target directory appeared outside this publication batch",
                reviewed.planned.source_raw_path, reviewed.planned.target_raw_path));
        }
    }
    return {};
}

[[nodiscard]] core::Result<OutputPathPreflight>
refresh_source_preflight(const OutputPathPreflight& reviewed, const std::size_t source_index,
                         const core::CancellationToken& cancellation) {
    auto source_plan = reviewed.plan;
    source_plan.sources = {reviewed.sources[source_index].planned};
    auto refreshed = preflight_output_paths(source_plan, cancellation);
    if (!refreshed) {
        return std::unexpected(std::move(refreshed.error()));
    }
    if (!refreshed->ready() || refreshed->sources.size() != 1U) {
        const auto blocking = std::ranges::find_if(
            refreshed->issues, [](const auto& issue) { return issue.blocking; });
        return std::unexpected(apply_error(core::ErrorCode::conflict,
                                           blocking == refreshed->issues.end()
                                               ? "Source no longer passes filesystem preflight"
                                               : blocking->message,
                                           reviewed.sources[source_index].planned.source_raw_path,
                                           reviewed.sources[source_index].planned.target_raw_path));
    }
    return refreshed;
}

} // namespace

std::size_t FilePublicationApplyResult::committed_source_count() const noexcept {
    return static_cast<std::size_t>(std::ranges::count(sources,
                                                       FilePublicationApplySourceState::committed,
                                                       &FilePublicationApplySourceResult::state));
}

std::size_t FilePublicationApplyResult::unchanged_source_count() const noexcept {
    return static_cast<std::size_t>(std::ranges::count(sources,
                                                       FilePublicationApplySourceState::unchanged,
                                                       &FilePublicationApplySourceResult::state));
}

std::size_t FilePublicationApplyResult::failed_source_count() const noexcept {
    return static_cast<std::size_t>(std::ranges::count(sources,
                                                       FilePublicationApplySourceState::failed,
                                                       &FilePublicationApplySourceResult::state));
}

std::size_t FilePublicationApplyResult::cancelled_source_count() const noexcept {
    return static_cast<std::size_t>(std::ranges::count(sources,
                                                       FilePublicationApplySourceState::cancelled,
                                                       &FilePublicationApplySourceResult::state));
}

core::Result<FilePublicationApplyResult>
apply_file_publications(const OutputPathPreflight& preflight, FilePublicationJournal& journal,
                        const FilePublicationDependentStateCommitter& dependent_state_committer,
                        const FilePublicationApplyProgressCallback& progress,
                        const core::CancellationToken& cancellation,
                        const FilePublicationApplyOptions& options) {
    if (!preflight.ready() || preflight.sources.size() != preflight.plan.sources.size() ||
        !dependent_state_committer || options.maximum_parallelism == 0U ||
        options.maximum_parallelism > maximum_apply_parallelism) {
        return std::unexpected(apply_error(
            core::ErrorCode::invalid_argument,
            "File Apply requires an entirely ready preflight, a dependent-state committer, and "
            "1–8 workers"));
    }

    FilePublicationApplyResult result;
    result.sources.reserve(preflight.sources.size());
    std::vector<std::size_t> changed_sources;
    changed_sources.reserve(preflight.sources.size());
    for (std::size_t index = 0U; index < preflight.sources.size(); ++index) {
        const auto& source = preflight.sources[index];
        const bool unchanged = source.publication == OutputPathPublicationKind::no_change;
        result.sources.push_back(FilePublicationApplySourceResult{
            .source_index = index,
            .source_raw_path = source.planned.source_raw_path,
            .target_raw_path = source.planned.target_raw_path,
            .publication = source.publication,
            .state = unchanged ? FilePublicationApplySourceState::unchanged
                               : FilePublicationApplySourceState::pending,
            .commit = std::nullopt,
            .metadata_commit = std::nullopt,
            .published_metadata = std::nullopt,
            .issue = std::nullopt,
        });
        if (!unchanged) {
            changed_sources.push_back(index);
        }
    }

    std::unordered_map<std::string, std::shared_ptr<DirectoryTopologyGroup>> groups_by_root;
    std::vector<std::shared_ptr<DirectoryTopologyGroup>> topology_groups(preflight.sources.size());
    for (const auto index : changed_sources) {
        const auto& missing = preflight.sources[index].missing_directory_raw_paths;
        if (missing.empty()) {
            continue;
        }
        auto [found, inserted] = groups_by_root.try_emplace(missing.front());
        if (inserted) {
            found->second = std::make_shared<DirectoryTopologyGroup>();
        }
        topology_groups[index] = found->second;
    }

    std::atomic_size_t next_changed_source{0U};
    std::size_t completed_sources{0U};
    std::mutex progress_mutex;
    const auto report = [&](const std::size_t index, const FilePublicationApplySourceState state,
                            const bool completes_source,
                            const std::optional<core::Error>& issue = std::nullopt) {
        std::scoped_lock lock{progress_mutex};
        if (completes_source) {
            ++completed_sources;
        }
        if (!progress) {
            return;
        }
        const auto& source = preflight.sources[index];
        const FilePublicationApplyProgress update{
            .source_index = index,
            .source_raw_path = source.planned.source_raw_path,
            .target_raw_path = source.planned.target_raw_path,
            .publication = source.publication,
            .state = state,
            .completed_sources = completed_sources,
            .total_sources = preflight.sources.size(),
            .issue = issue,
        };
        progress(update);
    };
    for (std::size_t index = 0U; index < preflight.sources.size(); ++index) {
        if (result.sources[index].state == FilePublicationApplySourceState::unchanged) {
            report(index, FilePublicationApplySourceState::unchanged, true);
        }
    }

    SynchronizedJournal synchronized_journal{journal};
    const auto worker = [&] {
        while (!cancellation.is_cancellation_requested()) {
            const auto changed_index = next_changed_source.fetch_add(1U, std::memory_order_relaxed);
            if (changed_index >= changed_sources.size()) {
                return;
            }
            const auto index = changed_sources[changed_index];
            auto& source_result = result.sources[index];
            if (cancellation.is_cancellation_requested()) {
                source_result.state = FilePublicationApplySourceState::cancelled;
                source_result.issue =
                    apply_error(core::ErrorCode::cancelled,
                                "File Apply was cancelled before this source started",
                                source_result.source_raw_path, source_result.target_raw_path);
            } else {
                source_result.state = FilePublicationApplySourceState::running;
                report(index, source_result.state, false);

                const auto& group = topology_groups[index];
                std::unique_lock<std::mutex> topology_lock;
                if (group) {
                    topology_lock = std::unique_lock{group->mutex};
                }
                auto refreshed = refresh_source_preflight(preflight, index, cancellation);
                if (refreshed) {
                    auto topology = require_reviewed_topology(
                        preflight.sources[index], refreshed->sources.front(), group.get());
                    if (!topology) {
                        refreshed = std::unexpected(std::move(topology.error()));
                    }
                }
                const bool creates_directories =
                    refreshed && !refreshed->sources.front().missing_directory_raw_paths.empty();
                if (topology_lock.owns_lock() && !creates_directories) {
                    topology_lock.unlock();
                }
                const auto remember_created_directories =
                    [&group](const std::span<const std::string> paths) {
                        if (group) {
                            group->created_raw_paths.insert(paths.begin(), paths.end());
                        }
                    };

                core::Result<FilePublicationCommitResult> committed =
                    std::unexpected(refreshed ? apply_error(core::ErrorCode::invariant,
                                                            "Refreshed publication kind is invalid",
                                                            source_result.source_raw_path,
                                                            source_result.target_raw_path)
                                              : std::move(refreshed.error()));
                if (refreshed) {
                    switch (refreshed->sources.front().publication) {
                    case OutputPathPublicationKind::same_filesystem_rename:
                        committed = commit_same_filesystem_publication(
                            *refreshed, 0U, synchronized_journal, dependent_state_committer,
                            cancellation, remember_created_directories);
                        break;
                    case OutputPathPublicationKind::cross_filesystem_copy:
                        committed = commit_cross_filesystem_publication(
                            *refreshed, 0U, synchronized_journal, dependent_state_committer,
                            cancellation, remember_created_directories);
                        break;
                    case OutputPathPublicationKind::no_change:
                        break;
                    }
                }
                if (committed) {
                    source_result.state = FilePublicationApplySourceState::committed;
                    source_result.commit = std::move(*committed);
                } else {
                    source_result.issue = std::move(committed.error());
                    source_result.state = source_result.issue->code == core::ErrorCode::cancelled
                                              ? FilePublicationApplySourceState::cancelled
                                              : FilePublicationApplySourceState::failed;
                }
            }
            report(index, source_result.state, true, source_result.issue);
        }
    };

    const auto worker_count = std::min(options.maximum_parallelism, changed_sources.size());
    std::vector<std::jthread> workers;
    workers.reserve(worker_count);
    for (std::size_t index = 0U; index < worker_count; ++index) {
        workers.emplace_back(worker);
    }
    workers.clear();

    for (const auto index : changed_sources) {
        auto& source_result = result.sources[index];
        if (source_result.state != FilePublicationApplySourceState::pending) {
            continue;
        }
        source_result.state = FilePublicationApplySourceState::cancelled;
        source_result.issue = apply_error(
            core::ErrorCode::cancelled, "File Apply was cancelled before this source started",
            source_result.source_raw_path, source_result.target_raw_path);
        report(index, source_result.state, true, source_result.issue);
    }
    result.cancellation_requested =
        cancellation.is_cancellation_requested() || result.cancelled_source_count() > 0U;
    return result;
}

core::Result<FilePublicationApplyResult> apply_preparation_publications(
    const PreparationPlan& plan, FilePublicationJournal& file_journal,
    MetadataOperationJournal& metadata_journal,
    const MetadataDependentStateCommitter& metadata_dependent_state_committer,
    const FilePublicationDependentStateCommitter& file_dependent_state_committer,
    const DestinationArtifactDependentStateCommitter& artifact_dependent_state_committer,
    const FilePublicationApplyProgressCallback& progress,
    const core::CancellationToken& cancellation, const FilePublicationApplyOptions& options) {
    if (!plan.ready() || !plan.has_path_operation() || !plan.path_preflight ||
        !metadata_dependent_state_committer || !file_dependent_state_committer ||
        !artifact_dependent_state_committer || options.maximum_parallelism == 0U ||
        options.maximum_parallelism > maximum_apply_parallelism) {
        return std::unexpected(apply_error(
            core::ErrorCode::invalid_argument,
            "Preparation Apply requires one ready path review, all dependent-state callbacks, "
            "and 1–8 workers"));
    }
    if (!plan.metadata) {
        return apply_file_publications(*plan.path_preflight, file_journal,
                                       file_dependent_state_committer, progress, cancellation,
                                       options);
    }

    const auto& preflight = *plan.path_preflight;
    std::vector<const metadata::MetadataWritePlanSource*> metadata_sources(preflight.sources.size(),
                                                                           nullptr);
    for (const auto& metadata_source : plan.metadata->sources) {
        const auto found = std::ranges::find(preflight.sources, metadata_source.raw_path,
                                             [](const auto& source) -> const std::string& {
                                                 return source.planned.source_raw_path;
                                             });
        if (found == preflight.sources.end() || !metadata_source.expected_revision ||
            !metadata_source.observed_revision ||
            *metadata_source.expected_revision != *metadata_source.observed_revision ||
            found->observed_revision != *metadata_source.observed_revision) {
            return std::unexpected(apply_error(
                core::ErrorCode::invalid_argument,
                "Preparation Apply metadata does not match its reviewed filesystem source",
                metadata_source.raw_path));
        }
        const auto index =
            static_cast<std::size_t>(std::distance(preflight.sources.begin(), found));
        if (metadata_sources[index] != nullptr) {
            return std::unexpected(
                apply_error(core::ErrorCode::invalid_argument,
                            "Preparation Apply contains duplicate metadata plans for one source",
                            metadata_source.raw_path));
        }
        metadata_sources[index] = &metadata_source;
    }

    FilePublicationApplyResult result;
    result.sources.reserve(preflight.sources.size());
    std::vector<std::size_t> work_sources;
    work_sources.reserve(preflight.sources.size());
    for (std::size_t index = 0U; index < preflight.sources.size(); ++index) {
        const auto& source = preflight.sources[index];
        const auto has_work = source.publication != OutputPathPublicationKind::no_change ||
                              metadata_sources[index] != nullptr;
        result.sources.push_back(FilePublicationApplySourceResult{
            .source_index = index,
            .source_raw_path = source.planned.source_raw_path,
            .target_raw_path = source.planned.target_raw_path,
            .publication = source.publication,
            .state = has_work ? FilePublicationApplySourceState::pending
                              : FilePublicationApplySourceState::unchanged,
            .commit = std::nullopt,
            .metadata_commit = std::nullopt,
            .published_metadata = std::nullopt,
            .issue = std::nullopt,
        });
        if (has_work) {
            work_sources.push_back(index);
        }
    }

    std::unordered_map<std::string, std::shared_ptr<DirectoryTopologyGroup>> groups_by_root;
    std::vector<std::shared_ptr<DirectoryTopologyGroup>> topology_groups(preflight.sources.size());
    for (const auto index : work_sources) {
        const auto& source = preflight.sources[index];
        if (source.publication == OutputPathPublicationKind::no_change ||
            source.missing_directory_raw_paths.empty()) {
            continue;
        }
        auto [found, inserted] =
            groups_by_root.try_emplace(source.missing_directory_raw_paths.front());
        if (inserted) {
            found->second = std::make_shared<DirectoryTopologyGroup>();
        }
        topology_groups[index] = found->second;
    }

    std::atomic_size_t next_work_source{0U};
    std::size_t completed_sources{0U};
    std::mutex progress_mutex;
    const auto report = [&](const std::size_t index, const FilePublicationApplySourceState state,
                            const bool completes_source,
                            const std::optional<core::Error>& issue = std::nullopt) {
        std::scoped_lock lock{progress_mutex};
        if (completes_source) {
            ++completed_sources;
        }
        if (progress) {
            const auto& source = preflight.sources[index];
            progress(FilePublicationApplyProgress{
                .source_index = index,
                .source_raw_path = source.planned.source_raw_path,
                .target_raw_path = source.planned.target_raw_path,
                .publication = source.publication,
                .state = state,
                .completed_sources = completed_sources,
                .total_sources = preflight.sources.size(),
                .issue = issue,
            });
        }
    };
    for (std::size_t index = 0U; index < preflight.sources.size(); ++index) {
        if (result.sources[index].state == FilePublicationApplySourceState::unchanged) {
            report(index, FilePublicationApplySourceState::unchanged, true);
        }
    }

    SynchronizedJournal synchronized_file_journal{file_journal};
    SynchronizedMetadataJournal synchronized_metadata_journal{metadata_journal};
    const auto worker = [&] {
        while (!cancellation.is_cancellation_requested()) {
            const auto work_index = next_work_source.fetch_add(1U, std::memory_order_relaxed);
            if (work_index >= work_sources.size()) {
                return;
            }
            const auto index = work_sources[work_index];
            auto& source_result = result.sources[index];
            source_result.state = FilePublicationApplySourceState::running;
            report(index, source_result.state, false);

            const auto& reviewed_source = preflight.sources[index];
            const auto* metadata_source = metadata_sources[index];
            std::optional<OutputPathPreflight> refreshed;
            core::Result<void> outcome;
            auto& group = topology_groups[index];
            std::unique_lock<std::mutex> topology_lock;
            if (group) {
                topology_lock = std::unique_lock{group->mutex};
            }
            if (reviewed_source.publication != OutputPathPublicationKind::no_change) {
                auto fresh = refresh_source_preflight(preflight, index, cancellation);
                if (!fresh) {
                    outcome = std::unexpected(std::move(fresh.error()));
                } else if (auto topology = require_reviewed_topology(
                               reviewed_source, fresh->sources.front(), group.get());
                           !topology) {
                    outcome = std::unexpected(std::move(topology.error()));
                } else {
                    refreshed = std::move(*fresh);
                }
            }
            const auto creates_directories =
                refreshed && !refreshed->sources.front().missing_directory_raw_paths.empty();
            if (topology_lock.owns_lock() && !creates_directories) {
                topology_lock.unlock();
            }
            const auto remember_created_directories =
                [&group](const std::span<const std::string> paths) {
                    if (group) {
                        group->created_raw_paths.insert(paths.begin(), paths.end());
                    }
                };

            if (outcome && metadata_source != nullptr && !refreshed) {
                auto committed =
                    commit_metadata_source(*metadata_source, synchronized_metadata_journal,
                                           metadata_dependent_state_committer, cancellation);
                if (committed) {
                    source_result.published_metadata = committed->document;
                    source_result.metadata_commit = std::move(*committed);
                } else {
                    outcome = std::unexpected(std::move(committed.error()));
                }
            } else if (outcome && metadata_source != nullptr && refreshed) {
                std::optional<metadata::MetadataDocument> published_document;
                const auto preparer = [metadata_source, &published_document](
                                          const std::string& prepared_raw_path,
                                          const core::CancellationToken& source_cancellation)
                    -> core::Result<core::LocalSourceRevision> {
                    auto prepared = metadata::prepare_qualified_metadata_write_copy(
                        *metadata_source, prepared_raw_path, source_cancellation);
                    if (!prepared) {
                        return std::unexpected(std::move(prepared.error()));
                    }
                    published_document = std::move(prepared->document);
                    return prepared->prepared_revision;
                };
                const auto dependent =
                    [&published_document, &artifact_dependent_state_committer](
                        const FilePublicationCommitResult& committed) -> core::Result<void> {
                    if (!published_document) {
                        return std::unexpected(apply_error(
                            core::ErrorCode::invariant,
                            "Destination artifact reached dependent state without verified "
                            "metadata",
                            committed.source_raw_path, committed.target_raw_path));
                    }
                    return artifact_dependent_state_committer(committed, *published_document);
                };
                auto committed = commit_destination_artifact_publication(
                    *refreshed, 0U, synchronized_file_journal, preparer, dependent, cancellation,
                    remember_created_directories);
                if (committed) {
                    source_result.commit = std::move(*committed);
                    source_result.published_metadata = std::move(published_document);
                } else {
                    outcome = std::unexpected(std::move(committed.error()));
                }
            } else if (outcome && refreshed) {
                core::Result<FilePublicationCommitResult> committed = std::unexpected(
                    apply_error(core::ErrorCode::invariant, "Refreshed publication kind is invalid",
                                source_result.source_raw_path, source_result.target_raw_path));
                switch (refreshed->sources.front().publication) {
                case OutputPathPublicationKind::same_filesystem_rename:
                    committed = commit_same_filesystem_publication(
                        *refreshed, 0U, synchronized_file_journal, file_dependent_state_committer,
                        cancellation, remember_created_directories);
                    break;
                case OutputPathPublicationKind::cross_filesystem_copy:
                    committed = commit_cross_filesystem_publication(
                        *refreshed, 0U, synchronized_file_journal, file_dependent_state_committer,
                        cancellation, remember_created_directories);
                    break;
                case OutputPathPublicationKind::no_change:
                    break;
                }
                if (committed) {
                    source_result.commit = std::move(*committed);
                } else {
                    outcome = std::unexpected(std::move(committed.error()));
                }
            }

            if (outcome) {
                source_result.state = FilePublicationApplySourceState::committed;
            } else {
                source_result.issue = std::move(outcome.error());
                source_result.state = source_result.issue->code == core::ErrorCode::cancelled
                                          ? FilePublicationApplySourceState::cancelled
                                          : FilePublicationApplySourceState::failed;
            }
            report(index, source_result.state, true, source_result.issue);
        }
    };

    const auto worker_count = std::min(options.maximum_parallelism, work_sources.size());
    std::vector<std::jthread> workers;
    workers.reserve(worker_count);
    for (std::size_t index = 0U; index < worker_count; ++index) {
        workers.emplace_back(worker);
    }
    workers.clear();

    for (const auto index : work_sources) {
        auto& source_result = result.sources[index];
        if (source_result.state != FilePublicationApplySourceState::pending) {
            continue;
        }
        source_result.state = FilePublicationApplySourceState::cancelled;
        source_result.issue =
            apply_error(core::ErrorCode::cancelled,
                        "Preparation Apply was cancelled before this source started",
                        source_result.source_raw_path, source_result.target_raw_path);
        report(index, source_result.state, true, source_result.issue);
    }
    result.cancellation_requested =
        cancellation.is_cancellation_requested() || result.cancelled_source_count() > 0U;
    return result;
}

} // namespace trackknife::operations

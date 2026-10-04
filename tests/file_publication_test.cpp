// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/local_sources.hpp"
#include "trackknife/core/posix.hpp"
#include "trackknife/core/stable_id.hpp"
#include "trackknife/operations/file_publication.hpp"
#include "trackknife/operations/output_path_preflight.hpp"
#include "trackknife/persistence/file_publication_journal.hpp"
#include "trackknife/persistence/list_repository.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

namespace core = trackknife::core;
namespace operations = trackknife::operations;
namespace persistence = trackknife::persistence;
using State = operations::FilePublicationJournalState;

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::abort();
    }
}

class TemporaryDirectory final {
  public:
    explicit TemporaryDirectory(
        const std::filesystem::path& root = std::filesystem::temp_directory_path())
        : path_{root /
                ("trackknife-file-publication-executor-" + core::StableId::random().to_string())} {
        require(std::filesystem::create_directory(path_),
                "temporary publication directory must be created");
    }
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

  private:
    std::filesystem::path path_;
};

void write_file(const std::filesystem::path& path, const std::string_view bytes) {
    std::ofstream stream{path, std::ios::binary | std::ios::trunc};
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    stream.close();
    require(stream.good(), "publication fixture must be written");
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream stream{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

operations::OutputPathPreflight
preflight_for(const std::filesystem::path& source, const std::filesystem::path& target,
              const operations::OutputPathPublicationKind expected_kind) {
    auto revision = core::observe_local_source_revision(source.native());
    require(revision.has_value(), "publication source revision must be observable");
    operations::PlannedOutputPathSource planned{
        .source_raw_path = source.native(),
        .source_revision = *revision,
        .target_raw_path = target.native(),
        .raw_relative_directory = {},
        .sanitized_relative_directory = {},
        .raw_basename = target.stem().native(),
        .sanitized_basename = target.stem().native(),
        .item_indexes = {2U, 7U},
        .sanitized = false,
        .no_change = false,
    };
    operations::OutputPathPlan plan{
        .layout = {},
        .destination = expected_kind == operations::OutputPathPublicationKind::cross_filesystem_copy
                           ? std::optional<operations::DestinationProfile>{{
                                 .schema_version = 1U,
                                 .name = "cross-filesystem test",
                                 .root_raw_path = "/dev/shm",
                                 .containment_policy = {"lexical-beneath-root", 1U},
                             }}
                           : std::nullopt,
        .operations =
            {
                .rename_files = true,
                .move_files =
                    expected_kind == operations::OutputPathPublicationKind::cross_filesystem_copy,
            },
        .sources = {planned},
        .issues = {},
    };
    auto checked = operations::preflight_output_paths(plan);
    if (!checked) {
        std::cerr << checked.error().message << '\n';
    } else if (!checked->ready()) {
        for (const auto& issue : checked->issues) {
            std::cerr << issue.message << " [" << issue.source_raw_path << " -> "
                      << issue.target_raw_path << "]\n";
        }
    }
    require(checked.has_value() && checked->ready(),
            "publication fixture must pass fresh preflight");
    require(checked->sources.front().publication == expected_kind,
            "publication fixture must have the expected filesystem topology");
    return std::move(*checked);
}

operations::OutputPathPreflight preflight(const std::filesystem::path& source,
                                          const std::filesystem::path& target) {
    return preflight_for(source, target,
                         operations::OutputPathPublicationKind::same_filesystem_rename);
}

operations::OutputPathPreflight cross_preflight(const std::filesystem::path& source,
                                                const std::filesystem::path& target) {
    return preflight_for(source, target,
                         operations::OutputPathPublicationKind::cross_filesystem_copy);
}

persistence::SqliteFilePublicationJournal open_journal(const TemporaryDirectory& directory,
                                                       const std::string_view name) {
    auto opened = persistence::SqliteFilePublicationJournal::open(directory.path() / name);
    if (!opened) {
        std::cerr << opened.error().message << '\n';
    }
    require(opened.has_value(), "file-publication journal must open");
    return std::move(*opened);
}

const operations::FilePublicationDependentStateCommitter successful_dependent_commit =
    [](const operations::FilePublicationCommitResult&) -> core::Result<void> { return {}; };

class FailingOnceTransitionJournal final : public operations::FilePublicationJournal {
  public:
    FailingOnceTransitionJournal(operations::FilePublicationJournal& journal,
                                 const State failed_state)
        : journal_{journal}, failed_state_{failed_state} {}

    core::Result<void> create(const operations::FilePublicationJournalRecord& record) override {
        return journal_.create(record);
    }

    core::Result<void>
    transition(const core::StableId& id,
               const operations::FilePublicationJournalTransition& requested) override {
        if (!failed_ && requested.state == failed_state_) {
            failed_ = true;
            return std::unexpected(core::Error{.code = core::ErrorCode::database,
                                               .message = "injected publication transition",
                                               .context = {}});
        }
        return journal_.transition(id, requested);
    }

    core::Result<std::optional<operations::FilePublicationJournalRecord>>
    load(const core::StableId& id) const override {
        return journal_.load(id);
    }

    core::Result<std::vector<operations::FilePublicationJournalRecord>>
    load_incomplete() const override {
        return journal_.load_incomplete();
    }

    core::Result<std::vector<operations::FilePublicationJournalRecord>>
    load_reversals(const core::StableId& journal_id) const override {
        return journal_.load_reversals(journal_id);
    }

    core::Result<std::optional<operations::FilePublicationBackupRecord>>
    load_backup(const core::StableId& id) const override {
        return journal_.load_backup(id);
    }

    core::Result<std::vector<operations::FilePublicationBackupRecord>>
    load_backups() const override {
        return journal_.load_backups();
    }

    core::Result<void>
    transition_backup(const core::StableId& id,
                      const operations::FilePublicationBackupTransition& transition) override {
        return journal_.transition_backup(id, transition);
    }

  private:
    operations::FilePublicationJournal& journal_;
    State failed_state_;
    bool failed_{false};
};

void commitsAtomicRenameAndCreatesOnlyPlannedDirectories() {
    TemporaryDirectory directory;
    const auto source = directory.path() / "source.flac";
    const auto target = directory.path() / "Artist" / "Album" / "track.flac";
    write_file(source, "exact audio bytes");
    const auto checked = preflight(source, target);
    require(!std::filesystem::exists(target.parent_path()),
            "preflight must not create target directories");
    auto journal = open_journal(directory, "success.sqlite3");
    std::size_t callback_count = 0U;
    const auto committed = operations::commit_same_filesystem_publication(
        checked, 0U, journal,
        [&](const operations::FilePublicationCommitResult& result) -> core::Result<void> {
            ++callback_count;
            require(result.source_raw_path == source.native() &&
                        result.target_raw_path == target.native() &&
                        result.source_revision == result.target_revision &&
                        result.occurrence_indexes == (std::vector<std::size_t>{2U, 7U}),
                    "dependent commit must receive exact publication evidence");
            require(!std::filesystem::exists(source) && read_file(target) == "exact audio bytes",
                    "dependent commit must run after physical publication");
            return {};
        });
    if (!committed) {
        std::cerr << committed.error().message << '\n';
    }
    require(committed.has_value() && callback_count == 1U,
            "same-filesystem publication must commit once");
    auto record = journal.load(committed->journal_id);
    require(record && *record && (**record).state == State::complete &&
                (**record).target_revision == committed->target_revision,
            "successful publication must retain complete target evidence");
}

void dependentFailureRestoresSourceAndLeavesHarmlessDirectories() {
    TemporaryDirectory directory;
    const auto source = directory.path() / "source.flac";
    const auto target = directory.path() / "new" / "tree" / "track.flac";
    write_file(source, "rollback bytes");
    const auto checked = preflight(source, target);
    auto journal = open_journal(directory, "dependent-failure.sqlite3");
    const auto committed = operations::commit_same_filesystem_publication(
        checked, 0U, journal,
        [](const operations::FilePublicationCommitResult&) -> core::Result<void> {
            return std::unexpected(core::Error{.code = core::ErrorCode::database,
                                               .message = "injected dependent failure",
                                               .context = {}});
        });
    require(!committed && committed.error().code == core::ErrorCode::database,
            "dependent-state failure must fail publication");
    require(read_file(source) == "rollback bytes" && !std::filesystem::exists(target),
            "dependent-state failure must restore the original path");
    require(std::filesystem::is_directory(target.parent_path()),
            "rollback must leave potentially shared empty directories intact");
    const auto incomplete = journal.load_incomplete();
    require(incomplete && incomplete->empty(),
            "rolled-back dependent failure must not replay at startup");
}

void journalFailureAfterRenameAlsoRestoresSource() {
    TemporaryDirectory directory;
    const auto source = directory.path() / "source.flac";
    const auto target = directory.path() / "target.flac";
    write_file(source, "journal rollback");
    const auto checked = preflight(source, target);
    auto durable = open_journal(directory, "journal-failure.sqlite3");
    FailingOnceTransitionJournal failing{durable, State::target_published};
    const auto committed = operations::commit_same_filesystem_publication(
        checked, 0U, failing, successful_dependent_commit);
    require(!committed && committed.error().code == core::ErrorCode::database,
            "injected publication-journal failure must surface");
    require(read_file(source) == "journal rollback" && !std::filesystem::exists(target),
            "publication-journal failure must roll the physical rename back");
    require(durable.load_incomplete()->empty(), "journal-transition rollback must become terminal");
}

void dependentJournalFailureReplaysIdempotentStateCommit() {
    TemporaryDirectory directory;
    const auto source = directory.path() / "source.flac";
    const auto target = directory.path() / "target.flac";
    write_file(source, "dependent replay");
    const auto checked = preflight(source, target);
    auto durable = open_journal(directory, "dependent-journal-failure.sqlite3");
    FailingOnceTransitionJournal failing{durable, State::dependent_state_committed};
    std::size_t callback_count = 0U;
    const auto callback =
        [&](const operations::FilePublicationCommitResult&) -> core::Result<void> {
        ++callback_count;
        return {};
    };
    const auto committed =
        operations::commit_same_filesystem_publication(checked, 0U, failing, callback);
    require(!committed && committed.error().code == core::ErrorCode::database &&
                callback_count == 1U && !std::filesystem::exists(source) &&
                read_file(target) == "dependent replay",
            "journal failure after state commit must retain the published target");
    const auto incomplete = durable.load_incomplete();
    require(incomplete && incomplete->size() == 1U &&
                incomplete->front().state == State::target_published,
            "failed dependent-state transition must remain replayable");
    const auto recovered = operations::recover_same_filesystem_publications(durable, callback);
    require(recovered && recovered->size() == 1U &&
                recovered->front().outcome ==
                    operations::FilePublicationRecoveryOutcome::completed &&
                callback_count == 2U,
            "recovery must replay an idempotent dependent-state commit and complete");
}

void changedSourceOrAppearedTargetFailsBeforeJournalCreation() {
    TemporaryDirectory directory;
    const auto changed_source = directory.path() / "changed.flac";
    const auto changed_target = directory.path() / "changed-target.flac";
    write_file(changed_source, "before");
    const auto changed = preflight(changed_source, changed_target);
    write_file(changed_source, "different size after preflight");
    auto changed_journal = open_journal(directory, "changed.sqlite3");
    const auto changed_result = operations::commit_same_filesystem_publication(
        changed, 0U, changed_journal, successful_dependent_commit);
    require(!changed_result && changed_result.error().code == core::ErrorCode::conflict &&
                changed_journal.load_incomplete()->empty(),
            "changed source must fail before journal creation");

    const auto occupied_source = directory.path() / "occupied.flac";
    const auto occupied_target = directory.path() / "occupied-target.flac";
    write_file(occupied_source, "source");
    const auto occupied = preflight(occupied_source, occupied_target);
    write_file(occupied_target, "intruder");
    auto occupied_journal = open_journal(directory, "occupied.sqlite3");
    const auto occupied_result = operations::commit_same_filesystem_publication(
        occupied, 0U, occupied_journal, successful_dependent_commit);
    require(!occupied_result && occupied_result.error().code == core::ErrorCode::conflict &&
                occupied_journal.load_incomplete()->empty() &&
                read_file(occupied_source) == "source" && read_file(occupied_target) == "intruder",
            "appeared target must fail before journal creation without replacement");
}

void recoversBothSidesOfTheAtomicJournalBoundary() {
    TemporaryDirectory directory;
    const auto unstarted_source = directory.path() / "unstarted.flac";
    const auto unstarted_target = directory.path() / "unstarted-target.flac";
    write_file(unstarted_source, "unstarted");
    const auto unstarted_preflight = preflight(unstarted_source, unstarted_target);
    auto unstarted_journal = open_journal(directory, "unstarted.sqlite3");
    auto unstarted_record = operations::make_file_publication_journal_record(
        unstarted_preflight, 0U, core::StableId::random());
    require(unstarted_record && unstarted_journal.create(*unstarted_record),
            "unstarted recovery record must be durable");
    const auto unstarted = operations::recover_same_filesystem_publications(
        unstarted_journal, successful_dependent_commit);
    require(
        unstarted && unstarted->size() == 1U &&
            unstarted->front().outcome == operations::FilePublicationRecoveryOutcome::rolled_back &&
            std::filesystem::exists(unstarted_source) && !std::filesystem::exists(unstarted_target),
        "planned journal with original source must recover as rolled back");

    const auto published_source = directory.path() / "published.flac";
    const auto published_target = directory.path() / "published-target.flac";
    write_file(published_source, "published");
    const auto published_preflight = preflight(published_source, published_target);
    auto published_journal = open_journal(directory, "published.sqlite3");
    auto published_record = operations::make_file_publication_journal_record(
        published_preflight, 0U, core::StableId::random());
    require(published_record && published_journal.create(*published_record),
            "interrupted publication record must be durable");
    std::filesystem::rename(published_source, published_target);
    require(!std::filesystem::exists(published_source) && std::filesystem::exists(published_target),
            "fixture must interrupt after atomic rename");
    std::size_t callback_count = 0U;
    const auto recovered = operations::recover_same_filesystem_publications(
        published_journal,
        [&](const operations::FilePublicationCommitResult& result) -> core::Result<void> {
            ++callback_count;
            require(result.journal_id == published_record->id &&
                        result.target_raw_path == published_target.native(),
                    "recovery must replay exact dependent-state evidence");
            return {};
        });
    if (!recovered) {
        std::cerr << recovered.error().message << '\n';
    }
    require(recovered && recovered->size() == 1U && callback_count == 1U &&
                recovered->front().outcome ==
                    operations::FilePublicationRecoveryOutcome::completed &&
                !std::filesystem::exists(published_source) &&
                read_file(published_target) == "published",
            "rename before its journal transition must recover to completion");
    const auto durable = published_journal.load(published_record->id);
    require(durable && *durable && (**durable).state == State::complete,
            "recovered rename must durably complete");
}

void recoveryRollbackAndPostDependentCompletionAreExact() {
    TemporaryDirectory directory;
    const auto failed_source = directory.path() / "failed.flac";
    const auto failed_target = directory.path() / "failed-target.flac";
    write_file(failed_source, "recovery rollback");
    const auto failed_preflight = preflight(failed_source, failed_target);
    auto failed_journal = open_journal(directory, "recovery-rollback.sqlite3");
    auto failed_record = operations::make_file_publication_journal_record(failed_preflight, 0U,
                                                                          core::StableId::random());
    require(failed_record && failed_journal.create(*failed_record),
            "recovery rollback record must be durable");
    std::filesystem::rename(failed_source, failed_target);
    const auto failed = operations::recover_same_filesystem_publications(
        failed_journal, [](const operations::FilePublicationCommitResult&) -> core::Result<void> {
            return std::unexpected(core::Error{.code = core::ErrorCode::database,
                                               .message = "recovery dependent failure",
                                               .context = {}});
        });
    require(failed && failed->size() == 1U &&
                failed->front().outcome ==
                    operations::FilePublicationRecoveryOutcome::rolled_back &&
                read_file(failed_source) == "recovery rollback" &&
                !std::filesystem::exists(failed_target),
            "recovery callback failure must safely restore the original path");

    const auto committed_source = directory.path() / "committed.flac";
    const auto committed_target = directory.path() / "committed-target.flac";
    write_file(committed_source, "already dependent");
    const auto committed_preflight = preflight(committed_source, committed_target);
    auto committed_journal = open_journal(directory, "already-dependent.sqlite3");
    auto committed_record = operations::make_file_publication_journal_record(
        committed_preflight, 0U, core::StableId::random());
    require(committed_record && committed_journal.create(*committed_record),
            "post-dependent recovery record must be durable");
    std::filesystem::rename(committed_source, committed_target);
    require(
        committed_journal.transition(committed_record->id,
                                     {.expected_state = State::planned,
                                      .state = State::target_published,
                                      .prepared_revision = std::nullopt,
                                      .target_revision = committed_record->expected_source_revision,
                                      .failure = std::nullopt}) &&
            committed_journal.transition(
                committed_record->id,
                {.expected_state = State::target_published,
                 .state = State::dependent_state_committed,
                 .prepared_revision = std::nullopt,
                 .target_revision = committed_record->expected_source_revision,
                 .failure = std::nullopt}),
        "post-dependent fixture must reach its durable boundary");
    std::size_t callback_count = 0U;
    const auto completed = operations::recover_same_filesystem_publications(
        committed_journal,
        [&](const operations::FilePublicationCommitResult&) -> core::Result<void> {
            ++callback_count;
            return {};
        });
    require(completed && completed->size() == 1U && callback_count == 0U &&
                completed->front().outcome == operations::FilePublicationRecoveryOutcome::completed,
            "durable dependent-state boundary must complete without replaying its callback");
}

void ambiguousRecoveryNeverDeletesEitherPath() {
    TemporaryDirectory directory;
    const auto source = directory.path() / "source.flac";
    const auto target = directory.path() / "target.flac";
    write_file(source, "source bytes");
    const auto checked = preflight(source, target);
    auto journal = open_journal(directory, "ambiguous.sqlite3");
    auto record =
        operations::make_file_publication_journal_record(checked, 0U, core::StableId::random());
    require(record && journal.create(*record), "ambiguous record must be durable");
    write_file(target, "unrelated target");
    const auto recovered =
        operations::recover_same_filesystem_publications(journal, successful_dependent_commit);
    require(recovered && recovered->size() == 1U &&
                recovered->front().outcome ==
                    operations::FilePublicationRecoveryOutcome::needs_reconciliation &&
                read_file(source) == "source bytes" && read_file(target) == "unrelated target",
            "ambiguous recovery must preserve both filesystem entries");
    const auto durable = journal.load(record->id);
    require(durable && *durable && (**durable).state == State::needs_reconciliation &&
                (**durable).failure,
            "ambiguous recovery must remain visible with failure evidence");
}

void realPublicationCommitsTheAllOccurrenceRelocationTransaction() {
    TemporaryDirectory directory;
    const auto source = directory.path() / "source.flac";
    const auto target = directory.path() / "nested" / "target.flac";
    write_file(source, "physical relocation bytes");
    const auto checked = preflight(source, target);
    const auto source_revision = checked.sources.front().observed_revision;
    const auto state_path = directory.path() / "state.sqlite3";
    auto repository_result = persistence::ListRepository::open(state_path);
    require(repository_result.has_value(), "relocation integration repository must open");
    auto repository = std::move(*repository_result);
    const std::vector documents{
        persistence::ListDocument{
            .id = core::StableId::random(),
            .kind = persistence::ListKind::scratch,
            .name = "First",
            .pinned = false,
            .dirty = false,
            .items = {persistence::ListItem{
                .source = persistence::ListSource::local,
                .profile_id = std::nullopt,
                .source_reference = source.native(),
                .logical_reference = std::nullopt,
                .segment = std::nullopt,
                .source_selection = std::nullopt,
                .duration_ms = std::nullopt,
                .source_revision = source_revision,
                .fields = {{"title", "First"}},
            }},
        },
        persistence::ListDocument{
            .id = core::StableId::random(),
            .kind = persistence::ListKind::saved,
            .name = "Duplicate",
            .pinned = true,
            .dirty = false,
            .items = {persistence::ListItem{
                .source = persistence::ListSource::local,
                .profile_id = std::nullopt,
                .source_reference = source.native(),
                .logical_reference = std::string{"cue-v1\0track", 12U},
                .segment = persistence::ListItemSegment{.start_sample = 0, .end_sample = 1},
                .source_selection = std::nullopt,
                .duration_ms = std::nullopt,
                .source_revision = source_revision,
                .fields = {{"title", "Logical"}},
            }},
        },
    };
    require(repository.replace_all(documents).has_value(),
            "relocation integration occurrences must persist");
    auto journal = open_journal(directory, "state.sqlite3");
    const auto committed = operations::commit_same_filesystem_publication(
        checked, 0U, journal,
        [&](const operations::FilePublicationCommitResult& result) -> core::Result<void> {
            auto relocated = repository.relocate_local_source(persistence::LocalSourceRelocation{
                .operation_id = result.journal_id,
                .source_reference = result.source_raw_path,
                .target_reference = result.target_raw_path,
                .previous_revision = result.source_revision,
                .published_revision = result.target_revision,
                .published_document = std::nullopt,
            });
            return relocated ? core::Result<void>{} : std::unexpected(std::move(relocated.error()));
        });
    require(committed.has_value() && !std::filesystem::exists(source) &&
                read_file(target) == "physical relocation bytes",
            "physical publication and dependent state must complete together");
    auto loaded = repository.load_all();
    require(loaded && (*loaded)[0].items[0].source_reference == target.native() &&
                (*loaded)[1].items[0].source_reference == target.native() &&
                (*loaded)[0].items[0].source_revision == committed->target_revision,
            "the real executor callback must re-key every persisted occurrence");
    require(repository.replace_all(documents).has_value(),
            "a pre-publication workspace snapshot may arrive after commit");
    loaded = repository.load_all();
    require(loaded && (*loaded)[0].items[0].source_reference == target.native() &&
                (*loaded)[1].items[0].source_reference == target.native(),
            "publication relocation history must suppress stale source paths");

    const auto undone = operations::undo_same_filesystem_publication(
        committed->journal_id, journal,
        [&](const operations::FilePublicationCommitResult& result) -> core::Result<void> {
            auto relocated = repository.relocate_local_source(persistence::LocalSourceRelocation{
                .operation_id = result.journal_id,
                .source_reference = result.source_raw_path,
                .target_reference = result.target_raw_path,
                .previous_revision = result.source_revision,
                .published_revision = result.target_revision,
                .published_document = std::nullopt,
            });
            return relocated ? core::Result<void>{} : std::unexpected(std::move(relocated.error()));
        });
    require(undone && read_file(source) == "physical relocation bytes" &&
                !std::filesystem::exists(target),
            "real dependent-state relocation must also commit the reverse publication");
    loaded = repository.load_all();
    require(loaded && (*loaded)[0].items[0].source_reference == source.native() &&
                (*loaded)[1].items[0].source_reference == source.native(),
            "undo must atomically return every persisted occurrence to the original path");
    require(repository.replace_all(documents).has_value(),
            "a snapshot from before publication may be saved after undo");
    loaded = repository.load_all();
    require(loaded && (*loaded)[0].items[0].source_reference == source.native() &&
                (*loaded)[1].items[0].source_reference == source.native(),
            "ordered relocation history must converge through publication and undo");
}

void completedSameFilesystemPublicationCanBeUndoneIdempotently() {
    TemporaryDirectory directory;
    const auto source = directory.path() / "source.flac";
    const auto target = directory.path() / "new" / "target.flac";
    write_file(source, "undo bytes");
    const auto checked = preflight(source, target);
    auto journal = open_journal(directory, "undo.sqlite3");
    const auto committed = operations::commit_same_filesystem_publication(
        checked, 0U, journal, successful_dependent_commit);
    require(committed.has_value() && !std::filesystem::exists(source) &&
                read_file(target) == "undo bytes",
            "undo fixture must begin with a completed publication");

    std::size_t callback_count = 0U;
    const auto undone = operations::undo_same_filesystem_publication(
        committed->journal_id, journal,
        [&](const operations::FilePublicationCommitResult& result) -> core::Result<void> {
            ++callback_count;
            require(result.journal_id != committed->journal_id &&
                        result.source_raw_path == target.native() &&
                        result.target_raw_path == source.native() &&
                        result.source_revision == committed->target_revision &&
                        result.target_revision == committed->source_revision,
                    "undo dependent state must receive the exact reverse publication");
            require(read_file(source) == "undo bytes" && !std::filesystem::exists(target),
                    "undo dependent state must run after reverse publication");
            return {};
        });
    require(undone.has_value() && callback_count == 1U && read_file(source) == "undo bytes" &&
                !std::filesystem::exists(target),
            "completed publication must reverse through a second journaled rename");
    const auto reversals = journal.load_reversals(committed->journal_id);
    require(reversals && reversals->size() == 1U && reversals->front().id == undone->journal_id &&
                reversals->front().reverses_journal_id == committed->journal_id &&
                reversals->front().state == State::complete,
            "completed undo must retain its relation and terminal evidence");

    const auto replayed = operations::undo_same_filesystem_publication(
        committed->journal_id, journal,
        [&](const operations::FilePublicationCommitResult&) -> core::Result<void> {
            ++callback_count;
            return {};
        });
    require(replayed == undone && callback_count == 1U,
            "repeated undo must return the completed reverse without replaying dependent state");
}

void failedUndoRestoresPublishedTargetAndCanBeRetried() {
    TemporaryDirectory directory;
    const auto source = directory.path() / "source.flac";
    const auto target = directory.path() / "target.flac";
    write_file(source, "retry undo");
    const auto checked = preflight(source, target);
    auto journal = open_journal(directory, "undo-retry.sqlite3");
    const auto committed = operations::commit_same_filesystem_publication(
        checked, 0U, journal, successful_dependent_commit);
    require(committed.has_value(), "undo retry fixture must publish");
    const auto failed = operations::undo_same_filesystem_publication(
        committed->journal_id, journal,
        [](const operations::FilePublicationCommitResult&) -> core::Result<void> {
            return std::unexpected(core::Error{.code = core::ErrorCode::database,
                                               .message = "injected undo dependent failure",
                                               .context = {}});
        });
    require(!failed && failed.error().code == core::ErrorCode::database &&
                !std::filesystem::exists(source) && read_file(target) == "retry undo",
            "undo dependent failure must roll the reverse rename back to the published target");
    auto reversals = journal.load_reversals(committed->journal_id);
    require(reversals && reversals->size() == 1U && reversals->front().state == State::rolled_back,
            "failed undo must retain a terminal retryable attempt");
    const auto retried = operations::undo_same_filesystem_publication(
        committed->journal_id, journal, successful_dependent_commit);
    reversals = journal.load_reversals(committed->journal_id);
    require(retried && reversals && reversals->size() == 2U &&
                reversals->back().id == retried->journal_id &&
                reversals->back().state == State::complete && read_file(source) == "retry undo" &&
                !std::filesystem::exists(target),
            "a rolled-back undo attempt must permit a fresh successful reversal");
}

void undoJournalBoundaryRecoversByReplayingDependentState() {
    TemporaryDirectory directory;
    const auto source = directory.path() / "source.flac";
    const auto target = directory.path() / "target.flac";
    write_file(source, "recover undo");
    const auto checked = preflight(source, target);
    auto durable = open_journal(directory, "undo-recovery.sqlite3");
    const auto committed = operations::commit_same_filesystem_publication(
        checked, 0U, durable, successful_dependent_commit);
    require(committed.has_value(), "undo recovery fixture must publish");
    FailingOnceTransitionJournal failing{durable, State::dependent_state_committed};
    std::size_t callback_count = 0U;
    std::optional<core::StableId> undo_id;
    const auto interrupted = operations::undo_same_filesystem_publication(
        committed->journal_id, failing,
        [&](const operations::FilePublicationCommitResult& result) -> core::Result<void> {
            ++callback_count;
            undo_id = result.journal_id;
            return {};
        });
    require(!interrupted && callback_count == 1U && undo_id &&
                read_file(source) == "recover undo" && !std::filesystem::exists(target),
            "a post-dependent undo journal failure must retain the restored source path");
    const auto incomplete = durable.load(*undo_id);
    require(incomplete && *incomplete && (**incomplete).state == State::target_published,
            "interrupted undo must remain at the replayable target-published boundary");
    const auto recovered = operations::recover_same_filesystem_publications(
        durable, [&](const operations::FilePublicationCommitResult& result) -> core::Result<void> {
            ++callback_count;
            require(result.journal_id == *undo_id,
                    "startup recovery must replay the reverse operation identity");
            return {};
        });
    require(recovered && recovered->size() == 1U && callback_count == 2U &&
                recovered->front().journal_id == *undo_id &&
                recovered->front().outcome ==
                    operations::FilePublicationRecoveryOutcome::completed &&
                read_file(source) == "recover undo" && !std::filesystem::exists(target),
            "startup recovery must complete an interrupted undo without moving the file again");
}

void changedUndoTopologyCreatesNoReverseJournal() {
    TemporaryDirectory directory;
    const auto source = directory.path() / "source.flac";
    const auto target = directory.path() / "target.flac";
    write_file(source, "original");
    const auto checked = preflight(source, target);
    auto journal = open_journal(directory, "undo-conflict.sqlite3");
    const auto committed = operations::commit_same_filesystem_publication(
        checked, 0U, journal, successful_dependent_commit);
    require(committed.has_value(), "undo conflict fixture must publish");
    write_file(source, "unrelated replacement");
    const auto rejected = operations::undo_same_filesystem_publication(
        committed->journal_id, journal, successful_dependent_commit);
    require(!rejected && rejected.error().code == core::ErrorCode::conflict &&
                read_file(source) == "unrelated replacement" && read_file(target) == "original" &&
                journal.load_reversals(committed->journal_id)->empty(),
            "an occupied original path must block undo without journal or filesystem mutation");
}

void crossFilesystemCopyPublishesExactBytesBeforeRemovingSource() {
    TemporaryDirectory source_directory;
    TemporaryDirectory target_directory{"/dev/shm"};
    const auto source = source_directory.path() / "source.flac";
    const auto target = target_directory.path() / "Artist" / "Album" / "target.flac";
    std::string bytes((1024U * 1024U) + 37U, 'a');
    bytes.replace(1024U * 1024U, 17U, "cross-device-tail");
    write_file(source, bytes);
    require(::chmod(source.c_str(), 0640) == 0, "copy source permissions must be set");
    constexpr std::string_view attribute_name{"user.trackknife-copy-test"};
    constexpr std::string_view attribute_value{"preserved-cross-filesystem-xattr"};
    const bool xattrs_supported =
        core::set_path_extended_attribute(source.c_str(), attribute_name.data(),
                                          attribute_value.data(), attribute_value.size()) == 0;
    struct stat source_status{};
    require(::stat(source.c_str(), &source_status) == 0,
            "copy source filesystem metadata must be observed");
    const auto checked = cross_preflight(source, target);
    auto journal = open_journal(source_directory, "cross-success.sqlite3");
    std::size_t callback_count = 0U;
    const auto committed = operations::commit_cross_filesystem_publication(
        checked, 0U, journal,
        [&](const operations::FilePublicationCommitResult& result) -> core::Result<void> {
            ++callback_count;
            require(std::filesystem::exists(source) && read_file(target) == bytes,
                    "dependent state must run after target publication and before source removal");
            require(result.source_revision.device != result.target_revision.device &&
                        result.occurrence_indexes == (std::vector<std::size_t>{2U, 7U}),
                    "cross-device callback must receive both exact revision identities");
            return {};
        });
    if (!committed) {
        std::cerr << committed.error().message << '\n';
    }
    require(committed.has_value() && callback_count == 1U && !std::filesystem::exists(source) &&
                read_file(target) == bytes,
            "verified cross-filesystem publication must leave only the exact target");
    struct stat target_status{};
    require(::stat(target.c_str(), &target_status) == 0 && (target_status.st_mode & 0777) == 0640,
            "verified copy must preserve source permissions");
    // Filesystems with server-side identity mapping (NFS) refuse ownership
    // changes; preservation is best-effort there (ADR-0111), so assert it
    // only where the filesystem can express it.
    const auto chown_supported =
        ::chown(target.c_str(), source_status.st_uid, source_status.st_gid) == 0;
    if (chown_supported) {
        require(target_status.st_uid == source_status.st_uid &&
                    target_status.st_gid == source_status.st_gid,
                "verified copy must preserve source ownership where supported");
    }
    if (xattrs_supported) {
        std::string value(attribute_value.size(), '\0');
        const auto read = core::get_path_extended_attribute(target.c_str(), attribute_name.data(),
                                                            value.data(), value.size());
        require(read == static_cast<ssize_t>(attribute_value.size()) && value == attribute_value,
                "verified copy must preserve source extended attributes");
    }
    const auto record = journal.load(committed->journal_id);
    require(record && *record && (**record).state == State::complete &&
                !std::filesystem::exists((**record).prepared_raw_path),
            "successful copy must retain complete evidence and no prepared sibling");
}

void crossFilesystemDependentFailureRemovesOnlyThePublishedCopy() {
    TemporaryDirectory source_directory;
    TemporaryDirectory target_directory{"/dev/shm"};
    const auto source = source_directory.path() / "source.flac";
    const auto target = target_directory.path() / "target.flac";
    write_file(source, "cross rollback bytes");
    const auto checked = cross_preflight(source, target);
    auto journal = open_journal(source_directory, "cross-rollback.sqlite3");
    const auto committed = operations::commit_cross_filesystem_publication(
        checked, 0U, journal,
        [](const operations::FilePublicationCommitResult&) -> core::Result<void> {
            return std::unexpected(core::Error{.code = core::ErrorCode::database,
                                               .message = "injected cross dependent failure",
                                               .context = {}});
        });
    require(!committed && committed.error().code == core::ErrorCode::database &&
                read_file(source) == "cross rollback bytes" && !std::filesystem::exists(target) &&
                journal.load_incomplete()->empty(),
            "dependent failure must preserve the original and remove only its published copy");
}

void crossFilesystemRecoveryReplaysDependentStateThenRemovesSource() {
    TemporaryDirectory source_directory;
    TemporaryDirectory target_directory{"/dev/shm"};
    const auto source = source_directory.path() / "source.flac";
    const auto target = target_directory.path() / "target.flac";
    write_file(source, "cross dependent replay");
    const auto checked = cross_preflight(source, target);
    auto durable = open_journal(source_directory, "cross-dependent-replay.sqlite3");
    FailingOnceTransitionJournal failing{durable, State::dependent_state_committed};
    std::size_t callback_count = 0U;
    const auto callback =
        [&](const operations::FilePublicationCommitResult&) -> core::Result<void> {
        ++callback_count;
        return {};
    };
    const auto interrupted =
        operations::commit_cross_filesystem_publication(checked, 0U, failing, callback);
    require(!interrupted && interrupted.error().code == core::ErrorCode::database &&
                callback_count == 1U && std::filesystem::exists(source) &&
                read_file(target) == "cross dependent replay",
            "post-dependent journal failure must retain both copies at its safe boundary");
    const auto incomplete = durable.load_incomplete();
    require(incomplete && incomplete->size() == 1U &&
                incomplete->front().state == State::target_published,
            "interrupted cross publication must remain replayable");
    const auto recovered = operations::recover_cross_filesystem_publications(durable, callback);
    require(recovered && recovered->size() == 1U &&
                recovered->front().outcome ==
                    operations::FilePublicationRecoveryOutcome::completed &&
                callback_count == 2U && !std::filesystem::exists(source) &&
                read_file(target) == "cross dependent replay",
            "recovery must replay dependent state before removing the source");
}

void crossFilesystemRecoveryAdoptsOnlyAnExactUnrecordedPreparedCopy() {
    TemporaryDirectory source_directory;
    TemporaryDirectory target_directory{"/dev/shm"};
    const auto source = source_directory.path() / "source.flac";
    const auto target = target_directory.path() / "target.flac";
    write_file(source, "exact unrecorded prepared copy");
    const auto checked = cross_preflight(source, target);
    auto journal = open_journal(source_directory, "cross-adopt-prepared.sqlite3");
    auto record =
        operations::make_file_publication_journal_record(checked, 0U, core::StableId::random());
    require(record && journal.create(*record),
            "planned prepared-copy recovery record must be durable");
    std::filesystem::copy_file(source, record->prepared_raw_path);
    std::filesystem::permissions(record->prepared_raw_path,
                                 std::filesystem::status(source).permissions());
    std::filesystem::last_write_time(record->prepared_raw_path,
                                     std::filesystem::last_write_time(source));
    std::size_t callback_count = 0U;
    const auto recovered = operations::recover_cross_filesystem_publications(
        journal, [&](const operations::FilePublicationCommitResult&) -> core::Result<void> {
            ++callback_count;
            return {};
        });
    require(recovered && recovered->size() == 1U &&
                recovered->front().outcome ==
                    operations::FilePublicationRecoveryOutcome::completed &&
                callback_count == 1U && !std::filesystem::exists(source) &&
                !std::filesystem::exists(record->prepared_raw_path) &&
                read_file(target) == "exact unrecorded prepared copy",
            "recovery must verify, adopt, publish, and complete an exact prepared sibling");

    const auto ambiguous_source = source_directory.path() / "ambiguous-source.flac";
    const auto ambiguous_target = target_directory.path() / "ambiguous-target.flac";
    write_file(ambiguous_source, "original bytes");
    const auto ambiguous_checked = cross_preflight(ambiguous_source, ambiguous_target);
    auto ambiguous_journal = open_journal(source_directory, "cross-ambiguous-prepared.sqlite3");
    auto ambiguous_record = operations::make_file_publication_journal_record(
        ambiguous_checked, 0U, core::StableId::random());
    require(ambiguous_record && ambiguous_journal.create(*ambiguous_record),
            "ambiguous prepared-copy recovery record must be durable");
    write_file(ambiguous_record->prepared_raw_path, "different bytes");
    const auto ambiguous = operations::recover_cross_filesystem_publications(
        ambiguous_journal, successful_dependent_commit);
    require(ambiguous && ambiguous->size() == 1U &&
                ambiguous->front().outcome ==
                    operations::FilePublicationRecoveryOutcome::needs_reconciliation &&
                read_file(ambiguous_source) == "original bytes" &&
                read_file(ambiguous_record->prepared_raw_path) == "different bytes" &&
                !std::filesystem::exists(ambiguous_target),
            "recovery must retain a differing unrecorded sibling for reconciliation");
}

void crossFilesystemRecoveryInfersPublishedAndRemovedBoundaries() {
    TemporaryDirectory source_directory;
    TemporaryDirectory target_directory{"/dev/shm"};
    const auto source = source_directory.path() / "source.flac";
    const auto target = target_directory.path() / "target.flac";
    write_file(source, "published boundary");
    const auto checked = cross_preflight(source, target);
    auto published_journal = open_journal(source_directory, "cross-published-boundary.sqlite3");
    auto record =
        operations::make_file_publication_journal_record(checked, 0U, core::StableId::random());
    require(record && published_journal.create(*record),
            "cross publication boundary record must be durable");
    std::filesystem::copy_file(source, record->prepared_raw_path);
    std::filesystem::permissions(record->prepared_raw_path,
                                 std::filesystem::status(source).permissions());
    std::filesystem::last_write_time(record->prepared_raw_path,
                                     std::filesystem::last_write_time(source));
    const auto prepared_revision = core::observe_local_source_revision(record->prepared_raw_path);
    require(prepared_revision &&
                published_journal.transition(record->id, {.expected_state = State::planned,
                                                          .state = State::target_prepared,
                                                          .prepared_revision = *prepared_revision,
                                                          .target_revision = std::nullopt,
                                                          .failure = std::nullopt}),
            "prepared target evidence must be durable before publication");
    std::filesystem::rename(record->prepared_raw_path, target);
    std::size_t callback_count = 0U;
    const auto recovered_published = operations::recover_cross_filesystem_publications(
        published_journal,
        [&](const operations::FilePublicationCommitResult&) -> core::Result<void> {
            ++callback_count;
            return {};
        });
    require(recovered_published && recovered_published->size() == 1U &&
                recovered_published->front().outcome ==
                    operations::FilePublicationRecoveryOutcome::completed &&
                callback_count == 1U && !std::filesystem::exists(source) &&
                read_file(target) == "published boundary",
            "recovery must infer a sibling rename completed before its journal transition");

    const auto removed_source = source_directory.path() / "removed-source.flac";
    const auto removed_target = target_directory.path() / "removed-target.flac";
    write_file(removed_source, "removed boundary");
    const auto removed_checked = cross_preflight(removed_source, removed_target);
    auto removed_journal = open_journal(source_directory, "cross-removed-boundary.sqlite3");
    FailingOnceTransitionJournal failing{removed_journal, State::source_removed};
    callback_count = 0U;
    const auto interrupted = operations::commit_cross_filesystem_publication(
        removed_checked, 0U, failing,
        [&](const operations::FilePublicationCommitResult&) -> core::Result<void> {
            ++callback_count;
            return {};
        });
    require(!interrupted && interrupted.error().code == core::ErrorCode::database &&
                callback_count == 1U && !std::filesystem::exists(removed_source) &&
                read_file(removed_target) == "removed boundary",
            "source-removal journal failure must retain the completed physical topology");
    const auto recovered_removed = operations::recover_cross_filesystem_publications(
        removed_journal, [&](const operations::FilePublicationCommitResult&) -> core::Result<void> {
            ++callback_count;
            return {};
        });
    require(recovered_removed && recovered_removed->size() == 1U &&
                recovered_removed->front().outcome ==
                    operations::FilePublicationRecoveryOutcome::completed &&
                callback_count == 1U && !std::filesystem::exists(removed_source) &&
                read_file(removed_target) == "removed boundary",
            "recovery must infer an already removed source without replaying dependent state");
}

// ADR-0263: a publication that writes a new file keeps the source it
// replaces; undo restores it, recovery finishes an interrupted undo, and
// retention releases it.
void publicationsKeepTheirSourceForUndo() {
    using Backup = operations::MetadataOperationBackupState;
    TemporaryDirectory directory;
    auto journal = open_journal(directory, "kept.sqlite3");
    const auto artifact = [&](const std::string& name) {
        const auto source = directory.path() / (name + ".flac");
        const auto target = directory.path() / ("Artist " + name) / "changed.flac";
        write_file(source, "original " + name);
        const auto checked = preflight(source, target);
        const auto committed = operations::commit_destination_artifact_publication(
            checked, 0U, journal,
            [&](const std::string& prepared, const core::CancellationToken&)
                -> core::Result<core::LocalSourceRevision> {
                write_file(prepared, "changed " + name);
                return core::observe_local_source_revision(prepared);
            },
            successful_dependent_commit);
        require(committed.has_value(), "a changed-content publication must commit");
        return std::tuple{source, target, committed->journal_id};
    };
    const auto kept_path = [](const std::filesystem::path& source, const core::StableId& id) {
        return operations::file_publication_retained_path(source, id);
    };
    const auto state_of = [&journal](const core::StableId& id) {
        const auto backup = journal.load_backup(id);
        require(backup && *backup, "a publication that kept its source must say so");
        return (**backup).state;
    };

    // A: kept, byte for byte; undone, the target and its folder gone.
    const auto [source, target, id] = artifact("a");
    require(!std::filesystem::exists(source) && read_file(kept_path(source, id)) == "original a" &&
                read_file(target) == "changed a" && state_of(id) == Backup::retained,
            "the replaced source must be kept beside itself");
    std::size_t followed = 0U;
    const auto undone = operations::undo_file_publication(
        id, journal, [&](const operations::FilePublicationCommitResult& result) -> core::Result<void> {
            ++followed;
            require(result.source_raw_path == target.native() &&
                        result.target_raw_path == source.native(),
                    "undo must follow the file from the target back to the source");
            return {};
        });
    require(undone.has_value() && followed == 1U && read_file(source) == "original a" &&
                !std::filesystem::exists(target) && !std::filesystem::exists(target.parent_path()) &&
                !std::filesystem::exists(kept_path(source, id)) && state_of(id) == Backup::undone,
            "undo must restore the source and remove the target and the folder it made");

    // B: the target changed since: refused, nothing moved.
    const auto [changed_source, changed_target, changed_id] = artifact("b");
    write_file(changed_target, "edited afterwards");
    const auto refused =
        operations::undo_file_publication(changed_id, journal, successful_dependent_commit);
    require(!refused && read_file(changed_target) == "edited afterwards" &&
                !std::filesystem::exists(changed_source) &&
                read_file(kept_path(changed_source, changed_id)) == "original b" &&
                state_of(changed_id) == Backup::needs_reconciliation,
            "undo must refuse, touching nothing, when the target changed");

    // C: interrupted after the source came back: recovery finishes it.
    const auto [crash_source, crash_target, crash_id] = artifact("c");
    require(journal
                .transition_backup(crash_id, operations::FilePublicationBackupTransition{
                                                 .expected_state = Backup::retained,
                                                 .state = Backup::undoing,
                                                 .undo_id = core::StableId::random(),
                                                 .failure = std::nullopt})
                .has_value(),
            "an undo must be able to begin");
    std::filesystem::rename(kept_path(crash_source, crash_id), crash_source);
    const auto recovered =
        operations::recover_publication_undos(journal, successful_dependent_commit);
    require(recovered && recovered->size() == 1U &&
                recovered->front().outcome == operations::FilePublicationRecoveryOutcome::completed &&
                read_file(crash_source) == "original c" && !std::filesystem::exists(crash_target) &&
                state_of(crash_id) == Backup::undone,
            "recovery must finish an interrupted undo");

    // D: retention releases what it does not keep.
    const auto [old_source, old_target, old_id] = artifact("d");
    const auto maintained = operations::maintain_publication_backups(
        journal,
        operations::MetadataBackupRetentionPolicy{
            .maximum_age_seconds = 0, .maximum_entries = 0U, .maximum_total_bytes = 0U},
        0);
    require(maintained.has_value() && !std::filesystem::exists(kept_path(old_source, old_id)) &&
                read_file(old_target) == "changed d" && state_of(old_id) == Backup::released,
            "retention must release a kept source it does not keep");

    // A same-filesystem rename keeps the file itself: nothing retained.
    const auto renamed_source = directory.path() / "plain.flac";
    write_file(renamed_source, "plain");
    const auto renamed = operations::commit_same_filesystem_publication(
        preflight(renamed_source, directory.path() / "renamed.flac"), 0U, journal,
        successful_dependent_commit);
    require(renamed && journal.load_backup(renamed->journal_id) &&
                !*journal.load_backup(renamed->journal_id),
            "a plain rename must keep no source");

    // E: across filesystems, the same.
    std::error_code shared_memory_error;
    if (!std::filesystem::is_directory("/dev/shm", shared_memory_error) || shared_memory_error) {
        return;
    }
    TemporaryDirectory elsewhere{"/dev/shm"};
    const auto cross_source = directory.path() / "cross.flac";
    const auto cross_target = elsewhere.path() / "Artist" / "Album" / "cross.flac";
    write_file(cross_source, "cross original");
    const auto crossed = operations::commit_cross_filesystem_publication(
        cross_preflight(cross_source, cross_target), 0U, journal, successful_dependent_commit);
    require(crossed && read_file(kept_path(cross_source, crossed->journal_id)) == "cross original",
            "a move to another filesystem must keep its source");
    const auto back =
        operations::undo_file_publication(crossed->journal_id, journal, successful_dependent_commit);
    require(back && read_file(cross_source) == "cross original" &&
                !std::filesystem::exists(cross_target) &&
                !std::filesystem::exists(elsewhere.path() / "Artist"),
            "undo across filesystems must restore the source and remove what it made");
}

void destinationArtifactPublishesChangedContentAndRecoversItsJournalBoundary() {
    TemporaryDirectory directory;
    const auto source = directory.path() / "source.flac";
    const auto target = directory.path() / "Artist" / "changed.flac";
    write_file(source, "original source bytes");
    require(::chmod(source.c_str(), 0640) == 0,
            "destination-artifact source permissions must be set");
    const auto checked = preflight(source, target);
    auto durable = open_journal(directory, "destination-artifact.sqlite3");
    FailingOnceTransitionJournal failing{durable, State::dependent_state_committed};
    std::size_t prepare_count = 0U;
    const auto preparer = [&](const std::string& prepared_raw_path,
                              const core::CancellationToken& cancellation)
        -> core::Result<core::LocalSourceRevision> {
        ++prepare_count;
        require(!cancellation.is_cancellation_requested() && std::filesystem::exists(source),
                "artifact preparation must run while the unchanged source is locked in place");
        write_file(prepared_raw_path, "verified changed destination bytes");
        return core::observe_local_source_revision(prepared_raw_path);
    };
    std::size_t callback_count = 0U;
    const auto callback =
        [&](const operations::FilePublicationCommitResult& result) -> core::Result<void> {
        ++callback_count;
        require(std::filesystem::exists(source) &&
                    read_file(target) == "verified changed destination bytes" &&
                    result.source_revision != result.target_revision &&
                    result.content ==
                        operations::FilePublicationContentKind::prepared_destination_artifact,
                "dependent state must see both original and changed destination identities");
        return {};
    };
    const auto interrupted = operations::commit_destination_artifact_publication(
        checked, 0U, failing, preparer, callback);
    require(!interrupted && interrupted.error().code == core::ErrorCode::database &&
                prepare_count == 1U && callback_count == 1U && std::filesystem::exists(source) &&
                read_file(source) == "original source bytes" &&
                read_file(target) == "verified changed destination bytes",
            "post-dependent journal failure must retain the recoverable two-file boundary");
    const auto incomplete = durable.load_incomplete();
    require(incomplete && incomplete->size() == 1U &&
                incomplete->front().state == State::target_published &&
                incomplete->front().publication ==
                    operations::OutputPathPublicationKind::same_filesystem_rename &&
                incomplete->front().content ==
                    operations::FilePublicationContentKind::prepared_destination_artifact,
            "same-filesystem changed content must use durable prepared-artifact evidence");

    const auto recovered = operations::recover_cross_filesystem_publications(durable, callback);
    require(
        recovered && recovered->size() == 1U &&
            recovered->front().outcome == operations::FilePublicationRecoveryOutcome::completed &&
            callback_count == 2U && !std::filesystem::exists(source) &&
            read_file(target) == "verified changed destination bytes" &&
            (std::filesystem::status(target).permissions() & std::filesystem::perms::owner_all) ==
                (std::filesystem::perms::owner_read | std::filesystem::perms::owner_write),
        "artifact recovery must replay state, preserve permissions, then remove the source");

    const auto orphan_source = directory.path() / "orphan-source.flac";
    const auto orphan_target = directory.path() / "orphan-target.flac";
    write_file(orphan_source, "orphan original bytes");
    const auto orphan_checked = preflight(orphan_source, orphan_target);
    auto orphan_journal = open_journal(directory, "destination-artifact-orphan.sqlite3");
    auto orphan_record = operations::make_destination_artifact_journal_record(
        orphan_checked, 0U, core::StableId::random());
    require(orphan_record && orphan_journal.create(*orphan_record),
            "planned destination-artifact evidence must be durable");
    write_file(orphan_record->prepared_raw_path, "unrecorded changed artifact");
    const auto orphan_recovery = operations::recover_cross_filesystem_publications(
        orphan_journal, successful_dependent_commit);
    require(orphan_recovery && orphan_recovery->size() == 1U &&
                orphan_recovery->front().outcome ==
                    operations::FilePublicationRecoveryOutcome::needs_reconciliation &&
                read_file(orphan_source) == "orphan original bytes" &&
                read_file(orphan_record->prepared_raw_path) == "unrecorded changed artifact" &&
                !std::filesystem::exists(orphan_target),
            "recovery must not infer ownership or intent for an unrecorded changed artifact");
}

void cancellationBeforeCommitCreatesNoJournal() {
    TemporaryDirectory directory;
    const auto source = directory.path() / "source.flac";
    const auto target = directory.path() / "target.flac";
    write_file(source, "cancelled");
    const auto checked = preflight(source, target);
    auto journal = open_journal(directory, "cancelled.sqlite3");
    core::CancellationSource cancellation;
    cancellation.request_cancellation();
    const auto committed = operations::commit_same_filesystem_publication(
        checked, 0U, journal, successful_dependent_commit, cancellation.token());
    require(!committed && committed.error().code == core::ErrorCode::cancelled &&
                journal.load_incomplete()->empty() && std::filesystem::exists(source) &&
                !std::filesystem::exists(target),
            "pre-commit cancellation must not create journal or filesystem mutations");
}

} // namespace

int main() {
    commitsAtomicRenameAndCreatesOnlyPlannedDirectories();
    dependentFailureRestoresSourceAndLeavesHarmlessDirectories();
    journalFailureAfterRenameAlsoRestoresSource();
    dependentJournalFailureReplaysIdempotentStateCommit();
    changedSourceOrAppearedTargetFailsBeforeJournalCreation();
    recoversBothSidesOfTheAtomicJournalBoundary();
    recoveryRollbackAndPostDependentCompletionAreExact();
    ambiguousRecoveryNeverDeletesEitherPath();
    realPublicationCommitsTheAllOccurrenceRelocationTransaction();
    completedSameFilesystemPublicationCanBeUndoneIdempotently();
    failedUndoRestoresPublishedTargetAndCanBeRetried();
    undoJournalBoundaryRecoversByReplayingDependentState();
    changedUndoTopologyCreatesNoReverseJournal();
    std::error_code shared_memory_error;
    if (std::filesystem::is_directory("/dev/shm", shared_memory_error) && !shared_memory_error) {
        crossFilesystemCopyPublishesExactBytesBeforeRemovingSource();
        crossFilesystemDependentFailureRemovesOnlyThePublishedCopy();
        crossFilesystemRecoveryReplaysDependentStateThenRemovesSource();
        crossFilesystemRecoveryAdoptsOnlyAnExactUnrecordedPreparedCopy();
        crossFilesystemRecoveryInfersPublishedAndRemovedBoundaries();
    }
    destinationArtifactPublishesChangedContentAndRecoversItsJournalBoundary();
    publicationsKeepTheirSourceForUndo();
    cancellationBeforeCommitCreatesNoJournal();
    std::cout << "file publication executor tests passed\n";
    return 0;
}

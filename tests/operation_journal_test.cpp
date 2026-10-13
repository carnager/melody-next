// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/persistence/operation_journal.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using State = trackknife::operations::MetadataOperationJournalState;
using BackupState = trackknife::operations::MetadataOperationBackupState;
using BackupTransition = trackknife::operations::MetadataOperationBackupTransition;
using Record = trackknife::operations::MetadataOperationJournalRecord;
using Transition = trackknife::operations::MetadataOperationJournalTransition;

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::abort();
    }
}

class TemporaryDatabase final {
  public:
    TemporaryDatabase()
        : path_{std::filesystem::temp_directory_path() /
                ("trackknife-operation-journal-" +
                 trackknife::core::StableId::random().to_string() + ".sqlite3")} {
        remove();
    }
    TemporaryDatabase(const TemporaryDatabase&) = delete;
    TemporaryDatabase& operator=(const TemporaryDatabase&) = delete;
    ~TemporaryDatabase() { remove(); }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

  private:
    void remove() const noexcept {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
        std::filesystem::remove(path_.string() + "-wal", ignored);
        std::filesystem::remove(path_.string() + "-shm", ignored);
    }

    std::filesystem::path path_;
};

[[nodiscard]] trackknife::core::LocalSourceRevision revision(const std::uint64_t seed) {
    return {
        .device = seed,
        .inode = seed + 1U,
        .size = seed + 2U,
        .modification_time_seconds = -static_cast<std::int64_t>(seed + 3U),
        .modification_time_nanoseconds = static_cast<std::int64_t>(seed + 4U),
    };
}

[[nodiscard]] trackknife::core::ContentFingerprint fingerprint(const std::uint8_t seed) {
    trackknife::core::ContentFingerprint result;
    for (std::size_t index = 0U; index < result.sha256.size(); ++index) {
        result.sha256[index] = static_cast<std::uint8_t>(seed + index);
    }
    return result;
}

[[nodiscard]] Record planned_record() {
    return Record{
        .id = trackknife::core::StableId::random(),
        .state = State::planned,
        .source_raw_path = std::string{"music/source-\xff.flac", 19U},
        .prepared_raw_path = std::string{"music/.prepared-\xfe", 17U},
        .backup_raw_path = std::string{"music/.backup-\xfd", 15U},
        .expected_revision = revision(10U),
        .prepared_revision = std::nullopt,
        .published_revision = std::nullopt,
        .occurrence_indexes = {7U, 2U, 99U},
        .content_kind = trackknife::operations::MetadataOperationContentKind::text_fields,
        .changes =
            {
                trackknife::operations::MetadataOperationJournalChange{
                    .field_index = 4U,
                    .canonical_name = "artist",
                    .property_name = "ARTIST",
                    .original_present = true,
                    .original_values = {"Old", "", "Old"},
                    .kind = trackknife::metadata::StagedMetadataPatchKind::replace_values,
                    .planned_values = {"New", "Other", "New"},
                    .item_indexes = {2U, 7U},
                    .exact_native_name = std::nullopt,
                },
                trackknife::operations::MetadataOperationJournalChange{
                    .field_index = 8U,
                    .canonical_name = "comment",
                    .property_name = "COMMENT",
                    .original_present = false,
                    .original_values = {},
                    .kind = trackknife::metadata::StagedMetadataPatchKind::remove_field,
                    .planned_values = {},
                    .item_indexes = {99U},
                    .exact_native_name = std::optional<std::string>{"comment"},
                },
            },
        .artwork = std::nullopt,
        .failure = std::nullopt,
    };
}

void journal_round_trips_and_guards_state_transitions() {
    TemporaryDatabase database;
    auto opened = trackknife::persistence::SqliteMetadataOperationJournal::open(database.path());
    require(opened.has_value(), "operation journal must create and migrate its database");
    auto journal = std::move(*opened);

    const auto planned = planned_record();
    const auto created = journal.create(planned);
    if (!created) {
        std::cerr << created.error().message << '\n';
    }
    require(created.has_value(), "a valid planned record must be durable");
    const auto loaded_planned = journal.load(planned.id);
    require(loaded_planned.has_value() && loaded_planned->has_value() &&
                **loaded_planned == planned,
            "paths, revisions, ordering, duplicates, and empty values must round trip exactly");
    const auto incomplete_planned = journal.load_incomplete();
    require(incomplete_planned.has_value() && *incomplete_planned == std::vector{planned},
            "planned records must be discoverable for startup recovery");

    const auto evidence_free_failure =
        journal.transition(planned.id, Transition{
                                           .expected_state = State::planned,
                                           .state = State::rolled_back,
                                           .prepared_revision = std::nullopt,
                                           .published_revision = std::nullopt,
                                           .failure = std::nullopt,
                                       });
    require(!evidence_free_failure &&
                evidence_free_failure.error().code == trackknife::core::ErrorCode::invalid_argument,
            "terminal failure states must retain durable failure evidence");

    auto duplicate_occurrence = planned_record();
    duplicate_occurrence.occurrence_indexes = {3U, 3U};
    const auto rejected = journal.create(duplicate_occurrence);
    require(!rejected && rejected.error().code == trackknife::core::ErrorCode::invalid_argument,
            "invalid recovery evidence must be rejected before a transaction starts");

    const auto prepared_revision = revision(30U);
    require(journal
                .transition(planned.id,
                            Transition{
                                .expected_state = State::planned,
                                .state = State::prepared,
                                .prepared_revision = prepared_revision,
                                .published_revision = std::nullopt,
                                .failure = std::nullopt,
                            })
                .has_value(),
            "planned must advance to prepared with a prepared identity");
    const auto stale =
        journal.transition(planned.id, Transition{
                                           .expected_state = State::planned,
                                           .state = State::rolled_back,
                                           .prepared_revision = prepared_revision,
                                           .published_revision = std::nullopt,
                                           .failure =
                                               trackknife::core::Error{
                                                   .code = trackknife::core::ErrorCode::conflict,
                                                   .message = "stale",
                                                   .context = {},
                                               },
                                       });
    require(!stale && stale.error().code == trackknife::core::ErrorCode::conflict,
            "state transitions must use optimistic expected-state guards");

    const auto published_revision = revision(50U);
    require(journal
                .transition(planned.id,
                            Transition{
                                .expected_state = State::prepared,
                                .state = State::published,
                                .prepared_revision = prepared_revision,
                                .published_revision = published_revision,
                                .failure = std::nullopt,
                            })
                .has_value(),
            "prepared must advance to published with both identities");
    auto expected_published = planned;
    expected_published.state = State::published;
    expected_published.prepared_revision = prepared_revision;
    expected_published.published_revision = published_revision;
    const auto loaded_published = journal.load(planned.id);
    require(loaded_published.has_value() && loaded_published->has_value() &&
                **loaded_published == expected_published,
            "journal transitions must preserve immutable plan evidence");

    require(journal
                .transition(planned.id,
                            Transition{
                                .expected_state = State::published,
                                .state = State::complete,
                                .prepared_revision = prepared_revision,
                                .published_revision = published_revision,
                                .failure = std::nullopt,
                            })
                .has_value(),
            "published must become complete only after all identities are present");
    const auto incomplete_after = journal.load_incomplete();
    require(incomplete_after.has_value() && incomplete_after->empty(),
            "complete records must not be replayed by startup recovery");
    const auto retained = journal.load_backup(planned.id);
    require(retained && *retained && (**retained).state == BackupState::retained &&
                !(**retained).undo_id && (**retained).completed_at_unix_seconds > 0 &&
                (**retained).updated_at_unix_seconds >= (**retained).completed_at_unix_seconds,
            "completion must atomically create a retained-backup lifecycle record");
    const auto undo_id = trackknife::core::StableId::random();
    require(
        journal
            .transition_backup(planned.id, BackupTransition{.expected_state = BackupState::retained,
                                                            .state = BackupState::undoing,
                                                            .undo_id = undo_id,
                                                            .failure = std::nullopt})
            .has_value(),
        "a retained backup must durably begin one identified undo");
    const auto stale_backup = journal.transition_backup(
        planned.id, BackupTransition{.expected_state = BackupState::retained,
                                     .state = BackupState::released,
                                     .undo_id = std::nullopt,
                                     .failure = std::nullopt});
    require(!stale_backup && stale_backup.error().code == trackknife::core::ErrorCode::conflict,
            "backup lifecycle transitions must reject stale expected states");
    require(
        journal
            .transition_backup(planned.id, BackupTransition{.expected_state = BackupState::undoing,
                                                            .state = BackupState::undone,
                                                            .undo_id = undo_id,
                                                            .failure = std::nullopt})
            .has_value(),
        "a completed undo must retain its idempotency identity");
    // Undone, it is history: its record stays, but the list of what still has
    // a backup leaves it out.
    const auto backups = journal.load_backups();
    require(backups && backups->empty(), "a file whose backups are all undone has none to keep");

    auto reopened = trackknife::persistence::SqliteMetadataOperationJournal::open(database.path());
    require(reopened.has_value(), "operation journal must reopen after a clean close");
    const auto durable = reopened->load(planned.id);
    require(durable.has_value() && durable->has_value() && (**durable).state == State::complete &&
                (**durable).published_revision == published_revision,
            "the terminal journal and revision evidence must survive reopening");
    const auto durable_backup = reopened->load_backup(planned.id);
    require(durable_backup && *durable_backup && (**durable_backup).state == BackupState::undone &&
                (**durable_backup).undo_id == undo_id,
            "the backup lifecycle and undo identity must survive reopening");
}

// A file written and its write completed: a retained backup.
[[nodiscard]] trackknife::core::StableId
completed_write(trackknife::persistence::SqliteMetadataOperationJournal& journal,
                const std::string& path) {
    auto record = planned_record();
    record.source_raw_path = path;
    require(journal.create(record).has_value(), "a write is planned");
    const auto prepared = revision(30U);
    const auto published = revision(50U);
    for (const auto& [from, to] :
         {std::pair{State::planned, State::prepared}, std::pair{State::prepared, State::published},
          std::pair{State::published, State::complete}}) {
        require(journal
                    .transition(record.id,
                                Transition{.expected_state = from,
                                           .state = to,
                                           .prepared_revision = prepared,
                                           .published_revision = to == State::prepared
                                                                     ? std::nullopt
                                                                     : std::optional{published},
                                           .failure = std::nullopt})
                    .has_value(),
                "a write completes");
    }
    return record.id;
}

void release(trackknife::persistence::SqliteMetadataOperationJournal& journal,
             const trackknife::core::StableId& id) {
    require(journal
                .transition_backup(id, BackupTransition{.expected_state = BackupState::retained,
                                                        .state = BackupState::released,
                                                        .undo_id = std::nullopt,
                                                        .failure = std::nullopt})
                .has_value(),
            "a backup is released");
}

// Recovery and maintenance read the files that still have a backup, with all
// their records, newest first -- not the history of every write ever made. A
// library tagged in full left more than the ten thousand records they would
// read, and neither could run again.
void only_files_with_a_backup_are_loaded() {
    TemporaryDatabase database;
    auto opened = trackknife::persistence::SqliteMetadataOperationJournal::open(database.path());
    require(opened.has_value(), "operation journal opens");
    auto journal = std::move(*opened);

    const auto first = completed_write(journal, "music/kept.flac");
    release(journal, first);
    const auto second = completed_write(journal, "music/kept.flac");
    release(journal, completed_write(journal, "music/history.flac"));
    for (std::size_t index = 0U; index < 10'050U; ++index) {
        static_cast<void>(completed_write(journal, "music/many-" + std::to_string(index)));
    }

    const auto backups = journal.load_backups();
    require(backups.has_value(), "more than ten thousand retained backups load");
    require(backups->size() == 10'052U, "every record of a file with a backup, and no other");
    require(
        !std::ranges::contains(*backups, std::string{"music/history.flac"},
                               [](const auto& backup) { return backup.operation.source_raw_path; }),
        "a file whose backups are all released is history");
    const auto kept =
        std::ranges::find(*backups, std::string{"music/kept.flac"},
                          [](const auto& backup) { return backup.operation.source_raw_path; });
    require(kept != backups->end() && kept->operation.id == second &&
                std::next(kept) != backups->end() && std::next(kept)->operation.id == first,
            "a file's records come newest first, its released one too");
}

void artwork_evidence_survives_reopen_without_image_payloads() {
    TemporaryDatabase database;
    auto opened = trackknife::persistence::SqliteMetadataOperationJournal::open(database.path());
    require(opened.has_value(), "artwork journal database must open");
    auto journal = std::move(*opened);
    const auto id = trackknife::core::StableId::random();
    const Record artwork{
        .id = id,
        .state = State::planned,
        .source_raw_path = "music/artwork.flac",
        .prepared_raw_path = "music/.artwork-prepared",
        .backup_raw_path = "music/.artwork-backup",
        .expected_revision = revision(70U),
        .prepared_revision = std::nullopt,
        .published_revision = std::nullopt,
        .occurrence_indexes = {1U, 8U},
        .content_kind = trackknife::operations::MetadataOperationContentKind::embedded_artwork,
        .changes = {},
        .artwork =
            trackknife::operations::MetadataOperationJournalArtwork{
                .kind = trackknife::metadata::ArtworkWritePlanIntentKind::replace,
                .target_ordinal = 1U,
                .original_item_count = 2U,
                .planned_item_count = 2U,
                .original_target_fingerprint = fingerprint(1U),
                .replacement_fingerprint = fingerprint(2U),
                .original_inventory_fingerprint = fingerprint(3U),
                .planned_inventory_fingerprint = fingerprint(4U),
            },
        .failure = std::nullopt,
    };
    require(journal.create(artwork).has_value(),
            "compact artwork recovery evidence must be durable");

    auto reopened = trackknife::persistence::SqliteMetadataOperationJournal::open(database.path());
    require(reopened.has_value(), "artwork journal must reopen");
    const auto loaded = reopened->load(id);
    require(loaded && *loaded && **loaded == artwork,
            "artwork kind, counts, ordinals, and fingerprints must survive reopening");

    auto invalid = artwork;
    invalid.id = trackknife::core::StableId::random();
    invalid.artwork->replacement_fingerprint.reset();
    const auto rejected = reopened->create(invalid);
    require(!rejected && rejected.error().code == trackknife::core::ErrorCode::invalid_argument,
            "artwork journal shape must reject incomplete recovery evidence");
}

} // namespace

int main() {
    journal_round_trips_and_guards_state_transitions();
    artwork_evidence_survives_reopen_without_image_payloads();
    only_files_with_a_backup_are_loaded();
    return EXIT_SUCCESS;
}

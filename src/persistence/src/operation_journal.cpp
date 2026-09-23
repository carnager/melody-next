// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/persistence/operation_journal.hpp"

#include "trackknife/persistence/list_repository.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace trackknife::persistence {
namespace {

constexpr std::size_t maximum_occurrences = 100'000U;
constexpr std::size_t maximum_changes = 4'096U;
constexpr std::size_t maximum_values_per_change = 16'384U;
constexpr std::size_t maximum_total_values = 100'000U;
constexpr std::size_t maximum_total_intents = 100'000U;
constexpr std::size_t maximum_incomplete_records = 10'000U;
constexpr std::size_t maximum_backup_records = 10'000U;
constexpr std::size_t maximum_artwork_items = 64U;
constexpr std::size_t maximum_text_bytes = 64U * 1024U * 1024U;

struct StatementDeleter {
    void operator()(sqlite3_stmt* statement) const noexcept {
        if (statement != nullptr) {
            sqlite3_finalize(statement);
        }
    }
};

using Statement = std::unique_ptr<sqlite3_stmt, StatementDeleter>;
using State = operations::MetadataOperationJournalState;
using Record = operations::MetadataOperationJournalRecord;
using Transition = operations::MetadataOperationJournalTransition;
using BackupState = operations::MetadataOperationBackupState;
using BackupRecord = operations::MetadataOperationBackupRecord;
using BackupTransition = operations::MetadataOperationBackupTransition;
using ContentKind = operations::MetadataOperationContentKind;

[[nodiscard]] core::Error database_error(sqlite3* database, std::string message) {
    if (database != nullptr) {
        message += ": ";
        message += sqlite3_errmsg(database);
    }
    return core::Error{
        .code = core::ErrorCode::database,
        .message = std::move(message),
        .context = {},
    };
}

[[nodiscard]] core::Error invalid_record(std::string message) {
    return core::Error{
        .code = core::ErrorCode::invalid_argument,
        .message = std::move(message),
        .context = {},
    };
}

[[nodiscard]] core::Result<void> execute(sqlite3* database, const char* sql) {
    char* message = nullptr;
    const auto result = sqlite3_exec(database, sql, nullptr, nullptr, &message);
    if (result == SQLITE_OK) {
        return {};
    }
    auto error = database_error(database, message == nullptr ? "SQLite statement failed"
                                                             : std::string{message});
    sqlite3_free(message);
    return std::unexpected(std::move(error));
}

[[nodiscard]] core::Result<Statement> prepare(sqlite3* database, const char* sql) {
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(database, sql, -1, &raw, nullptr) != SQLITE_OK) {
        return std::unexpected(database_error(database, "Could not prepare journal statement"));
    }
    return Statement{raw};
}

[[nodiscard]] bool bind_blob(sqlite3_stmt* statement, const int index,
                             const std::string_view value) {
    return sqlite3_bind_blob64(statement, index, value.data(), value.size(), SQLITE_TRANSIENT) ==
           SQLITE_OK;
}

[[nodiscard]] bool bind_fingerprint(sqlite3_stmt* statement, const int index,
                                    const core::ContentFingerprint& fingerprint) {
    return sqlite3_bind_blob64(statement, index, fingerprint.sha256.data(),
                               fingerprint.sha256.size(), SQLITE_TRANSIENT) == SQLITE_OK;
}

[[nodiscard]] bool
bind_optional_fingerprint(sqlite3_stmt* statement, const int index,
                          const std::optional<core::ContentFingerprint>& fingerprint) {
    return fingerprint ? bind_fingerprint(statement, index, *fingerprint)
                       : sqlite3_bind_null(statement, index) == SQLITE_OK;
}

[[nodiscard]] bool bind_optional_blob(sqlite3_stmt* statement, const int index,
                                      const std::optional<std::string>& value) {
    return value ? bind_blob(statement, index, *value)
                 : sqlite3_bind_null(statement, index) == SQLITE_OK;
}

[[nodiscard]] std::string column_blob(sqlite3_stmt* statement, const int column) {
    const auto size = sqlite3_column_bytes(statement, column);
    const auto* data = static_cast<const char*>(sqlite3_column_blob(statement, column));
    return data == nullptr || size <= 0 ? std::string{} : std::string{data, data + size};
}

[[nodiscard]] std::optional<std::string> optional_blob(sqlite3_stmt* statement, const int column) {
    return sqlite3_column_type(statement, column) == SQLITE_NULL
               ? std::nullopt
               : std::optional<std::string>{column_blob(statement, column)};
}

[[nodiscard]] core::Result<core::ContentFingerprint> read_fingerprint(sqlite3_stmt* statement,
                                                                      const int column) {
    if (sqlite3_column_type(statement, column) == SQLITE_NULL ||
        sqlite3_column_bytes(statement, column) != 32) {
        return std::unexpected(
            database_error(sqlite3_db_handle(statement),
                           "Operation journal contains an invalid artwork fingerprint"));
    }
    core::ContentFingerprint result;
    const auto* data = static_cast<const std::uint8_t*>(sqlite3_column_blob(statement, column));
    std::ranges::copy(std::span{data, result.sha256.size()}, result.sha256.begin());
    return result;
}

[[nodiscard]] core::Result<std::optional<core::ContentFingerprint>>
read_optional_fingerprint(sqlite3_stmt* statement, const int column) {
    if (sqlite3_column_type(statement, column) == SQLITE_NULL) {
        return std::optional<core::ContentFingerprint>{};
    }
    auto fingerprint = read_fingerprint(statement, column);
    if (!fingerprint) {
        return std::unexpected(std::move(fingerprint.error()));
    }
    return std::optional{*fingerprint};
}

[[nodiscard]] std::string encode_unsigned(const std::uint64_t value) {
    std::string bytes(8U, '\0');
    for (std::size_t index = 0U; index < bytes.size(); ++index) {
        const auto shift = static_cast<unsigned>((bytes.size() - index - 1U) * 8U);
        bytes[index] = static_cast<char>((value >> shift) & 0xFFU);
    }
    return bytes;
}

[[nodiscard]] std::optional<std::uint64_t> decode_unsigned(const std::string_view bytes) {
    if (bytes.size() != 8U) {
        return std::nullopt;
    }
    std::uint64_t value = 0U;
    for (const auto character : bytes) {
        value = (value << 8U) | static_cast<unsigned char>(character);
    }
    return value;
}

[[nodiscard]] std::string encode_signed(const std::int64_t value) {
    return encode_unsigned(std::bit_cast<std::uint64_t>(value));
}

[[nodiscard]] std::optional<std::int64_t> decode_signed(const std::string_view bytes) {
    const auto value = decode_unsigned(bytes);
    return value ? std::optional{std::bit_cast<std::int64_t>(*value)} : std::nullopt;
}

[[nodiscard]] bool bind_revision(sqlite3_stmt* statement, const int first,
                                 const core::LocalSourceRevision& revision) {
    const std::array values{
        encode_unsigned(revision.device),
        encode_unsigned(revision.inode),
        encode_unsigned(revision.size),
        encode_signed(revision.modification_time_seconds),
        encode_signed(revision.modification_time_nanoseconds),
    };
    for (std::size_t index = 0U; index < values.size(); ++index) {
        if (!bind_blob(statement, first + static_cast<int>(index), values[index])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool
bind_optional_revision(sqlite3_stmt* statement, const int first,
                       const std::optional<core::LocalSourceRevision>& revision) {
    if (revision) {
        return bind_revision(statement, first, *revision);
    }
    for (int index = 0; index < 5; ++index) {
        if (sqlite3_bind_null(statement, first + index) != SQLITE_OK) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] core::Result<core::LocalSourceRevision> read_revision(sqlite3_stmt* statement,
                                                                    const int first) {
    const auto device = decode_unsigned(column_blob(statement, first));
    const auto inode = decode_unsigned(column_blob(statement, first + 1));
    const auto size = decode_unsigned(column_blob(statement, first + 2));
    const auto seconds = decode_signed(column_blob(statement, first + 3));
    const auto nanoseconds = decode_signed(column_blob(statement, first + 4));
    if (!device || !inode || !size || !seconds || !nanoseconds) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::database,
            .message = "Operation journal contains an invalid source revision",
            .context = {},
        });
    }
    return core::LocalSourceRevision{
        .device = *device,
        .inode = *inode,
        .size = *size,
        .modification_time_seconds = *seconds,
        .modification_time_nanoseconds = *nanoseconds,
    };
}

[[nodiscard]] core::Result<std::optional<core::LocalSourceRevision>>
read_optional_revision(sqlite3_stmt* statement, const int first) {
    bool any_null = false;
    bool any_value = false;
    for (int index = 0; index < 5; ++index) {
        if (sqlite3_column_type(statement, first + index) == SQLITE_NULL) {
            any_null = true;
        } else {
            any_value = true;
        }
    }
    if (!any_value) {
        return std::optional<core::LocalSourceRevision>{};
    }
    if (any_null) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::database,
            .message = "Operation journal contains a partial source revision",
            .context = {},
        });
    }
    auto revision = read_revision(statement, first);
    if (!revision) {
        return std::unexpected(std::move(revision.error()));
    }
    return std::optional{*revision};
}

[[nodiscard]] bool bind_index(sqlite3_stmt* statement, const int column, const std::size_t value) {
    if (value > static_cast<std::size_t>(std::numeric_limits<sqlite3_int64>::max())) {
        return false;
    }
    return sqlite3_bind_int64(statement, column, static_cast<sqlite3_int64>(value)) == SQLITE_OK;
}

[[nodiscard]] core::Result<std::size_t> read_index(sqlite3_stmt* statement, const int column) {
    const auto value = sqlite3_column_int64(statement, column);
    if (value < 0 || static_cast<std::uint64_t>(value) >
                         static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::database,
            .message = "Operation journal contains an invalid item index",
            .context = {},
        });
    }
    return static_cast<std::size_t>(value);
}

[[nodiscard]] bool valid_state(const int value) {
    return value >= static_cast<int>(State::planned) &&
           value <= static_cast<int>(State::needs_reconciliation);
}

[[nodiscard]] bool valid_content_kind(const int value) {
    return value >= static_cast<int>(ContentKind::text_fields) &&
           value <= static_cast<int>(ContentKind::folder_image);
}

[[nodiscard]] bool valid_backup_state(const int value) {
    return value >= static_cast<int>(BackupState::retained) &&
           value <= static_cast<int>(BackupState::needs_reconciliation);
}

[[nodiscard]] bool legal_backup_transition(const BackupState from, const BackupState to) {
    switch (from) {
    case BackupState::retained:
        return to == BackupState::undoing || to == BackupState::released ||
               to == BackupState::needs_reconciliation;
    case BackupState::undoing:
        return to == BackupState::retained || to == BackupState::undone ||
               to == BackupState::needs_reconciliation;
    case BackupState::undone:
    case BackupState::released:
    case BackupState::needs_reconciliation:
        return false;
    }
    return false;
}

[[nodiscard]] core::Result<void> validate_backup_transition(const BackupTransition& transition) {
    if (!legal_backup_transition(transition.expected_state, transition.state)) {
        return std::unexpected(invalid_record("Invalid metadata-backup state transition"));
    }
    const bool requires_undo_id = transition.state == BackupState::undoing ||
                                  transition.state == BackupState::undone ||
                                  (transition.state == BackupState::needs_reconciliation &&
                                   transition.expected_state == BackupState::undoing);
    if (requires_undo_id != transition.undo_id.has_value() ||
        (transition.undo_id && transition.undo_id->is_nil())) {
        return std::unexpected(
            invalid_record("Metadata-backup undo state has invalid operation identity"));
    }
    if ((transition.state == BackupState::needs_reconciliation) != transition.failure.has_value()) {
        return std::unexpected(
            invalid_record("Metadata-backup reconciliation state requires failure evidence"));
    }
    return {};
}

[[nodiscard]] bool legal_transition(const State from, const State to) {
    switch (from) {
    case State::planned:
        return to == State::prepared || to == State::rolled_back ||
               to == State::needs_reconciliation;
    case State::prepared:
        return to == State::published || to == State::rolled_back ||
               to == State::needs_reconciliation;
    case State::published:
        return to == State::complete || to == State::rolled_back ||
               to == State::needs_reconciliation;
    case State::complete:
    case State::rolled_back:
    case State::needs_reconciliation:
        return false;
    }
    return false;
}

[[nodiscard]] core::Result<void> validate_transition(const Transition& transition) {
    if (!legal_transition(transition.expected_state, transition.state)) {
        return std::unexpected(invalid_record("Invalid operation-journal state transition"));
    }
    if ((transition.state == State::prepared || transition.state == State::published ||
         transition.state == State::complete) &&
        !transition.prepared_revision) {
        return std::unexpected(
            invalid_record("Prepared operation-journal state requires a prepared revision"));
    }
    if ((transition.state == State::published || transition.state == State::complete) &&
        !transition.published_revision) {
        return std::unexpected(
            invalid_record("Published operation-journal state requires a published revision"));
    }
    if (transition.state == State::prepared && transition.published_revision) {
        return std::unexpected(
            invalid_record("Prepared operation-journal state cannot have a published revision"));
    }
    if (transition.state == State::complete && transition.failure) {
        return std::unexpected(
            invalid_record("Complete operation-journal state cannot retain a failure"));
    }
    if ((transition.state == State::rolled_back ||
         transition.state == State::needs_reconciliation) &&
        !transition.failure) {
        return std::unexpected(
            invalid_record("Failed operation-journal state requires failure evidence"));
    }
    return {};
}

[[nodiscard]] bool add_text(std::size_t& total, const std::size_t amount) {
    if (amount > maximum_text_bytes - total) {
        return false;
    }
    total += amount;
    return true;
}

[[nodiscard]] bool account_column_blob(std::size_t& total, sqlite3_stmt* statement,
                                       const int column) {
    const auto size = sqlite3_column_bytes(statement, column);
    return size >= 0 && add_text(total, static_cast<std::size_t>(size));
}

[[nodiscard]] core::Result<void> validate_record_structure(const Record& record) {
    if (record.id.is_nil() || record.source_raw_path.empty() || record.prepared_raw_path.empty() ||
        record.backup_raw_path.empty() || record.source_raw_path.find('\0') != std::string::npos ||
        record.prepared_raw_path.find('\0') != std::string::npos ||
        record.backup_raw_path.find('\0') != std::string::npos ||
        record.source_raw_path == record.prepared_raw_path ||
        record.source_raw_path == record.backup_raw_path ||
        record.prepared_raw_path == record.backup_raw_path || record.occurrence_indexes.empty()) {
        return std::unexpected(invalid_record("Invalid metadata-operation journal structure"));
    }
    // ADR-0145: the CUE and sidecar carrier kinds share the text kind's
    // change-row evidence shape.
    const bool artwork_record = record.content_kind == ContentKind::embedded_artwork;
    if (!valid_content_kind(static_cast<int>(record.content_kind)) ||
        (!artwork_record && (record.changes.empty() || record.artwork)) ||
        (artwork_record && !record.artwork)) {
        return std::unexpected(invalid_record("Invalid metadata-operation content evidence"));
    }
    if (record.content_kind == ContentKind::folder_image) {
        const auto hash = [](const std::string& value) {
            return value.size() == 64 && std::ranges::all_of(value, [](char c) {
                       return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
                   });
        };
        if (record.changes.size() != 1 || record.changes.front().canonical_name != "folderimage" ||
            record.changes.front().kind != metadata::StagedMetadataPatchKind::replace_values ||
            record.changes.front().planned_values.size() != 1 ||
            !hash(record.changes.front().planned_values.front()) ||
            (record.changes.front().original_present &&
             (record.changes.front().original_values.size() != 1 ||
              !hash(record.changes.front().original_values.front()))))
            return std::unexpected(invalid_record("Invalid folder-image journal hash evidence"));
    }
    if (record.artwork) {
        const auto& artwork = *record.artwork;
        const bool replacing = artwork.kind == metadata::ArtworkWritePlanIntentKind::replace;
        const bool removing = artwork.kind == metadata::ArtworkWritePlanIntentKind::remove;
        const bool adding = artwork.kind == metadata::ArtworkWritePlanIntentKind::add;
        const bool batch = artwork.kind == metadata::ArtworkWritePlanIntentKind::batch;
        if ((!replacing && !removing && !adding && !batch) ||
            (batch && (artwork.target_ordinal != 0 || artwork.original_target_fingerprint ||
                       artwork.replacement_fingerprint)) ||
            (!batch && !record.changes.empty()) ||
            artwork.original_item_count > maximum_artwork_items ||
            artwork.planned_item_count > maximum_artwork_items ||
            ((replacing || removing) && (!artwork.original_target_fingerprint ||
                                         artwork.target_ordinal >= artwork.original_item_count)) ||
            (replacing && (!artwork.replacement_fingerprint ||
                           artwork.planned_item_count != artwork.original_item_count)) ||
            (removing && (artwork.replacement_fingerprint ||
                          artwork.planned_item_count + 1U != artwork.original_item_count)) ||
            (adding && (artwork.original_target_fingerprint || !artwork.replacement_fingerprint ||
                        artwork.target_ordinal != artwork.original_item_count ||
                        artwork.planned_item_count != artwork.original_item_count + 1U))) {
            return std::unexpected(invalid_record("Invalid artwork-operation journal evidence"));
        }
    }
    if (record.occurrence_indexes.size() > maximum_occurrences ||
        record.changes.size() > maximum_changes) {
        return std::unexpected(
            invalid_record("Metadata-operation journal exceeds its structural limits"));
    }
    std::unordered_set<std::size_t> occurrences;
    occurrences.reserve(record.occurrence_indexes.size());
    for (const auto index : record.occurrence_indexes) {
        if (!occurrences.insert(index).second ||
            index > static_cast<std::size_t>(std::numeric_limits<sqlite3_int64>::max())) {
            return std::unexpected(
                invalid_record("Metadata-operation occurrence indexes must be unique"));
        }
    }
    std::unordered_set<std::size_t> field_indexes;
    std::unordered_set<std::string> addressed_names;
    std::size_t total_values = 0U;
    std::size_t total_intents = 0U;
    std::size_t text_bytes = 0U;
    if (!add_text(text_bytes, record.source_raw_path.size()) ||
        !add_text(text_bytes, record.prepared_raw_path.size()) ||
        !add_text(text_bytes, record.backup_raw_path.size())) {
        return std::unexpected(invalid_record("Metadata-operation journal text exceeds its limit"));
    }
    // ADR-0145: carrier kinds address one field per (identity slug,
    // canonical name) pair; several entries legitimately share one staged
    // field index, and one slug carries several canonical names.
    const bool carrier_kind = record.content_kind == ContentKind::cue_replay_gain ||
                              record.content_kind == ContentKind::loudness_sidecar;
    for (const auto& change : record.changes) {
        if (change.canonical_name.empty() || change.property_name.empty() ||
            (change.exact_native_name &&
             (change.exact_native_name->empty() ||
              metadata::canonicalize_native_field_name(*change.exact_native_name) !=
                  *change.exact_native_name)) ||
            change.item_indexes.empty() ||
            (!change.original_present && !change.original_values.empty()) ||
            (carrier_kind && !change.exact_native_name) ||
            (!carrier_kind && !field_indexes.insert(change.field_index).second) ||
            !addressed_names
                 .insert((change.exact_native_name
                              ? std::string{"native:"} + *change.exact_native_name
                              : std::string{"logical:"} + change.canonical_name) +
                         (carrier_kind ? "|" + change.canonical_name : std::string{}))
                 .second ||
            change.original_values.size() > maximum_values_per_change ||
            change.planned_values.size() > maximum_values_per_change ||
            (change.kind == metadata::StagedMetadataPatchKind::replace_values &&
             change.planned_values.empty()) ||
            (change.kind == metadata::StagedMetadataPatchKind::remove_field &&
             !change.planned_values.empty())) {
            return std::unexpected(invalid_record("Invalid metadata-operation journal change"));
        }
        if (change.original_values.size() > maximum_total_values - total_values ||
            change.planned_values.size() >
                maximum_total_values - total_values - change.original_values.size() ||
            change.item_indexes.size() > maximum_total_intents - total_intents) {
            return std::unexpected(
                invalid_record("Metadata-operation journal exceeds its aggregate limits"));
        }
        total_values += change.original_values.size() + change.planned_values.size();
        total_intents += change.item_indexes.size();
        if (!add_text(text_bytes, change.canonical_name.size()) ||
            !add_text(text_bytes, change.property_name.size()) ||
            (change.exact_native_name && !add_text(text_bytes, change.exact_native_name->size()))) {
            return std::unexpected(
                invalid_record("Metadata-operation journal text exceeds its limit"));
        }
        for (const auto& value : change.original_values) {
            if (!add_text(text_bytes, value.size())) {
                return std::unexpected(
                    invalid_record("Metadata-operation journal text exceeds its limit"));
            }
        }
        for (const auto& value : change.planned_values) {
            if (!add_text(text_bytes, value.size())) {
                return std::unexpected(
                    invalid_record("Metadata-operation journal text exceeds its limit"));
            }
        }
        std::unordered_set<std::size_t> change_intents;
        change_intents.reserve(change.item_indexes.size());
        for (const auto index : change.item_indexes) {
            if (!occurrences.contains(index) || !change_intents.insert(index).second ||
                index > static_cast<std::size_t>(std::numeric_limits<sqlite3_int64>::max())) {
                return std::unexpected(
                    invalid_record("Metadata-operation intent is outside its physical source"));
            }
        }
    }
    if (record.failure && !add_text(text_bytes, record.failure->message.size())) {
        return std::unexpected(invalid_record("Metadata-operation journal text exceeds its limit"));
    }
    return {};
}

[[nodiscard]] core::Result<void> validate_record(const Record& record) {
    if (record.state != State::planned || record.prepared_revision || record.published_revision ||
        record.failure) {
        return std::unexpected(invalid_record("Invalid planned metadata-operation journal"));
    }
    return validate_record_structure(record);
}

[[nodiscard]] bool valid_loaded_state_evidence(const Record& record) {
    switch (record.state) {
    case State::planned:
        return !record.prepared_revision && !record.published_revision && !record.failure;
    case State::prepared:
        return record.prepared_revision && !record.published_revision && !record.failure;
    case State::published:
    case State::complete:
        return record.prepared_revision && record.published_revision && !record.failure;
    case State::rolled_back:
    case State::needs_reconciliation:
        return record.failure.has_value();
    }
    return false;
}

[[nodiscard]] core::Result<void> step_done(sqlite3* database, sqlite3_stmt* statement,
                                           const std::string_view operation) {
    if (sqlite3_step(statement) != SQLITE_DONE) {
        return std::unexpected(database_error(database, std::string{operation}));
    }
    sqlite3_reset(statement);
    sqlite3_clear_bindings(statement);
    return {};
}

[[nodiscard]] core::Result<void> load_children(sqlite3* database, Record& record) {
    std::size_t text_bytes = 0U;
    if (!add_text(text_bytes, record.source_raw_path.size()) ||
        !add_text(text_bytes, record.prepared_raw_path.size()) ||
        !add_text(text_bytes, record.backup_raw_path.size()) ||
        (record.failure && !add_text(text_bytes, record.failure->message.size()))) {
        return std::unexpected(
            database_error(database, "Operation journal text exceeds its limit"));
    }
    const auto id = record.id.to_string();
    auto occurrences = prepare(
        database, "SELECT item_index FROM operation_journal_occurrences WHERE journal_id = ? "
                  "ORDER BY position");
    if (!occurrences || !bind_blob(occurrences->get(), 1, id)) {
        return std::unexpected(occurrences ? database_error(database, "Could not bind journal ID")
                                           : std::move(occurrences.error()));
    }
    int occurrence_result = SQLITE_OK;
    while ((occurrence_result = sqlite3_step(occurrences->get())) == SQLITE_ROW) {
        if (record.occurrence_indexes.size() == maximum_occurrences) {
            return std::unexpected(
                database_error(database, "Operation journal has too many occurrences"));
        }
        auto index = read_index(occurrences->get(), 0);
        if (!index) {
            return std::unexpected(std::move(index.error()));
        }
        record.occurrence_indexes.push_back(*index);
    }
    if (occurrence_result != SQLITE_DONE) {
        return std::unexpected(database_error(database, "Could not read journal occurrences"));
    }

    auto changes =
        prepare(database,
                "SELECT position, field_index, canonical_name, property_name, exact_native_name, "
                "original_present, patch_kind FROM operation_journal_changes WHERE journal_id = ? "
                "ORDER BY position");
    if (!changes || !bind_blob(changes->get(), 1, id)) {
        return std::unexpected(changes ? database_error(database, "Could not bind journal ID")
                                       : std::move(changes.error()));
    }
    int change_result = SQLITE_OK;
    while ((change_result = sqlite3_step(changes->get())) == SQLITE_ROW) {
        if (record.changes.size() == maximum_changes) {
            return std::unexpected(
                database_error(database, "Operation journal has too many changes"));
        }
        auto position = read_index(changes->get(), 0);
        auto field_index = read_index(changes->get(), 1);
        const auto kind = sqlite3_column_int(changes->get(), 6);
        if (!position || !field_index || *position != record.changes.size() || kind < 0 ||
            kind > 1 || !account_column_blob(text_bytes, changes->get(), 2) ||
            !account_column_blob(text_bytes, changes->get(), 3) ||
            (sqlite3_column_type(changes->get(), 4) != SQLITE_NULL &&
             !account_column_blob(text_bytes, changes->get(), 4))) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::database,
                .message = "Operation journal contains an invalid change",
                .context = {},
            });
        }
        record.changes.push_back(operations::MetadataOperationJournalChange{
            .field_index = *field_index,
            .canonical_name = column_blob(changes->get(), 2),
            .property_name = column_blob(changes->get(), 3),
            .original_present = sqlite3_column_int(changes->get(), 5) != 0,
            .original_values = {},
            .kind = static_cast<metadata::StagedMetadataPatchKind>(kind),
            .planned_values = {},
            .item_indexes = {},
            .exact_native_name = optional_blob(changes->get(), 4),
        });
    }
    if (change_result != SQLITE_DONE) {
        return std::unexpected(database_error(database, "Could not read journal changes"));
    }

    auto values =
        prepare(database, "SELECT change_position, value_kind, value FROM operation_journal_values "
                          "WHERE journal_id = ? ORDER BY change_position, value_kind, position");
    if (!values || !bind_blob(values->get(), 1, id)) {
        return std::unexpected(values ? database_error(database, "Could not bind journal ID")
                                      : std::move(values.error()));
    }
    int value_result = SQLITE_OK;
    std::size_t total_values = 0U;
    while ((value_result = sqlite3_step(values->get())) == SQLITE_ROW) {
        if (total_values == maximum_total_values) {
            return std::unexpected(
                database_error(database, "Operation journal has too many values"));
        }
        auto change_position = read_index(values->get(), 0);
        const auto value_kind = sqlite3_column_int(values->get(), 1);
        if (!change_position || *change_position >= record.changes.size() || value_kind < 0 ||
            value_kind > 1 || !account_column_blob(text_bytes, values->get(), 2)) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::database,
                .message = "Operation journal contains an invalid field value",
                .context = {},
            });
        }
        auto& destination = value_kind == 0 ? record.changes[*change_position].original_values
                                            : record.changes[*change_position].planned_values;
        destination.push_back(column_blob(values->get(), 2));
        ++total_values;
    }
    if (value_result != SQLITE_DONE) {
        return std::unexpected(database_error(database, "Could not read journal values"));
    }

    auto intents =
        prepare(database, "SELECT change_position, item_index FROM operation_journal_intents "
                          "WHERE journal_id = ? ORDER BY change_position, position");
    if (!intents || !bind_blob(intents->get(), 1, id)) {
        return std::unexpected(intents ? database_error(database, "Could not bind journal ID")
                                       : std::move(intents.error()));
    }
    int intent_result = SQLITE_OK;
    std::size_t total_intents = 0U;
    while ((intent_result = sqlite3_step(intents->get())) == SQLITE_ROW) {
        if (total_intents == maximum_total_intents) {
            return std::unexpected(
                database_error(database, "Operation journal has too many logical intents"));
        }
        auto change_position = read_index(intents->get(), 0);
        auto item_index = read_index(intents->get(), 1);
        if (!change_position || !item_index || *change_position >= record.changes.size()) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::database,
                .message = "Operation journal contains an invalid logical intent",
                .context = {},
            });
        }
        record.changes[*change_position].item_indexes.push_back(*item_index);
        ++total_intents;
    }
    if (intent_result != SQLITE_DONE) {
        return std::unexpected(database_error(database, "Could not read journal intents"));
    }

    auto artwork = prepare(
        database, "SELECT intent_kind, target_ordinal, original_item_count, planned_item_count, "
                  "original_target_fingerprint, replacement_fingerprint, "
                  "original_inventory_fingerprint, planned_inventory_fingerprint "
                  "FROM operation_journal_artwork WHERE journal_id = ?");
    if (!artwork || !bind_blob(artwork->get(), 1, id)) {
        return std::unexpected(artwork ? database_error(database, "Could not bind journal ID")
                                       : std::move(artwork.error()));
    }
    const auto artwork_result = sqlite3_step(artwork->get());
    if (artwork_result == SQLITE_ROW) {
        const auto kind = sqlite3_column_int(artwork->get(), 0);
        auto target_ordinal = read_index(artwork->get(), 1);
        auto original_item_count = read_index(artwork->get(), 2);
        auto planned_item_count = read_index(artwork->get(), 3);
        auto original_target = read_optional_fingerprint(artwork->get(), 4);
        auto replacement = read_optional_fingerprint(artwork->get(), 5);
        auto original_inventory = read_fingerprint(artwork->get(), 6);
        auto planned_inventory = read_fingerprint(artwork->get(), 7);
        if (kind < static_cast<int>(metadata::ArtworkWritePlanIntentKind::replace) ||
            kind > static_cast<int>(metadata::ArtworkWritePlanIntentKind::batch) ||
            !target_ordinal || !original_item_count || !planned_item_count || !original_target ||
            !replacement || !original_inventory || !planned_inventory) {
            return std::unexpected(
                database_error(database, "Operation journal contains invalid artwork evidence"));
        }
        record.artwork = operations::MetadataOperationJournalArtwork{
            .kind = static_cast<metadata::ArtworkWritePlanIntentKind>(kind),
            .target_ordinal = *target_ordinal,
            .original_item_count = *original_item_count,
            .planned_item_count = *planned_item_count,
            .original_target_fingerprint = *original_target,
            .replacement_fingerprint = *replacement,
            .original_inventory_fingerprint = *original_inventory,
            .planned_inventory_fingerprint = *planned_inventory,
        };
        if (sqlite3_step(artwork->get()) != SQLITE_DONE) {
            return std::unexpected(
                database_error(database, "Operation journal contains duplicate artwork evidence"));
        }
    } else if (artwork_result != SQLITE_DONE) {
        return std::unexpected(database_error(database, "Could not read journal artwork"));
    }
    return {};
}

// How a record whose evidence does not hold together is loaded.
enum class DamagedEvidence : std::uint8_t {
    // Fail the load: the caller acts on this one record and cannot without it.
    refuse,
    // Return it as needing reconciliation. Recovery then only reports it, and
    // save admission still refuses its source, but one damaged record no
    // longer fails recovery of every other one and the backup maintenance
    // that follows it.
    reconcile,
};

[[nodiscard]] core::Result<std::vector<Record>>
load_records(sqlite3* database, const char* sql, const std::string_view id = {},
             const DamagedEvidence damaged = DamagedEvidence::refuse) {
    auto statement = prepare(database, sql);
    if (!statement) {
        return std::unexpected(std::move(statement.error()));
    }
    if (!id.empty() && !bind_blob(statement->get(), 1, id)) {
        return std::unexpected(database_error(database, "Could not bind operation-journal ID"));
    }
    std::vector<Record> records;
    int record_result = SQLITE_OK;
    while ((record_result = sqlite3_step(statement->get())) == SQLITE_ROW) {
        const auto id_text = column_blob(statement->get(), 0);
        auto parsed_id = core::StableId::parse(id_text);
        const auto state_value = sqlite3_column_int(statement->get(), 1);
        const auto content_kind = sqlite3_column_int(statement->get(), 22);
        auto expected = read_revision(statement->get(), 5);
        auto prepared_revision = read_optional_revision(statement->get(), 10);
        auto published_revision = read_optional_revision(statement->get(), 15);
        if (!parsed_id || !valid_state(state_value) || !valid_content_kind(content_kind) ||
            !expected || !prepared_revision || !published_revision) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::database,
                .message = "Operation journal contains an invalid record",
                .context = {{.key = "journal_id", .value = id_text}},
            });
        }
        std::optional<core::Error> failure;
        const auto error_type = sqlite3_column_type(statement->get(), 20);
        const auto message_type = sqlite3_column_type(statement->get(), 21);
        if (error_type != SQLITE_NULL || message_type != SQLITE_NULL) {
            const auto code = sqlite3_column_int(statement->get(), 20);
            if (error_type == SQLITE_NULL || message_type == SQLITE_NULL || code < 0 ||
                code > static_cast<int>(core::ErrorCode::invariant)) {
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::database,
                    .message = "Operation journal contains invalid failure evidence",
                    .context = {{.key = "journal_id", .value = id_text}},
                });
            }
            failure = core::Error{
                .code = static_cast<core::ErrorCode>(code),
                .message = column_blob(statement->get(), 21),
                .context = {},
            };
        }
        std::size_t main_text_bytes = 0U;
        if (!account_column_blob(main_text_bytes, statement->get(), 2) ||
            !account_column_blob(main_text_bytes, statement->get(), 3) ||
            !account_column_blob(main_text_bytes, statement->get(), 4) ||
            (failure && !account_column_blob(main_text_bytes, statement->get(), 21))) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::database,
                .message = "Operation journal text exceeds its limit",
                .context = {{.key = "journal_id", .value = id_text}},
            });
        }
        records.push_back(Record{
            .id = *parsed_id,
            .state = static_cast<State>(state_value),
            .source_raw_path = column_blob(statement->get(), 2),
            .prepared_raw_path = column_blob(statement->get(), 3),
            .backup_raw_path = column_blob(statement->get(), 4),
            .expected_revision = *expected,
            .prepared_revision = *prepared_revision,
            .published_revision = *published_revision,
            .occurrence_indexes = {},
            .content_kind = static_cast<ContentKind>(content_kind),
            .changes = {},
            .artwork = std::nullopt,
            .failure = std::move(failure),
        });
    }
    if (record_result != SQLITE_DONE) {
        return std::unexpected(database_error(database, "Could not read operation journals"));
    }
    statement->reset();
    for (auto& record : records) {
        // Only ever narrows what recovery does: a record marked here is
        // reported, never acted on, and nothing about it is written back.
        const auto mark_damaged = [&record] {
            record.state = State::needs_reconciliation;
            record.failure = core::Error{
                .code = core::ErrorCode::database,
                .message = "The journal's record of this operation is incomplete; check the "
                           "file before saving it again",
                .context = {},
            };
        };
        auto children = load_children(database, record);
        if (!children) {
            if (damaged == DamagedEvidence::reconcile) {
                mark_damaged();
                continue;
            }
            return std::unexpected(std::move(children.error()));
        }
        if (auto validated = validate_record_structure(record);
            !validated || !valid_loaded_state_evidence(record)) {
            if (damaged == DamagedEvidence::reconcile) {
                mark_damaged();
                continue;
            }
            return std::unexpected(core::Error{
                .code = core::ErrorCode::database,
                .message = "Operation journal contains inconsistent recovery evidence",
                .context = {{.key = "journal_id", .value = record.id.to_string()}},
            });
        }
    }
    return records;
}

[[nodiscard]] bool valid_backup_evidence(const BackupRecord& backup) {
    if (backup.operation.state != State::complete || backup.completed_at_unix_seconds <= 0 ||
        backup.updated_at_unix_seconds < backup.completed_at_unix_seconds) {
        return false;
    }
    switch (backup.state) {
    case BackupState::retained:
    case BackupState::released:
        return !backup.undo_id && !backup.failure;
    case BackupState::undoing:
    case BackupState::undone:
        return backup.undo_id && !backup.undo_id->is_nil() && !backup.failure;
    case BackupState::needs_reconciliation:
        return backup.failure.has_value() && (!backup.undo_id || !backup.undo_id->is_nil());
    }
    return false;
}

[[nodiscard]] core::Result<std::vector<BackupRecord>>
load_backup_records(sqlite3* database, const char* sql, const std::string_view id = {},
                    const DamagedEvidence damaged = DamagedEvidence::refuse) {
    auto statement = prepare(database, sql);
    if (!statement) {
        return std::unexpected(std::move(statement.error()));
    }
    if (!id.empty() && !bind_blob(statement->get(), 1, id)) {
        return std::unexpected(database_error(database, "Could not bind metadata-backup ID"));
    }
    struct BackupRow {
        std::string journal_id;
        BackupState state{BackupState::retained};
        std::optional<core::StableId> undo_id;
        std::int64_t completed_at{0};
        std::int64_t updated_at{0};
        std::optional<core::Error> failure;
    };
    std::vector<BackupRow> rows;
    int step = SQLITE_OK;
    while ((step = sqlite3_step(statement->get())) == SQLITE_ROW) {
        const auto journal_id = column_blob(statement->get(), 0);
        const auto state = sqlite3_column_int(statement->get(), 1);
        std::optional<core::StableId> undo_id;
        if (sqlite3_column_type(statement->get(), 2) != SQLITE_NULL) {
            auto parsed = core::StableId::parse(column_blob(statement->get(), 2));
            if (!parsed) {
                return std::unexpected(
                    database_error(database, "Metadata backup has an invalid undo ID"));
            }
            undo_id = *parsed;
        }
        std::optional<core::Error> failure;
        const auto error_type = sqlite3_column_type(statement->get(), 5);
        const auto message_type = sqlite3_column_type(statement->get(), 6);
        if (error_type != SQLITE_NULL || message_type != SQLITE_NULL) {
            const auto code = sqlite3_column_int(statement->get(), 5);
            if (error_type == SQLITE_NULL || message_type == SQLITE_NULL || code < 0 ||
                code > static_cast<int>(core::ErrorCode::invariant)) {
                return std::unexpected(
                    database_error(database, "Metadata backup has invalid failure evidence"));
            }
            failure = core::Error{.code = static_cast<core::ErrorCode>(code),
                                  .message = column_blob(statement->get(), 6),
                                  .context = {}};
        }
        if (!valid_backup_state(state) || journal_id.empty()) {
            return std::unexpected(
                database_error(database, "Metadata backup has an invalid record"));
        }
        rows.push_back(BackupRow{
            .journal_id = journal_id,
            .state = static_cast<BackupState>(state),
            .undo_id = undo_id,
            .completed_at = sqlite3_column_int64(statement->get(), 3),
            .updated_at = sqlite3_column_int64(statement->get(), 4),
            .failure = std::move(failure),
        });
        if (rows.size() > maximum_backup_records) {
            return std::unexpected(database_error(database, "Too many metadata backup records"));
        }
    }
    if (step != SQLITE_DONE) {
        return std::unexpected(database_error(database, "Could not read metadata backups"));
    }

    constexpr auto operation_sql =
        "SELECT id, state, source_path, prepared_path, backup_path, expected_device, "
        "expected_inode, expected_size, expected_mtime_seconds, expected_mtime_nanoseconds, "
        "prepared_device, prepared_inode, prepared_size, prepared_mtime_seconds, "
        "prepared_mtime_nanoseconds, published_device, published_inode, published_size, "
        "published_mtime_seconds, published_mtime_nanoseconds, error_code, error_message, "
        "content_kind "
        "FROM operation_journal WHERE id = ?";
    std::vector<BackupRecord> backups;
    backups.reserve(rows.size());
    for (auto& row : rows) {
        auto operations = load_records(database, operation_sql, row.journal_id, damaged);
        if (!operations || operations->size() != 1U) {
            return std::unexpected(
                operations ? database_error(database, "Metadata backup lost its journal")
                           : std::move(operations.error()));
        }
        BackupRecord backup{
            .operation = std::move(operations->front()),
            .state = row.state,
            .undo_id = row.undo_id,
            .completed_at_unix_seconds = row.completed_at,
            .updated_at_unix_seconds = row.updated_at,
            .failure = std::move(row.failure),
        };
        if (!valid_backup_evidence(backup)) {
            if (damaged == DamagedEvidence::refuse) {
                return std::unexpected(database_error(
                    database, "Metadata backup contains inconsistent lifecycle evidence"));
            }
            // Kept and reported, never released: maintenance leaves a backup
            // needing reconciliation alone.
            backup.state = BackupState::needs_reconciliation;
            backup.failure = backup.operation.failure.value_or(core::Error{
                .code = core::ErrorCode::database,
                .message = "Metadata backup contains inconsistent lifecycle evidence",
                .context = {},
            });
        }
        backups.push_back(std::move(backup));
    }
    return backups;
}

} // namespace

struct SqliteMetadataOperationJournal::Impl {
    sqlite3* database{nullptr};
    mutable std::mutex mutex;

    ~Impl() {
        if (database != nullptr) {
            sqlite3_close(database);
        }
    }
};

SqliteMetadataOperationJournal::SqliteMetadataOperationJournal(std::unique_ptr<Impl> implementation)
    : implementation_(std::move(implementation)) {}

SqliteMetadataOperationJournal::SqliteMetadataOperationJournal(
    SqliteMetadataOperationJournal&&) noexcept = default;
SqliteMetadataOperationJournal&
SqliteMetadataOperationJournal::operator=(SqliteMetadataOperationJournal&&) noexcept = default;
SqliteMetadataOperationJournal::~SqliteMetadataOperationJournal() = default;

core::Result<SqliteMetadataOperationJournal>
SqliteMetadataOperationJournal::open(const std::filesystem::path& path) {
    {
        auto migrated = ListRepository::open(path);
        if (!migrated) {
            return std::unexpected(std::move(migrated.error()));
        }
    }

    auto implementation = std::make_unique<Impl>();
    const auto& encoded = path.native();
    if (sqlite3_open_v2(encoded.c_str(), &implementation->database,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
        return std::unexpected(
            database_error(implementation->database, "Could not open metadata operation journal"));
    }
    sqlite3_busy_timeout(implementation->database, 2'000);
    static_cast<void>(sqlite3_limit(implementation->database, SQLITE_LIMIT_LENGTH,
                                    static_cast<int>(maximum_text_bytes)));
    if (auto result = execute(implementation->database, "PRAGMA foreign_keys = ON"); !result) {
        return std::unexpected(std::move(result.error()));
    }
    if (auto result = execute(implementation->database, "PRAGMA synchronous = FULL"); !result) {
        return std::unexpected(std::move(result.error()));
    }
    return SqliteMetadataOperationJournal{std::move(implementation)};
}

core::Result<void>
SqliteMetadataOperationJournal::create(const operations::MetadataOperationJournalRecord& record) {
    if (auto validated = validate_record(record); !validated) {
        return validated;
    }
    std::scoped_lock lock{implementation_->mutex};
    auto* database = implementation_->database;
    if (auto begun = execute(database, "BEGIN IMMEDIATE"); !begun) {
        return begun;
    }
    const auto rollback = [database] { static_cast<void>(execute(database, "ROLLBACK")); };
    auto insert = prepare(
        database,
        "INSERT INTO operation_journal("
        "id, kind, state, source_path, prepared_path, backup_path, expected_device, "
        "expected_inode, expected_size, expected_mtime_seconds, expected_mtime_nanoseconds, "
        "prepared_device, prepared_inode, prepared_size, prepared_mtime_seconds, "
        "prepared_mtime_nanoseconds, published_device, published_inode, published_size, "
        "published_mtime_seconds, published_mtime_nanoseconds, error_code, error_message, "
        "content_kind) "
        "VALUES(?, 0, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    const auto id = record.id.to_string();
    if (!insert || !bind_blob(insert->get(), 1, id) ||
        sqlite3_bind_int(insert->get(), 2, static_cast<int>(record.state)) != SQLITE_OK ||
        !bind_blob(insert->get(), 3, record.source_raw_path) ||
        !bind_blob(insert->get(), 4, record.prepared_raw_path) ||
        !bind_blob(insert->get(), 5, record.backup_raw_path) ||
        !bind_revision(insert->get(), 6, record.expected_revision) ||
        !bind_optional_revision(insert->get(), 11, record.prepared_revision) ||
        !bind_optional_revision(insert->get(), 16, record.published_revision) ||
        sqlite3_bind_null(insert->get(), 21) != SQLITE_OK ||
        sqlite3_bind_null(insert->get(), 22) != SQLITE_OK ||
        sqlite3_bind_int(insert->get(), 23, static_cast<int>(record.content_kind)) != SQLITE_OK) {
        auto error = insert ? database_error(database, "Could not bind operation journal")
                            : std::move(insert.error());
        rollback();
        return std::unexpected(std::move(error));
    }
    if (auto stepped = step_done(database, insert->get(), "Could not create operation journal");
        !stepped) {
        rollback();
        return stepped;
    }

    auto occurrence_insert = prepare(
        database, "INSERT INTO operation_journal_occurrences(journal_id, position, item_index) "
                  "VALUES(?, ?, ?)");
    auto change_insert = prepare(
        database,
        "INSERT INTO operation_journal_changes("
        "journal_id, position, field_index, canonical_name, property_name, exact_native_name, "
        "original_present, patch_kind) VALUES(?, ?, ?, ?, ?, ?, ?, ?)");
    auto value_insert =
        prepare(database,
                "INSERT INTO operation_journal_values("
                "journal_id, change_position, value_kind, position, value) VALUES(?, ?, ?, ?, ?)");
    auto intent_insert =
        prepare(database, "INSERT INTO operation_journal_intents("
                          "journal_id, change_position, position, item_index) VALUES(?, ?, ?, ?)");
    auto artwork_insert =
        prepare(database,
                "INSERT INTO operation_journal_artwork("
                "journal_id, intent_kind, target_ordinal, original_item_count, planned_item_count, "
                "original_target_fingerprint, replacement_fingerprint, "
                "original_inventory_fingerprint, planned_inventory_fingerprint) "
                "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?)");
    if (!occurrence_insert || !change_insert || !value_insert || !intent_insert ||
        !artwork_insert) {
        auto error = !occurrence_insert ? std::move(occurrence_insert.error())
                     : !change_insert   ? std::move(change_insert.error())
                     : !value_insert    ? std::move(value_insert.error())
                     : !intent_insert   ? std::move(intent_insert.error())
                                        : std::move(artwork_insert.error());
        rollback();
        return std::unexpected(std::move(error));
    }

    for (std::size_t position = 0U; position < record.occurrence_indexes.size(); ++position) {
        if (!bind_blob(occurrence_insert->get(), 1, id) ||
            !bind_index(occurrence_insert->get(), 2, position) ||
            !bind_index(occurrence_insert->get(), 3, record.occurrence_indexes[position])) {
            auto error = database_error(database, "Could not bind operation occurrence");
            rollback();
            return std::unexpected(std::move(error));
        }
        if (auto stepped = step_done(database, occurrence_insert->get(),
                                     "Could not store operation occurrence");
            !stepped) {
            rollback();
            return stepped;
        }
    }
    if (record.artwork) {
        const auto& artwork = *record.artwork;
        if (!bind_blob(artwork_insert->get(), 1, id) ||
            sqlite3_bind_int(artwork_insert->get(), 2, static_cast<int>(artwork.kind)) !=
                SQLITE_OK ||
            !bind_index(artwork_insert->get(), 3, artwork.target_ordinal) ||
            !bind_index(artwork_insert->get(), 4, artwork.original_item_count) ||
            !bind_index(artwork_insert->get(), 5, artwork.planned_item_count) ||
            !bind_optional_fingerprint(artwork_insert->get(), 6,
                                       artwork.original_target_fingerprint) ||
            !bind_optional_fingerprint(artwork_insert->get(), 7, artwork.replacement_fingerprint) ||
            !bind_fingerprint(artwork_insert->get(), 8, artwork.original_inventory_fingerprint) ||
            !bind_fingerprint(artwork_insert->get(), 9, artwork.planned_inventory_fingerprint)) {
            auto error = database_error(database, "Could not bind operation artwork evidence");
            rollback();
            return std::unexpected(std::move(error));
        }
        if (auto stepped = step_done(database, artwork_insert->get(),
                                     "Could not store operation artwork evidence");
            !stepped) {
            rollback();
            return stepped;
        }
    }
    for (std::size_t position = 0U; position < record.changes.size(); ++position) {
        const auto& change = record.changes[position];
        if (!bind_blob(change_insert->get(), 1, id) ||
            !bind_index(change_insert->get(), 2, position) ||
            !bind_index(change_insert->get(), 3, change.field_index) ||
            !bind_blob(change_insert->get(), 4, change.canonical_name) ||
            !bind_blob(change_insert->get(), 5, change.property_name) ||
            !bind_optional_blob(change_insert->get(), 6, change.exact_native_name) ||
            sqlite3_bind_int(change_insert->get(), 7, change.original_present ? 1 : 0) !=
                SQLITE_OK ||
            sqlite3_bind_int(change_insert->get(), 8, static_cast<int>(change.kind)) != SQLITE_OK) {
            auto error = database_error(database, "Could not bind operation change");
            rollback();
            return std::unexpected(std::move(error));
        }
        if (auto stepped =
                step_done(database, change_insert->get(), "Could not store operation change");
            !stepped) {
            rollback();
            return stepped;
        }
        const auto store_values =
            [&](const int value_kind,
                const std::span<const std::string> values) -> core::Result<void> {
            for (std::size_t value_position = 0U; value_position < values.size();
                 ++value_position) {
                if (!bind_blob(value_insert->get(), 1, id) ||
                    !bind_index(value_insert->get(), 2, position) ||
                    sqlite3_bind_int(value_insert->get(), 3, value_kind) != SQLITE_OK ||
                    !bind_index(value_insert->get(), 4, value_position) ||
                    !bind_blob(value_insert->get(), 5, values[value_position])) {
                    return std::unexpected(
                        database_error(database, "Could not bind operation value"));
                }
                if (auto stepped =
                        step_done(database, value_insert->get(), "Could not store operation value");
                    !stepped) {
                    return stepped;
                }
            }
            return {};
        };
        if (auto stored = store_values(0, change.original_values); !stored) {
            rollback();
            return stored;
        }
        if (auto stored = store_values(1, change.planned_values); !stored) {
            rollback();
            return stored;
        }
        for (std::size_t intent_position = 0U; intent_position < change.item_indexes.size();
             ++intent_position) {
            if (!bind_blob(intent_insert->get(), 1, id) ||
                !bind_index(intent_insert->get(), 2, position) ||
                !bind_index(intent_insert->get(), 3, intent_position) ||
                !bind_index(intent_insert->get(), 4, change.item_indexes[intent_position])) {
                auto error = database_error(database, "Could not bind operation intent");
                rollback();
                return std::unexpected(std::move(error));
            }
            if (auto stepped =
                    step_done(database, intent_insert->get(), "Could not store operation intent");
                !stepped) {
                rollback();
                return stepped;
            }
        }
    }
    if (auto committed = execute(database, "COMMIT"); !committed) {
        rollback();
        return committed;
    }
    return {};
}

core::Result<void> SqliteMetadataOperationJournal::transition(
    const core::StableId& id, const operations::MetadataOperationJournalTransition& transition) {
    if (id.is_nil()) {
        return std::unexpected(invalid_record("Operation-journal ID cannot be nil"));
    }
    if (auto validated = validate_transition(transition); !validated) {
        return validated;
    }
    std::scoped_lock lock{implementation_->mutex};
    auto* database = implementation_->database;
    if (auto begun = execute(database, "BEGIN IMMEDIATE"); !begun) {
        return begun;
    }
    const auto rollback = [database] { static_cast<void>(execute(database, "ROLLBACK")); };
    auto statement =
        prepare(database,
                "UPDATE operation_journal SET state = ?, prepared_device = ?, prepared_inode = ?, "
                "prepared_size = ?, prepared_mtime_seconds = ?, prepared_mtime_nanoseconds = ?, "
                "published_device = ?, published_inode = ?, published_size = ?, "
                "published_mtime_seconds = ?, published_mtime_nanoseconds = ?, error_code = ?, "
                "error_message = ? WHERE id = ? AND state = ?");
    if (!statement ||
        sqlite3_bind_int(statement->get(), 1, static_cast<int>(transition.state)) != SQLITE_OK ||
        !bind_optional_revision(statement->get(), 2, transition.prepared_revision) ||
        !bind_optional_revision(statement->get(), 7, transition.published_revision) ||
        (transition.failure
             ? (sqlite3_bind_int(statement->get(), 12,
                                 static_cast<int>(transition.failure->code)) != SQLITE_OK ||
                !bind_blob(statement->get(), 13, transition.failure->message))
             : (sqlite3_bind_null(statement->get(), 12) != SQLITE_OK ||
                sqlite3_bind_null(statement->get(), 13) != SQLITE_OK)) ||
        !bind_blob(statement->get(), 14, id.to_string()) ||
        sqlite3_bind_int(statement->get(), 15, static_cast<int>(transition.expected_state)) !=
            SQLITE_OK) {
        auto error = statement ? database_error(database, "Could not bind journal transition")
                               : std::move(statement.error());
        rollback();
        return std::unexpected(std::move(error));
    }
    if (sqlite3_step(statement->get()) != SQLITE_DONE) {
        auto error = database_error(database, "Could not update operation journal");
        rollback();
        return std::unexpected(std::move(error));
    }
    if (sqlite3_changes(database) != 1) {
        rollback();
        return std::unexpected(core::Error{
            .code = core::ErrorCode::conflict,
            .message = "Operation journal changed before the requested transition",
            .context = {{.key = "journal_id", .value = id.to_string()}},
        });
    }
    if (transition.state == State::complete) {
        auto backup = prepare(database, "INSERT INTO metadata_operation_backups("
                                        "journal_id, state, undo_id, completed_at_unix_seconds, "
                                        "updated_at_unix_seconds, error_code, error_message) "
                                        "VALUES(?, 0, NULL, "
                                        "CAST(strftime('%s', 'now') AS INTEGER), "
                                        "CAST(strftime('%s', 'now') AS INTEGER), NULL, NULL)");
        if (!backup || !bind_blob(backup->get(), 1, id.to_string())) {
            auto error = backup ? database_error(database, "Could not bind retained backup")
                                : std::move(backup.error());
            rollback();
            return std::unexpected(std::move(error));
        }
        if (auto stored =
                step_done(database, backup->get(), "Could not retain metadata-operation backup");
            !stored) {
            rollback();
            return stored;
        }
    }
    if (auto committed = execute(database, "COMMIT"); !committed) {
        rollback();
        return committed;
    }
    return {};
}

core::Result<std::optional<operations::MetadataOperationJournalRecord>>
SqliteMetadataOperationJournal::load(const core::StableId& id) const {
    if (id.is_nil()) {
        return std::unexpected(invalid_record("Operation-journal ID cannot be nil"));
    }
    std::scoped_lock lock{implementation_->mutex};
    constexpr auto sql =
        "SELECT id, state, source_path, prepared_path, backup_path, expected_device, "
        "expected_inode, expected_size, expected_mtime_seconds, expected_mtime_nanoseconds, "
        "prepared_device, prepared_inode, prepared_size, prepared_mtime_seconds, "
        "prepared_mtime_nanoseconds, published_device, published_inode, published_size, "
        "published_mtime_seconds, published_mtime_nanoseconds, error_code, error_message, "
        "content_kind "
        "FROM operation_journal WHERE id = ?";
    auto records = load_records(implementation_->database, sql, id.to_string());
    if (!records) {
        return std::unexpected(std::move(records.error()));
    }
    if (records->empty()) {
        return std::optional<Record>{};
    }
    if (records->size() != 1U) {
        return std::unexpected(
            database_error(implementation_->database, "Operation-journal ID is not unique"));
    }
    return std::optional{std::move(records->front())};
}

core::Result<std::vector<operations::MetadataOperationJournalRecord>>
SqliteMetadataOperationJournal::load_incomplete() const {
    std::scoped_lock lock{implementation_->mutex};
    constexpr auto sql =
        "SELECT id, state, source_path, prepared_path, backup_path, expected_device, "
        "expected_inode, expected_size, expected_mtime_seconds, expected_mtime_nanoseconds, "
        "prepared_device, prepared_inode, prepared_size, prepared_mtime_seconds, "
        "prepared_mtime_nanoseconds, published_device, published_inode, published_size, "
        "published_mtime_seconds, published_mtime_nanoseconds, error_code, error_message, "
        "content_kind "
        "FROM operation_journal WHERE state NOT IN (3, 4) ORDER BY rowid LIMIT 10001";
    auto records = load_records(implementation_->database, sql, {}, DamagedEvidence::reconcile);
    if (!records) {
        return std::unexpected(std::move(records.error()));
    }
    if (records->size() > maximum_incomplete_records) {
        return std::unexpected(
            database_error(implementation_->database, "Too many incomplete operation journals"));
    }
    return records;
}

core::Result<std::vector<operations::MetadataOperationJournalRecord>>
SqliteMetadataOperationJournal::load_incomplete_for_source(const std::string& raw_path) const {
    std::scoped_lock lock{implementation_->mutex};
    constexpr auto sql =
        "SELECT id, state, source_path, prepared_path, backup_path, expected_device, "
        "expected_inode, expected_size, expected_mtime_seconds, expected_mtime_nanoseconds, "
        "prepared_device, prepared_inode, prepared_size, prepared_mtime_seconds, "
        "prepared_mtime_nanoseconds, published_device, published_inode, published_size, "
        "published_mtime_seconds, published_mtime_nanoseconds, error_code, error_message, "
        "content_kind "
        "FROM operation_journal WHERE source_path = ? AND state NOT IN (3, 4) ORDER BY rowid LIMIT "
        "10001";
    auto records =
        load_records(implementation_->database, sql, raw_path, DamagedEvidence::reconcile);
    if (!records) {
        return std::unexpected(std::move(records.error()));
    }
    if (records->size() > maximum_incomplete_records) {
        return std::unexpected(
            database_error(implementation_->database, "Too many incomplete operation journals"));
    }
    return records;
}

core::Result<std::optional<operations::MetadataOperationBackupRecord>>
SqliteMetadataOperationJournal::load_backup(const core::StableId& id) const {
    if (id.is_nil()) {
        return std::unexpected(invalid_record("Metadata-backup ID cannot be nil"));
    }
    std::scoped_lock lock{implementation_->mutex};
    constexpr auto sql = "SELECT journal_id, state, undo_id, completed_at_unix_seconds, "
                         "updated_at_unix_seconds, error_code, error_message "
                         "FROM metadata_operation_backups WHERE journal_id = ?";
    auto backups = load_backup_records(implementation_->database, sql, id.to_string());
    if (!backups) {
        return std::unexpected(std::move(backups.error()));
    }
    if (backups->empty()) {
        return std::optional<BackupRecord>{};
    }
    if (backups->size() != 1U) {
        return std::unexpected(
            database_error(implementation_->database, "Metadata-backup ID is not unique"));
    }
    return std::optional{std::move(backups->front())};
}

core::Result<std::vector<operations::MetadataOperationBackupRecord>>
SqliteMetadataOperationJournal::load_backups() const {
    std::scoped_lock lock{implementation_->mutex};
    constexpr auto sql = "SELECT journal_id, state, undo_id, completed_at_unix_seconds, "
                         "updated_at_unix_seconds, error_code, error_message "
                         "FROM metadata_operation_backups "
                         "ORDER BY completed_at_unix_seconds DESC, rowid DESC LIMIT 10001";
    auto backups =
        load_backup_records(implementation_->database, sql, {}, DamagedEvidence::reconcile);
    if (!backups) {
        return std::unexpected(std::move(backups.error()));
    }
    if (backups->size() > maximum_backup_records) {
        return std::unexpected(
            database_error(implementation_->database, "Too many metadata backup records"));
    }
    return backups;
}

core::Result<void> SqliteMetadataOperationJournal::transition_backup(
    const core::StableId& id, const operations::MetadataOperationBackupTransition& transition) {
    if (id.is_nil()) {
        return std::unexpected(invalid_record("Metadata-backup ID cannot be nil"));
    }
    if (auto validated = validate_backup_transition(transition); !validated) {
        return validated;
    }
    std::scoped_lock lock{implementation_->mutex};
    auto* database = implementation_->database;
    if (auto begun = execute(database, "BEGIN IMMEDIATE"); !begun) {
        return begun;
    }
    const auto rollback = [database] { static_cast<void>(execute(database, "ROLLBACK")); };
    auto statement =
        prepare(database, "UPDATE metadata_operation_backups SET state = ?, undo_id = ?, "
                          "updated_at_unix_seconds = CAST(strftime('%s', 'now') AS INTEGER), "
                          "error_code = ?, error_message = ? "
                          "WHERE journal_id = ? AND state = ?");
    if (!statement) {
        auto error = std::move(statement.error());
        rollback();
        return std::unexpected(std::move(error));
    }
    const auto undo_bound = transition.undo_id
                                ? bind_blob(statement->get(), 2, transition.undo_id->to_string())
                                : sqlite3_bind_null(statement->get(), 2) == SQLITE_OK;
    const auto failure_bound =
        transition.failure
            ? sqlite3_bind_int(statement->get(), 3, static_cast<int>(transition.failure->code)) ==
                      SQLITE_OK &&
                  bind_blob(statement->get(), 4, transition.failure->message)
            : sqlite3_bind_null(statement->get(), 3) == SQLITE_OK &&
                  sqlite3_bind_null(statement->get(), 4) == SQLITE_OK;
    if (sqlite3_bind_int(statement->get(), 1, static_cast<int>(transition.state)) != SQLITE_OK ||
        !undo_bound || !failure_bound || !bind_blob(statement->get(), 5, id.to_string()) ||
        sqlite3_bind_int(statement->get(), 6, static_cast<int>(transition.expected_state)) !=
            SQLITE_OK) {
        auto error = database_error(database, "Could not bind backup transition");
        rollback();
        return std::unexpected(std::move(error));
    }
    if (sqlite3_step(statement->get()) != SQLITE_DONE) {
        auto error = database_error(database, "Could not update metadata backup");
        rollback();
        return std::unexpected(std::move(error));
    }
    if (sqlite3_changes(database) != 1) {
        rollback();
        return std::unexpected(core::Error{
            .code = core::ErrorCode::conflict,
            .message = "Metadata backup changed before the requested transition",
            .context = {{.key = "journal_id", .value = id.to_string()}},
        });
    }
    if (auto committed = execute(database, "COMMIT"); !committed) {
        rollback();
        return committed;
    }
    return {};
}

} // namespace trackknife::persistence

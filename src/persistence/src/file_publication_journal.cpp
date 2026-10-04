// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/persistence/file_publication_journal.hpp"

#include "trackknife/persistence/list_repository.hpp"

#include <sqlite3.h>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace trackknife::persistence {
namespace {

constexpr std::size_t maximum_occurrences = 100'000U;
constexpr std::size_t maximum_directories = 4'096U;
constexpr std::size_t maximum_incomplete_records = 10'000U;
constexpr std::size_t maximum_reversal_records = 1'024U;
constexpr std::size_t maximum_recent_records = 1'024U;
constexpr std::size_t maximum_text_bytes = 64U * 1024U * 1024U;

using Kind = operations::OutputPathPublicationKind;
using Content = operations::FilePublicationContentKind;
using State = operations::FilePublicationJournalState;
using Record = operations::FilePublicationJournalRecord;
using Transition = operations::FilePublicationJournalTransition;

struct StatementDeleter {
    void operator()(sqlite3_stmt* statement) const noexcept {
        if (statement != nullptr) {
            sqlite3_finalize(statement);
        }
    }
};

using Statement = std::unique_ptr<sqlite3_stmt, StatementDeleter>;

[[nodiscard]] core::Error database_error(sqlite3* database, std::string message) {
    if (database != nullptr) {
        message += ": ";
        message += sqlite3_errmsg(database);
    }
    return {.code = core::ErrorCode::database, .message = std::move(message), .context = {}};
}

[[nodiscard]] core::Error invalid_record(std::string message) {
    return {
        .code = core::ErrorCode::invalid_argument, .message = std::move(message), .context = {}};
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
        return std::unexpected(
            database_error(database, "Could not prepare file-publication statement"));
    }
    return Statement{raw};
}

[[nodiscard]] bool bind_blob(sqlite3_stmt* statement, const int index,
                             const std::string_view value) {
    return sqlite3_bind_blob64(statement, index, value.data(), value.size(), SQLITE_TRANSIENT) ==
           SQLITE_OK;
}

[[nodiscard]] std::string column_blob(sqlite3_stmt* statement, const int column) {
    const auto size = sqlite3_column_bytes(statement, column);
    const auto* data = static_cast<const char*>(sqlite3_column_blob(statement, column));
    return data == nullptr || size <= 0 ? std::string{} : std::string{data, data + size};
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
    const std::array values{encode_unsigned(revision.device), encode_unsigned(revision.inode),
                            encode_unsigned(revision.size),
                            encode_signed(revision.modification_time_seconds),
                            encode_signed(revision.modification_time_nanoseconds)};
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
    if (!device || !inode || !size || !seconds || !nanoseconds || *inode == 0U) {
        return std::unexpected(
            database_error(nullptr, "File-publication journal has an invalid revision"));
    }
    return core::LocalSourceRevision{.device = *device,
                                     .inode = *inode,
                                     .size = *size,
                                     .modification_time_seconds = *seconds,
                                     .modification_time_nanoseconds = *nanoseconds};
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
        return std::unexpected(
            database_error(nullptr, "File-publication journal has a partial revision"));
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

[[nodiscard]] bool bind_optional_id(sqlite3_stmt* statement, const int column,
                                    const std::optional<core::StableId>& id) {
    return id ? bind_blob(statement, column, id->to_string())
              : sqlite3_bind_null(statement, column) == SQLITE_OK;
}

[[nodiscard]] core::Result<std::size_t> read_index(sqlite3_stmt* statement, const int column) {
    const auto value = sqlite3_column_int64(statement, column);
    if (value < 0 || static_cast<std::uint64_t>(value) >
                         static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return std::unexpected(
            database_error(nullptr, "File-publication journal has an invalid index"));
    }
    return static_cast<std::size_t>(value);
}

[[nodiscard]] bool normal_absolute_file_path(const std::string& raw_path) {
    if (raw_path.empty() || raw_path.find('\0') != std::string::npos) {
        return false;
    }
    const std::filesystem::path path{raw_path};
    return path.is_absolute() && !path.filename().empty() && path == path.lexically_normal();
}

[[nodiscard]] bool contained_by(const std::filesystem::path& root,
                                const std::filesystem::path& target) {
    auto root_component = root.begin();
    auto target_component = target.begin();
    for (; root_component != root.end(); ++root_component, ++target_component) {
        if (target_component == target.end() || *root_component != *target_component) {
            return false;
        }
    }
    return target_component != target.end();
}

[[nodiscard]] bool valid_revision(const core::LocalSourceRevision& revision) {
    return revision.inode != 0U;
}

[[nodiscard]] bool
valid_optional_revision(const std::optional<core::LocalSourceRevision>& revision) {
    return !revision || valid_revision(*revision);
}

[[nodiscard]] bool prepared_lifecycle(const Kind kind, const Content content) {
    return kind == Kind::cross_filesystem_copy || content == Content::prepared_destination_artifact;
}

[[nodiscard]] bool valid_state_for_kind(const Kind kind, const Content content, const State state) {
    return prepared_lifecycle(kind, content) ||
           (state != State::target_prepared && state != State::source_removed);
}

[[nodiscard]] bool legal_transition(const Kind kind, const Content content, const State from,
                                    const State to) {
    const auto prepared = prepared_lifecycle(kind, content);
    if (to == State::needs_reconciliation) {
        return from != State::complete && from != State::rolled_back &&
               from != State::needs_reconciliation;
    }
    switch (from) {
    case State::planned:
        return (!prepared && to == State::target_published) ||
               (prepared && to == State::target_prepared) || to == State::rolled_back;
    case State::target_prepared:
        return prepared && (to == State::target_published || to == State::rolled_back);
    case State::target_published:
        return to == State::dependent_state_committed || to == State::rolled_back;
    case State::dependent_state_committed:
        return (!prepared && to == State::complete) || (prepared && to == State::source_removed);
    case State::source_removed:
        return prepared && to == State::complete;
    case State::complete:
    case State::rolled_back:
    case State::needs_reconciliation:
        return false;
    }
    return false;
}

[[nodiscard]] bool
valid_nonterminal_evidence(const Kind kind, const Content content, const State state,
                           const std::optional<core::LocalSourceRevision>& prepared_revision,
                           const std::optional<core::LocalSourceRevision>& target_revision) {
    if (!prepared_lifecycle(kind, content)) {
        if (prepared_revision) {
            return false;
        }
        return state == State::planned ? !target_revision : target_revision.has_value();
    }
    switch (state) {
    case State::planned:
        return !prepared_revision && !target_revision;
    case State::target_prepared:
        return prepared_revision && !target_revision;
    case State::target_published:
    case State::dependent_state_committed:
    case State::source_removed:
    case State::complete:
        return prepared_revision && target_revision;
    case State::rolled_back:
    case State::needs_reconciliation:
        break;
    }
    return false;
}

[[nodiscard]] bool
valid_failure_evidence(const Kind kind, const Content content,
                       const std::optional<core::LocalSourceRevision>& prepared_revision,
                       const std::optional<core::LocalSourceRevision>& target_revision) {
    return !prepared_lifecycle(kind, content) ? !prepared_revision
                                              : (!target_revision || prepared_revision);
}

[[nodiscard]] core::Result<void> validate_structure(const Record& record) {
    if (record.id.is_nil() ||
        (record.publication != Kind::same_filesystem_rename &&
         record.publication != Kind::cross_filesystem_copy) ||
        (record.content != Content::preserve_source_bytes &&
         record.content != Content::prepared_destination_artifact) ||
        !normal_absolute_file_path(record.source_raw_path) ||
        !normal_absolute_file_path(record.target_raw_path) ||
        record.source_raw_path == record.target_raw_path ||
        !valid_revision(record.expected_source_revision) ||
        !valid_optional_revision(record.prepared_revision) ||
        !valid_optional_revision(record.target_revision) || record.occurrence_indexes.empty() ||
        record.occurrence_indexes.size() > maximum_occurrences ||
        record.planned_missing_directory_raw_paths.size() > maximum_directories ||
        (record.reverses_journal_id && (record.reverses_journal_id == record.id ||
                                        record.publication != Kind::same_filesystem_rename ||
                                        record.content != Content::preserve_source_bytes)) ||
        (record.failure && !record.failure->context.empty())) {
        return std::unexpected(invalid_record("Invalid file-publication journal structure"));
    }
    if ((!prepared_lifecycle(record.publication, record.content) &&
         !record.prepared_raw_path.empty()) ||
        (prepared_lifecycle(record.publication, record.content) &&
         (!normal_absolute_file_path(record.prepared_raw_path) ||
          std::filesystem::path{record.prepared_raw_path} !=
              operations::file_publication_prepared_path(
                  std::filesystem::path{record.target_raw_path}, record.id)))) {
        return std::unexpected(invalid_record("Invalid prepared publication path"));
    }
    std::size_t text_bytes = record.source_raw_path.size() + record.target_raw_path.size() +
                             record.prepared_raw_path.size();
    if (text_bytes > maximum_text_bytes ||
        (record.failure && record.failure->message.size() > maximum_text_bytes - text_bytes)) {
        return std::unexpected(invalid_record("File-publication journal text exceeds its limit"));
    }
    if (record.failure) {
        text_bytes += record.failure->message.size();
    }
    for (std::size_t index = 0U; index < record.occurrence_indexes.size(); ++index) {
        if ((index > 0U &&
             record.occurrence_indexes[index - 1U] >= record.occurrence_indexes[index]) ||
            record.occurrence_indexes[index] >
                static_cast<std::size_t>(std::numeric_limits<sqlite3_int64>::max())) {
            return std::unexpected(
                invalid_record("File-publication occurrence indexes must be sorted and unique"));
        }
    }
    const auto target = std::filesystem::path{record.target_raw_path};
    if (!record.planned_missing_directory_raw_paths.empty() &&
        std::filesystem::path{record.planned_missing_directory_raw_paths.back()} !=
            target.parent_path()) {
        return std::unexpected(
            invalid_record("Planned directory chain does not reach the target parent"));
    }
    for (std::size_t index = 0U; index < record.planned_missing_directory_raw_paths.size();
         ++index) {
        const auto& raw = record.planned_missing_directory_raw_paths[index];
        if (!normal_absolute_file_path(raw) || !contained_by(std::filesystem::path{raw}, target) ||
            (index > 0U &&
             std::filesystem::path{raw}.parent_path() !=
                 std::filesystem::path{record.planned_missing_directory_raw_paths[index - 1U]}) ||
            raw.size() > maximum_text_bytes - text_bytes) {
            return std::unexpected(
                invalid_record("Invalid planned file-publication directory chain"));
        }
        text_bytes += raw.size();
    }
    return {};
}

[[nodiscard]] core::Result<void> validate_record(const Record& record) {
    if (record.state != State::planned || record.prepared_revision || record.target_revision ||
        record.failure) {
        return std::unexpected(invalid_record("Invalid planned file-publication journal"));
    }
    return validate_structure(record);
}

[[nodiscard]] core::Result<void> validate_reversal_parent(sqlite3* database, const Record& record) {
    if (!record.reverses_journal_id) {
        return {};
    }
    auto parent = prepare(
        database,
        "SELECT state, publication_kind, source_path, target_path, target_device, target_inode, "
        "target_size, target_mtime_seconds, target_mtime_nanoseconds, reverses_id "
        ", content_kind FROM file_publication_journal WHERE id = ?");
    if (!parent || !bind_blob(parent->get(), 1, record.reverses_journal_id->to_string())) {
        return std::unexpected(parent ? database_error(database, "Could not bind reversal parent")
                                      : std::move(parent.error()));
    }
    const auto parent_step = sqlite3_step(parent->get());
    if (parent_step == SQLITE_DONE) {
        return std::unexpected(invalid_record("File-publication reversal parent does not exist"));
    }
    if (parent_step != SQLITE_ROW) {
        return std::unexpected(database_error(database, "Could not load reversal parent"));
    }
    auto target_revision = read_optional_revision(parent->get(), 4);
    if (!target_revision) {
        return std::unexpected(std::move(target_revision.error()));
    }
    if (sqlite3_column_int(parent->get(), 0) != static_cast<int>(State::complete) ||
        sqlite3_column_int(parent->get(), 1) != static_cast<int>(Kind::same_filesystem_rename) ||
        column_blob(parent->get(), 2) != record.target_raw_path ||
        column_blob(parent->get(), 3) != record.source_raw_path || !*target_revision ||
        **target_revision != record.expected_source_revision ||
        sqlite3_column_type(parent->get(), 9) != SQLITE_NULL ||
        sqlite3_column_int(parent->get(), 10) != static_cast<int>(Content::preserve_source_bytes) ||
        !record.planned_missing_directory_raw_paths.empty()) {
        return std::unexpected(
            invalid_record("File-publication reversal does not match its completed original"));
    }

    auto occurrences =
        prepare(database, "SELECT position, item_index FROM file_publication_journal_occurrences "
                          "WHERE journal_id = ? ORDER BY position");
    if (!occurrences ||
        !bind_blob(occurrences->get(), 1, record.reverses_journal_id->to_string())) {
        return std::unexpected(occurrences
                                   ? database_error(database, "Could not bind reversal occurrences")
                                   : std::move(occurrences.error()));
    }
    std::size_t position = 0U;
    int step = SQLITE_ROW;
    while ((step = sqlite3_step(occurrences->get())) == SQLITE_ROW) {
        auto persisted_position = read_index(occurrences->get(), 0);
        auto item_index = read_index(occurrences->get(), 1);
        if (!persisted_position || !item_index || *persisted_position != position ||
            position >= record.occurrence_indexes.size() ||
            record.occurrence_indexes[position] != *item_index) {
            return std::unexpected(
                invalid_record("File-publication reversal occurrence evidence differs"));
        }
        ++position;
    }
    if (step != SQLITE_DONE) {
        return std::unexpected(database_error(database, "Could not load reversal occurrences"));
    }
    if (position != record.occurrence_indexes.size()) {
        return std::unexpected(
            invalid_record("File-publication reversal occurrence evidence differs"));
    }
    return {};
}

[[nodiscard]] core::Result<void> validate_transition(const Kind kind, const Content content,
                                                     const Transition& transition) {
    if (!legal_transition(kind, content, transition.expected_state, transition.state)) {
        return std::unexpected(invalid_record("Invalid file-publication state transition"));
    }
    const bool failure_state =
        transition.state == State::rolled_back || transition.state == State::needs_reconciliation;
    if (failure_state != transition.failure.has_value() ||
        (transition.failure && !transition.failure->context.empty()) ||
        !valid_optional_revision(transition.prepared_revision) ||
        !valid_optional_revision(transition.target_revision)) {
        return std::unexpected(
            invalid_record("File-publication failure state has invalid evidence"));
    }
    if (failure_state) {
        if (!valid_nonterminal_evidence(kind, content, transition.expected_state,
                                        transition.prepared_revision, transition.target_revision) ||
            !valid_failure_evidence(kind, content, transition.prepared_revision,
                                    transition.target_revision)) {
            return std::unexpected(
                invalid_record("File-publication failure lost its current revision evidence"));
        }
        return {};
    }
    if (!valid_nonterminal_evidence(kind, content, transition.state, transition.prepared_revision,
                                    transition.target_revision)) {
        return std::unexpected(
            invalid_record("File-publication transition has invalid revision evidence"));
    }
    return {};
}

[[nodiscard]] bool valid_loaded_evidence(const Record& record) {
    if (!valid_state_for_kind(record.publication, record.content, record.state)) {
        return false;
    }
    if (record.state == State::rolled_back || record.state == State::needs_reconciliation) {
        return record.failure &&
               valid_failure_evidence(record.publication, record.content, record.prepared_revision,
                                      record.target_revision);
    }
    return !record.failure &&
           valid_nonterminal_evidence(record.publication, record.content, record.state,
                                      record.prepared_revision, record.target_revision);
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
    const auto id = record.id.to_string();
    auto occurrences =
        prepare(database, "SELECT position, item_index FROM file_publication_journal_occurrences "
                          "WHERE journal_id = ? ORDER BY position");
    if (!occurrences || !bind_blob(occurrences->get(), 1, id)) {
        return std::unexpected(occurrences ? database_error(database, "Could not bind journal ID")
                                           : std::move(occurrences.error()));
    }
    int row = SQLITE_OK;
    while ((row = sqlite3_step(occurrences->get())) == SQLITE_ROW) {
        auto position = read_index(occurrences->get(), 0);
        auto item_index = read_index(occurrences->get(), 1);
        if (!position || !item_index || *position != record.occurrence_indexes.size() ||
            record.occurrence_indexes.size() == maximum_occurrences) {
            return std::unexpected(database_error(database, "Invalid file-publication occurrence"));
        }
        record.occurrence_indexes.push_back(*item_index);
    }
    if (row != SQLITE_DONE) {
        return std::unexpected(
            database_error(database, "Could not load file-publication occurrences"));
    }

    auto directories =
        prepare(database, "SELECT position, raw_path FROM file_publication_journal_directories "
                          "WHERE journal_id = ? ORDER BY position");
    if (!directories || !bind_blob(directories->get(), 1, id)) {
        return std::unexpected(directories ? database_error(database, "Could not bind journal ID")
                                           : std::move(directories.error()));
    }
    while ((row = sqlite3_step(directories->get())) == SQLITE_ROW) {
        auto position = read_index(directories->get(), 0);
        if (!position || *position != record.planned_missing_directory_raw_paths.size() ||
            record.planned_missing_directory_raw_paths.size() == maximum_directories) {
            return std::unexpected(database_error(database, "Invalid file-publication directory"));
        }
        record.planned_missing_directory_raw_paths.push_back(column_blob(directories->get(), 1));
    }
    if (row != SQLITE_DONE) {
        return std::unexpected(
            database_error(database, "Could not load file-publication directories"));
    }
    return {};
}

[[nodiscard]] core::Result<std::vector<Record>> load_records(sqlite3* database, const char* sql,
                                                             const std::string_view id = {}) {
    auto statement = prepare(database, sql);
    if (!statement) {
        return std::unexpected(std::move(statement.error()));
    }
    if (!id.empty() && !bind_blob(statement->get(), 1, id)) {
        return std::unexpected(database_error(database, "Could not bind file-publication ID"));
    }
    std::vector<Record> records;
    int row = SQLITE_OK;
    while ((row = sqlite3_step(statement->get())) == SQLITE_ROW) {
        const auto id_text = column_blob(statement->get(), 0);
        auto parsed_id = core::StableId::parse(id_text);
        const auto state_value = sqlite3_column_int(statement->get(), 1);
        const auto kind_value = sqlite3_column_int(statement->get(), 2);
        const auto content_value = sqlite3_column_int(statement->get(), 24);
        auto expected = read_revision(statement->get(), 6);
        auto prepared_revision = read_optional_revision(statement->get(), 11);
        auto target_revision = read_optional_revision(statement->get(), 16);
        std::optional<core::StableId> reverses_journal_id;
        if (sqlite3_column_type(statement->get(), 23) != SQLITE_NULL) {
            auto parsed = core::StableId::parse(column_blob(statement->get(), 23));
            if (!parsed || parsed->is_nil()) {
                return std::unexpected(
                    database_error(database, "Invalid file-publication reversal identity"));
            }
            reverses_journal_id = *parsed;
        }
        if (!parsed_id || parsed_id->is_nil() || state_value < 0 || state_value > 7 ||
            kind_value < static_cast<int>(Kind::same_filesystem_rename) ||
            kind_value > static_cast<int>(Kind::cross_filesystem_copy) ||
            content_value < static_cast<int>(Content::preserve_source_bytes) ||
            content_value > static_cast<int>(Content::prepared_destination_artifact) || !expected ||
            !prepared_revision || !target_revision) {
            return std::unexpected(
                database_error(database, "Invalid file-publication journal row"));
        }
        std::optional<core::Error> failure;
        const bool has_code = sqlite3_column_type(statement->get(), 21) != SQLITE_NULL;
        const bool has_message = sqlite3_column_type(statement->get(), 22) != SQLITE_NULL;
        if (has_code != has_message) {
            return std::unexpected(
                database_error(database, "Partial file-publication failure evidence"));
        }
        if (has_code) {
            const auto code = sqlite3_column_int(statement->get(), 21);
            if (code < static_cast<int>(core::ErrorCode::cancelled) ||
                code > static_cast<int>(core::ErrorCode::invariant)) {
                return std::unexpected(
                    database_error(database, "Invalid file-publication error code"));
            }
            failure = core::Error{.code = static_cast<core::ErrorCode>(code),
                                  .message = column_blob(statement->get(), 22),
                                  .context = {}};
        }
        Record record{.id = *parsed_id,
                      .state = static_cast<State>(state_value),
                      .publication = static_cast<Kind>(kind_value),
                      .content = static_cast<Content>(content_value),
                      .source_raw_path = column_blob(statement->get(), 3),
                      .target_raw_path = column_blob(statement->get(), 4),
                      .prepared_raw_path = column_blob(statement->get(), 5),
                      .expected_source_revision = *expected,
                      .prepared_revision = *prepared_revision,
                      .target_revision = *target_revision,
                      .occurrence_indexes = {},
                      .planned_missing_directory_raw_paths = {},
                      .reverses_journal_id = reverses_journal_id,
                      .failure = std::move(failure)};
        if (auto children = load_children(database, record); !children) {
            return std::unexpected(std::move(children.error()));
        }
        if (auto structure = validate_structure(record);
            !structure || !valid_loaded_evidence(record)) {
            return std::unexpected(
                database_error(database, "Invalid persisted file-publication evidence"));
        }
        records.push_back(std::move(record));
    }
    if (row != SQLITE_DONE) {
        return std::unexpected(database_error(database, "Could not load file-publication rows"));
    }
    return records;
}

constexpr auto record_columns =
    "id, state, publication_kind, source_path, target_path, prepared_path, expected_device, "
    "expected_inode, expected_size, expected_mtime_seconds, expected_mtime_nanoseconds, "
    "prepared_device, prepared_inode, prepared_size, prepared_mtime_seconds, "
    "prepared_mtime_nanoseconds, target_device, target_inode, target_size, "
    "target_mtime_seconds, target_mtime_nanoseconds, error_code, error_message, reverses_id, "
    "content_kind ";

} // namespace

struct SqliteFilePublicationJournal::Impl {
    sqlite3* database{nullptr};
    mutable std::mutex mutex;

    ~Impl() {
        if (database != nullptr) {
            sqlite3_close(database);
        }
    }
};

SqliteFilePublicationJournal::SqliteFilePublicationJournal(std::unique_ptr<Impl> implementation)
    : implementation_(std::move(implementation)) {}

SqliteFilePublicationJournal::SqliteFilePublicationJournal(
    SqliteFilePublicationJournal&&) noexcept = default;
SqliteFilePublicationJournal&
SqliteFilePublicationJournal::operator=(SqliteFilePublicationJournal&&) noexcept = default;
SqliteFilePublicationJournal::~SqliteFilePublicationJournal() = default;

core::Result<SqliteFilePublicationJournal>
SqliteFilePublicationJournal::open(const std::filesystem::path& path) {
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
            database_error(implementation->database, "Could not open file-publication journal"));
    }
    sqlite3_busy_timeout(implementation->database, 2'000);
    static_cast<void>(sqlite3_limit(implementation->database, SQLITE_LIMIT_LENGTH,
                                    static_cast<int>(maximum_text_bytes)));
    if (auto foreign_keys = execute(implementation->database, "PRAGMA foreign_keys = ON");
        !foreign_keys) {
        return std::unexpected(std::move(foreign_keys.error()));
    }
    if (auto synchronous = execute(implementation->database, "PRAGMA synchronous = FULL");
        !synchronous) {
        return std::unexpected(std::move(synchronous.error()));
    }
    return SqliteFilePublicationJournal{std::move(implementation)};
}

core::Result<void>
SqliteFilePublicationJournal::create(const operations::FilePublicationJournalRecord& record) {
    if (auto valid = validate_record(record); !valid) {
        return valid;
    }
    std::scoped_lock lock{implementation_->mutex};
    auto* database = implementation_->database;
    if (auto begun = execute(database, "BEGIN IMMEDIATE"); !begun) {
        return begun;
    }
    const auto rollback = [database] { static_cast<void>(execute(database, "ROLLBACK")); };
    if (auto parent = validate_reversal_parent(database, record); !parent) {
        auto error = std::move(parent.error());
        rollback();
        return std::unexpected(std::move(error));
    }
    const auto insert_sql = "INSERT INTO file_publication_journal(" + std::string{record_columns} +
                            ") VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
    auto insert = prepare(database, insert_sql.c_str());
    const auto id = record.id.to_string();
    if (!insert || !bind_blob(insert->get(), 1, id) ||
        sqlite3_bind_int(insert->get(), 2, static_cast<int>(record.state)) != SQLITE_OK ||
        sqlite3_bind_int(insert->get(), 3, static_cast<int>(record.publication)) != SQLITE_OK ||
        !bind_blob(insert->get(), 4, record.source_raw_path) ||
        !bind_blob(insert->get(), 5, record.target_raw_path) ||
        !bind_blob(insert->get(), 6, record.prepared_raw_path) ||
        !bind_revision(insert->get(), 7, record.expected_source_revision) ||
        !bind_optional_revision(insert->get(), 12, record.prepared_revision) ||
        !bind_optional_revision(insert->get(), 17, record.target_revision) ||
        sqlite3_bind_null(insert->get(), 22) != SQLITE_OK ||
        sqlite3_bind_null(insert->get(), 23) != SQLITE_OK ||
        !bind_optional_id(insert->get(), 24, record.reverses_journal_id) ||
        sqlite3_bind_int(insert->get(), 25, static_cast<int>(record.content)) != SQLITE_OK) {
        auto error = insert ? database_error(database, "Could not bind file-publication journal")
                            : std::move(insert.error());
        rollback();
        return std::unexpected(std::move(error));
    }
    if (auto stored = step_done(database, insert->get(), "Could not create file publication");
        !stored) {
        rollback();
        return stored;
    }

    auto occurrence = prepare(database, "INSERT INTO file_publication_journal_occurrences"
                                        "(journal_id, position, item_index) VALUES(?,?,?)");
    auto directory = prepare(database, "INSERT INTO file_publication_journal_directories"
                                       "(journal_id, position, raw_path) VALUES(?,?,?)");
    if (!occurrence || !directory) {
        auto error = !occurrence ? std::move(occurrence.error()) : std::move(directory.error());
        rollback();
        return std::unexpected(std::move(error));
    }
    for (std::size_t index = 0U; index < record.occurrence_indexes.size(); ++index) {
        if (!bind_blob(occurrence->get(), 1, id) || !bind_index(occurrence->get(), 2, index) ||
            !bind_index(occurrence->get(), 3, record.occurrence_indexes[index])) {
            auto error = database_error(database, "Could not bind file-publication occurrence");
            rollback();
            return std::unexpected(std::move(error));
        }
        if (auto stored = step_done(database, occurrence->get(),
                                    "Could not store file-publication occurrence");
            !stored) {
            rollback();
            return stored;
        }
    }
    for (std::size_t index = 0U; index < record.planned_missing_directory_raw_paths.size();
         ++index) {
        if (!bind_blob(directory->get(), 1, id) || !bind_index(directory->get(), 2, index) ||
            !bind_blob(directory->get(), 3, record.planned_missing_directory_raw_paths[index])) {
            auto error = database_error(database, "Could not bind file-publication directory");
            rollback();
            return std::unexpected(std::move(error));
        }
        if (auto stored =
                step_done(database, directory->get(), "Could not store file-publication directory");
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

core::Result<void> SqliteFilePublicationJournal::transition(
    const core::StableId& id, const operations::FilePublicationJournalTransition& transition) {
    if (id.is_nil()) {
        return std::unexpected(invalid_record("File-publication journal ID cannot be nil"));
    }
    std::scoped_lock lock{implementation_->mutex};
    auto* database = implementation_->database;
    auto kind_query =
        prepare(database,
                "SELECT publication_kind, content_kind FROM file_publication_journal WHERE id = ?");
    if (!kind_query || !bind_blob(kind_query->get(), 1, id.to_string())) {
        return std::unexpected(kind_query ? database_error(database, "Could not bind publication")
                                          : std::move(kind_query.error()));
    }
    const auto kind_result = sqlite3_step(kind_query->get());
    if (kind_result == SQLITE_DONE) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::not_found,
            .message = "File-publication journal was not found",
            .context = {{.key = "journal_id", .value = id.to_string()}},
        });
    }
    if (kind_result != SQLITE_ROW) {
        return std::unexpected(database_error(database, "Could not load publication"));
    }
    const auto kind_value = sqlite3_column_int(kind_query->get(), 0);
    const auto content_value = sqlite3_column_int(kind_query->get(), 1);
    kind_query->reset();
    if (kind_value < static_cast<int>(Kind::same_filesystem_rename) ||
        kind_value > static_cast<int>(Kind::cross_filesystem_copy) ||
        content_value < static_cast<int>(Content::preserve_source_bytes) ||
        content_value > static_cast<int>(Content::prepared_destination_artifact)) {
        return std::unexpected(database_error(database, "Invalid publication kind"));
    }
    if (auto valid = validate_transition(static_cast<Kind>(kind_value),
                                         static_cast<Content>(content_value), transition);
        !valid) {
        return valid;
    }
    if (auto begun = execute(database, "BEGIN IMMEDIATE"); !begun) {
        return begun;
    }
    const auto rollback = [database] { static_cast<void>(execute(database, "ROLLBACK")); };
    auto statement =
        prepare(database,
                "UPDATE file_publication_journal SET state=?, prepared_device=?, prepared_inode=?, "
                "prepared_size=?, prepared_mtime_seconds=?, prepared_mtime_nanoseconds=?, "
                "target_device=?, target_inode=?, target_size=?, target_mtime_seconds=?, "
                "target_mtime_nanoseconds=?, error_code=?, error_message=? WHERE id=? AND state=?");
    if (!statement ||
        sqlite3_bind_int(statement->get(), 1, static_cast<int>(transition.state)) != SQLITE_OK ||
        !bind_optional_revision(statement->get(), 2, transition.prepared_revision) ||
        !bind_optional_revision(statement->get(), 7, transition.target_revision) ||
        (transition.failure
             ? (sqlite3_bind_int(statement->get(), 12,
                                 static_cast<int>(transition.failure->code)) != SQLITE_OK ||
                !bind_blob(statement->get(), 13, transition.failure->message))
             : (sqlite3_bind_null(statement->get(), 12) != SQLITE_OK ||
                sqlite3_bind_null(statement->get(), 13) != SQLITE_OK)) ||
        !bind_blob(statement->get(), 14, id.to_string()) ||
        sqlite3_bind_int(statement->get(), 15, static_cast<int>(transition.expected_state)) !=
            SQLITE_OK) {
        auto error = statement ? database_error(database, "Could not bind publication transition")
                               : std::move(statement.error());
        rollback();
        return std::unexpected(std::move(error));
    }
    if (auto updated =
            step_done(database, statement->get(), "Could not update file-publication journal");
        !updated) {
        rollback();
        return updated;
    }
    if (sqlite3_changes(database) != 1) {
        rollback();
        return std::unexpected(core::Error{
            .code = core::ErrorCode::conflict,
            .message = "File-publication journal changed before the requested transition",
            .context = {{.key = "journal_id", .value = id.to_string()}},
        });
    }
    // ADR-0263: completed, a publication that wrote a new file keeps the
    // source it replaced, retained for undo.
    if (transition.state == operations::FilePublicationJournalState::complete &&
        (static_cast<Kind>(kind_value) == Kind::cross_filesystem_copy ||
         static_cast<Content>(content_value) == Content::prepared_destination_artifact)) {
        auto backup =
            prepare(database, "INSERT INTO file_publication_backups(journal_id, state, undo_id, "
                              "completed_at_unix_seconds, updated_at_unix_seconds, error_code, "
                              "error_message) VALUES(?, 0, NULL, "
                              "CAST(strftime('%s', 'now') AS INTEGER), "
                              "CAST(strftime('%s', 'now') AS INTEGER), NULL, NULL)");
        if (!backup || !bind_blob(backup->get(), 1, id.to_string())) {
            auto error = backup ? database_error(database, "Could not bind retained source")
                                : std::move(backup.error());
            rollback();
            return std::unexpected(std::move(error));
        }
        if (auto stored = step_done(database, backup->get(), "Could not retain the source");
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

core::Result<std::optional<operations::FilePublicationJournalRecord>>
SqliteFilePublicationJournal::load(const core::StableId& id) const {
    if (id.is_nil()) {
        return std::unexpected(invalid_record("File-publication journal ID cannot be nil"));
    }
    std::scoped_lock lock{implementation_->mutex};
    const auto sql =
        "SELECT " + std::string{record_columns} + "FROM file_publication_journal WHERE id = ?";
    auto records = load_records(implementation_->database, sql.c_str(), id.to_string());
    if (!records) {
        return std::unexpected(std::move(records.error()));
    }
    if (records->empty()) {
        return std::optional<Record>{};
    }
    if (records->size() != 1U) {
        return std::unexpected(
            database_error(implementation_->database, "File-publication ID is not unique"));
    }
    return std::optional{std::move(records->front())};
}

core::Result<std::vector<operations::FilePublicationJournalRecord>>
SqliteFilePublicationJournal::load_incomplete() const {
    std::scoped_lock lock{implementation_->mutex};
    const auto sql = "SELECT " + std::string{record_columns} +
                     "FROM file_publication_journal WHERE state NOT IN (5,6) "
                     "ORDER BY rowid LIMIT 10001";
    auto records = load_records(implementation_->database, sql.c_str());
    if (!records) {
        return std::unexpected(std::move(records.error()));
    }
    if (records->size() > maximum_incomplete_records) {
        return std::unexpected(
            database_error(implementation_->database, "Too many incomplete file publications"));
    }
    return records;
}

core::Result<std::vector<operations::FilePublicationJournalRecord>>
SqliteFilePublicationJournal::load_reversals(const core::StableId& journal_id) const {
    if (journal_id.is_nil()) {
        return std::unexpected(invalid_record("File-publication journal ID cannot be nil"));
    }
    std::scoped_lock lock{implementation_->mutex};
    const auto sql = "SELECT " + std::string{record_columns} +
                     "FROM file_publication_journal WHERE reverses_id = ? "
                     "ORDER BY rowid LIMIT 1025";
    auto records = load_records(implementation_->database, sql.c_str(), journal_id.to_string());
    if (!records) {
        return std::unexpected(std::move(records.error()));
    }
    if (records->size() > maximum_reversal_records) {
        return std::unexpected(
            database_error(implementation_->database, "Too many file-publication reversals"));
    }
    return records;
}

namespace {

using BackupState = operations::MetadataOperationBackupState;

// The backup rows asked for, each with its publication.
[[nodiscard]] core::Result<std::vector<operations::FilePublicationBackupRecord>>
load_publication_backups(sqlite3* database, const char* sql, const std::string& id = {}) {
    auto statement = prepare(database, sql);
    if (!statement || (!id.empty() && !bind_blob(statement->get(), 1, id))) {
        return std::unexpected(statement ? database_error(database, "Could not bind backup")
                                         : std::move(statement.error()));
    }
    struct Row {
        std::string id;
        int state{0};
        std::optional<std::string> undo_id;
        std::int64_t completed{0};
        std::int64_t updated{0};
        std::optional<core::Error> failure;
    };
    std::vector<Row> rows;
    while (true) {
        const auto stepped = sqlite3_step(statement->get());
        if (stepped == SQLITE_DONE) {
            break;
        }
        if (stepped != SQLITE_ROW) {
            return std::unexpected(database_error(database, "Could not load retained sources"));
        }
        const auto text = [&statement](const int column) {
            const auto* bytes =
                static_cast<const char*>(sqlite3_column_blob(statement->get(), column));
            return std::string{bytes == nullptr ? "" : bytes,
                               static_cast<std::size_t>(sqlite3_column_bytes(statement->get(),
                                                                             column))};
        };
        Row row{.id = text(0),
                .state = sqlite3_column_int(statement->get(), 1),
                .undo_id = sqlite3_column_type(statement->get(), 2) == SQLITE_NULL
                               ? std::nullopt
                               : std::optional{text(2)},
                .completed = sqlite3_column_int64(statement->get(), 3),
                .updated = sqlite3_column_int64(statement->get(), 4),
                .failure = std::nullopt};
        if (sqlite3_column_type(statement->get(), 5) != SQLITE_NULL) {
            row.failure = core::Error{
                .code = static_cast<core::ErrorCode>(sqlite3_column_int(statement->get(), 5)),
                .message = text(6),
                .context = {}};
        }
        if (row.state < static_cast<int>(BackupState::retained) ||
            row.state > static_cast<int>(BackupState::needs_reconciliation)) {
            return std::unexpected(database_error(database, "Invalid retained-source state"));
        }
        rows.push_back(std::move(row));
    }
    statement->reset();
    std::vector<operations::FilePublicationBackupRecord> backups;
    backups.reserve(rows.size());
    const auto record_sql =
        "SELECT " + std::string{record_columns} + "FROM file_publication_journal WHERE id = ?";
    for (auto& row : rows) {
        auto records = load_records(database, record_sql.c_str(), row.id);
        if (!records) {
            return std::unexpected(std::move(records.error()));
        }
        if (records->size() != 1U) {
            return std::unexpected(database_error(database, "Retained source has no publication"));
        }
        std::optional<core::StableId> undo_id;
        if (row.undo_id) {
            auto parsed = core::StableId::parse(*row.undo_id);
            if (!parsed) {
                return std::unexpected(database_error(database, "Invalid undo ID"));
            }
            undo_id = *parsed;
        }
        backups.push_back(operations::FilePublicationBackupRecord{
            .publication = std::move(records->front()),
            .state = static_cast<BackupState>(row.state),
            .undo_id = undo_id,
            .completed_at_unix_seconds = row.completed,
            .updated_at_unix_seconds = row.updated,
            .failure = std::move(row.failure),
        });
    }
    return backups;
}

} // namespace

core::Result<std::optional<operations::FilePublicationBackupRecord>>
SqliteFilePublicationJournal::load_backup(const core::StableId& id) const {
    if (id.is_nil()) {
        return std::unexpected(invalid_record("Retained-source ID cannot be nil"));
    }
    std::scoped_lock lock{implementation_->mutex};
    auto backups = load_publication_backups(
        implementation_->database,
        "SELECT journal_id, state, undo_id, completed_at_unix_seconds, updated_at_unix_seconds, "
        "error_code, error_message FROM file_publication_backups WHERE journal_id = ?",
        id.to_string());
    if (!backups) {
        return std::unexpected(std::move(backups.error()));
    }
    if (backups->empty()) {
        return std::optional<operations::FilePublicationBackupRecord>{};
    }
    return std::optional{std::move(backups->front())};
}

core::Result<std::vector<operations::FilePublicationBackupRecord>>
SqliteFilePublicationJournal::load_backups() const {
    std::scoped_lock lock{implementation_->mutex};
    auto backups = load_publication_backups(
        implementation_->database,
        "SELECT journal_id, state, undo_id, completed_at_unix_seconds, updated_at_unix_seconds, "
        "error_code, error_message FROM file_publication_backups "
        "ORDER BY completed_at_unix_seconds DESC, rowid DESC LIMIT 10001");
    if (backups && backups->size() > 10'000U) {
        return std::unexpected(
            database_error(implementation_->database, "Too many retained sources"));
    }
    return backups;
}

core::Result<void> SqliteFilePublicationJournal::transition_backup(
    const core::StableId& id, const operations::FilePublicationBackupTransition& transition) {
    if (id.is_nil()) {
        return std::unexpected(invalid_record("Retained-source ID cannot be nil"));
    }
    std::scoped_lock lock{implementation_->mutex};
    auto* database = implementation_->database;
    auto statement =
        prepare(database, "UPDATE file_publication_backups SET state = ?, undo_id = ?, "
                          "updated_at_unix_seconds = CAST(strftime('%s', 'now') AS INTEGER), "
                          "error_code = ?, error_message = ? WHERE journal_id = ? AND state = ?");
    if (!statement) {
        return std::unexpected(std::move(statement.error()));
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
        return std::unexpected(database_error(database, "Could not bind retained-source state"));
    }
    if (auto updated = step_done(database, statement->get(), "Could not update retained source");
        !updated) {
        return updated;
    }
    if (sqlite3_changes(database) != 1) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::conflict,
            .message = "Retained source changed before the requested transition",
            .context = {{.key = "journal_id", .value = id.to_string()}},
        });
    }
    return {};
}

core::Result<std::vector<operations::FilePublicationJournalRecord>>
SqliteFilePublicationJournal::load_recent() const {
    std::scoped_lock lock{implementation_->mutex};
    const auto sql = "SELECT " + std::string{record_columns} +
                     "FROM file_publication_journal ORDER BY rowid DESC LIMIT 1024";
    auto records = load_records(implementation_->database, sql.c_str());
    if (!records) {
        return std::unexpected(std::move(records.error()));
    }
    if (records->size() > maximum_recent_records) {
        return std::unexpected(
            database_error(implementation_->database, "Too many recent file publications"));
    }
    return records;
}

} // namespace trackknife::persistence

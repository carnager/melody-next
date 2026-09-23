// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/persistence/list_repository.hpp"
#include "trackknife/core/sha256.hpp"
#include "trackknife/core/unicode.hpp"
#include "trackknife/persistence/local_library.hpp"
#include "trackknife/query/tkq.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace trackknife::persistence {
namespace {

constexpr unsigned current_schema_version = 43U;
constexpr std::size_t maximum_documents = 1'024U;
constexpr std::size_t maximum_items_per_document = 1'000'000U;
constexpr std::size_t maximum_fields_per_item = 4'096U;
constexpr std::size_t maximum_refresh_occurrences = 1'000'000U;
constexpr std::size_t maximum_cached_sources = 1'000'000U;
constexpr std::size_t maximum_source_relocations = 1'000'000U;
constexpr std::size_t maximum_metadata_transformation_chains = 256U;
constexpr std::size_t maximum_output_layout_profiles = 256U;
constexpr std::size_t maximum_encoder_presets = 256U;
constexpr std::size_t maximum_destination_profiles = 256U;

[[nodiscard]] bool valid_provenance(const metadata::FieldProvenance provenance) {
    return provenance >= metadata::FieldProvenance::cached_snapshot &&
           provenance <= metadata::FieldProvenance::sidecar;
}

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
    return core::Error{
        .code = core::ErrorCode::database, .message = std::move(message), .context = {}};
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
        return std::unexpected(database_error(database, "Could not prepare SQLite statement"));
    }
    return Statement{raw};
}

[[nodiscard]] bool bind_blob(sqlite3_stmt* statement, const int index,
                             const std::string_view value) {
    return sqlite3_bind_blob64(statement, index, value.data(), value.size(), SQLITE_TRANSIENT) ==
           SQLITE_OK;
}

[[nodiscard]] bool bind_text(sqlite3_stmt* statement, const int index,
                             const std::string_view value) {
    return sqlite3_bind_text64(statement, index, value.data(), value.size(), SQLITE_TRANSIENT,
                               SQLITE_UTF8) == SQLITE_OK;
}

[[nodiscard]] std::string column_blob(sqlite3_stmt* statement, const int column) {
    const auto size = sqlite3_column_bytes(statement, column);
    const auto* data = static_cast<const char*>(sqlite3_column_blob(statement, column));
    return data == nullptr || size <= 0 ? std::string{} : std::string{data, data + size};
}

[[nodiscard]] std::string column_text(sqlite3_stmt* statement, const int column) {
    const auto size = sqlite3_column_bytes(statement, column);
    const auto* data = reinterpret_cast<const char*>(sqlite3_column_text(statement, column));
    return data == nullptr || size <= 0 ? std::string{} : std::string{data, data + size};
}

std::string listening_observation(const std::string_view path,
                                  const core::LocalSourceRevision& revision) {
    // The path is length-prefixed raw bytes, never decoded or normalized.
    return core::sha256_hex(std::to_string(path.size()) + ":" + std::string{path} + ":" +
                            std::to_string(revision.device) + ":" + std::to_string(revision.inode) +
                            ":" + std::to_string(revision.size) + ":" +
                            std::to_string(revision.modification_time_seconds) + ":" +
                            std::to_string(revision.modification_time_nanoseconds));
}

bool valid_listening_source(const ListItem& source) {
    return source.source == ListSource::local && !source.source_reference.empty() &&
           source.source_reference.find('\0') == std::string::npos && source.source_revision &&
           source.source_revision->inode != 0 &&
           (!source.segment || (source.segment->start_sample >= 0 &&
                                (!source.segment->end_sample ||
                                 *source.segment->end_sample > source.segment->start_sample))) &&
           (!source.source_selection ||
            (source.source_selection->audio_stream_index.value_or(0) >= 0 &&
             source.source_selection->subsong_index.value_or(0) >= 0));
}

std::string listening_track_key(const std::string& id, const ListItem& source) {
    const auto number = [](const auto& value) {
        return value ? std::to_string(*value) : std::string{"default"};
    };
    const auto selection = source.source_selection.value_or(ListItemSourceSelection{});
    const auto range = source.segment ? std::to_string(source.segment->start_sample) + ":" +
                                            number(source.segment->end_sample)
                                      : std::string{"whole"};
    return core::sha256_hex("local-listen-1:" + id + ":" + number(selection.audio_stream_index) +
                            ":" + number(selection.subsong_index) + ":" + range);
}

core::Result<std::string> listening_source(sqlite3* database, const std::string& observation,
                                           const std::optional<std::string>& desired = {}) {
    auto insert =
        prepare(database, "INSERT INTO local_listening_sources(observation_hash,source_id) "
                          "VALUES(?,?) ON CONFLICT(observation_hash) DO NOTHING");
    const auto id = desired ? *desired : core::StableId::random().to_string();
    if (!insert || !bind_text(insert->get(), 1, observation) || !bind_text(insert->get(), 2, id) ||
        sqlite3_step(insert->get()) != SQLITE_DONE)
        return std::unexpected(database_error(database, "Could not store listening identity"));
    auto query =
        prepare(database, "SELECT source_id FROM local_listening_sources WHERE observation_hash=?");
    if (!query || !bind_text(query->get(), 1, observation) ||
        sqlite3_step(query->get()) != SQLITE_ROW)
        return std::unexpected(database_error(database, "Could not resolve listening identity"));
    auto resolved = column_text(query->get(), 0);
    if (desired && resolved != *desired)
        return std::unexpected(core::Error{.code = core::ErrorCode::conflict,
                                           .message = "Conflicting listening source identity",
                                           .context = {}});
    return resolved;
}

core::Result<void> link_listening_source(sqlite3* database, const std::string_view previous_path,
                                         const core::LocalSourceRevision& previous,
                                         const std::string_view published_path,
                                         const core::LocalSourceRevision& published) {
    auto id = listening_source(database, listening_observation(previous_path, previous));
    if (!id)
        return std::unexpected(std::move(id.error()));
    auto linked = listening_source(database, listening_observation(published_path, published), *id);
    if (!linked)
        return std::unexpected(std::move(linked.error()));
    return {};
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

[[nodiscard]] core::Result<core::LocalSourceRevision>
read_revision(sqlite3_stmt* statement, const int first, const std::string_view description) {
    const auto device = decode_unsigned(column_blob(statement, first));
    const auto inode = decode_unsigned(column_blob(statement, first + 1));
    const auto size = decode_unsigned(column_blob(statement, first + 2));
    const auto seconds = decode_signed(column_blob(statement, first + 3));
    const auto nanoseconds = decode_signed(column_blob(statement, first + 4));
    if (!device || !inode || !size || !seconds || !nanoseconds) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::database,
            .message = std::string{description} + " contains an invalid source revision",
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

[[nodiscard]] core::Result<std::optional<core::LocalSourceRevision>>
read_optional_revision(sqlite3_stmt* statement, const int first,
                       const std::string_view description) {
    bool any_null = false;
    bool any_value = false;
    for (int index = 0; index < 5; ++index) {
        const bool is_null = sqlite3_column_type(statement, first + index) == SQLITE_NULL;
        any_null = any_null || is_null;
        any_value = any_value || !is_null;
    }
    if (!any_value) {
        return std::optional<core::LocalSourceRevision>{};
    }
    if (any_null) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::database,
            .message = std::string{description} + " contains a partial source revision",
            .context = {},
        });
    }
    auto revision = read_revision(statement, first, description);
    if (!revision) {
        return std::unexpected(std::move(revision.error()));
    }
    return std::optional{*revision};
}

[[nodiscard]] bool bind_optional_blob(sqlite3_stmt* statement, const int index,
                                      const std::optional<std::string>& value) {
    return value ? bind_blob(statement, index, *value)
                 : sqlite3_bind_null(statement, index) == SQLITE_OK;
}

[[nodiscard]] std::optional<std::string> optional_blob(sqlite3_stmt* statement, const int column) {
    return sqlite3_column_type(statement, column) == SQLITE_NULL
               ? std::nullopt
               : std::optional{column_blob(statement, column)};
}

[[nodiscard]] core::Result<void> step_done(sqlite3* database, sqlite3_stmt* statement,
                                           std::string_view operation) {
    if (sqlite3_step(statement) != SQLITE_DONE) {
        return std::unexpected(database_error(database, std::string{operation}));
    }
    sqlite3_reset(statement);
    sqlite3_clear_bindings(statement);
    return {};
}

[[nodiscard]] core::Result<void> migrate(sqlite3* database) {
    if (auto result = execute(database, "BEGIN IMMEDIATE"); !result) {
        return result;
    }
    const auto rollback = [database] { static_cast<void>(execute(database, "ROLLBACK")); };
    if (auto result = execute(database, "CREATE TABLE IF NOT EXISTS schema_version ("
                                        "version INTEGER NOT NULL CHECK(version >= 0))");
        !result) {
        rollback();
        return result;
    }
    if (auto result = execute(database, "INSERT INTO schema_version(version) "
                                        "SELECT 0 WHERE NOT EXISTS (SELECT 1 FROM schema_version)");
        !result) {
        rollback();
        return result;
    }
    auto query = prepare(database, "SELECT version FROM schema_version LIMIT 1");
    if (!query) {
        rollback();
        return std::unexpected(std::move(query.error()));
    }
    if (sqlite3_step(query->get()) != SQLITE_ROW) {
        auto error = database_error(database, "Could not read SQLite schema version");
        rollback();
        return std::unexpected(std::move(error));
    }
    const auto version = sqlite3_column_int64(query->get(), 0);
    query->reset();
    if (version > current_schema_version) {
        rollback();
        return std::unexpected(core::Error{
            .code = core::ErrorCode::unsupported,
            .message = "Trackknife state database was created by a newer application version",
            .context = {{"schema_version", std::to_string(version)}},
        });
    }
    if (version == current_schema_version) {
        // Nothing to change, so nothing to check. The engine opens the
        // database for every library request, and the foreign-key check below
        // reads all of it: on a library of 66,000 tracks that was half a
        // second per request, spent confirming that no migration had run.
        return execute(database, "COMMIT");
    }
    if (version == 0) {
        constexpr auto migration =
            "CREATE TABLE list_documents ("
            "id TEXT PRIMARY KEY NOT NULL, kind INTEGER NOT NULL, name BLOB NOT NULL, "
            "pinned INTEGER NOT NULL CHECK(pinned IN (0,1)), "
            "dirty INTEGER NOT NULL CHECK(dirty IN (0,1)), position INTEGER NOT NULL UNIQUE);"
            "CREATE TABLE list_items ("
            "document_id TEXT NOT NULL REFERENCES list_documents(id) ON DELETE CASCADE, "
            "position INTEGER NOT NULL, source INTEGER NOT NULL, profile_id TEXT, "
            "source_reference BLOB NOT NULL, duration_ms INTEGER, "
            "PRIMARY KEY(document_id, position));"
            "CREATE TABLE list_item_fields ("
            "document_id TEXT NOT NULL, item_position INTEGER NOT NULL, position INTEGER NOT NULL, "
            "name BLOB NOT NULL, value BLOB NOT NULL, "
            "PRIMARY KEY(document_id, item_position, position), "
            "FOREIGN KEY(document_id, item_position) "
            "REFERENCES list_items(document_id, position) ON DELETE CASCADE);"
            "CREATE INDEX list_item_fields_item "
            "ON list_item_fields(document_id, item_position, position);"
            "UPDATE schema_version SET version = 1;";
        if (auto result = execute(database, migration); !result) {
            rollback();
            return result;
        }
    }
    if (version <= 1) {
        constexpr auto migration =
            "CREATE TABLE connection_profiles ("
            "id TEXT PRIMARY KEY NOT NULL, name BLOB NOT NULL, host BLOB NOT NULL, "
            "port INTEGER NOT NULL CHECK(port BETWEEN 1 AND 65535), local_music_root BLOB, "
            "auto_connect INTEGER NOT NULL CHECK(auto_connect IN (0,1)), "
            "position INTEGER NOT NULL UNIQUE);"
            "UPDATE schema_version SET version = 2;";
        if (auto result = execute(database, migration); !result) {
            rollback();
            return result;
        }
    }
    if (version <= 2) {
        constexpr auto migration = "CREATE TABLE track_view_presets ("
                                   "binding BLOB PRIMARY KEY NOT NULL, header_state BLOB NOT NULL, "
                                   "position INTEGER NOT NULL UNIQUE);"
                                   "UPDATE schema_version SET version = 3;";
        if (auto result = execute(database, migration); !result) {
            rollback();
            return result;
        }
    }
    if (version <= 3) {
        constexpr auto migration = "ALTER TABLE list_items ADD COLUMN logical_reference BLOB;"
                                   "ALTER TABLE list_items ADD COLUMN segment_start_sample INTEGER;"
                                   "ALTER TABLE list_items ADD COLUMN segment_end_sample INTEGER;"
                                   "UPDATE schema_version SET version = 4;";
        if (auto result = execute(database, migration); !result) {
            rollback();
            return result;
        }
    }
    if (version <= 4) {
        constexpr auto migration =
            "ALTER TABLE list_items ADD COLUMN selected_audio_stream INTEGER;"
            "ALTER TABLE list_items ADD COLUMN codec_subsong_index INTEGER;"
            "UPDATE schema_version SET version = 5;";
        if (auto result = execute(database, migration); !result) {
            rollback();
            return result;
        }
    }
    if (version <= 5) {
        constexpr auto migration =
            "CREATE TABLE operation_journal ("
            "id TEXT PRIMARY KEY NOT NULL, kind INTEGER NOT NULL CHECK(kind = 0), "
            "state INTEGER NOT NULL CHECK(state BETWEEN 0 AND 5), "
            "source_path BLOB NOT NULL, prepared_path BLOB NOT NULL, backup_path BLOB NOT NULL, "
            "expected_device BLOB NOT NULL, expected_inode BLOB NOT NULL, "
            "expected_size BLOB NOT NULL, expected_mtime_seconds BLOB NOT NULL, "
            "expected_mtime_nanoseconds BLOB NOT NULL, prepared_device BLOB, "
            "prepared_inode BLOB, prepared_size BLOB, prepared_mtime_seconds BLOB, "
            "prepared_mtime_nanoseconds BLOB, published_device BLOB, published_inode BLOB, "
            "published_size BLOB, published_mtime_seconds BLOB, "
            "published_mtime_nanoseconds BLOB, error_code INTEGER, error_message BLOB);"
            "CREATE INDEX operation_journal_state ON operation_journal(state);"
            "CREATE TABLE operation_journal_occurrences ("
            "journal_id TEXT NOT NULL REFERENCES operation_journal(id) ON DELETE CASCADE, "
            "position INTEGER NOT NULL, item_index INTEGER NOT NULL, "
            "PRIMARY KEY(journal_id, position));"
            "CREATE TABLE operation_journal_changes ("
            "journal_id TEXT NOT NULL REFERENCES operation_journal(id) ON DELETE CASCADE, "
            "position INTEGER NOT NULL, field_index INTEGER NOT NULL, "
            "canonical_name BLOB NOT NULL, property_name BLOB NOT NULL, "
            "original_present INTEGER NOT NULL CHECK(original_present IN (0,1)), "
            "patch_kind INTEGER NOT NULL CHECK(patch_kind BETWEEN 0 AND 1), "
            "PRIMARY KEY(journal_id, position));"
            "CREATE TABLE operation_journal_values ("
            "journal_id TEXT NOT NULL, change_position INTEGER NOT NULL, "
            "value_kind INTEGER NOT NULL CHECK(value_kind BETWEEN 0 AND 1), "
            "position INTEGER NOT NULL, value BLOB NOT NULL, "
            "PRIMARY KEY(journal_id, change_position, value_kind, position), "
            "FOREIGN KEY(journal_id, change_position) "
            "REFERENCES operation_journal_changes(journal_id, position) ON DELETE CASCADE);"
            "CREATE TABLE operation_journal_intents ("
            "journal_id TEXT NOT NULL, change_position INTEGER NOT NULL, "
            "position INTEGER NOT NULL, item_index INTEGER NOT NULL, "
            "PRIMARY KEY(journal_id, change_position, position), "
            "FOREIGN KEY(journal_id, change_position) "
            "REFERENCES operation_journal_changes(journal_id, position) ON DELETE CASCADE);"
            "UPDATE schema_version SET version = 6;";
        if (auto result = execute(database, migration); !result) {
            rollback();
            return result;
        }
    }
    if (version <= 6) {
        constexpr auto migration =
            "ALTER TABLE list_item_fields ADD COLUMN native_name BLOB NOT NULL DEFAULT X'';"
            "ALTER TABLE list_item_fields ADD COLUMN provenance INTEGER NOT NULL DEFAULT 0 "
            "CHECK(provenance BETWEEN 0 AND 5);"
            "ALTER TABLE list_item_fields ADD COLUMN language BLOB;"
            "ALTER TABLE list_item_fields ADD COLUMN description BLOB;"
            "ALTER TABLE list_items ADD COLUMN observed_device BLOB;"
            "ALTER TABLE list_items ADD COLUMN observed_inode BLOB;"
            "ALTER TABLE list_items ADD COLUMN observed_size BLOB;"
            "ALTER TABLE list_items ADD COLUMN observed_mtime_seconds BLOB;"
            "ALTER TABLE list_items ADD COLUMN observed_mtime_nanoseconds BLOB;"
            "CREATE TABLE local_metadata_cache ("
            "source_reference BLOB PRIMARY KEY NOT NULL, previous_device BLOB NOT NULL, "
            "previous_inode BLOB NOT NULL, previous_size BLOB NOT NULL, "
            "previous_mtime_seconds BLOB NOT NULL, previous_mtime_nanoseconds BLOB NOT NULL, "
            "published_device BLOB NOT NULL, "
            "published_inode BLOB NOT NULL, published_size BLOB NOT NULL, "
            "published_mtime_seconds BLOB NOT NULL, published_mtime_nanoseconds BLOB NOT NULL);"
            "CREATE TABLE local_metadata_cache_fields ("
            "source_reference BLOB NOT NULL REFERENCES local_metadata_cache(source_reference) "
            "ON DELETE CASCADE, position INTEGER NOT NULL, name BLOB NOT NULL, "
            "value BLOB NOT NULL, native_name BLOB NOT NULL, provenance INTEGER NOT NULL "
            "CHECK(provenance BETWEEN 0 AND 5), language BLOB, description BLOB, "
            "PRIMARY KEY(source_reference, position));"
            "CREATE TABLE local_metadata_refreshes ("
            "operation_id TEXT PRIMARY KEY NOT NULL, source_reference BLOB NOT NULL, "
            "previous_device BLOB NOT NULL, previous_inode BLOB NOT NULL, "
            "previous_size BLOB NOT NULL, previous_mtime_seconds BLOB NOT NULL, "
            "previous_mtime_nanoseconds BLOB NOT NULL, "
            "published_device BLOB NOT NULL, published_inode BLOB NOT NULL, "
            "published_size BLOB NOT NULL, published_mtime_seconds BLOB NOT NULL, "
            "published_mtime_nanoseconds BLOB NOT NULL, affected_occurrences INTEGER NOT NULL "
            "CHECK(affected_occurrences > 0));"
            "UPDATE schema_version SET version = 7;";
        if (auto result = execute(database, migration); !result) {
            rollback();
            return result;
        }
    }
    if (version <= 7) {
        constexpr auto migration =
            "CREATE TABLE metadata_operation_backups ("
            "journal_id TEXT PRIMARY KEY NOT NULL REFERENCES operation_journal(id) "
            "ON DELETE CASCADE, state INTEGER NOT NULL CHECK(state BETWEEN 0 AND 4), "
            "undo_id TEXT, completed_at_unix_seconds INTEGER NOT NULL, "
            "updated_at_unix_seconds INTEGER NOT NULL, error_code INTEGER, error_message BLOB);"
            "CREATE INDEX metadata_operation_backups_state_time ON "
            "metadata_operation_backups(state, completed_at_unix_seconds DESC);"
            "INSERT INTO metadata_operation_backups(journal_id, state, undo_id, "
            "completed_at_unix_seconds, updated_at_unix_seconds, error_code, error_message) "
            "SELECT id, 0, NULL, CAST(strftime('%s', 'now') AS INTEGER), "
            "CAST(strftime('%s', 'now') AS INTEGER), NULL, NULL "
            "FROM operation_journal WHERE state = 3;"
            "UPDATE schema_version SET version = 8;";
        if (auto result = execute(database, migration); !result) {
            rollback();
            return result;
        }
    }
    if (version <= 8) {
        constexpr auto migration =
            "CREATE TABLE metadata_transformation_chains ("
            "id TEXT PRIMARY KEY NOT NULL, schema_version INTEGER NOT NULL "
            "CHECK(schema_version = 1), name BLOB NOT NULL UNIQUE);"
            "CREATE TABLE metadata_transformation_actions ("
            "chain_id TEXT NOT NULL REFERENCES metadata_transformation_chains(id) "
            "ON DELETE CASCADE, position INTEGER NOT NULL, kind INTEGER NOT NULL "
            "CHECK(kind BETWEEN 0 AND 9), target_field BLOB NOT NULL, argument BLOB, "
            "dialect BLOB, dialect_version INTEGER, compiler_schema INTEGER, "
            "PRIMARY KEY(chain_id, position));"
            "CREATE TABLE metadata_transformation_action_values ("
            "chain_id TEXT NOT NULL, action_position INTEGER NOT NULL, "
            "position INTEGER NOT NULL, value BLOB NOT NULL, "
            "PRIMARY KEY(chain_id, action_position, position), "
            "FOREIGN KEY(chain_id, action_position) REFERENCES "
            "metadata_transformation_actions(chain_id, position) ON DELETE CASCADE);"
            "UPDATE schema_version SET version = 9;";
        if (auto result = execute(database, migration); !result) {
            rollback();
            return result;
        }
    }
    if (version <= 9) {
        constexpr auto migration =
            "ALTER TABLE metadata_transformation_action_values "
            "RENAME TO metadata_transformation_action_values_v9;"
            "ALTER TABLE metadata_transformation_actions "
            "RENAME TO metadata_transformation_actions_v9;"
            "CREATE TABLE metadata_transformation_actions ("
            "chain_id TEXT NOT NULL REFERENCES metadata_transformation_chains(id) "
            "ON DELETE CASCADE, position INTEGER NOT NULL, kind INTEGER NOT NULL "
            "CHECK(kind BETWEEN 0 AND 10), target_field BLOB NOT NULL, argument BLOB, "
            "dialect BLOB, dialect_version INTEGER, compiler_schema INTEGER, "
            "PRIMARY KEY(chain_id, position));"
            "CREATE TABLE metadata_transformation_action_values ("
            "chain_id TEXT NOT NULL, action_position INTEGER NOT NULL, "
            "position INTEGER NOT NULL, value BLOB NOT NULL, "
            "PRIMARY KEY(chain_id, action_position, position), "
            "FOREIGN KEY(chain_id, action_position) REFERENCES "
            "metadata_transformation_actions(chain_id, position) ON DELETE CASCADE);"
            "INSERT INTO metadata_transformation_actions "
            "SELECT * FROM metadata_transformation_actions_v9;"
            "INSERT INTO metadata_transformation_action_values "
            "SELECT * FROM metadata_transformation_action_values_v9;"
            "DROP TABLE metadata_transformation_action_values_v9;"
            "DROP TABLE metadata_transformation_actions_v9;"
            "UPDATE schema_version SET version = 10;";
        if (auto result = execute(database, migration); !result) {
            rollback();
            return result;
        }
    }
    if (version <= 10) {
        constexpr auto migration =
            "ALTER TABLE metadata_transformation_chains ADD COLUMN automatic INTEGER NOT NULL "
            "DEFAULT 0 CHECK(automatic IN (0,1));"
            "UPDATE schema_version SET version = 11;";
        if (auto result = execute(database, migration); !result) {
            rollback();
            return result;
        }
    }
    if (version <= 11) {
        constexpr auto migration =
            "ALTER TABLE metadata_transformation_action_values "
            "RENAME TO metadata_transformation_action_values_v11;"
            "ALTER TABLE metadata_transformation_actions "
            "RENAME TO metadata_transformation_actions_v11;"
            "CREATE TABLE metadata_transformation_actions ("
            "chain_id TEXT NOT NULL REFERENCES metadata_transformation_chains(id) "
            "ON DELETE CASCADE, position INTEGER NOT NULL, kind INTEGER NOT NULL "
            "CHECK(kind BETWEEN 0 AND 13), target_field BLOB NOT NULL, argument BLOB, "
            "dialect BLOB, dialect_version INTEGER, compiler_schema INTEGER, "
            "integer_argument INTEGER, integer_argument_2 INTEGER, "
            "PRIMARY KEY(chain_id, position));"
            "CREATE TABLE metadata_transformation_action_values ("
            "chain_id TEXT NOT NULL, action_position INTEGER NOT NULL, "
            "position INTEGER NOT NULL, value BLOB NOT NULL, "
            "PRIMARY KEY(chain_id, action_position, position), "
            "FOREIGN KEY(chain_id, action_position) REFERENCES "
            "metadata_transformation_actions(chain_id, position) ON DELETE CASCADE);"
            "INSERT INTO metadata_transformation_actions "
            "SELECT chain_id, position, kind, target_field, argument, dialect, "
            "dialect_version, compiler_schema, NULL, NULL "
            "FROM metadata_transformation_actions_v11;"
            "INSERT INTO metadata_transformation_action_values "
            "SELECT * FROM metadata_transformation_action_values_v11;"
            "DROP TABLE metadata_transformation_action_values_v11;"
            "DROP TABLE metadata_transformation_actions_v11;"
            "UPDATE schema_version SET version = 12;";
        if (auto result = execute(database, migration); !result) {
            rollback();
            return result;
        }
    }
    if (version <= 12) {
        constexpr auto migration =
            "CREATE TABLE output_layout_profiles ("
            "id TEXT PRIMARY KEY NOT NULL, schema_version INTEGER NOT NULL "
            "CHECK(schema_version = 1), name BLOB NOT NULL UNIQUE, dialect BLOB NOT NULL, "
            "dialect_version INTEGER NOT NULL, compiler_schema INTEGER NOT NULL, "
            "relative_directory_expression BLOB NOT NULL, basename_expression BLOB NOT NULL, "
            "sanitization_policy BLOB NOT NULL, sanitization_version INTEGER NOT NULL);"
            "CREATE TABLE destination_profiles ("
            "id TEXT PRIMARY KEY NOT NULL, schema_version INTEGER NOT NULL "
            "CHECK(schema_version = 1), name BLOB NOT NULL UNIQUE, root_raw_path BLOB NOT NULL, "
            "containment_policy BLOB NOT NULL, containment_version INTEGER NOT NULL);"
            "UPDATE schema_version SET version = 13;";
        if (auto result = execute(database, migration); !result) {
            rollback();
            return result;
        }
    }
    if (version <= 13) {
        constexpr auto migration =
            "CREATE TABLE file_publication_journal ("
            "id TEXT PRIMARY KEY NOT NULL, state INTEGER NOT NULL CHECK(state BETWEEN 0 AND 7), "
            "publication_kind INTEGER NOT NULL CHECK(publication_kind BETWEEN 1 AND 2), "
            "source_path BLOB NOT NULL, target_path BLOB NOT NULL, prepared_path BLOB NOT NULL, "
            "expected_device BLOB NOT NULL, expected_inode BLOB NOT NULL, "
            "expected_size BLOB NOT NULL, expected_mtime_seconds BLOB NOT NULL, "
            "expected_mtime_nanoseconds BLOB NOT NULL, prepared_device BLOB, "
            "prepared_inode BLOB, prepared_size BLOB, prepared_mtime_seconds BLOB, "
            "prepared_mtime_nanoseconds BLOB, target_device BLOB, target_inode BLOB, "
            "target_size BLOB, target_mtime_seconds BLOB, target_mtime_nanoseconds BLOB, "
            "error_code INTEGER, error_message BLOB);"
            "CREATE INDEX file_publication_journal_state ON file_publication_journal(state);"
            "CREATE TABLE file_publication_journal_occurrences ("
            "journal_id TEXT NOT NULL REFERENCES file_publication_journal(id) ON DELETE CASCADE, "
            "position INTEGER NOT NULL, item_index INTEGER NOT NULL, "
            "PRIMARY KEY(journal_id, position));"
            "CREATE TABLE file_publication_journal_directories ("
            "journal_id TEXT NOT NULL REFERENCES file_publication_journal(id) ON DELETE CASCADE, "
            "position INTEGER NOT NULL, raw_path BLOB NOT NULL, "
            "PRIMARY KEY(journal_id, position));"
            "UPDATE schema_version SET version = 14;";
        if (auto result = execute(database, migration); !result) {
            rollback();
            return result;
        }
    }
    if (version <= 14) {
        constexpr auto migration =
            "CREATE TABLE local_source_relocations ("
            "sequence INTEGER PRIMARY KEY AUTOINCREMENT, operation_id TEXT NOT NULL UNIQUE, "
            "source_reference BLOB NOT NULL, target_reference BLOB NOT NULL, "
            "previous_device BLOB NOT NULL, previous_inode BLOB NOT NULL, "
            "previous_size BLOB NOT NULL, previous_mtime_seconds BLOB NOT NULL, "
            "previous_mtime_nanoseconds BLOB NOT NULL, published_device BLOB NOT NULL, "
            "published_inode BLOB NOT NULL, published_size BLOB NOT NULL, "
            "published_mtime_seconds BLOB NOT NULL, published_mtime_nanoseconds BLOB NOT NULL, "
            "affected_occurrences INTEGER NOT NULL CHECK(affected_occurrences > 0), "
            "cache_rekeyed INTEGER NOT NULL CHECK(cache_rekeyed IN (0,1)), "
            "CHECK(source_reference != target_reference));"
            "CREATE INDEX local_source_relocations_source "
            "ON local_source_relocations(source_reference, sequence);"
            "UPDATE schema_version SET version = 15;";
        if (auto result = execute(database, migration); !result) {
            rollback();
            return result;
        }
    }
    if (version <= 15) {
        constexpr auto migration =
            "ALTER TABLE file_publication_journal ADD COLUMN reverses_id TEXT "
            "REFERENCES file_publication_journal(id);"
            "CREATE INDEX file_publication_journal_reverses "
            "ON file_publication_journal(reverses_id);"
            "UPDATE schema_version SET version = 16;";
        if (auto result = execute(database, migration); !result) {
            rollback();
            return result;
        }
    }
    if (version <= 16) {
        constexpr auto migration =
            "ALTER TABLE metadata_transformation_action_values "
            "RENAME TO metadata_transformation_action_values_v16;"
            "ALTER TABLE metadata_transformation_actions "
            "RENAME TO metadata_transformation_actions_v16;"
            "CREATE TABLE metadata_transformation_actions ("
            "chain_id TEXT NOT NULL REFERENCES metadata_transformation_chains(id) "
            "ON DELETE CASCADE, position INTEGER NOT NULL, kind INTEGER NOT NULL "
            "CHECK(kind BETWEEN 0 AND 14), target_field BLOB NOT NULL, argument BLOB, "
            "dialect BLOB, dialect_version INTEGER, compiler_schema INTEGER, "
            "integer_argument INTEGER, integer_argument_2 INTEGER, "
            "PRIMARY KEY(chain_id, position));"
            "CREATE TABLE metadata_transformation_action_values ("
            "chain_id TEXT NOT NULL, action_position INTEGER NOT NULL, "
            "position INTEGER NOT NULL, value BLOB NOT NULL, "
            "PRIMARY KEY(chain_id, action_position, position), "
            "FOREIGN KEY(chain_id, action_position) REFERENCES "
            "metadata_transformation_actions(chain_id, position) ON DELETE CASCADE);"
            "INSERT INTO metadata_transformation_actions "
            "SELECT * FROM metadata_transformation_actions_v16;"
            "INSERT INTO metadata_transformation_action_values "
            "SELECT * FROM metadata_transformation_action_values_v16;"
            "DROP TABLE metadata_transformation_action_values_v16;"
            "DROP TABLE metadata_transformation_actions_v16;"
            "UPDATE schema_version SET version = 17;";
        if (auto result = execute(database, migration); !result) {
            rollback();
            return result;
        }
    }
    if (version <= 17) {
        constexpr auto migration =
            "ALTER TABLE metadata_transformation_action_values "
            "RENAME TO metadata_transformation_action_values_v17;"
            "ALTER TABLE metadata_transformation_actions "
            "RENAME TO metadata_transformation_actions_v17;"
            "CREATE TABLE metadata_transformation_actions ("
            "chain_id TEXT NOT NULL REFERENCES metadata_transformation_chains(id) "
            "ON DELETE CASCADE, position INTEGER NOT NULL, kind INTEGER NOT NULL "
            "CHECK(kind BETWEEN 0 AND 15), target_field BLOB NOT NULL, argument BLOB, "
            "dialect BLOB, dialect_version INTEGER, compiler_schema INTEGER, "
            "integer_argument INTEGER, integer_argument_2 INTEGER, "
            "PRIMARY KEY(chain_id, position));"
            "CREATE TABLE metadata_transformation_action_values ("
            "chain_id TEXT NOT NULL, action_position INTEGER NOT NULL, "
            "position INTEGER NOT NULL, value BLOB NOT NULL, "
            "PRIMARY KEY(chain_id, action_position, position), "
            "FOREIGN KEY(chain_id, action_position) REFERENCES "
            "metadata_transformation_actions(chain_id, position) ON DELETE CASCADE);"
            "INSERT INTO metadata_transformation_actions "
            "SELECT * FROM metadata_transformation_actions_v17;"
            "INSERT INTO metadata_transformation_action_values "
            "SELECT * FROM metadata_transformation_action_values_v17;"
            "DROP TABLE metadata_transformation_action_values_v17;"
            "DROP TABLE metadata_transformation_actions_v17;"
            "UPDATE schema_version SET version = 18;";
        if (auto result = execute(database, migration); !result) {
            rollback();
            return result;
        }
    }
    if (version <= 18) {
        constexpr auto migration =
            "ALTER TABLE operation_journal_changes ADD COLUMN exact_native_name BLOB;"
            "UPDATE schema_version SET version = 19;";
        if (auto result = execute(database, migration); !result) {
            rollback();
            return result;
        }
    }
    if (version <= 19) {
        constexpr auto migration =
            "ALTER TABLE metadata_transformation_action_values "
            "RENAME TO metadata_transformation_action_values_v19;"
            "ALTER TABLE metadata_transformation_actions "
            "RENAME TO metadata_transformation_actions_v19;"
            "CREATE TABLE metadata_transformation_actions ("
            "chain_id TEXT NOT NULL REFERENCES metadata_transformation_chains(id) "
            "ON DELETE CASCADE, position INTEGER NOT NULL, kind INTEGER NOT NULL "
            "CHECK(kind BETWEEN 0 AND 16), target_field BLOB NOT NULL, argument BLOB, "
            "dialect BLOB, dialect_version INTEGER, compiler_schema INTEGER, "
            "integer_argument INTEGER, integer_argument_2 INTEGER, "
            "PRIMARY KEY(chain_id, position));"
            "CREATE TABLE metadata_transformation_action_values ("
            "chain_id TEXT NOT NULL, action_position INTEGER NOT NULL, "
            "position INTEGER NOT NULL, value BLOB NOT NULL, "
            "PRIMARY KEY(chain_id, action_position, position), "
            "FOREIGN KEY(chain_id, action_position) REFERENCES "
            "metadata_transformation_actions(chain_id, position) ON DELETE CASCADE);"
            "INSERT INTO metadata_transformation_actions "
            "SELECT * FROM metadata_transformation_actions_v19;"
            "INSERT INTO metadata_transformation_action_values "
            "SELECT * FROM metadata_transformation_action_values_v19;"
            "DROP TABLE metadata_transformation_action_values_v19;"
            "DROP TABLE metadata_transformation_actions_v19;"
            "UPDATE schema_version SET version = 20;";
        if (auto result = execute(database, migration); !result) {
            rollback();
            return result;
        }
    }
    if (version <= 20) {
        constexpr auto migration =
            "ALTER TABLE file_publication_journal ADD COLUMN content_kind INTEGER NOT NULL "
            "DEFAULT 0 CHECK(content_kind BETWEEN 0 AND 1);"
            "UPDATE schema_version SET version = 21;";
        if (auto result = execute(database, migration); !result) {
            rollback();
            return result;
        }
    }
    if (version <= 21) {
        constexpr auto migration =
            "ALTER TABLE local_source_relocations ADD COLUMN metadata_refreshed INTEGER NOT NULL "
            "DEFAULT 0 CHECK(metadata_refreshed IN (0,1));"
            "UPDATE schema_version SET version = 22;";
        if (auto result = execute(database, migration); !result) {
            rollback();
            return result;
        }
    }
    if (version <= 22) {
        constexpr auto migration =
            "ALTER TABLE operation_journal ADD COLUMN content_kind INTEGER NOT NULL DEFAULT 0 "
            "CHECK(content_kind BETWEEN 0 AND 1);"
            "CREATE TABLE operation_journal_artwork ("
            "journal_id TEXT PRIMARY KEY NOT NULL REFERENCES operation_journal(id) ON DELETE "
            "CASCADE, intent_kind INTEGER NOT NULL CHECK(intent_kind BETWEEN 0 AND 1), "
            "target_ordinal INTEGER NOT NULL CHECK(target_ordinal >= 0), "
            "original_item_count INTEGER NOT NULL CHECK(original_item_count > 0), "
            "planned_item_count INTEGER NOT NULL CHECK(planned_item_count >= 0), "
            "original_target_fingerprint BLOB NOT NULL "
            "CHECK(length(original_target_fingerprint) = 32), replacement_fingerprint BLOB "
            "CHECK(replacement_fingerprint IS NULL OR length(replacement_fingerprint) = 32), "
            "original_inventory_fingerprint BLOB NOT NULL "
            "CHECK(length(original_inventory_fingerprint) = 32), "
            "planned_inventory_fingerprint BLOB NOT NULL "
            "CHECK(length(planned_inventory_fingerprint) = 32), "
            "CHECK((intent_kind = 0 AND replacement_fingerprint IS NOT NULL AND "
            "planned_item_count = original_item_count) OR "
            "(intent_kind = 1 AND replacement_fingerprint IS NULL AND "
            "planned_item_count + 1 = original_item_count)));"
            "UPDATE schema_version SET version = 23;";
        if (auto result = execute(database, migration); !result) {
            rollback();
            return result;
        }
    }
    if (version <= 23) {
        constexpr auto migration =
            "ALTER TABLE operation_journal_artwork RENAME TO operation_journal_artwork_v23;"
            "CREATE TABLE operation_journal_artwork ("
            "journal_id TEXT PRIMARY KEY NOT NULL REFERENCES operation_journal(id) ON DELETE "
            "CASCADE, intent_kind INTEGER NOT NULL CHECK(intent_kind BETWEEN 0 AND 2), "
            "target_ordinal INTEGER NOT NULL CHECK(target_ordinal >= 0), "
            "original_item_count INTEGER NOT NULL CHECK(original_item_count >= 0), "
            "planned_item_count INTEGER NOT NULL CHECK(planned_item_count >= 0), "
            "original_target_fingerprint BLOB CHECK(original_target_fingerprint IS NULL OR "
            "length(original_target_fingerprint) = 32), replacement_fingerprint BLOB "
            "CHECK(replacement_fingerprint IS NULL OR length(replacement_fingerprint) = 32), "
            "original_inventory_fingerprint BLOB NOT NULL "
            "CHECK(length(original_inventory_fingerprint) = 32), "
            "planned_inventory_fingerprint BLOB NOT NULL "
            "CHECK(length(planned_inventory_fingerprint) = 32), "
            "CHECK((intent_kind = 0 AND original_target_fingerprint IS NOT NULL AND "
            "replacement_fingerprint IS NOT NULL AND planned_item_count = original_item_count "
            "AND target_ordinal < original_item_count) OR "
            "(intent_kind = 1 AND original_target_fingerprint IS NOT NULL AND "
            "replacement_fingerprint IS NULL AND planned_item_count + 1 = original_item_count "
            "AND target_ordinal < original_item_count) OR "
            "(intent_kind = 2 AND original_target_fingerprint IS NULL AND "
            "replacement_fingerprint IS NOT NULL AND target_ordinal = original_item_count AND "
            "planned_item_count = original_item_count + 1)));"
            "INSERT INTO operation_journal_artwork SELECT * FROM operation_journal_artwork_v23;"
            "DROP TABLE operation_journal_artwork_v23;"
            "UPDATE schema_version SET version = 24;";
        if (auto result = execute(database, migration); !result) {
            rollback();
            return result;
        }
    }
    if (version <= 24) {
        constexpr auto migration =
            "CREATE TABLE musicbrainz_response_cache ("
            "url BLOB PRIMARY KEY NOT NULL, "
            "body BLOB NOT NULL, "
            "fetched_at_unix_seconds INTEGER NOT NULL CHECK(fetched_at_unix_seconds >= 0));"
            "CREATE INDEX musicbrainz_response_cache_age "
            "ON musicbrainz_response_cache(fetched_at_unix_seconds);"
            "UPDATE schema_version SET version = 25;";
        if (auto result = execute(database, migration); !result) {
            rollback();
            return result;
        }
    }
    if (version <= 25) {
        constexpr auto migration =
            "ALTER TABLE metadata_transformation_action_values "
            "RENAME TO metadata_transformation_action_values_v25;"
            "ALTER TABLE metadata_transformation_actions "
            "RENAME TO metadata_transformation_actions_v25;"
            "CREATE TABLE metadata_transformation_actions ("
            "chain_id TEXT NOT NULL REFERENCES metadata_transformation_chains(id) "
            "ON DELETE CASCADE, position INTEGER NOT NULL, kind INTEGER NOT NULL "
            "CHECK(kind BETWEEN 0 AND 17), target_field BLOB NOT NULL, argument BLOB, "
            "dialect BLOB, dialect_version INTEGER, compiler_schema INTEGER, "
            "integer_argument INTEGER, integer_argument_2 INTEGER, "
            "PRIMARY KEY(chain_id, position));"
            "CREATE TABLE metadata_transformation_action_values ("
            "chain_id TEXT NOT NULL, action_position INTEGER NOT NULL, "
            "position INTEGER NOT NULL, value BLOB NOT NULL, "
            "PRIMARY KEY(chain_id, action_position, position), "
            "FOREIGN KEY(chain_id, action_position) REFERENCES "
            "metadata_transformation_actions(chain_id, position) ON DELETE CASCADE);"
            "INSERT INTO metadata_transformation_actions "
            "SELECT * FROM metadata_transformation_actions_v25;"
            "INSERT INTO metadata_transformation_action_values "
            "SELECT * FROM metadata_transformation_action_values_v25;"
            "DROP TABLE metadata_transformation_action_values_v25;"
            "DROP TABLE metadata_transformation_actions_v25;"
            "UPDATE schema_version SET version = 26;";
        if (auto result = execute(database, migration); !result) {
            rollback();
            return result;
        }
    }
    if (version <= 26) {
        constexpr auto migration =
            "CREATE TABLE encoder_presets ("
            "id TEXT PRIMARY KEY NOT NULL, preset_id BLOB NOT NULL, "
            "preset_version INTEGER NOT NULL CHECK(preset_version >= 1), "
            "display_name BLOB NOT NULL UNIQUE, codec_name BLOB NOT NULL, "
            "container_name BLOB NOT NULL, file_extension BLOB NOT NULL, "
            "lossless INTEGER NOT NULL CHECK(lossless IN (0, 1)), "
            "bit_rate INTEGER, vbr_quality INTEGER, sample_format_hint BLOB NOT NULL);"
            "UPDATE schema_version SET version = 27;";
        if (auto result = execute(database, migration); !result) {
            rollback();
            return result;
        }
    }
    if (version <= 27) {
        constexpr auto migration =
            "CREATE TABLE local_library_roots (raw_path BLOB PRIMARY KEY NOT NULL, "
            "available INTEGER NOT NULL DEFAULT 0 CHECK(available IN (0,1)), "
            "error TEXT NOT NULL DEFAULT '', scan_token TEXT NOT NULL DEFAULT '');"
            "CREATE TABLE local_library_tracks (raw_path BLOB PRIMARY KEY NOT NULL, "
            "root BLOB NOT NULL REFERENCES local_library_roots(raw_path) ON DELETE CASCADE, "
            "revision TEXT NOT NULL, title TEXT NOT NULL, artist TEXT NOT NULL, "
            "album TEXT NOT NULL, album_key TEXT NOT NULL, release_id TEXT NOT NULL, "
            "date TEXT NOT NULL, disc INTEGER NOT NULL, track INTEGER NOT NULL, "
            "search_track TEXT NOT NULL, search_album TEXT NOT NULL, "
            "available INTEGER NOT NULL CHECK(available IN (0,1)), seen TEXT NOT NULL);"
            "CREATE INDEX local_library_artist ON "
            "local_library_tracks(artist,album_key,disc,track);"
            "CREATE INDEX local_library_album ON local_library_tracks(album_key,disc,track);"
            "CREATE INDEX local_library_root ON local_library_tracks(root,seen);"
            "UPDATE schema_version SET version = 28;";
        if (auto result = execute(database, migration); !result) {
            rollback();
            return result;
        }
    }
    if (version <= 28) {
        // ADR-0145: widen the operation-journal content kinds for CUE and
        // sidecar rewrites. SQLite cannot relax a CHECK, so the table is
        // rebuilt. The old table must be dropped rather than renamed:
        // RENAME rewrites the child tables' REFERENCES clauses, while
        // dropping and renaming the replacement into place leaves them
        // pointing at "operation_journal" throughout. That needs foreign
        // keys off, or the drop cascades. open() ensures that now; databases
        // migrated before it did lost the journal's children here.
        constexpr auto migration =
            "DROP INDEX operation_journal_state;"
            "CREATE TABLE operation_journal_v29 ("
            "id TEXT PRIMARY KEY NOT NULL, kind INTEGER NOT NULL CHECK(kind = 0), "
            "state INTEGER NOT NULL CHECK(state BETWEEN 0 AND 5), "
            "source_path BLOB NOT NULL, prepared_path BLOB NOT NULL, backup_path BLOB NOT NULL, "
            "expected_device BLOB NOT NULL, expected_inode BLOB NOT NULL, "
            "expected_size BLOB NOT NULL, expected_mtime_seconds BLOB NOT NULL, "
            "expected_mtime_nanoseconds BLOB NOT NULL, prepared_device BLOB, "
            "prepared_inode BLOB, prepared_size BLOB, prepared_mtime_seconds BLOB, "
            "prepared_mtime_nanoseconds BLOB, published_device BLOB, published_inode BLOB, "
            "published_size BLOB, published_mtime_seconds BLOB, "
            "published_mtime_nanoseconds BLOB, error_code INTEGER, error_message BLOB, "
            "content_kind INTEGER NOT NULL DEFAULT 0 CHECK(content_kind BETWEEN 0 AND 3));"
            "INSERT INTO operation_journal_v29 SELECT * FROM operation_journal;"
            "DROP TABLE operation_journal;"
            "ALTER TABLE operation_journal_v29 RENAME TO operation_journal;"
            "CREATE INDEX operation_journal_state ON operation_journal(state);"
            "UPDATE schema_version SET version = 29;";
        if (auto result = execute(database, migration); !result) {
            rollback();
            return result;
        }
    }
    if (version <= 29) {
        // ADR-0150: query substrate for the tkq-1 dialect — a bounded
        // per-value field table plus retained probe technicals. DDL-only;
        // existing rows repopulate on their next explicit Refresh.
        constexpr auto migration =
            "CREATE TABLE local_library_fields (raw_path BLOB NOT NULL "
            "REFERENCES local_library_tracks(raw_path) ON DELETE CASCADE, "
            "canonical_name TEXT NOT NULL, position INTEGER NOT NULL, "
            "value BLOB NOT NULL, value_lower BLOB NOT NULL, "
            "PRIMARY KEY (raw_path, canonical_name, position));"
            "CREATE INDEX local_library_fields_lookup ON "
            "local_library_fields(canonical_name, value_lower);"
            "ALTER TABLE local_library_tracks ADD COLUMN codec_name TEXT NOT NULL DEFAULT '';"
            "ALTER TABLE local_library_tracks ADD COLUMN sample_rate INTEGER NOT NULL DEFAULT 0;"
            "ALTER TABLE local_library_tracks ADD COLUMN bits INTEGER NOT NULL DEFAULT 0;"
            "ALTER TABLE local_library_tracks ADD COLUMN channels INTEGER NOT NULL DEFAULT 0;"
            "ALTER TABLE local_library_tracks ADD COLUMN duration_ms INTEGER NOT NULL DEFAULT -1;"
            "UPDATE schema_version SET version = 30;";
        if (auto result = execute(database, migration); !result) {
            rollback();
            return result;
        }
    }
    if (version <= 30) {
        constexpr auto migration = R"sql(-- SPDX-License-Identifier: GPL-3.0-only

ALTER TABLE operation_journal_artwork RENAME TO operation_journal_artwork_v30;

CREATE TABLE operation_journal_artwork (
    journal_id TEXT PRIMARY KEY NOT NULL
        REFERENCES operation_journal(id) ON DELETE CASCADE,
    intent_kind INTEGER NOT NULL CHECK(intent_kind BETWEEN 0 AND 3),
    target_ordinal INTEGER NOT NULL CHECK(target_ordinal >= 0),
    original_item_count INTEGER NOT NULL CHECK(original_item_count >= 0),
    planned_item_count INTEGER NOT NULL CHECK(planned_item_count >= 0),
    original_target_fingerprint BLOB
        CHECK(original_target_fingerprint IS NULL OR
              length(original_target_fingerprint) = 32),
    replacement_fingerprint BLOB
        CHECK(replacement_fingerprint IS NULL OR length(replacement_fingerprint) = 32),
    original_inventory_fingerprint BLOB NOT NULL
        CHECK(length(original_inventory_fingerprint) = 32),
    planned_inventory_fingerprint BLOB NOT NULL
        CHECK(length(planned_inventory_fingerprint) = 32),
    CHECK((intent_kind = 0 AND original_target_fingerprint IS NOT NULL AND
           replacement_fingerprint IS NOT NULL AND
           planned_item_count = original_item_count AND
           target_ordinal < original_item_count) OR
          (intent_kind = 1 AND original_target_fingerprint IS NOT NULL AND
           replacement_fingerprint IS NULL AND
           planned_item_count + 1 = original_item_count AND
           target_ordinal < original_item_count) OR
          (intent_kind = 2 AND original_target_fingerprint IS NULL AND
           replacement_fingerprint IS NOT NULL AND
           target_ordinal = original_item_count AND
           planned_item_count = original_item_count + 1) OR
          (intent_kind = 3 AND target_ordinal = 0 AND original_target_fingerprint IS NULL AND replacement_fingerprint IS NULL))
);

INSERT INTO operation_journal_artwork
SELECT * FROM operation_journal_artwork_v30;
DROP TABLE operation_journal_artwork_v30;
UPDATE schema_version SET version = 31;
)sql";
        if (auto result = execute(database, migration); !result) {
            rollback();
            return result;
        }
    }
    if (version <= 31) {
        constexpr auto migration = R"sql(-- SPDX-License-Identifier: GPL-3.0-only
CREATE TABLE saved_searches (
    id TEXT PRIMARY KEY NOT NULL,
    name BLOB NOT NULL UNIQUE CHECK(length(name) BETWEEN 1 AND 256),
    expression BLOB NOT NULL CHECK(length(expression) BETWEEN 1 AND 4096),
    dialect TEXT NOT NULL CHECK(dialect IN ('tkq-1', 'words-1')),
    scope INTEGER NOT NULL CHECK(scope IN (0, 1)),
    revision INTEGER NOT NULL CHECK(revision > 0)
);
UPDATE schema_version SET version = 32;
)sql";
        if (auto result = execute(database, migration); !result) {
            rollback();
            return result;
        }
    }
    if (version <= 32) {
        constexpr auto migration = R"sql(-- SPDX-License-Identifier: GPL-3.0-only
ALTER TABLE local_library_tracks ADD COLUMN field_index_complete INTEGER NOT NULL DEFAULT 0 CHECK(field_index_complete IN (0,1));
CREATE INDEX local_library_incomplete_fields ON local_library_tracks(raw_path) WHERE available=1 AND field_index_complete=0;
UPDATE schema_version SET version = 33;
)sql";
        if (auto result = execute(database, migration); !result) {
            rollback();
            return result;
        }
    }
    if (version <= 33) {
        constexpr auto migration = R"sql(-- SPDX-License-Identifier: GPL-3.0-only
ALTER TABLE metadata_transformation_action_values RENAME TO metadata_transformation_action_values_v33;
ALTER TABLE metadata_transformation_actions RENAME TO metadata_transformation_actions_v33;
CREATE TABLE metadata_transformation_actions (
    chain_id TEXT NOT NULL REFERENCES metadata_transformation_chains(id) ON DELETE CASCADE,
    position INTEGER NOT NULL,
    kind INTEGER NOT NULL CHECK(kind BETWEEN 0 AND 19),
    target_field BLOB NOT NULL,
    argument BLOB,
    dialect BLOB,
    dialect_version INTEGER,
    compiler_schema INTEGER,
    integer_argument INTEGER,
    integer_argument_2 INTEGER,
    PRIMARY KEY(chain_id, position)
);
CREATE TABLE metadata_transformation_action_values (
    chain_id TEXT NOT NULL,
    action_position INTEGER NOT NULL,
    position INTEGER NOT NULL,
    value BLOB NOT NULL,
    PRIMARY KEY(chain_id, action_position, position),
    FOREIGN KEY(chain_id, action_position)
        REFERENCES metadata_transformation_actions(chain_id, position) ON DELETE CASCADE
);
INSERT INTO metadata_transformation_actions SELECT * FROM metadata_transformation_actions_v33;
INSERT INTO metadata_transformation_action_values SELECT * FROM metadata_transformation_action_values_v33;
DROP TABLE metadata_transformation_action_values_v33;
DROP TABLE metadata_transformation_actions_v33;
UPDATE schema_version SET version = 34;
)sql";
        if (auto result = execute(database, migration); !result) {
            rollback();
            return result;
        }
    }
    if (version <= 34) {
        constexpr auto migration = R"sql(-- SPDX-License-Identifier: GPL-3.0-only
CREATE TABLE local_ratings (
    hash TEXT PRIMARY KEY,
    type TEXT NOT NULL CHECK(type IN ('track','album')),
    rating INTEGER NOT NULL CHECK(rating BETWEEN 1 AND 10),
    updated_at TEXT NOT NULL DEFAULT (datetime('now'))
);
ALTER TABLE local_library_tracks ADD COLUMN rating_hash TEXT NOT NULL DEFAULT '';
ALTER TABLE local_library_tracks ADD COLUMN album_rating_hash TEXT NOT NULL DEFAULT '';
CREATE INDEX local_library_rating_hash ON local_library_tracks(rating_hash);
CREATE INDEX local_library_album_rating_hash ON local_library_tracks(album_rating_hash);
UPDATE schema_version SET version = 35;
)sql";
        if (auto result = execute(database, migration); !result) {
            rollback();
            return result;
        }
    }
    if (version <= 35) {
        constexpr auto migration = R"sql(-- SPDX-License-Identifier: GPL-3.0-only
ALTER TABLE saved_searches RENAME TO saved_searches_v35;
CREATE TABLE saved_searches (
    id TEXT PRIMARY KEY NOT NULL,
    name BLOB NOT NULL UNIQUE CHECK(length(name) BETWEEN 1 AND 256),
    expression BLOB NOT NULL CHECK(length(expression) BETWEEN 1 AND 4096),
    dialect TEXT NOT NULL CHECK(dialect IN ('tkq-1', 'words-1')),
    scope INTEGER NOT NULL CHECK(scope IN (0, 1, 2)),
    revision INTEGER NOT NULL CHECK(revision > 0)
);
INSERT INTO saved_searches SELECT * FROM saved_searches_v35;
DROP TABLE saved_searches_v35;
UPDATE schema_version SET version = 36;
)sql";
        if (auto result = execute(database, migration); !result) {
            rollback();
            return result;
        }
    }
    if (version <= 36) {
        // ADR-0181: list documents may now use kind 2 (mpd). No DDL change;
        // the bump makes older builds refuse the database cleanly instead
        // of failing on an unknown document kind at load time.
        constexpr auto migration = "UPDATE schema_version SET version = 37;";
        if (auto result = execute(database, migration); !result) {
            rollback();
            return result;
        }
    }
    if (version <= 37) {
        constexpr auto migration =
            "DROP INDEX operation_journal_state;"
            "CREATE TABLE operation_journal_v38 ("
            "id TEXT PRIMARY KEY NOT NULL, kind INTEGER NOT NULL CHECK(kind = 0), "
            "state INTEGER NOT NULL CHECK(state BETWEEN 0 AND 5), "
            "source_path BLOB NOT NULL, prepared_path BLOB NOT NULL, backup_path BLOB NOT NULL, "
            "expected_device BLOB NOT NULL, expected_inode BLOB NOT NULL, "
            "expected_size BLOB NOT NULL, expected_mtime_seconds BLOB NOT NULL, "
            "expected_mtime_nanoseconds BLOB NOT NULL, prepared_device BLOB, "
            "prepared_inode BLOB, prepared_size BLOB, prepared_mtime_seconds BLOB, "
            "prepared_mtime_nanoseconds BLOB, published_device BLOB, published_inode BLOB, "
            "published_size BLOB, published_mtime_seconds BLOB, "
            "published_mtime_nanoseconds BLOB, error_code INTEGER, error_message BLOB, "
            "content_kind INTEGER NOT NULL DEFAULT 0 CHECK(content_kind BETWEEN 0 AND 4));"
            "INSERT INTO operation_journal_v38 SELECT * FROM operation_journal;"
            "DROP TABLE operation_journal;"
            "ALTER TABLE operation_journal_v38 RENAME TO operation_journal;"
            "CREATE INDEX operation_journal_state ON operation_journal(state);"
            "UPDATE schema_version SET version = 38;";
        if (auto result = execute(database, migration); !result) {
            rollback();
            return result;
        }
    }
    if (version <= 38) {
        constexpr auto migration = R"sql(-- SPDX-License-Identifier: GPL-3.0-only
CREATE TABLE local_listening_history (
    track_hash TEXT PRIMARY KEY NOT NULL,
    play_count INTEGER NOT NULL DEFAULT 0 CHECK(play_count >= 0),
    last_played_ms INTEGER NOT NULL DEFAULT 0 CHECK(last_played_ms >= 0),
    resume_position_ms INTEGER NOT NULL DEFAULT 0 CHECK(resume_position_ms >= 0),
    resume_updated_at_ms INTEGER NOT NULL DEFAULT 0 CHECK(resume_updated_at_ms >= 0)
);
CREATE INDEX local_listening_history_last_played
    ON local_listening_history(last_played_ms DESC);
UPDATE schema_version SET version = 39;
)sql";
        if (auto result = execute(database, migration); !result) {
            rollback();
            return result;
        }
    }
    if (version <= 39) {
        constexpr auto migration = R"sql(-- SPDX-License-Identifier: GPL-3.0-only
CREATE TABLE local_listening_sources (
    observation_hash TEXT PRIMARY KEY NOT NULL,
    source_id TEXT NOT NULL
);
CREATE TABLE local_listening_occurrences (
    occurrence_id TEXT PRIMARY KEY NOT NULL,
    track_hash TEXT NOT NULL,
    played_at_ms INTEGER NOT NULL CHECK(played_at_ms > 0)
);
UPDATE schema_version SET version = 40;
)sql";
        if (auto result = execute(database, migration); !result) {
            rollback();
            return result;
        }
    }
    if (version <= 40) {
        // ADR-0221: entries gain an identity independent of their row, so
        // reordering becomes an UPDATE of position alone. Uniqueness is per
        // document, not global: a ListItem copy carries its source's identity,
        // and the same track legitimately appears in several lists. NULLs
        // compare distinct in a SQLite unique index, so the index may be
        // created before the existing rows are stamped.
        constexpr auto migration = R"sql(-- SPDX-License-Identifier: GPL-3.0-only
ALTER TABLE list_items ADD COLUMN entry_id TEXT;
CREATE UNIQUE INDEX list_items_entry ON list_items(document_id, entry_id);
UPDATE schema_version SET version = 41;
)sql";
        if (auto result = execute(database, migration); !result) {
            rollback();
            return result;
        }
        // Rows written before this migration are stamped from C++: SQLite
        // cannot produce a StableId, and randomblob() in a subquery risks
        // being folded to a single value for every row, which would silently
        // give every entry the same identity. Addresses are collected before
        // any write so the read is not invalidated by its own updates.
        std::vector<std::pair<std::string, sqlite3_int64>> pending;
        {
            auto select =
                prepare(database, "SELECT document_id, position FROM list_items "
                                  "WHERE entry_id IS NULL ORDER BY document_id, position");
            if (!select) {
                rollback();
                return std::unexpected(std::move(select.error()));
            }
            while (sqlite3_step(select->get()) == SQLITE_ROW) {
                pending.emplace_back(column_text(select->get(), 0),
                                     sqlite3_column_int64(select->get(), 1));
            }
        }
        if (!pending.empty()) {
            auto update = prepare(database, "UPDATE list_items SET entry_id = ? "
                                            "WHERE document_id = ? AND position = ?");
            if (!update) {
                rollback();
                return std::unexpected(std::move(update.error()));
            }
            for (const auto& [document_id, position] : pending) {
                auto* statement = update->get();
                const auto entry_id = core::StableId::random().to_string();
                if (!bind_text(statement, 1, entry_id) || !bind_text(statement, 2, document_id) ||
                    sqlite3_bind_int64(statement, 3, position) != SQLITE_OK) {
                    auto error = database_error(database, "Could not bind list entry identity");
                    rollback();
                    return std::unexpected(std::move(error));
                }
                if (auto result =
                        step_done(database, statement, "Could not assign list entry identity");
                    !result) {
                    rollback();
                    return result;
                }
            }
        }
    }
    if (version <= 41) {
        // ADR-0220: the engine remembers its own queue, because the queue is
        // the engine's and a window is a view of it. Key/value rather than
        // modelled: what is stored is the engine's state document, versioned
        // in the key, and columns would mean a migration for every mode the
        // engine learns.
        constexpr auto migration = R"sql(-- SPDX-License-Identifier: GPL-3.0-only
CREATE TABLE engine_state (
    key TEXT PRIMARY KEY NOT NULL,
    value TEXT NOT NULL,
    updated_at_ms INTEGER NOT NULL
);
UPDATE schema_version SET version = 42;
)sql";
        if (auto result = execute(database, migration); !result) {
            rollback();
            return result;
        }
    }
    if (version <= 42) {
        // ADR-0227: which engine connection a list belongs to.
        constexpr auto migration = R"sql(-- SPDX-License-Identifier: GPL-3.0-only
ALTER TABLE list_documents ADD COLUMN remote INTEGER NOT NULL DEFAULT 0;
UPDATE schema_version SET version = 43;
)sql";
        if (auto result = execute(database, migration); !result) {
            rollback();
            return result;
        }
    }
    // Foreign keys are off while the schema changes (see open()); a rebuild
    // that left a reference dangling is refused here rather than committed.
    auto dangling = prepare(database, "PRAGMA foreign_key_check");
    if (!dangling) {
        rollback();
        return std::unexpected(std::move(dangling.error()));
    }
    if (sqlite3_step(dangling->get()) == SQLITE_ROW) {
        auto error = database_error(database, "A schema migration left a dangling reference");
        dangling->reset();
        rollback();
        return std::unexpected(std::move(error));
    }
    dangling->reset();
    if (auto result = execute(database, "COMMIT"); !result) {
        rollback();
        return result;
    }

    return {};
}

[[nodiscard]] core::Result<void> validate(std::span<const ListDocument> documents) {
    if (documents.size() > maximum_documents) {
        return std::unexpected(core::Error{.code = core::ErrorCode::limit_exceeded,
                                           .message = "At most 1024 list documents can be stored",
                                           .context = {}});
    }
    std::unordered_set<std::string> ids;
    ids.reserve(documents.size());
    for (const auto& document : documents) {
        const auto id = document.id.to_string();
        if (document.id.is_nil() || !ids.insert(id).second) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::invalid_argument,
                .message = "List document IDs must be non-nil and unique",
                .context = {},
            });
        }
        if (document.name.empty()) {
            return std::unexpected(core::Error{.code = core::ErrorCode::invalid_argument,
                                               .message = "List document names cannot be empty",
                                               .context = {}});
        }
        if (document.items.size() > maximum_items_per_document) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::limit_exceeded,
                .message = "A list document cannot contain more than 1000000 items",
                .context = {},
            });
        }
        for (const auto& item : document.items) {
            if (item.source_reference.empty()) {
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::invalid_argument,
                    .message = "List item source references cannot be empty",
                    .context = {},
                });
            }
            if (item.source == ListSource::mpd && !item.profile_id) {
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::invalid_argument,
                    .message = "Remote list items require an MPD profile identity",
                    .context = {},
                });
            }
            if (item.logical_reference && item.logical_reference->empty()) {
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::invalid_argument,
                    .message = "List item logical references cannot be empty",
                    .context = {},
                });
            }
            if (item.segment && (item.segment->start_sample < 0 ||
                                 (item.segment->end_sample &&
                                  *item.segment->end_sample <= item.segment->start_sample))) {
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::invalid_argument,
                    .message = "List item sample segments must be non-negative and increasing",
                    .context = {},
                });
            }
            if (item.source_selection && ((!item.source_selection->audio_stream_index &&
                                           !item.source_selection->subsong_index) ||
                                          (item.source_selection->audio_stream_index &&
                                           *item.source_selection->audio_stream_index < 0) ||
                                          (item.source_selection->subsong_index &&
                                           *item.source_selection->subsong_index < 0) ||
                                          item.source != ListSource::local)) {
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::invalid_argument,
                    .message = "List item decoder selections must be non-negative local sources",
                    .context = {},
                });
            }
            if (item.source_revision && item.source != ListSource::local) {
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::invalid_argument,
                    .message = "Only local list items may retain source revisions",
                    .context = {},
                });
            }
            if (item.fields.size() > maximum_fields_per_item) {
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::limit_exceeded,
                    .message = "A list item cannot contain more than 4096 snapshot fields",
                    .context = {},
                });
            }
            if (std::ranges::any_of(item.fields, [](const SnapshotField& field) {
                    return field.name.empty() || !valid_provenance(field.provenance);
                })) {
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::invalid_argument,
                    .message = "List item snapshot fields require names and valid provenance",
                    .context = {},
                });
            }
        }
    }
    return {};
}

[[nodiscard]] core::Result<std::vector<SnapshotField>>
flatten_source_fields(const metadata::MetadataDocument& document) {
    std::vector<SnapshotField> fields;
    for (const auto& field : document.fields) {
        if (field.canonical_name.empty() || !valid_provenance(field.provenance) ||
            field.provenance != metadata::FieldProvenance::embedded) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::invalid_argument,
                .message = "Committed source metadata must contain named embedded fields",
                .context = {},
            });
        }
        if (field.values.size() > maximum_fields_per_item - fields.size()) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::limit_exceeded,
                .message = "Committed source metadata exceeds the snapshot field limit",
                .context = {},
            });
        }
        for (const auto& value : field.values) {
            fields.push_back(SnapshotField{
                .name = field.canonical_name,
                .value = value,
                .native_name = field.native_name,
                .provenance = field.provenance,
                .language = field.qualifier.language,
                .description = field.qualifier.description,
            });
        }
    }
    return fields;
}

[[nodiscard]] bool source_layer(const SnapshotField& field) {
    return field.provenance == metadata::FieldProvenance::cached_snapshot ||
           field.provenance == metadata::FieldProvenance::embedded ||
           field.provenance == metadata::FieldProvenance::stream;
}

class LocalSourceRelocationResolver final {
  public:
    LocalSourceRelocationResolver(sqlite3* database, Statement next,
                                  std::unordered_set<std::string> source_references)
        : database_{database}, next_{std::move(next)},
          source_references_{std::move(source_references)} {}

    [[nodiscard]] core::Result<void>
    resolve(std::string& source_reference,
            std::optional<core::LocalSourceRevision>& source_revision) {
        if (!source_revision || !source_references_.contains(source_reference)) {
            return {};
        }
        sqlite3_int64 preceding_sequence = 0;
        for (std::size_t step_count = 0U; step_count < maximum_source_relocations; ++step_count) {
            auto* statement = next_.get();
            if (!bind_blob(statement, 1, source_reference) ||
                !bind_revision(statement, 2, *source_revision) ||
                sqlite3_bind_int64(statement, 7, preceding_sequence) != SQLITE_OK) {
                return std::unexpected(
                    database_error(database_, "Could not bind local-source relocation"));
            }
            const auto step = sqlite3_step(statement);
            if (step == SQLITE_DONE) {
                sqlite3_reset(statement);
                sqlite3_clear_bindings(statement);
                return {};
            }
            if (step != SQLITE_ROW) {
                auto error = database_error(database_, "Could not resolve local-source relocation");
                sqlite3_reset(statement);
                sqlite3_clear_bindings(statement);
                return std::unexpected(std::move(error));
            }
            const auto sequence = sqlite3_column_int64(statement, 0);
            auto target_reference = column_blob(statement, 1);
            auto published_revision = read_revision(statement, 2, "Local-source relocation");
            sqlite3_reset(statement);
            sqlite3_clear_bindings(statement);
            if (!published_revision) {
                return std::unexpected(std::move(published_revision.error()));
            }
            if (sequence <= preceding_sequence || target_reference.empty() ||
                target_reference == source_reference || published_revision->inode == 0U) {
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::database,
                    .message = "Local-source relocation history is invalid",
                    .context = {},
                });
            }
            preceding_sequence = sequence;
            source_reference = std::move(target_reference);
            source_revision = *published_revision;
            if (!source_references_.contains(source_reference)) {
                return {};
            }
        }
        return std::unexpected(core::Error{
            .code = core::ErrorCode::limit_exceeded,
            .message = "Local-source relocation history exceeds the replay limit",
            .context = {},
        });
    }

  private:
    sqlite3* database_{nullptr};
    Statement next_;
    std::unordered_set<std::string> source_references_;
};

[[nodiscard]] core::Result<LocalSourceRelocationResolver>
local_source_relocation_resolver(sqlite3* database) {
    auto sources = prepare(database, "SELECT DISTINCT source_reference "
                                     "FROM local_source_relocations");
    if (!sources) {
        return std::unexpected(std::move(sources.error()));
    }
    std::unordered_set<std::string> source_references;
    int step = SQLITE_ROW;
    while ((step = sqlite3_step(sources->get())) == SQLITE_ROW) {
        auto source_reference = column_blob(sources->get(), 0);
        if (source_reference.empty() || source_references.size() >= maximum_source_relocations ||
            !source_references.insert(std::move(source_reference)).second) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::database,
                .message = "Local-source relocation index is invalid",
                .context = {},
            });
        }
    }
    if (step != SQLITE_DONE) {
        return std::unexpected(
            database_error(database, "Could not load local-source relocation index"));
    }
    auto next = prepare(
        database,
        "SELECT sequence, target_reference, published_device, published_inode, published_size, "
        "published_mtime_seconds, published_mtime_nanoseconds "
        "FROM local_source_relocations WHERE source_reference = ? AND previous_device = ? "
        "AND previous_inode = ? AND previous_size = ? AND previous_mtime_seconds = ? "
        "AND previous_mtime_nanoseconds = ? AND sequence > ? ORDER BY sequence LIMIT 1");
    if (!next) {
        return std::unexpected(std::move(next.error()));
    }
    return LocalSourceRelocationResolver{database, std::move(*next), std::move(source_references)};
}

struct SerializedTransformationAction {
    int kind{0};
    std::string_view target_field;
    std::optional<std::string_view> argument;
    std::optional<std::string_view> dialect;
    std::optional<std::uint32_t> dialect_version;
    std::optional<std::uint32_t> compiler_schema;
    std::optional<std::uint32_t> integer_argument;
    std::optional<std::uint32_t> integer_argument_2;
    const std::vector<std::string>* values{nullptr};
};

[[nodiscard]] SerializedTransformationAction
serialize_transformation_action(const metadata::MetadataTransformationAction& action) {
    return std::visit(
        [](const auto& typed) -> SerializedTransformationAction {
            using Action = std::decay_t<decltype(typed)>;
            const std::string_view target = [&]() -> std::string_view {
                if constexpr (std::is_same_v<Action, metadata::MetadataCaptureValuesAction>) {
                    return typed.source;
                } else if constexpr (std::is_same_v<Action,
                                                    metadata::MetadataBlocklistFieldsAction> ||
                                     std::is_same_v<Action,
                                                    metadata::MetadataAllowlistFieldsAction>) {
                    return "*";
                } else {
                    return typed.target_field;
                }
            }();
            SerializedTransformationAction serialized{
                .kind = 0,
                .target_field = target,
                .argument = std::nullopt,
                .dialect = std::nullopt,
                .dialect_version = std::nullopt,
                .compiler_schema = std::nullopt,
                .integer_argument = std::nullopt,
                .integer_argument_2 = std::nullopt,
                .values = nullptr,
            };
            if constexpr (std::is_same_v<Action, metadata::MetadataSetValuesAction>) {
                serialized.kind = 0;
                serialized.values = &typed.values;
            } else if constexpr (std::is_same_v<Action, metadata::MetadataAddValuesAction>) {
                serialized.kind = 1;
                serialized.values = &typed.values;
            } else if constexpr (std::is_same_v<Action, metadata::MetadataRemoveFieldAction>) {
                serialized.kind = 2;
                if (typed.match_mode == metadata::MetadataFieldMatchMode::exact_native) {
                    serialized.integer_argument = 1U;
                }
            } else if constexpr (std::is_same_v<Action, metadata::MetadataRemoveFieldIfAction>) {
                serialized.kind = 15;
                serialized.argument = typed.condition;
                serialized.dialect = typed.dialect.dialect;
                serialized.dialect_version = typed.dialect.dialect_version;
                serialized.compiler_schema = typed.dialect.compiler_schema;
                if (typed.match_mode == metadata::MetadataFieldMatchMode::exact_native) {
                    serialized.integer_argument = 1U;
                }
            } else if constexpr (std::is_same_v<Action, metadata::MetadataTransformValuesAction>) {
                switch (typed.transform) {
                case metadata::MetadataValueTransformKind::trim_ascii:
                    serialized.kind = 3;
                    break;
                case metadata::MetadataValueTransformKind::lowercase:
                    serialized.kind = 4;
                    break;
                case metadata::MetadataValueTransformKind::uppercase:
                    serialized.kind = 5;
                    break;
                case metadata::MetadataValueTransformKind::capitalize_first:
                    serialized.kind = 10;
                    break;
                }
            } else if constexpr (std::is_same_v<Action, metadata::MetadataCopyFieldAction>) {
                serialized.kind = 6;
                serialized.argument = typed.source_field;
            } else if constexpr (std::is_same_v<Action, metadata::MetadataSplitValuesAction>) {
                serialized.kind = 7;
                serialized.argument = typed.separator;
            } else if constexpr (std::is_same_v<Action, metadata::MetadataJoinValuesAction>) {
                serialized.kind = 8;
                serialized.argument = typed.separator;
            } else if constexpr (std::is_same_v<Action, metadata::MetadataFormatValueAction>) {
                serialized.kind = 9;
                serialized.argument = typed.source;
                serialized.dialect = typed.dialect.dialect;
                serialized.dialect_version = typed.dialect.dialect_version;
                serialized.compiler_schema = typed.dialect.compiler_schema;
            } else if constexpr (std::is_same_v<Action,
                                                metadata::MetadataRemoveMatchingValuesAction>) {
                serialized.kind = 11;
                serialized.argument = typed.match;
            } else if constexpr (std::is_same_v<Action,
                                                metadata::MetadataReplaceMatchingValuesAction>) {
                serialized.kind = 12;
                serialized.argument = typed.match;
                serialized.values = &typed.replacement_values;
            } else if constexpr (std::is_same_v<Action,
                                                metadata::MetadataNumberSelectedItemsAction>) {
                serialized.kind = 13;
                serialized.integer_argument = typed.start;
                serialized.integer_argument_2 = typed.padding;
            } else if constexpr (std::is_same_v<Action,
                                                metadata::MetadataNumberGroupedItemsAction>) {
                serialized.kind = 17;
                serialized.argument = typed.group_expression;
                serialized.dialect = typed.dialect.dialect;
                serialized.dialect_version = typed.dialect.dialect_version;
                serialized.compiler_schema = typed.dialect.compiler_schema;
                serialized.integer_argument = typed.start;
                serialized.integer_argument_2 = typed.padding;
            } else if constexpr (std::is_same_v<Action,
                                                metadata::MetadataKeepFirstCharactersAction>) {
                serialized.kind = 14;
                serialized.integer_argument = typed.character_count;
            } else if constexpr (std::is_same_v<Action, metadata::MetadataCaptureValuesAction>) {
                serialized.kind = 16;
                serialized.argument = typed.pattern;
                serialized.dialect = typed.dialect.dialect;
                serialized.dialect_version = typed.dialect.dialect_version;
                serialized.compiler_schema = typed.dialect.compiler_schema;
                serialized.integer_argument = static_cast<std::uint32_t>(typed.source_kind);
            } else if constexpr (std::is_same_v<Action, metadata::MetadataBlocklistFieldsAction>) {
                serialized.kind = 18;
                serialized.values = &typed.fields;
            } else if constexpr (std::is_same_v<Action, metadata::MetadataAllowlistFieldsAction>) {
                serialized.kind = 19;
                serialized.values = &typed.fields;
            }
            return serialized;
        },
        action);
}

[[nodiscard]] core::Result<void>
validate_saved_transformation_chain(const SavedMetadataTransformationChain& saved_chain) {
    if (saved_chain.id.is_nil() || saved_chain.chain.name.empty()) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::invalid_argument,
            .message = "Saved metadata transformations require a non-nil ID and non-empty name",
            .context = {},
        });
    }
    return metadata::validate_metadata_transformation_chain(saved_chain.chain);
}

[[nodiscard]] core::Result<void>
validate_saved_output_layout_profile(const SavedOutputLayoutProfile& saved_profile) {
    if (saved_profile.id.is_nil()) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::invalid_argument,
            .message = "Saved output layouts require a non-nil ID",
            .context = {},
        });
    }
    return operations::validate_output_layout_profile(saved_profile.profile);
}

[[nodiscard]] core::Result<void>
validate_saved_destination_profile(const SavedDestinationProfile& saved_profile) {
    if (saved_profile.id.is_nil()) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::invalid_argument,
            .message = "Saved destinations require a non-nil ID",
            .context = {},
        });
    }
    return operations::validate_destination_profile(saved_profile.profile);
}

[[nodiscard]] core::Result<void>
validate_saved_encoder_preset(const SavedEncoderPreset& saved_preset) {
    const auto invalid = [](std::string message) {
        return std::unexpected(core::Error{.code = core::ErrorCode::invalid_argument,
                                           .message = std::move(message),
                                           .context = {}});
    };
    if (saved_preset.id.is_nil()) {
        return invalid("Saved encoder presets require a non-nil ID");
    }
    const auto& preset = saved_preset.preset;
    if (preset.id.empty() || preset.id.size() > 128U) {
        return invalid("Encoder preset identifiers must be 1-128 bytes");
    }
    if (preset.display_name.empty() || preset.display_name.size() > 1'024U) {
        return invalid("Encoder preset names must be 1-1024 bytes");
    }
    if (preset.codec_name.empty() || preset.codec_name.size() > 128U ||
        preset.container_name.empty() || preset.container_name.size() > 128U) {
        return invalid("Encoder preset codec and container names must be 1-128 bytes");
    }
    if (preset.file_extension.empty() || preset.file_extension.size() > 32U ||
        preset.file_extension.find('/') != std::string::npos ||
        preset.file_extension.find('.') != std::string::npos) {
        return invalid("Encoder preset extensions must be 1-32 bytes without separators");
    }
    if (preset.version < 1) {
        return invalid("Encoder preset versions start at 1");
    }
    if (preset.bit_rate && preset.vbr_quality) {
        return invalid("Encoder presets choose bit rate or VBR quality, never both");
    }
    if (preset.bit_rate && (*preset.bit_rate < 8'000 || *preset.bit_rate > 2'000'000)) {
        return invalid("Encoder preset bit rates must be 8-2000 kbps");
    }
    if (preset.vbr_quality && (*preset.vbr_quality < -2 || *preset.vbr_quality > 12)) {
        return invalid("Encoder preset VBR qualities must be between -2 and 12");
    }
    if (preset.sample_format_hint.size() > 32U) {
        return invalid("Encoder preset sample-format hints must be at most 32 bytes");
    }
    return {};
}

} // namespace

struct ListRepository::Impl {
    sqlite3* database{nullptr};

    ~Impl() {
        if (database != nullptr) {
            sqlite3_close(database);
        }
    }
};

ListRepository::ListRepository(std::unique_ptr<Impl> implementation)
    : implementation_(std::move(implementation)) {}

ListRepository::ListRepository(ListRepository&&) noexcept = default;
ListRepository& ListRepository::operator=(ListRepository&&) noexcept = default;
ListRepository::~ListRepository() = default;

core::Result<ListRepository> ListRepository::open(const std::filesystem::path& path) {
    auto implementation = std::make_unique<Impl>();
    const auto& encoded = path.native();
    if (sqlite3_open_v2(encoded.c_str(), &implementation->database,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK) {
        return std::unexpected(
            database_error(implementation->database, "Could not open Trackknife state database"));
    }
    sqlite3_busy_timeout(implementation->database, 2'000);
    if (auto result = execute(implementation->database, "PRAGMA journal_mode = WAL"); !result) {
        return std::unexpected(std::move(result.error()));
    }
    // Migrations run with foreign keys off. Rebuilding a table (create the
    // new shape, copy, drop the old, rename) is how SQLite changes a CHECK,
    // and with foreign keys on, the drop is a DELETE of every row that
    // cascades into every child table. Schemas 29 and 38 did exactly that
    // to the operation journal's evidence and its backup records. The pragma
    // is a no-op inside a transaction, so it is set before migrate() begins.
    if (auto result = execute(implementation->database, "PRAGMA foreign_keys = OFF"); !result) {
        return std::unexpected(std::move(result.error()));
    }
    if (auto result = migrate(implementation->database); !result) {
        return std::unexpected(std::move(result.error()));
    }
    if (auto result = execute(implementation->database, "PRAGMA foreign_keys = ON"); !result) {
        return std::unexpected(std::move(result.error()));
    }
    return ListRepository{std::move(implementation)};
}

core::Result<unsigned> ListRepository::schema_version() const {
    auto statement = prepare(implementation_->database, "SELECT version FROM schema_version");
    if (!statement) {
        return std::unexpected(std::move(statement.error()));
    }
    if (sqlite3_step(statement->get()) != SQLITE_ROW) {
        return std::unexpected(
            database_error(implementation_->database, "Could not read schema version"));
    }
    const auto version = sqlite3_column_int64(statement->get(), 0);
    if (version < 0 || version > std::numeric_limits<unsigned>::max()) {
        return std::unexpected(core::Error{.code = core::ErrorCode::database,
                                           .message = "Invalid SQLite schema version",
                                           .context = {}});
    }
    return static_cast<unsigned>(version);
}

core::Result<std::vector<ListDocument>> ListRepository::load_all() const {
    auto relocation_resolver = local_source_relocation_resolver(implementation_->database);
    if (!relocation_resolver) {
        return std::unexpected(std::move(relocation_resolver.error()));
    }
    auto documents_query = prepare(
        implementation_->database,
        "SELECT id, kind, name, pinned, dirty, remote FROM list_documents ORDER BY position");
    if (!documents_query) {
        return std::unexpected(std::move(documents_query.error()));
    }
    std::vector<ListDocument> documents;
    std::unordered_map<std::string, std::size_t> document_indices;
    while (sqlite3_step(documents_query->get()) == SQLITE_ROW) {
        const auto id_text = column_text(documents_query->get(), 0);
        auto id = core::StableId::parse(id_text);
        const auto kind = sqlite3_column_int(documents_query->get(), 1);
        if (!id || (kind != static_cast<int>(ListKind::scratch) &&
                    kind != static_cast<int>(ListKind::saved) &&
                    kind != static_cast<int>(ListKind::mpd))) {
            return std::unexpected(core::Error{.code = core::ErrorCode::database,
                                               .message = "Invalid persisted list document",
                                               .context = {{"document_id", id_text}}});
        }
        document_indices.emplace(id_text, documents.size());
        documents.push_back(ListDocument{
            .id = *id,
            .kind = static_cast<ListKind>(kind),
            .name = column_blob(documents_query->get(), 2),
            .pinned = sqlite3_column_int(documents_query->get(), 3) != 0,
            .dirty = sqlite3_column_int(documents_query->get(), 4) != 0,
            .items = {},
            .remote = sqlite3_column_int(documents_query->get(), 5) != 0,
        });
    }
    if (sqlite3_errcode(implementation_->database) != SQLITE_OK &&
        sqlite3_errcode(implementation_->database) != SQLITE_DONE) {
        return std::unexpected(
            database_error(implementation_->database, "Could not load list documents"));
    }

    auto items_query =
        prepare(implementation_->database,
                "SELECT document_id, position, source, profile_id, source_reference, duration_ms, "
                "logical_reference, segment_start_sample, segment_end_sample, "
                "selected_audio_stream, codec_subsong_index, observed_device, observed_inode, "
                "observed_size, observed_mtime_seconds, observed_mtime_nanoseconds, entry_id "
                "FROM list_items ORDER BY document_id, position");
    if (!items_query) {
        return std::unexpected(std::move(items_query.error()));
    }
    while (sqlite3_step(items_query->get()) == SQLITE_ROW) {
        const auto document_id = column_text(items_query->get(), 0);
        const auto found = document_indices.find(document_id);
        const auto position = sqlite3_column_int64(items_query->get(), 1);
        const auto source = sqlite3_column_int(items_query->get(), 2);
        if (found == document_indices.end() || position < 0 ||
            static_cast<std::size_t>(position) != documents[found->second].items.size() ||
            (source != static_cast<int>(ListSource::mpd) &&
             source != static_cast<int>(ListSource::local))) {
            return std::unexpected(core::Error{.code = core::ErrorCode::database,
                                               .message = "Invalid persisted list item order",
                                               .context = {{"document_id", document_id}}});
        }
        std::optional<core::StableId> profile_id;
        if (sqlite3_column_type(items_query->get(), 3) != SQLITE_NULL) {
            auto parsed = core::StableId::parse(column_text(items_query->get(), 3));
            if (!parsed) {
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::database,
                    .message = "Invalid persisted list item profile identity",
                    .context = {{"document_id", document_id}},
                });
            }
            profile_id = *parsed;
        }
        std::optional<std::int64_t> duration;
        if (sqlite3_column_type(items_query->get(), 5) != SQLITE_NULL) {
            duration = sqlite3_column_int64(items_query->get(), 5);
        }
        std::optional<std::string> logical_reference;
        if (sqlite3_column_type(items_query->get(), 6) != SQLITE_NULL) {
            logical_reference = column_blob(items_query->get(), 6);
        }
        std::optional<ListItemSegment> segment;
        if (sqlite3_column_type(items_query->get(), 7) != SQLITE_NULL) {
            segment = ListItemSegment{.start_sample = sqlite3_column_int64(items_query->get(), 7),
                                      .end_sample = std::nullopt};
            if (sqlite3_column_type(items_query->get(), 8) != SQLITE_NULL) {
                segment->end_sample = sqlite3_column_int64(items_query->get(), 8);
            }
        } else if (sqlite3_column_type(items_query->get(), 8) != SQLITE_NULL) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::database,
                .message = "Persisted list item has a segment end without a start",
                .context = {{"document_id", document_id}},
            });
        }
        std::optional<ListItemSourceSelection> source_selection;
        const bool has_audio_stream = sqlite3_column_type(items_query->get(), 9) != SQLITE_NULL;
        const bool has_subsong = sqlite3_column_type(items_query->get(), 10) != SQLITE_NULL;
        if (has_audio_stream || has_subsong) {
            source_selection = ListItemSourceSelection{
                .audio_stream_index = has_audio_stream
                                          ? std::optional{sqlite3_column_int(items_query->get(), 9)}
                                          : std::nullopt,
                .subsong_index = has_subsong
                                     ? std::optional{sqlite3_column_int(items_query->get(), 10)}
                                     : std::nullopt,
            };
        }
        auto source_revision = read_optional_revision(items_query->get(), 11, "List item");
        if (!source_revision) {
            return std::unexpected(std::move(source_revision.error()));
        }
        if ((logical_reference && logical_reference->empty()) ||
            (segment && (segment->start_sample < 0 ||
                         (segment->end_sample && *segment->end_sample <= segment->start_sample))) ||
            (source_selection &&
             ((source_selection->audio_stream_index && *source_selection->audio_stream_index < 0) ||
              (source_selection->subsong_index && *source_selection->subsong_index < 0) ||
              source != static_cast<int>(ListSource::local))) ||
            (*source_revision && source != static_cast<int>(ListSource::local))) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::database,
                .message = "Persisted list item has an invalid logical segment",
                .context = {{"document_id", document_id}},
            });
        }
        auto source_reference = column_blob(items_query->get(), 4);
        if (source == static_cast<int>(ListSource::local)) {
            if (auto resolved = relocation_resolver->resolve(source_reference, *source_revision);
                !resolved) {
                return std::unexpected(std::move(resolved.error()));
            }
        }
        // A row predating migration 41, or one written by an older build,
        // carries no identity. The default-constructed fresh one stands in
        // rather than failing the load; it is persisted by the next save.
        auto entry_id = core::StableId::random();
        if (sqlite3_column_type(items_query->get(), 16) != SQLITE_NULL) {
            auto parsed = core::StableId::parse(column_text(items_query->get(), 16));
            if (!parsed) {
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::database,
                    .message = "Invalid persisted list entry identity",
                    .context = {{"document_id", document_id}},
                });
            }
            entry_id = *parsed;
        }
        documents[found->second].items.push_back(ListItem{
            .entry_id = entry_id,
            .source = static_cast<ListSource>(source),
            .profile_id = profile_id,
            .source_reference = std::move(source_reference),
            .logical_reference = std::move(logical_reference),
            .segment = segment,
            .source_selection = source_selection,
            .duration_ms = duration,
            .source_revision = *source_revision,
            .fields = {},
        });
    }

    auto fields_query =
        prepare(implementation_->database,
                "SELECT document_id, item_position, position, name, value, native_name, "
                "provenance, language, description FROM list_item_fields "
                "ORDER BY document_id, item_position, position");
    if (!fields_query) {
        return std::unexpected(std::move(fields_query.error()));
    }
    while (sqlite3_step(fields_query->get()) == SQLITE_ROW) {
        const auto document_id = column_text(fields_query->get(), 0);
        const auto found = document_indices.find(document_id);
        const auto item_position = sqlite3_column_int64(fields_query->get(), 1);
        const auto field_position = sqlite3_column_int64(fields_query->get(), 2);
        if (found == document_indices.end() || item_position < 0 ||
            static_cast<std::size_t>(item_position) >= documents[found->second].items.size()) {
            return std::unexpected(core::Error{.code = core::ErrorCode::database,
                                               .message = "Invalid persisted snapshot field item",
                                               .context = {{"document_id", document_id}}});
        }
        auto& fields =
            documents[found->second].items[static_cast<std::size_t>(item_position)].fields;
        if (field_position < 0 || static_cast<std::size_t>(field_position) != fields.size()) {
            return std::unexpected(core::Error{.code = core::ErrorCode::database,
                                               .message = "Invalid persisted snapshot field order",
                                               .context = {{"document_id", document_id}}});
        }
        const auto provenance = sqlite3_column_int(fields_query->get(), 6);
        if (provenance < static_cast<int>(metadata::FieldProvenance::cached_snapshot) ||
            provenance > static_cast<int>(metadata::FieldProvenance::sidecar)) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::database,
                .message = "Persisted snapshot field has invalid provenance",
                .context = {{"document_id", document_id}},
            });
        }
        fields.push_back(SnapshotField{
            .name = column_blob(fields_query->get(), 3),
            .value = column_blob(fields_query->get(), 4),
            .native_name = column_blob(fields_query->get(), 5),
            .provenance = static_cast<metadata::FieldProvenance>(provenance),
            .language = optional_blob(fields_query->get(), 7),
            .description = optional_blob(fields_query->get(), 8),
        });
    }

    struct CachedSource {
        core::LocalSourceRevision previous_revision;
        core::LocalSourceRevision published_revision;
        std::vector<SnapshotField> fields;
    };
    std::unordered_map<std::string, CachedSource> cached_sources;
    auto caches_query = prepare(
        implementation_->database,
        "SELECT source_reference, previous_device, previous_inode, previous_size, "
        "previous_mtime_seconds, previous_mtime_nanoseconds, published_device, published_inode, "
        "published_size, published_mtime_seconds, published_mtime_nanoseconds "
        "FROM local_metadata_cache");
    if (!caches_query) {
        return std::unexpected(std::move(caches_query.error()));
    }
    while (sqlite3_step(caches_query->get()) == SQLITE_ROW) {
        auto previous_revision = read_revision(caches_query->get(), 1, "Local metadata cache");
        auto published_revision = read_revision(caches_query->get(), 6, "Local metadata cache");
        if (!previous_revision) {
            auto error = std::move(previous_revision.error());
            return std::unexpected(std::move(error));
        }
        if (!published_revision) {
            auto error = std::move(published_revision.error());
            return std::unexpected(std::move(error));
        }
        const auto source_reference = column_blob(caches_query->get(), 0);
        if (source_reference.empty() || cached_sources.size() >= maximum_cached_sources ||
            !cached_sources
                 .emplace(source_reference, CachedSource{.previous_revision = *previous_revision,
                                                         .published_revision = *published_revision,
                                                         .fields = {}})
                 .second) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::database,
                .message = "Local metadata cache contains an invalid source identity",
                .context = {},
            });
        }
    }
    auto cache_fields_query = prepare(
        implementation_->database,
        "SELECT source_reference, position, name, value, native_name, provenance, language, "
        "description FROM local_metadata_cache_fields ORDER BY source_reference, position");
    if (!cache_fields_query) {
        return std::unexpected(std::move(cache_fields_query.error()));
    }
    while (sqlite3_step(cache_fields_query->get()) == SQLITE_ROW) {
        const auto source_reference = column_blob(cache_fields_query->get(), 0);
        const auto found = cached_sources.find(source_reference);
        const auto position = sqlite3_column_int64(cache_fields_query->get(), 1);
        const auto provenance = sqlite3_column_int(cache_fields_query->get(), 5);
        if (found == cached_sources.end() || position < 0 ||
            static_cast<std::size_t>(position) != found->second.fields.size() ||
            found->second.fields.size() >= maximum_fields_per_item ||
            provenance != static_cast<int>(metadata::FieldProvenance::embedded)) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::database,
                .message = "Local metadata cache contains an invalid field",
                .context = {},
            });
        }
        found->second.fields.push_back(SnapshotField{
            .name = column_blob(cache_fields_query->get(), 2),
            .value = column_blob(cache_fields_query->get(), 3),
            .native_name = column_blob(cache_fields_query->get(), 4),
            .provenance = metadata::FieldProvenance::embedded,
            .language = optional_blob(cache_fields_query->get(), 6),
            .description = optional_blob(cache_fields_query->get(), 7),
        });
    }
    std::unordered_set<std::string> externally_refreshed_sources;
    for (const auto& document : documents) {
        for (const auto& item : document.items) {
            const auto cached = cached_sources.find(item.source_reference);
            if (item.source == ListSource::local && cached != cached_sources.end() &&
                item.source_revision && *item.source_revision != cached->second.previous_revision &&
                *item.source_revision != cached->second.published_revision) {
                externally_refreshed_sources.insert(item.source_reference);
            }
        }
    }
    for (auto& document : documents) {
        for (auto& item : document.items) {
            if (item.source != ListSource::local) {
                continue;
            }
            const auto cached = cached_sources.find(item.source_reference);
            if (cached == cached_sources.end() ||
                externally_refreshed_sources.contains(item.source_reference)) {
                continue;
            }
            if (item.logical_reference &&
                std::ranges::any_of(item.fields, [](const SnapshotField& field) {
                    return field.provenance == metadata::FieldProvenance::cached_snapshot;
                })) {
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::database,
                    .message = "A logical-track snapshot must be reprobed before cache refresh",
                    .context = {{"document_id", document.id.to_string()}},
                });
            }
            std::erase_if(item.fields, source_layer);
            item.fields.insert(item.fields.begin(), cached->second.fields.begin(),
                               cached->second.fields.end());
            item.source_revision = cached->second.published_revision;
        }
    }
    return documents;
}

core::Result<void> ListRepository::replace_all(const std::span<const ListDocument> documents) {
    if (auto result = validate(documents); !result) {
        return result;
    }
    auto* database = implementation_->database;
    if (auto result = execute(database, "BEGIN IMMEDIATE"); !result) {
        return result;
    }
    const auto rollback = [database] { static_cast<void>(execute(database, "ROLLBACK")); };
    auto relocation_resolver = local_source_relocation_resolver(database);
    if (!relocation_resolver) {
        auto error = std::move(relocation_resolver.error());
        rollback();
        return std::unexpected(std::move(error));
    }
    if (auto result = execute(database, "DELETE FROM list_documents"); !result) {
        rollback();
        return result;
    }
    auto insert_document = prepare(
        database, "INSERT INTO list_documents(id, kind, name, pinned, dirty, position, remote) "
                  "VALUES(?,?,?,?,?,?,?)");
    auto insert_item = prepare(
        database,
        "INSERT INTO list_items(document_id, position, source, profile_id, source_reference, "
        "duration_ms, logical_reference, segment_start_sample, segment_end_sample, "
        "selected_audio_stream, codec_subsong_index, observed_device, observed_inode, "
        "observed_size, observed_mtime_seconds, observed_mtime_nanoseconds, entry_id) "
        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
    auto insert_field = prepare(
        database, "INSERT INTO list_item_fields(document_id, item_position, position, name, value, "
                  "native_name, provenance, language, description) VALUES(?,?,?,?,?,?,?,?,?)");
    if (!insert_document || !insert_item || !insert_field) {
        auto error = !insert_document ? std::move(insert_document.error())
                     : !insert_item   ? std::move(insert_item.error())
                                      : std::move(insert_field.error());
        rollback();
        return std::unexpected(std::move(error));
    }

    for (std::size_t document_position = 0U; document_position < documents.size();
         ++document_position) {
        const auto& document = documents[document_position];
        const auto id = document.id.to_string();
        auto* document_statement = insert_document->get();
        if (!bind_text(document_statement, 1, id) ||
            sqlite3_bind_int(document_statement, 2, static_cast<int>(document.kind)) != SQLITE_OK ||
            !bind_blob(document_statement, 3, document.name) ||
            sqlite3_bind_int(document_statement, 4, document.pinned ? 1 : 0) != SQLITE_OK ||
            sqlite3_bind_int(document_statement, 5, document.dirty ? 1 : 0) != SQLITE_OK ||
            sqlite3_bind_int64(document_statement, 6,
                               static_cast<sqlite3_int64>(document_position)) != SQLITE_OK ||
            sqlite3_bind_int(document_statement, 7, document.remote ? 1 : 0) != SQLITE_OK) {
            auto error = database_error(database, "Could not bind list document");
            rollback();
            return std::unexpected(std::move(error));
        }
        if (auto result = step_done(database, document_statement, "Could not store list document");
            !result) {
            rollback();
            return std::unexpected(std::move(result.error()));
        }

        // Entry identities are unique within a document; see the stamping
        // note below.
        std::set<core::StableId> document_entry_ids;
        for (std::size_t item_position = 0U; item_position < document.items.size();
             ++item_position) {
            const auto& item = document.items[item_position];
            auto source_reference = item.source_reference;
            auto source_revision = item.source_revision;
            if (item.source == ListSource::local) {
                if (auto resolved = relocation_resolver->resolve(source_reference, source_revision);
                    !resolved) {
                    auto error = std::move(resolved.error());
                    rollback();
                    return std::unexpected(std::move(error));
                }
            }
            auto* item_statement = insert_item->get();
            const auto profile_id = item.profile_id ? item.profile_id->to_string() : std::string{};
            const auto profile_bound = item.profile_id
                                           ? bind_text(item_statement, 4, profile_id)
                                           : sqlite3_bind_null(item_statement, 4) == SQLITE_OK;
            const auto duration_bound =
                item.duration_ms
                    ? sqlite3_bind_int64(item_statement, 6, *item.duration_ms) == SQLITE_OK
                    : sqlite3_bind_null(item_statement, 6) == SQLITE_OK;
            const auto logical_reference_bound =
                item.logical_reference ? bind_blob(item_statement, 7, *item.logical_reference)
                                       : sqlite3_bind_null(item_statement, 7) == SQLITE_OK;
            const auto segment_start_bound =
                item.segment
                    ? sqlite3_bind_int64(item_statement, 8, item.segment->start_sample) == SQLITE_OK
                    : sqlite3_bind_null(item_statement, 8) == SQLITE_OK;
            const auto segment_end_bound =
                item.segment && item.segment->end_sample
                    ? sqlite3_bind_int64(item_statement, 9, *item.segment->end_sample) == SQLITE_OK
                    : sqlite3_bind_null(item_statement, 9) == SQLITE_OK;
            const auto audio_stream_bound =
                item.source_selection && item.source_selection->audio_stream_index
                    ? sqlite3_bind_int(item_statement, 10,
                                       *item.source_selection->audio_stream_index) == SQLITE_OK
                    : sqlite3_bind_null(item_statement, 10) == SQLITE_OK;
            const auto subsong_bound =
                item.source_selection && item.source_selection->subsong_index
                    ? sqlite3_bind_int(item_statement, 11, *item.source_selection->subsong_index) ==
                          SQLITE_OK
                    : sqlite3_bind_null(item_statement, 11) == SQLITE_OK;
            // A nil identity means the entry was built by code predating
            // ADR-0221; stamp one rather than writing a NULL that the unique
            // index would tolerate and the loader would have to invent again.
            //
            // A repeat within this document means the entry was copied from
            // another — `items = {item, item}` is ordinary — and a copy is a
            // new entry that needs its own identity. Stamping here keeps the
            // invariant at the persistence boundary instead of asking every
            // caller to remember it.
            auto resolved_entry_id = item.entry_id;
            while (resolved_entry_id.is_nil() ||
                   !document_entry_ids.insert(resolved_entry_id).second) {
                resolved_entry_id = core::StableId::random();
            }
            const auto entry_id = resolved_entry_id.to_string();
            if (!bind_text(item_statement, 1, id) ||
                sqlite3_bind_int64(item_statement, 2, static_cast<sqlite3_int64>(item_position)) !=
                    SQLITE_OK ||
                sqlite3_bind_int(item_statement, 3, static_cast<int>(item.source)) != SQLITE_OK ||
                !profile_bound || !bind_blob(item_statement, 5, source_reference) ||
                !duration_bound || !logical_reference_bound || !segment_start_bound ||
                !segment_end_bound || !audio_stream_bound || !subsong_bound ||
                !bind_optional_revision(item_statement, 12, source_revision) ||
                !bind_text(item_statement, 17, entry_id)) {
                auto error = database_error(database, "Could not bind list item");
                rollback();
                return std::unexpected(std::move(error));
            }
            if (auto result = step_done(database, item_statement, "Could not store list item");
                !result) {
                rollback();
                return result;
            }

            for (std::size_t field_position = 0U; field_position < item.fields.size();
                 ++field_position) {
                const auto& field = item.fields[field_position];
                auto* field_statement = insert_field->get();
                if (!bind_text(field_statement, 1, id) ||
                    sqlite3_bind_int64(field_statement, 2,
                                       static_cast<sqlite3_int64>(item_position)) != SQLITE_OK ||
                    sqlite3_bind_int64(field_statement, 3,
                                       static_cast<sqlite3_int64>(field_position)) != SQLITE_OK ||
                    !bind_blob(field_statement, 4, field.name) ||
                    !bind_blob(field_statement, 5, field.value) ||
                    !bind_blob(field_statement, 6, field.native_name) ||
                    sqlite3_bind_int(field_statement, 7, static_cast<int>(field.provenance)) !=
                        SQLITE_OK ||
                    !bind_optional_blob(field_statement, 8, field.language) ||
                    !bind_optional_blob(field_statement, 9, field.description)) {
                    auto error = database_error(database, "Could not bind snapshot field");
                    rollback();
                    return std::unexpected(std::move(error));
                }
                if (auto result =
                        step_done(database, field_statement, "Could not store snapshot field");
                    !result) {
                    rollback();
                    return result;
                }
            }
        }
    }
    if (auto result = execute(database, "COMMIT"); !result) {
        rollback();
        return result;
    }
    return {};
}

core::Result<LocalMetadataRefreshResult>
ListRepository::refresh_local_metadata(const LocalMetadataRefresh& refresh) {
    if (refresh.operation_id.is_nil() || refresh.source_reference.empty() ||
        refresh.source_reference.find('\0') != std::string::npos) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::invalid_argument,
            .message = "Local metadata refresh requires an operation identity and raw path",
            .context = {},
        });
    }
    auto source_fields = flatten_source_fields(refresh.document);
    if (!source_fields) {
        return std::unexpected(std::move(source_fields.error()));
    }

    auto* database = implementation_->database;
    if (auto result = execute(database, "BEGIN IMMEDIATE"); !result) {
        return std::unexpected(std::move(result.error()));
    }
    const auto rollback = [database] { static_cast<void>(execute(database, "ROLLBACK")); };
    const auto operation_id = refresh.operation_id.to_string();
    auto replay_query = prepare(
        database,
        "SELECT source_reference, previous_device, previous_inode, previous_size, "
        "previous_mtime_seconds, previous_mtime_nanoseconds, published_device, published_inode, "
        "published_size, published_mtime_seconds, published_mtime_nanoseconds, "
        "affected_occurrences "
        "FROM local_metadata_refreshes WHERE operation_id = ?");
    if (!replay_query || !bind_text(replay_query->get(), 1, operation_id)) {
        auto error = replay_query ? database_error(database, "Could not bind metadata replay")
                                  : std::move(replay_query.error());
        rollback();
        return std::unexpected(std::move(error));
    }
    const auto replay_step = sqlite3_step(replay_query->get());
    if (replay_step == SQLITE_ROW) {
        auto previous_revision = read_revision(replay_query->get(), 1, "Local metadata refresh");
        auto published_revision = read_revision(replay_query->get(), 6, "Local metadata refresh");
        const auto affected = sqlite3_column_int64(replay_query->get(), 11);
        if (!previous_revision) {
            auto error = std::move(previous_revision.error());
            rollback();
            return std::unexpected(std::move(error));
        }
        if (!published_revision) {
            auto error = std::move(published_revision.error());
            rollback();
            return std::unexpected(std::move(error));
        }
        if (column_blob(replay_query->get(), 0) != refresh.source_reference ||
            *previous_revision != refresh.previous_revision ||
            *published_revision != refresh.published_revision || affected <= 0 ||
            static_cast<std::uint64_t>(affected) > maximum_refresh_occurrences) {
            rollback();
            return std::unexpected(core::Error{
                .code = core::ErrorCode::conflict,
                .message = "Metadata refresh identity was replayed with different content",
                .context = {{"operation_id", operation_id}},
            });
        }
        rollback();
        return LocalMetadataRefreshResult{
            .affected_occurrences = static_cast<std::size_t>(affected),
            .already_applied = true,
        };
    }
    if (replay_step != SQLITE_DONE) {
        auto error = database_error(database, "Could not inspect metadata refresh replay");
        rollback();
        return std::unexpected(std::move(error));
    }

    struct Occurrence {
        std::string document_id;
        sqlite3_int64 position{0};
        std::vector<SnapshotField> retained_fields;
    };
    std::vector<Occurrence> occurrences;
    auto occurrence_query = prepare(
        database, "SELECT li.document_id, li.position, li.logical_reference, "
                  "EXISTS(SELECT 1 FROM list_item_fields f WHERE f.document_id = li.document_id "
                  "AND f.item_position = li.position AND f.provenance = 0) "
                  "FROM list_items li WHERE li.source = ? AND li.source_reference = ? "
                  "ORDER BY li.document_id, li.position");
    if (!occurrence_query ||
        sqlite3_bind_int(occurrence_query->get(), 1, static_cast<int>(ListSource::local)) !=
            SQLITE_OK ||
        !bind_blob(occurrence_query->get(), 2, refresh.source_reference)) {
        auto error = occurrence_query
                         ? database_error(database, "Could not bind metadata occurrences")
                         : std::move(occurrence_query.error());
        rollback();
        return std::unexpected(std::move(error));
    }
    int occurrence_step = SQLITE_ROW;
    while ((occurrence_step = sqlite3_step(occurrence_query->get())) == SQLITE_ROW) {
        const bool logical = sqlite3_column_type(occurrence_query->get(), 2) != SQLITE_NULL;
        const bool legacy_snapshot = sqlite3_column_int(occurrence_query->get(), 3) != 0;
        if (logical && legacy_snapshot) {
            rollback();
            return std::unexpected(core::Error{
                .code = core::ErrorCode::conflict,
                .message = "Logical tracks must be freshly probed before metadata commit",
                .context = {{"source_path", refresh.source_reference}},
            });
        }
        if (occurrences.size() >= maximum_refresh_occurrences) {
            rollback();
            return std::unexpected(core::Error{
                .code = core::ErrorCode::limit_exceeded,
                .message = "Metadata refresh exceeds the occurrence limit",
                .context = {{"source_path", refresh.source_reference}},
            });
        }
        const auto position = sqlite3_column_int64(occurrence_query->get(), 1);
        if (position < 0) {
            auto error = database_error(database, "Metadata occurrence has an invalid position");
            rollback();
            return std::unexpected(std::move(error));
        }
        occurrences.push_back(Occurrence{
            .document_id = column_text(occurrence_query->get(), 0),
            .position = position,
            .retained_fields = {},
        });
    }
    if (occurrence_step != SQLITE_DONE) {
        auto error = database_error(database, "Could not enumerate metadata occurrences");
        rollback();
        return std::unexpected(std::move(error));
    }
    if (auto linked =
            link_listening_source(database, refresh.source_reference, refresh.previous_revision,
                                  refresh.source_reference, refresh.published_revision);
        !linked) {
        rollback();
        return std::unexpected(std::move(linked.error()));
    }
    if (occurrences.empty()) {
        // A mapped-file editor need not create a list occurrence. There is no
        // list snapshot to reconcile or record, but the optional library may
        // still contain this source. Invalidate its scan revision (retaining
        // displayed tags until rescan) rather than publishing possibly stale
        // document data on a recovery retry. The operation journal owns commit
        // evidence; there is no list-refresh record for zero affected rows.
        if (auto result = refresh_library_source(database, refresh.source_reference,
                                                 refresh.source_reference, nullptr);
            !result) {
            rollback();
            return std::unexpected(std::move(result.error()));
        }
        if (auto result = execute(database, "COMMIT"); !result) {
            rollback();
            return std::unexpected(std::move(result.error()));
        }
        return LocalMetadataRefreshResult{.affected_occurrences = 0U, .already_applied = false};
    }

    auto retained_query =
        prepare(database, "SELECT name, value, native_name, provenance, language, description "
                          "FROM list_item_fields WHERE document_id = ? AND item_position = ? "
                          "AND provenance NOT IN (0, 2, 3) ORDER BY position");
    if (!retained_query) {
        auto error = std::move(retained_query.error());
        rollback();
        return std::unexpected(std::move(error));
    }
    for (auto& occurrence : occurrences) {
        auto* statement = retained_query->get();
        if (!bind_text(statement, 1, occurrence.document_id) ||
            sqlite3_bind_int64(statement, 2, occurrence.position) != SQLITE_OK) {
            auto error = database_error(database, "Could not bind retained metadata fields");
            rollback();
            return std::unexpected(std::move(error));
        }
        int step = SQLITE_ROW;
        while ((step = sqlite3_step(statement)) == SQLITE_ROW) {
            const auto provenance = sqlite3_column_int(statement, 3);
            if (provenance < static_cast<int>(metadata::FieldProvenance::annotation) ||
                provenance > static_cast<int>(metadata::FieldProvenance::sidecar) ||
                provenance == static_cast<int>(metadata::FieldProvenance::embedded) ||
                provenance == static_cast<int>(metadata::FieldProvenance::stream)) {
                auto error = database_error(database, "Invalid retained metadata provenance");
                rollback();
                return std::unexpected(std::move(error));
            }
            occurrence.retained_fields.push_back(SnapshotField{
                .name = column_blob(statement, 0),
                .value = column_blob(statement, 1),
                .native_name = column_blob(statement, 2),
                .provenance = static_cast<metadata::FieldProvenance>(provenance),
                .language = optional_blob(statement, 4),
                .description = optional_blob(statement, 5),
            });
        }
        sqlite3_reset(statement);
        sqlite3_clear_bindings(statement);
        if (step != SQLITE_DONE ||
            occurrence.retained_fields.size() > maximum_fields_per_item - source_fields->size()) {
            if (step != SQLITE_DONE) {
                auto error = database_error(database, "Could not load retained metadata fields");
                rollback();
                return std::unexpected(std::move(error));
            }
            rollback();
            return std::unexpected(core::Error{
                .code = core::ErrorCode::limit_exceeded,
                .message = "Refreshed occurrence exceeds the field limit",
                .context = {{"source_path", refresh.source_reference}},
            });
        }
    }

    auto delete_fields = prepare(
        database, "DELETE FROM list_item_fields WHERE document_id = ? AND item_position = ?");
    auto insert_field = prepare(
        database, "INSERT INTO list_item_fields(document_id, item_position, position, name, value, "
                  "native_name, provenance, language, description) VALUES(?,?,?,?,?,?,?,?,?)");
    auto update_revision =
        prepare(database,
                "UPDATE list_items SET observed_device = ?, observed_inode = ?, observed_size = ?, "
                "observed_mtime_seconds = ?, observed_mtime_nanoseconds = ? "
                "WHERE document_id = ? AND position = ?");
    if (!delete_fields || !insert_field || !update_revision) {
        auto error = !delete_fields  ? std::move(delete_fields.error())
                     : !insert_field ? std::move(insert_field.error())
                                     : std::move(update_revision.error());
        rollback();
        return std::unexpected(std::move(error));
    }
    for (const auto& occurrence : occurrences) {
        auto* deletion = delete_fields->get();
        if (!bind_text(deletion, 1, occurrence.document_id) ||
            sqlite3_bind_int64(deletion, 2, occurrence.position) != SQLITE_OK) {
            auto error = database_error(database, "Could not bind refreshed occurrence");
            rollback();
            return std::unexpected(std::move(error));
        }
        if (auto result = step_done(database, deletion, "Could not clear occurrence metadata");
            !result) {
            rollback();
            return std::unexpected(std::move(result.error()));
        }
        std::size_t position = 0U;
        const auto store_fields =
            [&](const std::span<const SnapshotField> fields) -> core::Result<void> {
            for (const auto& field : fields) {
                auto* insertion = insert_field->get();
                if (!bind_text(insertion, 1, occurrence.document_id) ||
                    sqlite3_bind_int64(insertion, 2, occurrence.position) != SQLITE_OK ||
                    sqlite3_bind_int64(insertion, 3, static_cast<sqlite3_int64>(position)) !=
                        SQLITE_OK ||
                    !bind_blob(insertion, 4, field.name) || !bind_blob(insertion, 5, field.value) ||
                    !bind_blob(insertion, 6, field.native_name) ||
                    sqlite3_bind_int(insertion, 7, static_cast<int>(field.provenance)) !=
                        SQLITE_OK ||
                    !bind_optional_blob(insertion, 8, field.language) ||
                    !bind_optional_blob(insertion, 9, field.description)) {
                    return std::unexpected(
                        database_error(database, "Could not bind refreshed metadata field"));
                }
                if (auto result =
                        step_done(database, insertion, "Could not store refreshed metadata field");
                    !result) {
                    return result;
                }
                ++position;
            }
            return {};
        };
        if (auto result = store_fields(*source_fields); !result) {
            rollback();
            return std::unexpected(std::move(result.error()));
        }
        if (auto result = store_fields(occurrence.retained_fields); !result) {
            rollback();
            return std::unexpected(std::move(result.error()));
        }
        auto* revision_statement = update_revision->get();
        if (!bind_revision(revision_statement, 1, refresh.published_revision) ||
            !bind_text(revision_statement, 6, occurrence.document_id) ||
            sqlite3_bind_int64(revision_statement, 7, occurrence.position) != SQLITE_OK) {
            auto error = database_error(database, "Could not bind refreshed source revision");
            rollback();
            return std::unexpected(std::move(error));
        }
        if (auto result = step_done(database, revision_statement,
                                    "Could not store refreshed source revision");
            !result) {
            rollback();
            return std::unexpected(std::move(result.error()));
        }
    }

    auto delete_cache =
        prepare(database, "DELETE FROM local_metadata_cache WHERE source_reference = ?");
    auto insert_cache = prepare(
        database,
        "INSERT INTO local_metadata_cache(source_reference, previous_device, previous_inode, "
        "previous_size, previous_mtime_seconds, previous_mtime_nanoseconds, published_device, "
        "published_inode, published_size, published_mtime_seconds, "
        "published_mtime_nanoseconds) VALUES(?,?,?,?,?,?,?,?,?,?,?)");
    auto insert_cache_field =
        prepare(database,
                "INSERT INTO local_metadata_cache_fields(source_reference, position, name, value, "
                "native_name, provenance, language, description) VALUES(?,?,?,?,?,?,?,?)");
    if (!delete_cache || !insert_cache || !insert_cache_field) {
        auto error = !delete_cache   ? std::move(delete_cache.error())
                     : !insert_cache ? std::move(insert_cache.error())
                                     : std::move(insert_cache_field.error());
        rollback();
        return std::unexpected(std::move(error));
    }
    if (!bind_blob(delete_cache->get(), 1, refresh.source_reference)) {
        auto error = database_error(database, "Could not bind metadata cache deletion");
        rollback();
        return std::unexpected(std::move(error));
    }
    if (auto result = step_done(database, delete_cache->get(), "Could not replace metadata cache");
        !result) {
        rollback();
        return std::unexpected(std::move(result.error()));
    }
    if (!bind_blob(insert_cache->get(), 1, refresh.source_reference) ||
        !bind_revision(insert_cache->get(), 2, refresh.previous_revision) ||
        !bind_revision(insert_cache->get(), 7, refresh.published_revision)) {
        auto error = database_error(database, "Could not bind metadata cache revision");
        rollback();
        return std::unexpected(std::move(error));
    }
    if (auto result = step_done(database, insert_cache->get(), "Could not store metadata cache");
        !result) {
        rollback();
        return std::unexpected(std::move(result.error()));
    }
    for (std::size_t position = 0U; position < source_fields->size(); ++position) {
        const auto& field = (*source_fields)[position];
        auto* statement = insert_cache_field->get();
        if (!bind_blob(statement, 1, refresh.source_reference) ||
            sqlite3_bind_int64(statement, 2, static_cast<sqlite3_int64>(position)) != SQLITE_OK ||
            !bind_blob(statement, 3, field.name) || !bind_blob(statement, 4, field.value) ||
            !bind_blob(statement, 5, field.native_name) ||
            sqlite3_bind_int(statement, 6, static_cast<int>(field.provenance)) != SQLITE_OK ||
            !bind_optional_blob(statement, 7, field.language) ||
            !bind_optional_blob(statement, 8, field.description)) {
            auto error = database_error(database, "Could not bind metadata cache field");
            rollback();
            return std::unexpected(std::move(error));
        }
        if (auto result = step_done(database, statement, "Could not store metadata cache field");
            !result) {
            rollback();
            return std::unexpected(std::move(result.error()));
        }
    }

    auto insert_refresh = prepare(
        database,
        "INSERT INTO local_metadata_refreshes(operation_id, source_reference, previous_device, "
        "previous_inode, previous_size, previous_mtime_seconds, previous_mtime_nanoseconds, "
        "published_device, published_inode, published_size, published_mtime_seconds, "
        "published_mtime_nanoseconds, affected_occurrences) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)");
    if (!insert_refresh || !bind_text(insert_refresh->get(), 1, operation_id) ||
        !bind_blob(insert_refresh->get(), 2, refresh.source_reference) ||
        !bind_revision(insert_refresh->get(), 3, refresh.previous_revision) ||
        !bind_revision(insert_refresh->get(), 8, refresh.published_revision) ||
        sqlite3_bind_int64(insert_refresh->get(), 13,
                           static_cast<sqlite3_int64>(occurrences.size())) != SQLITE_OK) {
        auto error = insert_refresh ? database_error(database, "Could not bind metadata refresh")
                                    : std::move(insert_refresh.error());
        rollback();
        return std::unexpected(std::move(error));
    }
    if (auto result =
            step_done(database, insert_refresh->get(), "Could not store metadata refresh");
        !result) {
        rollback();
        return std::unexpected(std::move(result.error()));
    }
    if (auto result = refresh_library_source(database, refresh.source_reference,
                                             refresh.source_reference, &refresh.document);
        !result) {
        rollback();
        return std::unexpected(std::move(result.error()));
    }
    if (auto result = execute(database, "COMMIT"); !result) {
        rollback();
        return std::unexpected(std::move(result.error()));
    }
    return LocalMetadataRefreshResult{
        .affected_occurrences = occurrences.size(),
        .already_applied = false,
    };
}

core::Result<LocalSourceRelocationResult>
ListRepository::relocate_local_source(const LocalSourceRelocation& relocation) {
    if (relocation.operation_id.is_nil() || relocation.source_reference.empty() ||
        relocation.target_reference.empty() ||
        relocation.source_reference.find('\0') != std::string::npos ||
        relocation.target_reference.find('\0') != std::string::npos ||
        relocation.source_reference == relocation.target_reference ||
        relocation.previous_revision.inode == 0U || relocation.published_revision.inode == 0U) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::invalid_argument,
            .message = "Local-source relocation requires distinct raw paths and valid evidence",
            .context = {},
        });
    }
    std::optional<std::vector<SnapshotField>> published_fields;
    if (relocation.published_document) {
        auto flattened = flatten_source_fields(*relocation.published_document);
        if (!flattened) {
            return std::unexpected(std::move(flattened.error()));
        }
        published_fields = std::move(*flattened);
    }

    auto* database = implementation_->database;
    if (auto result = execute(database, "BEGIN IMMEDIATE"); !result) {
        return std::unexpected(std::move(result.error()));
    }
    const auto rollback = [database] { static_cast<void>(execute(database, "ROLLBACK")); };
    const auto operation_id = relocation.operation_id.to_string();

    auto replay = prepare(
        database,
        "SELECT source_reference, target_reference, previous_device, previous_inode, "
        "previous_size, previous_mtime_seconds, previous_mtime_nanoseconds, published_device, "
        "published_inode, published_size, published_mtime_seconds, "
        "published_mtime_nanoseconds, affected_occurrences, cache_rekeyed, metadata_refreshed "
        "FROM local_source_relocations WHERE operation_id = ?");
    if (!replay || !bind_text(replay->get(), 1, operation_id)) {
        auto error = replay ? database_error(database, "Could not bind relocation replay")
                            : std::move(replay.error());
        rollback();
        return std::unexpected(std::move(error));
    }
    const auto replay_step = sqlite3_step(replay->get());
    if (replay_step == SQLITE_ROW) {
        auto previous_revision = read_revision(replay->get(), 2, "Local-source relocation");
        auto published_revision = read_revision(replay->get(), 7, "Local-source relocation");
        const auto affected = sqlite3_column_int64(replay->get(), 12);
        const auto cache_rekeyed = sqlite3_column_int(replay->get(), 13);
        const auto metadata_refreshed = sqlite3_column_int(replay->get(), 14);
        if (!previous_revision) {
            auto error = std::move(previous_revision.error());
            rollback();
            return std::unexpected(std::move(error));
        }
        if (!published_revision) {
            auto error = std::move(published_revision.error());
            rollback();
            return std::unexpected(std::move(error));
        }
        if (column_blob(replay->get(), 0) != relocation.source_reference ||
            column_blob(replay->get(), 1) != relocation.target_reference ||
            *previous_revision != relocation.previous_revision ||
            *published_revision != relocation.published_revision || affected <= 0 ||
            static_cast<std::uint64_t>(affected) > maximum_refresh_occurrences ||
            (cache_rekeyed != 0 && cache_rekeyed != 1) ||
            (metadata_refreshed != 0 && metadata_refreshed != 1) ||
            (metadata_refreshed != 0) != published_fields.has_value()) {
            rollback();
            return std::unexpected(core::Error{
                .code = core::ErrorCode::conflict,
                .message = "Local-source relocation identity was replayed with different content",
                .context = {{"operation_id", operation_id}},
            });
        }
        rollback();
        return LocalSourceRelocationResult{
            .affected_occurrences = static_cast<std::size_t>(affected),
            .cache_rekeyed = cache_rekeyed != 0,
            .metadata_refreshed = metadata_refreshed != 0,
            .already_applied = true,
        };
    }
    if (replay_step != SQLITE_DONE) {
        auto error = database_error(database, "Could not inspect relocation replay");
        rollback();
        return std::unexpected(std::move(error));
    }

    struct RelocatedOccurrence {
        std::string document_id;
        sqlite3_int64 position{0};
        std::vector<SnapshotField> retained_fields;
    };
    std::vector<RelocatedOccurrence> relocated_occurrences;
    const auto enumerate_occurrences =
        [&](const std::string& source_reference) -> core::Result<void> {
        auto occurrences =
            prepare(database, "SELECT li.document_id, li.position, li.logical_reference, "
                              "EXISTS(SELECT 1 FROM list_item_fields f "
                              "WHERE f.document_id = li.document_id "
                              "AND f.item_position = li.position AND f.provenance = 0), "
                              "li.observed_device, li.observed_inode, li.observed_size, "
                              "li.observed_mtime_seconds, li.observed_mtime_nanoseconds "
                              "FROM list_items li WHERE li.source = ? AND li.source_reference = ? "
                              "ORDER BY li.document_id, li.position");
        if (!occurrences ||
            sqlite3_bind_int(occurrences->get(), 1, static_cast<int>(ListSource::local)) !=
                SQLITE_OK ||
            !bind_blob(occurrences->get(), 2, source_reference)) {
            return std::unexpected(
                occurrences ? database_error(database, "Could not bind relocation occurrences")
                            : std::move(occurrences.error()));
        }
        int occurrence_step = SQLITE_ROW;
        while ((occurrence_step = sqlite3_step(occurrences->get())) == SQLITE_ROW) {
            if (relocated_occurrences.size() >= maximum_refresh_occurrences) {
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::limit_exceeded,
                    .message = "Local-source relocation exceeds the occurrence limit",
                    .context = {{"source_path", relocation.source_reference}},
                });
            }
            const auto logical = sqlite3_column_type(occurrences->get(), 2) != SQLITE_NULL;
            const auto legacy_snapshot = sqlite3_column_int(occurrences->get(), 3) != 0;
            if (published_fields && logical && legacy_snapshot) {
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::conflict,
                    .message = "Logical tracks must be freshly probed before combined publication",
                    .context = {{"source_path", relocation.source_reference}},
                });
            }
            const auto position = sqlite3_column_int64(occurrences->get(), 1);
            if (position < 0) {
                return std::unexpected(
                    database_error(database, "Relocated occurrence has an invalid position"));
            }
            auto revision = read_optional_revision(occurrences->get(), 4, "Relocated list item");
            if (!revision) {
                return std::unexpected(std::move(revision.error()));
            }
            if (!*revision || **revision != relocation.previous_revision) {
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::conflict,
                    .message = "Every relocated occurrence must identify the published source",
                    .context = {{"source_path", relocation.source_reference}},
                });
            }
            relocated_occurrences.push_back(RelocatedOccurrence{
                .document_id = column_text(occurrences->get(), 0),
                .position = position,
                .retained_fields = {},
            });
        }
        if (occurrence_step != SQLITE_DONE) {
            return std::unexpected(
                database_error(database, "Could not enumerate relocation occurrences"));
        }
        return {};
    };
    if (auto enumerated = enumerate_occurrences(relocation.source_reference); !enumerated) {
        auto error = std::move(enumerated.error());
        rollback();
        return std::unexpected(std::move(error));
    }
    auto occurrences_already_at_target = false;
    if (relocated_occurrences.empty()) {
        if (auto enumerated = enumerate_occurrences(relocation.target_reference); !enumerated) {
            auto error = std::move(enumerated.error());
            rollback();
            return std::unexpected(std::move(error));
        }
        occurrences_already_at_target = !relocated_occurrences.empty();
        if (!occurrences_already_at_target) {
            rollback();
            return std::unexpected(core::Error{
                .code = core::ErrorCode::not_found,
                .message = "Relocated source has no persisted list occurrence",
                .context = {{"source_path", relocation.source_reference}},
            });
        }
    }
    const auto affected_occurrences = relocated_occurrences.size();

    if (!occurrences_already_at_target) {
        auto target_occurrence = prepare(
            database, "SELECT 1 FROM list_items WHERE source = ? AND source_reference = ? LIMIT 1");
        if (!target_occurrence ||
            sqlite3_bind_int(target_occurrence->get(), 1, static_cast<int>(ListSource::local)) !=
                SQLITE_OK ||
            !bind_blob(target_occurrence->get(), 2, relocation.target_reference)) {
            auto error = target_occurrence
                             ? database_error(database, "Could not bind relocation target")
                             : std::move(target_occurrence.error());
            rollback();
            return std::unexpected(std::move(error));
        }
        const auto target_step = sqlite3_step(target_occurrence->get());
        if (target_step == SQLITE_ROW) {
            rollback();
            return std::unexpected(core::Error{
                .code = core::ErrorCode::conflict,
                .message = "Relocation target already has a persisted local source",
                .context = {{"target_path", relocation.target_reference}},
            });
        }
        if (target_step != SQLITE_DONE) {
            auto error = database_error(database, "Could not inspect relocation target");
            rollback();
            return std::unexpected(std::move(error));
        }
    }

    const auto cache_exists = [&](const std::string& source_reference) -> core::Result<bool> {
        auto query = prepare(database, "SELECT 1 FROM local_metadata_cache "
                                       "WHERE source_reference = ? LIMIT 1");
        if (!query || !bind_blob(query->get(), 1, source_reference)) {
            return std::unexpected(query ? database_error(database, "Could not bind source cache")
                                         : std::move(query.error()));
        }
        const auto step = sqlite3_step(query->get());
        if (step == SQLITE_ROW) {
            return true;
        }
        if (step == SQLITE_DONE) {
            return false;
        }
        return std::unexpected(database_error(database, "Could not inspect source cache"));
    };
    auto source_cache = cache_exists(relocation.source_reference);
    auto target_cache = cache_exists(relocation.target_reference);
    if (!source_cache || !target_cache) {
        auto error =
            !source_cache ? std::move(source_cache.error()) : std::move(target_cache.error());
        rollback();
        return std::unexpected(std::move(error));
    }
    if (*target_cache &&
        (!occurrences_already_at_target || *source_cache || published_fields.has_value())) {
        auto delete_target_cache =
            prepare(database, "DELETE FROM local_metadata_cache WHERE source_reference = ?");
        if (!delete_target_cache ||
            !bind_blob(delete_target_cache->get(), 1, relocation.target_reference)) {
            auto error = delete_target_cache
                             ? database_error(database, "Could not bind unowned target cache")
                             : std::move(delete_target_cache.error());
            rollback();
            return std::unexpected(std::move(error));
        }
        if (auto removed = step_done(database, delete_target_cache->get(),
                                     "Could not remove unowned target metadata cache");
            !removed) {
            rollback();
            return std::unexpected(std::move(removed.error()));
        }
    }

    if (published_fields) {
        auto retained_query =
            prepare(database, "SELECT name, value, native_name, provenance, language, description "
                              "FROM list_item_fields WHERE document_id = ? AND item_position = ? "
                              "AND provenance NOT IN (0, 2, 3) ORDER BY position");
        if (!retained_query) {
            auto error = std::move(retained_query.error());
            rollback();
            return std::unexpected(std::move(error));
        }
        for (auto& occurrence : relocated_occurrences) {
            auto* statement = retained_query->get();
            if (!bind_text(statement, 1, occurrence.document_id) ||
                sqlite3_bind_int64(statement, 2, occurrence.position) != SQLITE_OK) {
                auto error =
                    database_error(database, "Could not bind retained publication metadata");
                rollback();
                return std::unexpected(std::move(error));
            }
            int step = SQLITE_ROW;
            while ((step = sqlite3_step(statement)) == SQLITE_ROW) {
                const auto provenance = sqlite3_column_int(statement, 3);
                if (provenance < static_cast<int>(metadata::FieldProvenance::annotation) ||
                    provenance > static_cast<int>(metadata::FieldProvenance::sidecar) ||
                    provenance == static_cast<int>(metadata::FieldProvenance::embedded) ||
                    provenance == static_cast<int>(metadata::FieldProvenance::stream)) {
                    auto error = database_error(
                        database, "Combined publication has invalid retained metadata provenance");
                    rollback();
                    return std::unexpected(std::move(error));
                }
                occurrence.retained_fields.push_back(SnapshotField{
                    .name = column_blob(statement, 0),
                    .value = column_blob(statement, 1),
                    .native_name = column_blob(statement, 2),
                    .provenance = static_cast<metadata::FieldProvenance>(provenance),
                    .language = optional_blob(statement, 4),
                    .description = optional_blob(statement, 5),
                });
            }
            sqlite3_reset(statement);
            sqlite3_clear_bindings(statement);
            if (step != SQLITE_DONE || occurrence.retained_fields.size() >
                                           maximum_fields_per_item - published_fields->size()) {
                if (step != SQLITE_DONE) {
                    auto error = database_error(
                        database, "Could not load retained combined-publication metadata");
                    rollback();
                    return std::unexpected(std::move(error));
                }
                rollback();
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::limit_exceeded,
                    .message = "Combined publication occurrence exceeds the field limit",
                    .context = {{"source_path", relocation.source_reference}},
                });
            }
        }
    }

    auto update_occurrences = prepare(
        database,
        "UPDATE list_items SET source_reference = ?, observed_device = ?, observed_inode = ?, "
        "observed_size = ?, observed_mtime_seconds = ?, observed_mtime_nanoseconds = ? "
        "WHERE source = ? AND source_reference = ?");
    if (!update_occurrences ||
        !bind_blob(update_occurrences->get(), 1, relocation.target_reference) ||
        !bind_revision(update_occurrences->get(), 2, relocation.published_revision) ||
        sqlite3_bind_int(update_occurrences->get(), 7, static_cast<int>(ListSource::local)) !=
            SQLITE_OK ||
        !bind_blob(update_occurrences->get(), 8,
                   occurrences_already_at_target ? relocation.target_reference
                                                 : relocation.source_reference)) {
        auto error = update_occurrences
                         ? database_error(database, "Could not bind relocated occurrences")
                         : std::move(update_occurrences.error());
        rollback();
        return std::unexpected(std::move(error));
    }
    if (auto updated =
            step_done(database, update_occurrences->get(), "Could not relocate source occurrences");
        !updated || static_cast<std::size_t>(sqlite3_changes(database)) != affected_occurrences) {
        auto error = updated ? core::Error{.code = core::ErrorCode::database,
                                           .message = "Relocation occurrence count changed",
                                           .context = {}}
                             : std::move(updated.error());
        rollback();
        return std::unexpected(std::move(error));
    }

    if (published_fields) {
        auto delete_fields = prepare(
            database, "DELETE FROM list_item_fields WHERE document_id = ? AND item_position = ?");
        auto insert_field = prepare(
            database,
            "INSERT INTO list_item_fields(document_id, item_position, position, name, value, "
            "native_name, provenance, language, description) VALUES(?,?,?,?,?,?,?,?,?)");
        if (!delete_fields || !insert_field) {
            auto error =
                !delete_fields ? std::move(delete_fields.error()) : std::move(insert_field.error());
            rollback();
            return std::unexpected(std::move(error));
        }
        for (const auto& occurrence : relocated_occurrences) {
            auto* deletion = delete_fields->get();
            if (!bind_text(deletion, 1, occurrence.document_id) ||
                sqlite3_bind_int64(deletion, 2, occurrence.position) != SQLITE_OK) {
                auto error =
                    database_error(database, "Could not bind combined-publication occurrence");
                rollback();
                return std::unexpected(std::move(error));
            }
            if (auto deleted =
                    step_done(database, deletion, "Could not clear combined-publication metadata");
                !deleted) {
                rollback();
                return std::unexpected(std::move(deleted.error()));
            }
            std::size_t position = 0U;
            const auto store_fields =
                [&](const std::span<const SnapshotField> fields) -> core::Result<void> {
                for (const auto& field : fields) {
                    auto* insertion = insert_field->get();
                    if (!bind_text(insertion, 1, occurrence.document_id) ||
                        sqlite3_bind_int64(insertion, 2, occurrence.position) != SQLITE_OK ||
                        sqlite3_bind_int64(insertion, 3, static_cast<sqlite3_int64>(position)) !=
                            SQLITE_OK ||
                        !bind_blob(insertion, 4, field.name) ||
                        !bind_blob(insertion, 5, field.value) ||
                        !bind_blob(insertion, 6, field.native_name) ||
                        sqlite3_bind_int(insertion, 7, static_cast<int>(field.provenance)) !=
                            SQLITE_OK ||
                        !bind_optional_blob(insertion, 8, field.language) ||
                        !bind_optional_blob(insertion, 9, field.description)) {
                        return std::unexpected(database_error(
                            database, "Could not bind combined-publication metadata field"));
                    }
                    if (auto stored =
                            step_done(database, insertion,
                                      "Could not store combined-publication metadata field");
                        !stored) {
                        return stored;
                    }
                    ++position;
                }
                return {};
            };
            if (auto stored = store_fields(*published_fields); !stored) {
                rollback();
                return std::unexpected(std::move(stored.error()));
            }
            if (auto stored = store_fields(occurrence.retained_fields); !stored) {
                rollback();
                return std::unexpected(std::move(stored.error()));
            }
        }
    }

    if (*source_cache && !published_fields) {
        auto copy_cache = prepare(
            database,
            "INSERT INTO local_metadata_cache(source_reference, previous_device, previous_inode, "
            "previous_size, previous_mtime_seconds, previous_mtime_nanoseconds, published_device, "
            "published_inode, published_size, published_mtime_seconds, "
            "published_mtime_nanoseconds) SELECT ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ? "
            "FROM local_metadata_cache WHERE source_reference = ?");
        auto copy_fields = prepare(
            database,
            "INSERT INTO local_metadata_cache_fields(source_reference, position, name, value, "
            "native_name, provenance, language, description) SELECT ?, position, name, value, "
            "native_name, provenance, language, description FROM local_metadata_cache_fields "
            "WHERE source_reference = ? ORDER BY position");
        auto delete_cache =
            prepare(database, "DELETE FROM local_metadata_cache WHERE source_reference = ?");
        if (!copy_cache || !copy_fields || !delete_cache) {
            auto error = !copy_cache    ? std::move(copy_cache.error())
                         : !copy_fields ? std::move(copy_fields.error())
                                        : std::move(delete_cache.error());
            rollback();
            return std::unexpected(std::move(error));
        }
        if (!bind_blob(copy_cache->get(), 1, relocation.target_reference) ||
            !bind_revision(copy_cache->get(), 2, relocation.previous_revision) ||
            !bind_revision(copy_cache->get(), 7, relocation.published_revision) ||
            !bind_blob(copy_cache->get(), 12, relocation.source_reference) ||
            !bind_blob(copy_fields->get(), 1, relocation.target_reference) ||
            !bind_blob(copy_fields->get(), 2, relocation.source_reference) ||
            !bind_blob(delete_cache->get(), 1, relocation.source_reference)) {
            auto error = database_error(database, "Could not bind source-cache relocation");
            rollback();
            return std::unexpected(std::move(error));
        }
        if (auto copied =
                step_done(database, copy_cache->get(), "Could not copy relocated source cache");
            !copied) {
            rollback();
            return std::unexpected(std::move(copied.error()));
        }
        if (auto copied = step_done(database, copy_fields->get(),
                                    "Could not copy relocated source-cache fields");
            !copied) {
            rollback();
            return std::unexpected(std::move(copied.error()));
        }
        if (auto removed =
                step_done(database, delete_cache->get(), "Could not remove prior source cache");
            !removed) {
            rollback();
            return std::unexpected(std::move(removed.error()));
        }
    }
    if (published_fields) {
        auto delete_source_cache =
            prepare(database, "DELETE FROM local_metadata_cache WHERE source_reference = ?");
        auto insert_target_cache = prepare(
            database,
            "INSERT INTO local_metadata_cache(source_reference, previous_device, previous_inode, "
            "previous_size, previous_mtime_seconds, previous_mtime_nanoseconds, published_device, "
            "published_inode, published_size, published_mtime_seconds, "
            "published_mtime_nanoseconds) VALUES(?,?,?,?,?,?,?,?,?,?,?)");
        auto insert_target_field = prepare(
            database,
            "INSERT INTO local_metadata_cache_fields(source_reference, position, name, value, "
            "native_name, provenance, language, description) VALUES(?,?,?,?,?,?,?,?)");
        if (!delete_source_cache || !insert_target_cache || !insert_target_field) {
            auto error = !delete_source_cache   ? std::move(delete_source_cache.error())
                         : !insert_target_cache ? std::move(insert_target_cache.error())
                                                : std::move(insert_target_field.error());
            rollback();
            return std::unexpected(std::move(error));
        }
        if (!bind_blob(delete_source_cache->get(), 1, relocation.source_reference) ||
            !bind_blob(insert_target_cache->get(), 1, relocation.target_reference) ||
            !bind_revision(insert_target_cache->get(), 2, relocation.previous_revision) ||
            !bind_revision(insert_target_cache->get(), 7, relocation.published_revision)) {
            auto error =
                database_error(database, "Could not bind combined-publication metadata cache");
            rollback();
            return std::unexpected(std::move(error));
        }
        if (auto removed = step_done(database, delete_source_cache->get(),
                                     "Could not remove prior publication metadata cache");
            !removed) {
            rollback();
            return std::unexpected(std::move(removed.error()));
        }
        if (auto stored = step_done(database, insert_target_cache->get(),
                                    "Could not store published metadata cache");
            !stored) {
            rollback();
            return std::unexpected(std::move(stored.error()));
        }
        for (std::size_t position = 0U; position < published_fields->size(); ++position) {
            const auto& field = (*published_fields)[position];
            auto* statement = insert_target_field->get();
            if (!bind_blob(statement, 1, relocation.target_reference) ||
                sqlite3_bind_int64(statement, 2, static_cast<sqlite3_int64>(position)) !=
                    SQLITE_OK ||
                !bind_blob(statement, 3, field.name) || !bind_blob(statement, 4, field.value) ||
                !bind_blob(statement, 5, field.native_name) ||
                sqlite3_bind_int(statement, 6, static_cast<int>(field.provenance)) != SQLITE_OK ||
                !bind_optional_blob(statement, 7, field.language) ||
                !bind_optional_blob(statement, 8, field.description)) {
                auto error = database_error(
                    database, "Could not bind combined-publication metadata-cache field");
                rollback();
                return std::unexpected(std::move(error));
            }
            if (auto stored =
                    step_done(database, statement,
                              "Could not store combined-publication metadata-cache field");
                !stored) {
                rollback();
                return std::unexpected(std::move(stored.error()));
            }
        }
    }

    auto insert_relocation = prepare(
        database,
        "INSERT INTO local_source_relocations(operation_id, source_reference, target_reference, "
        "previous_device, previous_inode, previous_size, previous_mtime_seconds, "
        "previous_mtime_nanoseconds, published_device, published_inode, published_size, "
        "published_mtime_seconds, published_mtime_nanoseconds, affected_occurrences, "
        "cache_rekeyed, metadata_refreshed) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
    if (!insert_relocation || !bind_text(insert_relocation->get(), 1, operation_id) ||
        !bind_blob(insert_relocation->get(), 2, relocation.source_reference) ||
        !bind_blob(insert_relocation->get(), 3, relocation.target_reference) ||
        !bind_revision(insert_relocation->get(), 4, relocation.previous_revision) ||
        !bind_revision(insert_relocation->get(), 9, relocation.published_revision) ||
        sqlite3_bind_int64(insert_relocation->get(), 14,
                           static_cast<sqlite3_int64>(affected_occurrences)) != SQLITE_OK ||
        sqlite3_bind_int(insert_relocation->get(), 15, *source_cache ? 1 : 0) != SQLITE_OK ||
        sqlite3_bind_int(insert_relocation->get(), 16, published_fields ? 1 : 0) != SQLITE_OK) {
        auto error = insert_relocation
                         ? database_error(database, "Could not bind local-source relocation")
                         : std::move(insert_relocation.error());
        rollback();
        return std::unexpected(std::move(error));
    }
    if (auto stored = step_done(database, insert_relocation->get(),
                                "Could not store local-source relocation");
        !stored) {
        rollback();
        return std::unexpected(std::move(stored.error()));
    }
    if (auto result = refresh_library_source(
            database, relocation.source_reference, relocation.target_reference,
            relocation.published_document ? &*relocation.published_document : nullptr);
        !result) {
        rollback();
        return std::unexpected(std::move(result.error()));
    }
    if (auto linked = link_listening_source(
            database, relocation.source_reference, relocation.previous_revision,
            relocation.target_reference, relocation.published_revision);
        !linked) {
        rollback();
        return std::unexpected(std::move(linked.error()));
    }
    if (auto committed = execute(database, "COMMIT"); !committed) {
        rollback();
        return std::unexpected(std::move(committed.error()));
    }
    return LocalSourceRelocationResult{
        .affected_occurrences = affected_occurrences,
        .cache_rekeyed = *source_cache,
        .metadata_refreshed = published_fields.has_value(),
        .already_applied = false,
    };
}

core::Result<std::vector<ConnectionProfile>> ListRepository::load_profiles() const {
    auto statement = prepare(implementation_->database,
                             "SELECT id, name, host, port, local_music_root, auto_connect "
                             "FROM connection_profiles ORDER BY position");
    if (!statement) {
        return std::unexpected(std::move(statement.error()));
    }
    std::vector<ConnectionProfile> profiles;
    int result = SQLITE_ROW;
    while ((result = sqlite3_step(statement->get())) == SQLITE_ROW) {
        const auto id_text = column_text(statement->get(), 0);
        auto id = core::StableId::parse(id_text);
        const auto port = sqlite3_column_int64(statement->get(), 3);
        if (!id || port < 1 || port > 65'535) {
            return std::unexpected(core::Error{.code = core::ErrorCode::database,
                                               .message = "Invalid persisted connection profile",
                                               .context = {{"profile_id", id_text}}});
        }
        std::optional<std::string> root;
        if (sqlite3_column_type(statement->get(), 4) != SQLITE_NULL) {
            root = column_blob(statement->get(), 4);
        }
        profiles.push_back(ConnectionProfile{
            .id = *id,
            .name = column_blob(statement->get(), 1),
            .host = column_blob(statement->get(), 2),
            .port = static_cast<unsigned>(port),
            .local_music_root = std::move(root),
            .auto_connect = sqlite3_column_int(statement->get(), 5) != 0,
        });
    }
    if (result != SQLITE_DONE) {
        return std::unexpected(
            database_error(implementation_->database, "Could not load connection profiles"));
    }
    return profiles;
}

core::Result<void>
ListRepository::replace_profiles(const std::span<const ConnectionProfile> profiles) {
    std::unordered_set<std::string> ids;
    ids.reserve(profiles.size());
    std::size_t auto_connect_count = 0U;
    for (const auto& profile : profiles) {
        if (profile.id.is_nil() || profile.name.empty() || profile.host.empty() ||
            profile.port < 1U || profile.port > 65'535U ||
            !ids.insert(profile.id.to_string()).second) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::invalid_argument,
                .message = "Connection profiles require unique IDs, names, hosts, and valid ports",
                .context = {},
            });
        }
        auto_connect_count += profile.auto_connect ? 1U : 0U;
    }
    if (auto_connect_count > 1U) {
        return std::unexpected(core::Error{.code = core::ErrorCode::conflict,
                                           .message = "Only one profile may auto-connect",
                                           .context = {}});
    }

    auto* database = implementation_->database;
    if (auto result = execute(database, "BEGIN IMMEDIATE"); !result) {
        return result;
    }
    const auto rollback = [database] { static_cast<void>(execute(database, "ROLLBACK")); };
    if (auto result = execute(database, "DELETE FROM connection_profiles"); !result) {
        rollback();
        return result;
    }
    auto statement = prepare(
        database,
        "INSERT INTO connection_profiles(id, name, host, port, local_music_root, auto_connect, "
        "position) VALUES(?,?,?,?,?,?,?)");
    if (!statement) {
        rollback();
        return std::unexpected(std::move(statement.error()));
    }
    for (std::size_t position = 0U; position < profiles.size(); ++position) {
        const auto& profile = profiles[position];
        const auto id = profile.id.to_string();
        auto* raw = statement->get();
        const auto root_bound = profile.local_music_root
                                    ? bind_blob(raw, 5, *profile.local_music_root)
                                    : sqlite3_bind_null(raw, 5) == SQLITE_OK;
        if (!bind_text(raw, 1, id) || !bind_blob(raw, 2, profile.name) ||
            !bind_blob(raw, 3, profile.host) ||
            sqlite3_bind_int(raw, 4, static_cast<int>(profile.port)) != SQLITE_OK || !root_bound ||
            sqlite3_bind_int(raw, 6, profile.auto_connect ? 1 : 0) != SQLITE_OK ||
            sqlite3_bind_int64(raw, 7, static_cast<sqlite3_int64>(position)) != SQLITE_OK) {
            auto error = database_error(database, "Could not bind connection profile");
            rollback();
            return std::unexpected(std::move(error));
        }
        if (auto result = step_done(database, raw, "Could not store connection profile"); !result) {
            rollback();
            return result;
        }
    }
    if (auto result = execute(database, "COMMIT"); !result) {
        rollback();
        return result;
    }
    return {};
}

core::Result<std::vector<TrackViewPreset>> ListRepository::load_view_presets() const {
    auto statement =
        prepare(implementation_->database, "SELECT binding, header_state FROM track_view_presets "
                                           "ORDER BY position");
    if (!statement) {
        return std::unexpected(std::move(statement.error()));
    }
    std::vector<TrackViewPreset> presets;
    int result = SQLITE_ROW;
    while ((result = sqlite3_step(statement->get())) == SQLITE_ROW) {
        presets.push_back(TrackViewPreset{.binding = column_blob(statement->get(), 0),
                                          .header_state = column_blob(statement->get(), 1)});
        if (presets.back().binding.empty()) {
            return std::unexpected(core::Error{.code = core::ErrorCode::database,
                                               .message = "Invalid persisted track view preset",
                                               .context = {}});
        }
    }
    if (result != SQLITE_DONE) {
        return std::unexpected(
            database_error(implementation_->database, "Could not load track view presets"));
    }
    return presets;
}

core::Result<void>
ListRepository::replace_view_presets(const std::span<const TrackViewPreset> presets) {
    constexpr std::size_t maximum_presets = 4'096U;
    constexpr std::size_t maximum_state_size = 1024U * 1024U;
    if (presets.size() > maximum_presets) {
        return std::unexpected(core::Error{.code = core::ErrorCode::limit_exceeded,
                                           .message = "At most 4096 view presets can be stored",
                                           .context = {}});
    }
    std::unordered_set<std::string> bindings;
    for (const auto& preset : presets) {
        if (preset.binding.empty() || preset.header_state.size() > maximum_state_size ||
            !bindings.insert(preset.binding).second) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::invalid_argument,
                .message = "View preset bindings must be non-empty, unique, and bounded",
                .context = {},
            });
        }
    }
    auto* database = implementation_->database;
    if (auto result = execute(database, "BEGIN IMMEDIATE"); !result) {
        return result;
    }
    const auto rollback = [database] { static_cast<void>(execute(database, "ROLLBACK")); };
    if (auto result = execute(database, "DELETE FROM track_view_presets"); !result) {
        rollback();
        return result;
    }
    auto statement = prepare(
        database, "INSERT INTO track_view_presets(binding, header_state, position) VALUES(?,?,?)");
    if (!statement) {
        rollback();
        return std::unexpected(std::move(statement.error()));
    }
    for (std::size_t position = 0U; position < presets.size(); ++position) {
        const auto& preset = presets[position];
        auto* raw = statement->get();
        if (!bind_blob(raw, 1, preset.binding) || !bind_blob(raw, 2, preset.header_state) ||
            sqlite3_bind_int64(raw, 3, static_cast<sqlite3_int64>(position)) != SQLITE_OK) {
            auto error = database_error(database, "Could not bind track view preset");
            rollback();
            return std::unexpected(std::move(error));
        }
        if (auto result = step_done(database, raw, "Could not store track view preset"); !result) {
            rollback();
            return result;
        }
    }
    if (auto result = execute(database, "COMMIT"); !result) {
        rollback();
        return result;
    }
    return {};
}

core::Result<std::vector<SavedMetadataTransformationChain>>
ListRepository::load_metadata_transformation_chains() const {
    auto* database = implementation_->database;
    auto chains_query = prepare(
        database, "SELECT id, schema_version, name, automatic FROM metadata_transformation_chains "
                  "ORDER BY name, id");
    if (!chains_query) {
        return std::unexpected(std::move(chains_query.error()));
    }
    std::vector<SavedMetadataTransformationChain> chains;
    std::unordered_map<std::string, std::size_t> chain_indices;
    int result = SQLITE_ROW;
    while ((result = sqlite3_step(chains_query->get())) == SQLITE_ROW) {
        const auto id_text = column_text(chains_query->get(), 0);
        auto id = core::StableId::parse(id_text);
        const auto schema_version = sqlite3_column_int64(chains_query->get(), 1);
        auto name = column_blob(chains_query->get(), 2);
        const auto automatic = sqlite3_column_int(chains_query->get(), 3);
        if (!id || id->is_nil() || schema_version != 1 || name.empty() ||
            (automatic != 0 && automatic != 1) ||
            chains.size() >= maximum_metadata_transformation_chains ||
            !chain_indices.emplace(id_text, chains.size()).second) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::database,
                .message = "Invalid persisted metadata transformation chain",
                .context = {{"chain_id", id_text}},
            });
        }
        chains.push_back(SavedMetadataTransformationChain{
            .id = *id,
            .chain =
                metadata::MetadataTransformationChain{
                    .schema_version = static_cast<std::uint32_t>(schema_version),
                    .name = std::move(name),
                    .actions = {},
                },
            .automatic = automatic != 0,
        });
    }
    if (result != SQLITE_DONE) {
        return std::unexpected(
            database_error(database, "Could not load metadata transformation chains"));
    }

    auto actions_query = prepare(
        database,
        "SELECT chain_id, position, kind, target_field, argument, dialect, dialect_version, "
        "compiler_schema, integer_argument, integer_argument_2 "
        "FROM metadata_transformation_actions ORDER BY chain_id, position");
    if (!actions_query) {
        return std::unexpected(std::move(actions_query.error()));
    }
    while ((result = sqlite3_step(actions_query->get())) == SQLITE_ROW) {
        const auto chain_id = column_text(actions_query->get(), 0);
        const auto found = chain_indices.find(chain_id);
        const auto position = sqlite3_column_int64(actions_query->get(), 1);
        const auto kind = sqlite3_column_int(actions_query->get(), 2);
        if (found == chain_indices.end() || position < 0 ||
            static_cast<std::size_t>(position) != chains[found->second].chain.actions.size() ||
            kind < 0 || kind > 19) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::database,
                .message = "Invalid persisted metadata transformation action order",
                .context = {{"chain_id", chain_id}},
            });
        }
        const auto target = column_blob(actions_query->get(), 3);
        const auto argument = optional_blob(actions_query->get(), 4);
        const auto dialect = optional_blob(actions_query->get(), 5);
        const bool has_dialect_version =
            sqlite3_column_type(actions_query->get(), 6) != SQLITE_NULL;
        const bool has_compiler_schema =
            sqlite3_column_type(actions_query->get(), 7) != SQLITE_NULL;
        const auto dialect_version = sqlite3_column_int64(actions_query->get(), 6);
        const auto compiler_schema = sqlite3_column_int64(actions_query->get(), 7);
        const bool has_integer_argument =
            sqlite3_column_type(actions_query->get(), 8) != SQLITE_NULL;
        const bool has_integer_argument_2 =
            sqlite3_column_type(actions_query->get(), 9) != SQLITE_NULL;
        const auto integer_argument = sqlite3_column_int64(actions_query->get(), 8);
        const auto integer_argument_2 = sqlite3_column_int64(actions_query->get(), 9);
        const bool has_any_dialect = dialect || has_dialect_version || has_compiler_schema;
        const bool has_complete_dialect = dialect && has_dialect_version && has_compiler_schema;
        const bool has_any_integer = has_integer_argument || has_integer_argument_2;
        metadata::MetadataTransformationAction action;
        switch (kind) {
        case 0:
            if (argument || has_any_dialect || has_any_integer) {
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::database,
                    .message = "Persisted set-values action contains unexpected data",
                    .context = {{"chain_id", chain_id}},
                });
            }
            action = metadata::MetadataSetValuesAction{.target_field = target, .values = {}};
            break;
        case 1:
            if (argument || has_any_dialect || has_any_integer) {
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::database,
                    .message = "Persisted add-values action contains unexpected data",
                    .context = {{"chain_id", chain_id}},
                });
            }
            action = metadata::MetadataAddValuesAction{.target_field = target, .values = {}};
            break;
        case 3:
        case 4:
        case 5:
        case 10: {
            if (argument || has_any_dialect || has_any_integer) {
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::database,
                    .message = "Persisted metadata transformation action contains unexpected data",
                    .context = {{"chain_id", chain_id}},
                });
            }
            const auto transform = kind == 3   ? metadata::MetadataValueTransformKind::trim_ascii
                                   : kind == 4 ? metadata::MetadataValueTransformKind::lowercase
                                   : kind == 5
                                       ? metadata::MetadataValueTransformKind::uppercase
                                       : metadata::MetadataValueTransformKind::capitalize_first;
            action = metadata::MetadataTransformValuesAction{
                .target_field = target,
                .transform = transform,
            };
            break;
        }
        case 2:
            if (argument || has_any_dialect || has_integer_argument_2 ||
                (has_integer_argument && integer_argument != 1)) {
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::database,
                    .message = "Persisted remove-field action has an invalid match mode",
                    .context = {{"chain_id", chain_id}},
                });
            }
            action = metadata::MetadataRemoveFieldAction{
                .target_field = target,
                .match_mode = has_integer_argument ? metadata::MetadataFieldMatchMode::exact_native
                                                   : metadata::MetadataFieldMatchMode::logical,
            };
            break;
        case 6:
        case 7:
        case 8:
            if (!argument || has_any_dialect || has_any_integer) {
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::database,
                    .message = "Persisted exact-value action is missing its argument",
                    .context = {{"chain_id", chain_id}},
                });
            }
            if (kind == 6) {
                action = metadata::MetadataCopyFieldAction{.target_field = target,
                                                           .source_field = *argument};
            } else if (kind == 7) {
                action = metadata::MetadataSplitValuesAction{.target_field = target,
                                                             .separator = *argument};
            } else {
                action = metadata::MetadataJoinValuesAction{.target_field = target,
                                                            .separator = *argument};
            }
            break;
        case 9:
            if (!argument || !has_complete_dialect || dialect_version < 0 || compiler_schema < 0 ||
                dialect_version > std::numeric_limits<std::uint32_t>::max() ||
                compiler_schema > std::numeric_limits<std::uint32_t>::max() || has_any_integer) {
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::database,
                    .message = "Persisted formatting action is missing its dialect",
                    .context = {{"chain_id", chain_id}},
                });
            }
            action = metadata::MetadataFormatValueAction{
                .target_field = target,
                .dialect =
                    titleformat::DialectVersion{
                        .dialect = *dialect,
                        .dialect_version = static_cast<std::uint32_t>(dialect_version),
                        .compiler_schema = static_cast<std::uint32_t>(compiler_schema),
                    },
                .source = *argument,
            };
            break;
        case 11:
        case 12:
            if (!argument || has_any_dialect || has_any_integer) {
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::database,
                    .message = "Persisted matching action is missing its exact value",
                    .context = {{"chain_id", chain_id}},
                });
            }
            if (kind == 11) {
                action = metadata::MetadataRemoveMatchingValuesAction{
                    .target_field = target,
                    .match = *argument,
                };
            } else {
                action = metadata::MetadataReplaceMatchingValuesAction{
                    .target_field = target,
                    .match = *argument,
                    .replacement_values = {},
                };
            }
            break;
        case 13:
            if (argument || has_any_dialect || !has_integer_argument || !has_integer_argument_2 ||
                integer_argument < 0 || integer_argument_2 < 0 ||
                integer_argument > std::numeric_limits<std::uint32_t>::max() ||
                integer_argument_2 > std::numeric_limits<std::uint32_t>::max()) {
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::database,
                    .message = "Persisted numbering action has invalid numeric arguments",
                    .context = {{"chain_id", chain_id}},
                });
            }
            action = metadata::MetadataNumberSelectedItemsAction{
                .target_field = target,
                .start = static_cast<std::uint32_t>(integer_argument),
                .padding = static_cast<std::uint32_t>(integer_argument_2),
            };
            break;
        case 17:
            if (!argument || !has_complete_dialect || dialect_version < 0 || compiler_schema < 0 ||
                dialect_version > std::numeric_limits<std::uint32_t>::max() ||
                compiler_schema > std::numeric_limits<std::uint32_t>::max() ||
                !has_integer_argument || !has_integer_argument_2 || integer_argument < 0 ||
                integer_argument_2 < 0 ||
                integer_argument > std::numeric_limits<std::uint32_t>::max() ||
                integer_argument_2 > std::numeric_limits<std::uint32_t>::max()) {
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::database,
                    .message = "Persisted grouped numbering action has invalid arguments",
                    .context = {{"chain_id", chain_id}},
                });
            }
            action = metadata::MetadataNumberGroupedItemsAction{
                .target_field = target,
                .dialect =
                    titleformat::DialectVersion{
                        .dialect = *dialect,
                        .dialect_version = static_cast<std::uint32_t>(dialect_version),
                        .compiler_schema = static_cast<std::uint32_t>(compiler_schema),
                    },
                .group_expression = *argument,
                .start = static_cast<std::uint32_t>(integer_argument),
                .padding = static_cast<std::uint32_t>(integer_argument_2),
            };
            break;
        case 14:
            if (argument || has_any_dialect || !has_integer_argument || has_integer_argument_2 ||
                integer_argument <= 0 ||
                integer_argument > std::numeric_limits<std::uint32_t>::max()) {
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::database,
                    .message = "Persisted keep-first action has an invalid character count",
                    .context = {{"chain_id", chain_id}},
                });
            }
            action = metadata::MetadataKeepFirstCharactersAction{
                .target_field = target,
                .character_count = static_cast<std::uint32_t>(integer_argument),
            };
            break;
        case 15:
            if (!argument || !has_complete_dialect || dialect_version < 0 || compiler_schema < 0 ||
                dialect_version > std::numeric_limits<std::uint32_t>::max() ||
                compiler_schema > std::numeric_limits<std::uint32_t>::max() ||
                has_integer_argument_2 || (has_integer_argument && integer_argument != 1)) {
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::database,
                    .message = "Persisted conditional-remove action is missing its dialect",
                    .context = {{"chain_id", chain_id}},
                });
            }
            action = metadata::MetadataRemoveFieldIfAction{
                .target_field = target,
                .dialect =
                    titleformat::DialectVersion{
                        .dialect = *dialect,
                        .dialect_version = static_cast<std::uint32_t>(dialect_version),
                        .compiler_schema = static_cast<std::uint32_t>(compiler_schema),
                    },
                .condition = *argument,
                .match_mode = has_integer_argument ? metadata::MetadataFieldMatchMode::exact_native
                                                   : metadata::MetadataFieldMatchMode::logical,
            };
            break;
        case 16:
            if (!argument || !has_complete_dialect || dialect_version < 0 || compiler_schema < 0 ||
                dialect_version > std::numeric_limits<std::uint32_t>::max() ||
                compiler_schema > std::numeric_limits<std::uint32_t>::max() ||
                !has_integer_argument || has_integer_argument_2 || integer_argument < 0 ||
                integer_argument > 3) {
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::database,
                    .message = "Persisted capture action has invalid dialect or source data",
                    .context = {{"chain_id", chain_id}},
                });
            }
            action = metadata::MetadataCaptureValuesAction{
                .dialect =
                    metadata::CapturePatternDialectVersion{
                        .dialect = *dialect,
                        .dialect_version = static_cast<std::uint32_t>(dialect_version),
                        .compiler_schema = static_cast<std::uint32_t>(compiler_schema),
                    },
                .source_kind = static_cast<metadata::MetadataCaptureSourceKind>(integer_argument),
                .source = target,
                .pattern = *argument,
            };
            break;
        case 18:
        case 19:
            if (target != "*" || argument || has_any_dialect || has_any_integer) {
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::database,
                    .message = "Persisted field-filter action contains unexpected data",
                    .context = {{"chain_id", chain_id}},
                });
            }
            action =
                kind == 18
                    ? metadata::
                          MetadataTransformationAction{metadata::MetadataBlocklistFieldsAction{}}
                    : metadata::MetadataTransformationAction{
                          metadata::MetadataAllowlistFieldsAction{}};
            break;
        default:
            return std::unexpected(core::Error{
                .code = core::ErrorCode::database,
                .message = "Unknown persisted metadata transformation action",
                .context = {{"chain_id", chain_id}},
            });
        }
        chains[found->second].chain.actions.push_back(std::move(action));
    }
    if (result != SQLITE_DONE) {
        return std::unexpected(
            database_error(database, "Could not load metadata transformation actions"));
    }

    auto values_query = prepare(
        database,
        "SELECT chain_id, action_position, position, value FROM "
        "metadata_transformation_action_values ORDER BY chain_id, action_position, position");
    if (!values_query) {
        return std::unexpected(std::move(values_query.error()));
    }
    while ((result = sqlite3_step(values_query->get())) == SQLITE_ROW) {
        const auto chain_id = column_text(values_query->get(), 0);
        const auto found = chain_indices.find(chain_id);
        const auto action_position = sqlite3_column_int64(values_query->get(), 1);
        const auto value_position = sqlite3_column_int64(values_query->get(), 2);
        if (found == chain_indices.end() || action_position < 0 || value_position < 0 ||
            static_cast<std::size_t>(action_position) >=
                chains[found->second].chain.actions.size()) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::database,
                .message = "Invalid persisted metadata transformation value order",
                .context = {{"chain_id", chain_id}},
            });
        }
        auto& action =
            chains[found->second].chain.actions[static_cast<std::size_t>(action_position)];
        auto* values = [&]() -> std::vector<std::string>* {
            if (auto* set = std::get_if<metadata::MetadataSetValuesAction>(&action)) {
                return &set->values;
            }
            if (auto* add = std::get_if<metadata::MetadataAddValuesAction>(&action)) {
                return &add->values;
            }
            if (auto* replace =
                    std::get_if<metadata::MetadataReplaceMatchingValuesAction>(&action)) {
                return &replace->replacement_values;
            }
            if (auto* blocklist = std::get_if<metadata::MetadataBlocklistFieldsAction>(&action)) {
                return &blocklist->fields;
            }
            if (auto* allowlist = std::get_if<metadata::MetadataAllowlistFieldsAction>(&action)) {
                return &allowlist->fields;
            }
            return nullptr;
        }();
        if (values == nullptr || static_cast<std::size_t>(value_position) != values->size()) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::database,
                .message = "Invalid persisted metadata transformation literal value",
                .context = {{"chain_id", chain_id}},
            });
        }
        values->push_back(column_blob(values_query->get(), 3));
    }
    if (result != SQLITE_DONE) {
        return std::unexpected(
            database_error(database, "Could not load metadata transformation literal values"));
    }
    for (const auto& chain : chains) {
        if (auto valid = validate_saved_transformation_chain(chain); !valid) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::database,
                .message = "Persisted metadata transformation chain failed validation: " +
                           valid.error().message,
                .context = {{"chain_id", chain.id.to_string()}},
            });
        }
    }
    return chains;
}

core::Result<void> ListRepository::upsert_metadata_transformation_chain(
    const SavedMetadataTransformationChain& saved_chain) {
    if (auto valid = validate_saved_transformation_chain(saved_chain); !valid) {
        return valid;
    }
    auto* database = implementation_->database;
    if (auto result = execute(database, "BEGIN IMMEDIATE"); !result) {
        return result;
    }
    const auto rollback = [database] { static_cast<void>(execute(database, "ROLLBACK")); };
    const auto id = saved_chain.id.to_string();
    auto count_query = prepare(
        database,
        "SELECT COUNT(*), EXISTS(SELECT 1 FROM metadata_transformation_chains WHERE id = ?) "
        "FROM metadata_transformation_chains");
    if (!count_query || !bind_text(count_query->get(), 1, id) ||
        sqlite3_step(count_query->get()) != SQLITE_ROW) {
        auto error = count_query ? database_error(database, "Could not count saved transformations")
                                 : std::move(count_query.error());
        rollback();
        return std::unexpected(std::move(error));
    }
    const auto count = sqlite3_column_int64(count_query->get(), 0);
    const bool already_exists = sqlite3_column_int(count_query->get(), 1) != 0;
    sqlite3_reset(count_query->get());
    if (!already_exists &&
        count >= static_cast<sqlite3_int64>(maximum_metadata_transformation_chains)) {
        rollback();
        return std::unexpected(core::Error{
            .code = core::ErrorCode::limit_exceeded,
            .message = "At most 256 metadata transformation chains can be saved",
            .context = {},
        });
    }
    auto name_query =
        prepare(database, "SELECT id FROM metadata_transformation_chains WHERE name = ?");
    if (!name_query || !bind_blob(name_query->get(), 1, saved_chain.chain.name)) {
        auto error = name_query ? database_error(database, "Could not bind transformation name")
                                : std::move(name_query.error());
        rollback();
        return std::unexpected(std::move(error));
    }
    const auto name_result = sqlite3_step(name_query->get());
    const auto name_owner =
        name_result == SQLITE_ROW ? std::optional{column_text(name_query->get(), 0)} : std::nullopt;
    sqlite3_reset(name_query->get());
    if (name_owner && *name_owner != id) {
        rollback();
        return std::unexpected(core::Error{
            .code = core::ErrorCode::conflict,
            .message = "A saved metadata transformation already uses that exact name",
            .context = {{"name", saved_chain.chain.name}},
        });
    }
    if (name_result != SQLITE_ROW && name_result != SQLITE_DONE) {
        auto error = database_error(database, "Could not check transformation name");
        rollback();
        return std::unexpected(std::move(error));
    }

    auto chain_statement = prepare(
        database, "INSERT INTO metadata_transformation_chains(id, schema_version, name, automatic) "
                  "VALUES(?,?,?,?) "
                  "ON CONFLICT(id) DO UPDATE SET schema_version = excluded.schema_version, "
                  "name = excluded.name, automatic = excluded.automatic");
    if (!chain_statement || !bind_text(chain_statement->get(), 1, id) ||
        sqlite3_bind_int64(chain_statement->get(), 2, saved_chain.chain.schema_version) !=
            SQLITE_OK ||
        !bind_blob(chain_statement->get(), 3, saved_chain.chain.name) ||
        sqlite3_bind_int(chain_statement->get(), 4, saved_chain.automatic ? 1 : 0) != SQLITE_OK) {
        auto error = chain_statement
                         ? database_error(database, "Could not bind metadata transformation chain")
                         : std::move(chain_statement.error());
        rollback();
        return std::unexpected(std::move(error));
    }
    if (auto result = step_done(database, chain_statement->get(),
                                "Could not store metadata transformation chain");
        !result) {
        rollback();
        return result;
    }
    auto delete_actions =
        prepare(database, "DELETE FROM metadata_transformation_actions WHERE chain_id = ?");
    if (!delete_actions || !bind_text(delete_actions->get(), 1, id)) {
        auto error = delete_actions
                         ? database_error(database, "Could not bind transformation replacement")
                         : std::move(delete_actions.error());
        rollback();
        return std::unexpected(std::move(error));
    }
    if (auto result = step_done(database, delete_actions->get(),
                                "Could not replace metadata transformation actions");
        !result) {
        rollback();
        return result;
    }
    auto action_statement = prepare(
        database,
        "INSERT INTO metadata_transformation_actions(chain_id, position, kind, target_field, "
        "argument, dialect, dialect_version, compiler_schema, integer_argument, "
        "integer_argument_2) VALUES(?,?,?,?,?,?,?,?,?,?)");
    auto value_statement = prepare(
        database,
        "INSERT INTO metadata_transformation_action_values(chain_id, action_position, position, "
        "value) VALUES(?,?,?,?)");
    if (!action_statement || !value_statement) {
        auto error = !action_statement ? std::move(action_statement.error())
                                       : std::move(value_statement.error());
        rollback();
        return std::unexpected(std::move(error));
    }
    const auto bind_optional_view = [](sqlite3_stmt* statement, const int index,
                                       const std::optional<std::string_view>& value) {
        return value ? bind_blob(statement, index, *value)
                     : sqlite3_bind_null(statement, index) == SQLITE_OK;
    };
    const auto bind_optional_number = [](sqlite3_stmt* statement, const int index,
                                         const std::optional<std::uint32_t> value) {
        return value ? sqlite3_bind_int64(statement, index, *value) == SQLITE_OK
                     : sqlite3_bind_null(statement, index) == SQLITE_OK;
    };
    for (std::size_t position = 0U; position < saved_chain.chain.actions.size(); ++position) {
        const auto serialized =
            serialize_transformation_action(saved_chain.chain.actions[position]);
        auto* statement = action_statement->get();
        if (!bind_text(statement, 1, id) ||
            sqlite3_bind_int64(statement, 2, static_cast<sqlite3_int64>(position)) != SQLITE_OK ||
            sqlite3_bind_int(statement, 3, serialized.kind) != SQLITE_OK ||
            !bind_blob(statement, 4, serialized.target_field) ||
            !bind_optional_view(statement, 5, serialized.argument) ||
            !bind_optional_view(statement, 6, serialized.dialect) ||
            !bind_optional_number(statement, 7, serialized.dialect_version) ||
            !bind_optional_number(statement, 8, serialized.compiler_schema) ||
            !bind_optional_number(statement, 9, serialized.integer_argument) ||
            !bind_optional_number(statement, 10, serialized.integer_argument_2)) {
            auto error = database_error(database, "Could not bind metadata transformation action");
            rollback();
            return std::unexpected(std::move(error));
        }
        if (auto result =
                step_done(database, statement, "Could not store metadata transformation action");
            !result) {
            rollback();
            return result;
        }
        if (serialized.values == nullptr) {
            continue;
        }
        for (std::size_t value_position = 0U; value_position < serialized.values->size();
             ++value_position) {
            auto* value = value_statement->get();
            if (!bind_text(value, 1, id) ||
                sqlite3_bind_int64(value, 2, static_cast<sqlite3_int64>(position)) != SQLITE_OK ||
                sqlite3_bind_int64(value, 3, static_cast<sqlite3_int64>(value_position)) !=
                    SQLITE_OK ||
                !bind_blob(value, 4, (*serialized.values)[value_position])) {
                auto error =
                    database_error(database, "Could not bind transformation literal value");
                rollback();
                return std::unexpected(std::move(error));
            }
            if (auto result =
                    step_done(database, value, "Could not store transformation literal value");
                !result) {
                rollback();
                return result;
            }
        }
    }
    if (auto result = execute(database, "COMMIT"); !result) {
        rollback();
        return result;
    }
    return {};
}

core::Result<void> ListRepository::remove_metadata_transformation_chain(const core::StableId& id) {
    if (id.is_nil()) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::invalid_argument,
            .message = "A saved metadata transformation ID cannot be nil",
            .context = {},
        });
    }
    auto* database = implementation_->database;
    if (auto result = execute(database, "BEGIN IMMEDIATE"); !result) {
        return result;
    }
    const auto rollback = [database] { static_cast<void>(execute(database, "ROLLBACK")); };
    auto statement = prepare(database, "DELETE FROM metadata_transformation_chains WHERE id = ?");
    const auto encoded = id.to_string();
    if (!statement || !bind_text(statement->get(), 1, encoded)) {
        auto error = statement ? database_error(database, "Could not bind transformation deletion")
                               : std::move(statement.error());
        rollback();
        return std::unexpected(std::move(error));
    }
    if (auto result =
            step_done(database, statement->get(), "Could not remove metadata transformation chain");
        !result) {
        rollback();
        return result;
    }
    if (sqlite3_changes(database) != 1) {
        rollback();
        return std::unexpected(core::Error{
            .code = core::ErrorCode::not_found,
            .message = "Saved metadata transformation was not found",
            .context = {{"chain_id", encoded}},
        });
    }
    if (auto result = execute(database, "COMMIT"); !result) {
        rollback();
        return result;
    }
    return {};
}

core::Result<std::vector<SavedOutputLayoutProfile>>
ListRepository::load_output_layout_profiles() const {
    auto* database = implementation_->database;
    auto statement = prepare(
        database, "SELECT id, schema_version, name, dialect, dialect_version, compiler_schema, "
                  "relative_directory_expression, basename_expression, sanitization_policy, "
                  "sanitization_version FROM output_layout_profiles ORDER BY name, id");
    if (!statement) {
        return std::unexpected(std::move(statement.error()));
    }
    std::vector<SavedOutputLayoutProfile> profiles;
    int result = SQLITE_ROW;
    while ((result = sqlite3_step(statement->get())) == SQLITE_ROW) {
        const auto id_text = column_text(statement->get(), 0);
        auto id = core::StableId::parse(id_text);
        const auto schema_version = sqlite3_column_int64(statement->get(), 1);
        const auto dialect_version = sqlite3_column_int64(statement->get(), 4);
        const auto compiler_schema = sqlite3_column_int64(statement->get(), 5);
        const auto sanitization_version = sqlite3_column_int64(statement->get(), 9);
        if (!id || id->is_nil() || schema_version < 0 || dialect_version < 0 ||
            compiler_schema < 0 || sanitization_version < 0 ||
            schema_version > std::numeric_limits<std::uint32_t>::max() ||
            dialect_version > std::numeric_limits<std::uint32_t>::max() ||
            compiler_schema > std::numeric_limits<std::uint32_t>::max() ||
            sanitization_version > std::numeric_limits<std::uint32_t>::max() ||
            profiles.size() >= maximum_output_layout_profiles) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::database,
                .message = "Invalid persisted output layout profile",
                .context = {{"profile_id", id_text}},
            });
        }
        SavedOutputLayoutProfile profile{
            .id = *id,
            .profile =
                operations::OutputLayoutProfile{
                    .schema_version = static_cast<std::uint32_t>(schema_version),
                    .name = column_blob(statement->get(), 2),
                    .dialect =
                        titleformat::DialectVersion{
                            .dialect = column_blob(statement->get(), 3),
                            .dialect_version = static_cast<std::uint32_t>(dialect_version),
                            .compiler_schema = static_cast<std::uint32_t>(compiler_schema),
                        },
                    .relative_directory_expression = column_blob(statement->get(), 6),
                    .basename_expression = column_blob(statement->get(), 7),
                    .sanitization_policy =
                        operations::PolicyVersion{
                            .name = column_blob(statement->get(), 8),
                            .version = static_cast<std::uint32_t>(sanitization_version),
                        },
                },
        };
        if (auto valid = validate_saved_output_layout_profile(profile); !valid) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::database,
                .message = "Persisted output layout failed validation: " + valid.error().message,
                .context = {{"profile_id", id_text}},
            });
        }
        profiles.push_back(std::move(profile));
    }
    if (result != SQLITE_DONE) {
        return std::unexpected(database_error(database, "Could not load output layout profiles"));
    }
    return profiles;
}

core::Result<void>
ListRepository::upsert_output_layout_profile(const SavedOutputLayoutProfile& saved_profile) {
    if (auto valid = validate_saved_output_layout_profile(saved_profile); !valid) {
        return valid;
    }
    auto* database = implementation_->database;
    if (auto result = execute(database, "BEGIN IMMEDIATE"); !result) {
        return result;
    }
    const auto rollback = [database] { static_cast<void>(execute(database, "ROLLBACK")); };
    const auto id = saved_profile.id.to_string();
    auto count = prepare(
        database, "SELECT COUNT(*), EXISTS(SELECT 1 FROM output_layout_profiles WHERE id = ?) "
                  "FROM output_layout_profiles");
    if (!count || !bind_text(count->get(), 1, id) || sqlite3_step(count->get()) != SQLITE_ROW) {
        auto error = count ? database_error(database, "Could not count output layouts")
                           : std::move(count.error());
        rollback();
        return std::unexpected(std::move(error));
    }
    const auto profile_count = sqlite3_column_int64(count->get(), 0);
    const auto already_exists = sqlite3_column_int(count->get(), 1) != 0;
    count->reset();
    if (!already_exists &&
        profile_count >= static_cast<sqlite3_int64>(maximum_output_layout_profiles)) {
        rollback();
        return std::unexpected(core::Error{
            .code = core::ErrorCode::limit_exceeded,
            .message = "At most 256 output layout profiles can be saved",
            .context = {},
        });
    }
    auto name = prepare(database, "SELECT id FROM output_layout_profiles WHERE name = ?");
    if (!name || !bind_blob(name->get(), 1, saved_profile.profile.name)) {
        auto error = name ? database_error(database, "Could not bind output layout name")
                          : std::move(name.error());
        rollback();
        return std::unexpected(std::move(error));
    }
    const auto name_result = sqlite3_step(name->get());
    const auto name_owner =
        name_result == SQLITE_ROW ? std::optional{column_text(name->get(), 0)} : std::nullopt;
    name->reset();
    if (name_owner && *name_owner != id) {
        rollback();
        return std::unexpected(core::Error{
            .code = core::ErrorCode::conflict,
            .message = "A saved output layout already uses that exact name",
            .context = {{"name", saved_profile.profile.name}},
        });
    }
    if (name_result != SQLITE_ROW && name_result != SQLITE_DONE) {
        auto error = database_error(database, "Could not check output layout name");
        rollback();
        return std::unexpected(std::move(error));
    }
    const auto& profile = saved_profile.profile;
    auto statement = prepare(
        database,
        "INSERT INTO output_layout_profiles(id, schema_version, name, dialect, dialect_version, "
        "compiler_schema, relative_directory_expression, basename_expression, "
        "sanitization_policy, sanitization_version) VALUES(?,?,?,?,?,?,?,?,?,?) "
        "ON CONFLICT(id) DO UPDATE SET schema_version=excluded.schema_version, "
        "name=excluded.name, dialect=excluded.dialect, "
        "dialect_version=excluded.dialect_version, compiler_schema=excluded.compiler_schema, "
        "relative_directory_expression=excluded.relative_directory_expression, "
        "basename_expression=excluded.basename_expression, "
        "sanitization_policy=excluded.sanitization_policy, "
        "sanitization_version=excluded.sanitization_version");
    if (!statement || !bind_text(statement->get(), 1, id) ||
        sqlite3_bind_int64(statement->get(), 2, profile.schema_version) != SQLITE_OK ||
        !bind_blob(statement->get(), 3, profile.name) ||
        !bind_blob(statement->get(), 4, profile.dialect.dialect) ||
        sqlite3_bind_int64(statement->get(), 5, profile.dialect.dialect_version) != SQLITE_OK ||
        sqlite3_bind_int64(statement->get(), 6, profile.dialect.compiler_schema) != SQLITE_OK ||
        !bind_blob(statement->get(), 7, profile.relative_directory_expression) ||
        !bind_blob(statement->get(), 8, profile.basename_expression) ||
        !bind_blob(statement->get(), 9, profile.sanitization_policy.name) ||
        sqlite3_bind_int64(statement->get(), 10, profile.sanitization_policy.version) !=
            SQLITE_OK) {
        auto error = statement ? database_error(database, "Could not bind output layout profile")
                               : std::move(statement.error());
        rollback();
        return std::unexpected(std::move(error));
    }
    if (auto result =
            step_done(database, statement->get(), "Could not store output layout profile");
        !result) {
        rollback();
        return result;
    }
    if (auto result = execute(database, "COMMIT"); !result) {
        rollback();
        return result;
    }
    return {};
}

core::Result<void> ListRepository::remove_output_layout_profile(const core::StableId& id) {
    if (id.is_nil()) {
        return std::unexpected(core::Error{.code = core::ErrorCode::invalid_argument,
                                           .message = "A saved output layout ID cannot be nil",
                                           .context = {}});
    }
    auto* database = implementation_->database;
    if (auto result = execute(database, "BEGIN IMMEDIATE"); !result) {
        return result;
    }
    const auto rollback = [database] { static_cast<void>(execute(database, "ROLLBACK")); };
    auto statement = prepare(database, "DELETE FROM output_layout_profiles WHERE id = ?");
    const auto encoded = id.to_string();
    if (!statement || !bind_text(statement->get(), 1, encoded)) {
        auto error = statement ? database_error(database, "Could not bind output layout deletion")
                               : std::move(statement.error());
        rollback();
        return std::unexpected(std::move(error));
    }
    if (auto result = step_done(database, statement->get(), "Could not remove output layout");
        !result) {
        rollback();
        return result;
    }
    if (sqlite3_changes(database) != 1) {
        rollback();
        return std::unexpected(core::Error{
            .code = core::ErrorCode::not_found,
            .message = "Saved output layout was not found",
            .context = {{"profile_id", encoded}},
        });
    }
    if (auto result = execute(database, "COMMIT"); !result) {
        rollback();
        return result;
    }
    return {};
}

core::Result<std::vector<SavedDestinationProfile>>
ListRepository::load_destination_profiles() const {
    auto* database = implementation_->database;
    auto statement =
        prepare(database, "SELECT id, schema_version, name, root_raw_path, containment_policy, "
                          "containment_version FROM destination_profiles ORDER BY name, id");
    if (!statement) {
        return std::unexpected(std::move(statement.error()));
    }
    std::vector<SavedDestinationProfile> profiles;
    int result = SQLITE_ROW;
    while ((result = sqlite3_step(statement->get())) == SQLITE_ROW) {
        const auto id_text = column_text(statement->get(), 0);
        auto id = core::StableId::parse(id_text);
        const auto schema_version = sqlite3_column_int64(statement->get(), 1);
        const auto containment_version = sqlite3_column_int64(statement->get(), 5);
        if (!id || id->is_nil() || schema_version < 0 || containment_version < 0 ||
            schema_version > std::numeric_limits<std::uint32_t>::max() ||
            containment_version > std::numeric_limits<std::uint32_t>::max() ||
            profiles.size() >= maximum_destination_profiles) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::database,
                .message = "Invalid persisted destination profile",
                .context = {{"profile_id", id_text}},
            });
        }
        SavedDestinationProfile profile{
            .id = *id,
            .profile =
                operations::DestinationProfile{
                    .schema_version = static_cast<std::uint32_t>(schema_version),
                    .name = column_blob(statement->get(), 2),
                    .root_raw_path = column_blob(statement->get(), 3),
                    .containment_policy =
                        operations::PolicyVersion{
                            .name = column_blob(statement->get(), 4),
                            .version = static_cast<std::uint32_t>(containment_version),
                        },
                },
        };
        if (auto valid = validate_saved_destination_profile(profile); !valid) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::database,
                .message = "Persisted destination failed validation: " + valid.error().message,
                .context = {{"profile_id", id_text}},
            });
        }
        profiles.push_back(std::move(profile));
    }
    if (result != SQLITE_DONE) {
        return std::unexpected(database_error(database, "Could not load destination profiles"));
    }
    return profiles;
}

core::Result<void>
ListRepository::upsert_destination_profile(const SavedDestinationProfile& saved_profile) {
    if (auto valid = validate_saved_destination_profile(saved_profile); !valid) {
        return valid;
    }
    auto* database = implementation_->database;
    if (auto result = execute(database, "BEGIN IMMEDIATE"); !result) {
        return result;
    }
    const auto rollback = [database] { static_cast<void>(execute(database, "ROLLBACK")); };
    const auto id = saved_profile.id.to_string();
    auto count = prepare(database,
                         "SELECT COUNT(*), EXISTS(SELECT 1 FROM destination_profiles WHERE id = ?) "
                         "FROM destination_profiles");
    if (!count || !bind_text(count->get(), 1, id) || sqlite3_step(count->get()) != SQLITE_ROW) {
        auto error = count ? database_error(database, "Could not count destinations")
                           : std::move(count.error());
        rollback();
        return std::unexpected(std::move(error));
    }
    const auto profile_count = sqlite3_column_int64(count->get(), 0);
    const auto already_exists = sqlite3_column_int(count->get(), 1) != 0;
    count->reset();
    if (!already_exists &&
        profile_count >= static_cast<sqlite3_int64>(maximum_destination_profiles)) {
        rollback();
        return std::unexpected(core::Error{
            .code = core::ErrorCode::limit_exceeded,
            .message = "At most 256 destination profiles can be saved",
            .context = {},
        });
    }
    auto name = prepare(database, "SELECT id FROM destination_profiles WHERE name = ?");
    if (!name || !bind_blob(name->get(), 1, saved_profile.profile.name)) {
        auto error = name ? database_error(database, "Could not bind destination name")
                          : std::move(name.error());
        rollback();
        return std::unexpected(std::move(error));
    }
    const auto name_result = sqlite3_step(name->get());
    const auto name_owner =
        name_result == SQLITE_ROW ? std::optional{column_text(name->get(), 0)} : std::nullopt;
    name->reset();
    if (name_owner && *name_owner != id) {
        rollback();
        return std::unexpected(core::Error{
            .code = core::ErrorCode::conflict,
            .message = "A saved destination already uses that exact name",
            .context = {{"name", saved_profile.profile.name}},
        });
    }
    if (name_result != SQLITE_ROW && name_result != SQLITE_DONE) {
        auto error = database_error(database, "Could not check destination name");
        rollback();
        return std::unexpected(std::move(error));
    }
    const auto& profile = saved_profile.profile;
    auto statement = prepare(
        database, "INSERT INTO destination_profiles(id, schema_version, name, root_raw_path, "
                  "containment_policy, containment_version) VALUES(?,?,?,?,?,?) "
                  "ON CONFLICT(id) DO UPDATE SET schema_version=excluded.schema_version, "
                  "name=excluded.name, root_raw_path=excluded.root_raw_path, "
                  "containment_policy=excluded.containment_policy, "
                  "containment_version=excluded.containment_version");
    if (!statement || !bind_text(statement->get(), 1, id) ||
        sqlite3_bind_int64(statement->get(), 2, profile.schema_version) != SQLITE_OK ||
        !bind_blob(statement->get(), 3, profile.name) ||
        !bind_blob(statement->get(), 4, profile.root_raw_path) ||
        !bind_blob(statement->get(), 5, profile.containment_policy.name) ||
        sqlite3_bind_int64(statement->get(), 6, profile.containment_policy.version) != SQLITE_OK) {
        auto error = statement ? database_error(database, "Could not bind destination profile")
                               : std::move(statement.error());
        rollback();
        return std::unexpected(std::move(error));
    }
    if (auto result = step_done(database, statement->get(), "Could not store destination profile");
        !result) {
        rollback();
        return result;
    }
    if (auto result = execute(database, "COMMIT"); !result) {
        rollback();
        return result;
    }
    return {};
}

core::Result<void> ListRepository::remove_destination_profile(const core::StableId& id) {
    if (id.is_nil()) {
        return std::unexpected(core::Error{.code = core::ErrorCode::invalid_argument,
                                           .message = "A saved destination ID cannot be nil",
                                           .context = {}});
    }
    auto* database = implementation_->database;
    if (auto result = execute(database, "BEGIN IMMEDIATE"); !result) {
        return result;
    }
    const auto rollback = [database] { static_cast<void>(execute(database, "ROLLBACK")); };
    auto statement = prepare(database, "DELETE FROM destination_profiles WHERE id = ?");
    const auto encoded = id.to_string();
    if (!statement || !bind_text(statement->get(), 1, encoded)) {
        auto error = statement ? database_error(database, "Could not bind destination deletion")
                               : std::move(statement.error());
        rollback();
        return std::unexpected(std::move(error));
    }
    if (auto result = step_done(database, statement->get(), "Could not remove destination");
        !result) {
        rollback();
        return result;
    }
    if (sqlite3_changes(database) != 1) {
        rollback();
        return std::unexpected(core::Error{
            .code = core::ErrorCode::not_found,
            .message = "Saved destination was not found",
            .context = {{"profile_id", encoded}},
        });
    }
    if (auto result = execute(database, "COMMIT"); !result) {
        rollback();
        return result;
    }
    return {};
}

core::Result<std::vector<SavedEncoderPreset>> ListRepository::load_encoder_presets() const {
    auto* database = implementation_->database;
    auto statement =
        prepare(database, "SELECT id, preset_id, preset_version, display_name, codec_name, "
                          "container_name, file_extension, lossless, bit_rate, vbr_quality, "
                          "sample_format_hint FROM encoder_presets ORDER BY display_name, id");
    if (!statement) {
        return std::unexpected(std::move(statement.error()));
    }
    std::vector<SavedEncoderPreset> presets;
    int result = SQLITE_ROW;
    while ((result = sqlite3_step(statement->get())) == SQLITE_ROW) {
        const auto id_text = column_text(statement->get(), 0);
        auto id = core::StableId::parse(id_text);
        const auto preset_version = sqlite3_column_int64(statement->get(), 2);
        const auto lossless = sqlite3_column_int64(statement->get(), 7);
        if (!id || id->is_nil() || preset_version < 1 ||
            preset_version > std::numeric_limits<int>::max() || (lossless != 0 && lossless != 1) ||
            presets.size() >= maximum_encoder_presets) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::database,
                .message = "Invalid persisted encoder preset",
                .context = {{"preset_id", id_text}},
            });
        }
        SavedEncoderPreset preset{
            .id = *id,
            .preset =
                convert::EncoderPreset{
                    .id = column_blob(statement->get(), 1),
                    .version = static_cast<int>(preset_version),
                    .display_name = column_blob(statement->get(), 3),
                    .codec_name = column_blob(statement->get(), 4),
                    .container_name = column_blob(statement->get(), 5),
                    .file_extension = column_blob(statement->get(), 6),
                    .lossless = lossless == 1,
                    .bit_rate = sqlite3_column_type(statement->get(), 8) == SQLITE_NULL
                                    ? std::nullopt
                                    : std::optional{static_cast<std::int64_t>(
                                          sqlite3_column_int64(statement->get(), 8))},
                    .vbr_quality = sqlite3_column_type(statement->get(), 9) == SQLITE_NULL
                                       ? std::nullopt
                                       : std::optional{static_cast<int>(
                                             sqlite3_column_int64(statement->get(), 9))},
                    .sample_format_hint = column_blob(statement->get(), 10),
                },
        };
        if (auto valid = validate_saved_encoder_preset(preset); !valid) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::database,
                .message = "Persisted encoder preset failed validation: " + valid.error().message,
                .context = {{"preset_id", id_text}},
            });
        }
        presets.push_back(std::move(preset));
    }
    if (result != SQLITE_DONE) {
        return std::unexpected(database_error(database, "Could not load encoder presets"));
    }
    return presets;
}

core::Result<void> ListRepository::upsert_encoder_preset(const SavedEncoderPreset& saved_preset) {
    if (auto valid = validate_saved_encoder_preset(saved_preset); !valid) {
        return valid;
    }
    auto* database = implementation_->database;
    if (auto result = execute(database, "BEGIN IMMEDIATE"); !result) {
        return result;
    }
    const auto rollback = [database] { static_cast<void>(execute(database, "ROLLBACK")); };
    const auto id = saved_preset.id.to_string();
    auto count =
        prepare(database, "SELECT COUNT(*), EXISTS(SELECT 1 FROM encoder_presets WHERE id = ?) "
                          "FROM encoder_presets");
    if (!count || !bind_text(count->get(), 1, id) || sqlite3_step(count->get()) != SQLITE_ROW) {
        auto error = count ? database_error(database, "Could not count encoder presets")
                           : std::move(count.error());
        rollback();
        return std::unexpected(std::move(error));
    }
    const auto preset_count = sqlite3_column_int64(count->get(), 0);
    const auto already_exists = sqlite3_column_int(count->get(), 1) != 0;
    count->reset();
    if (!already_exists && preset_count >= static_cast<sqlite3_int64>(maximum_encoder_presets)) {
        rollback();
        return std::unexpected(core::Error{
            .code = core::ErrorCode::limit_exceeded,
            .message = "At most 256 encoder presets can be saved",
            .context = {},
        });
    }
    auto name = prepare(database, "SELECT id FROM encoder_presets WHERE display_name = ?");
    if (!name || !bind_blob(name->get(), 1, saved_preset.preset.display_name)) {
        auto error = name ? database_error(database, "Could not bind encoder preset name")
                          : std::move(name.error());
        rollback();
        return std::unexpected(std::move(error));
    }
    const auto name_result = sqlite3_step(name->get());
    const auto name_owner =
        name_result == SQLITE_ROW ? std::optional{column_text(name->get(), 0)} : std::nullopt;
    name->reset();
    if (name_owner && *name_owner != id) {
        rollback();
        return std::unexpected(core::Error{
            .code = core::ErrorCode::conflict,
            .message = "A saved encoder preset already uses that exact name",
            .context = {{"name", saved_preset.preset.display_name}},
        });
    }
    if (name_result != SQLITE_ROW && name_result != SQLITE_DONE) {
        auto error = database_error(database, "Could not check encoder preset name");
        rollback();
        return std::unexpected(std::move(error));
    }
    const auto& preset = saved_preset.preset;
    auto statement = prepare(
        database,
        "INSERT INTO encoder_presets(id, preset_id, preset_version, display_name, codec_name, "
        "container_name, file_extension, lossless, bit_rate, vbr_quality, sample_format_hint) "
        "VALUES(?,?,?,?,?,?,?,?,?,?,?) "
        "ON CONFLICT(id) DO UPDATE SET preset_id=excluded.preset_id, "
        "preset_version=excluded.preset_version, display_name=excluded.display_name, "
        "codec_name=excluded.codec_name, container_name=excluded.container_name, "
        "file_extension=excluded.file_extension, lossless=excluded.lossless, "
        "bit_rate=excluded.bit_rate, vbr_quality=excluded.vbr_quality, "
        "sample_format_hint=excluded.sample_format_hint");
    const auto bind_optional_int = [](sqlite3_stmt* target, const int column,
                                      const std::optional<std::int64_t> value) {
        return value ? sqlite3_bind_int64(target, column, *value) == SQLITE_OK
                     : sqlite3_bind_null(target, column) == SQLITE_OK;
    };
    if (!statement || !bind_text(statement->get(), 1, id) ||
        !bind_blob(statement->get(), 2, preset.id) ||
        sqlite3_bind_int64(statement->get(), 3, preset.version) != SQLITE_OK ||
        !bind_blob(statement->get(), 4, preset.display_name) ||
        !bind_blob(statement->get(), 5, preset.codec_name) ||
        !bind_blob(statement->get(), 6, preset.container_name) ||
        !bind_blob(statement->get(), 7, preset.file_extension) ||
        sqlite3_bind_int64(statement->get(), 8, preset.lossless ? 1 : 0) != SQLITE_OK ||
        !bind_optional_int(statement->get(), 9, preset.bit_rate) ||
        !bind_optional_int(statement->get(), 10,
                           preset.vbr_quality
                               ? std::optional{static_cast<std::int64_t>(*preset.vbr_quality)}
                               : std::nullopt) ||
        !bind_blob(statement->get(), 11, preset.sample_format_hint)) {
        auto error = statement ? database_error(database, "Could not bind encoder preset")
                               : std::move(statement.error());
        rollback();
        return std::unexpected(std::move(error));
    }
    if (auto result = step_done(database, statement->get(), "Could not store encoder preset");
        !result) {
        rollback();
        return result;
    }
    if (auto result = execute(database, "COMMIT"); !result) {
        rollback();
        return result;
    }
    return {};
}

core::Result<void> ListRepository::remove_encoder_preset(const core::StableId& id) {
    if (id.is_nil()) {
        return std::unexpected(core::Error{.code = core::ErrorCode::invalid_argument,
                                           .message = "A saved encoder preset ID cannot be nil",
                                           .context = {}});
    }
    auto* database = implementation_->database;
    if (auto result = execute(database, "BEGIN IMMEDIATE"); !result) {
        return result;
    }
    const auto rollback = [database] { static_cast<void>(execute(database, "ROLLBACK")); };
    auto statement = prepare(database, "DELETE FROM encoder_presets WHERE id = ?");
    const auto encoded = id.to_string();
    if (!statement || !bind_text(statement->get(), 1, encoded)) {
        auto error = statement ? database_error(database, "Could not bind encoder preset deletion")
                               : std::move(statement.error());
        rollback();
        return std::unexpected(std::move(error));
    }
    if (auto result = step_done(database, statement->get(), "Could not remove encoder preset");
        !result) {
        rollback();
        return result;
    }
    if (sqlite3_changes(database) != 1) {
        rollback();
        return std::unexpected(core::Error{
            .code = core::ErrorCode::not_found,
            .message = "Saved encoder preset was not found",
            .context = {{"preset_id", encoded}},
        });
    }
    if (auto result = execute(database, "COMMIT"); !result) {
        rollback();
        return result;
    }
    return {};
}

namespace {
core::Result<void> validate_search(const SavedSearch& search) {
    if (search.id.is_nil() || search.name.empty() || search.name.size() > 256U ||
        search.name.find('\0') != std::string::npos || search.expression.empty() ||
        search.expression.size() > 4096U ||
        (search.dialect != "tkq-1" && search.dialect != "words-1") ||
        (search.scope != SavedSearchScope::library &&
         search.scope != SavedSearchScope::current_tab &&
         search.scope != SavedSearchScope::server) ||
        search.revision >= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return std::unexpected(
            core::Error{.code = core::ErrorCode::invalid_argument,
                        .message = "Invalid saved search (name: 1–256 bytes; query: 1–4096 bytes)",
                        .context = {}});
    }
    if (!core::unicodeCodePointCount(search.name) ||
        search.expression.find('\0') != std::string::npos ||
        search.name.find_first_not_of(" \t\r\n") == std::string::npos) {
        return std::unexpected(
            core::Error{.code = core::ErrorCode::invalid_argument,
                        .message = "Saved searches require a valid name and query",
                        .context = {}});
    }
    const auto compiled = search.dialect == "tkq-1"
                              ? query::compile_tkq(search.expression)
                              : query::compile_tkq_word_search(search.expression);
    if (!compiled) {
        return std::unexpected(compiled.error());
    }
    return {};
}
} // namespace

core::Result<std::vector<SavedSearch>> ListRepository::load_saved_searches() const {
    auto* database = implementation_->database;
    auto statement = prepare(database, "SELECT id, name, expression, dialect, scope, revision FROM "
                                       "saved_searches ORDER BY name, id");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    std::vector<SavedSearch> searches;
    int result;
    while ((result = sqlite3_step(statement->get())) == SQLITE_ROW) {
        const auto id = core::StableId::parse(column_text(statement->get(), 0));
        const auto revision = sqlite3_column_int64(statement->get(), 5);
        const auto scope = sqlite3_column_int(statement->get(), 4);
        if (!id || revision < 1 || scope < 0 || scope > 1 || searches.size() >= 256U) {
            return std::unexpected(database_error(database, "Invalid saved search record"));
        }
        SavedSearch search{.id = *id,
                           .name = column_blob(statement->get(), 1),
                           .expression = column_blob(statement->get(), 2),
                           .dialect = column_text(statement->get(), 3),
                           .scope = static_cast<SavedSearchScope>(scope),
                           .revision = static_cast<std::uint64_t>(revision)};
        if (auto valid = validate_search(search); !valid) {
            return std::unexpected(valid.error());
        }
        searches.push_back(std::move(search));
    }
    if (result != SQLITE_DONE) {
        return std::unexpected(database_error(database, "Could not load saved searches"));
    }
    return searches;
}

core::Result<void> ListRepository::save_search(const SavedSearch& search) {
    if (auto valid = validate_search(search); !valid) {
        return valid;
    }
    auto* database = implementation_->database;
    auto statement = prepare(
        database, search.revision == 0
                      ? "INSERT INTO saved_searches(id,name,expression,dialect,scope,revision) "
                        "SELECT ?1,?2,?3,?4,?5,1 WHERE (SELECT count(*) FROM saved_searches) < 256"
                      : "UPDATE saved_searches SET name=?2, expression=?3, dialect=?4, scope=?5, "
                        "revision=revision+1 WHERE id=?1 AND revision=?6");
    if (!statement) {
        return std::unexpected(statement.error());
    }
    auto* stmt = statement->get();
    if (!bind_text(stmt, 1, search.id.to_string()) || !bind_blob(stmt, 2, search.name) ||
        !bind_blob(stmt, 3, search.expression) || !bind_text(stmt, 4, search.dialect) ||
        sqlite3_bind_int(stmt, 5, static_cast<int>(search.scope)) != SQLITE_OK ||
        (search.revision != 0 &&
         sqlite3_bind_int64(stmt, 6, static_cast<sqlite3_int64>(search.revision)) != SQLITE_OK)) {
        return std::unexpected(database_error(database, "Could not bind saved search"));
    }
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        if (sqlite3_errcode(database) == SQLITE_CONSTRAINT) {
            return std::unexpected(
                core::Error{.code = core::ErrorCode::conflict,
                            .message = "A saved search with this name or identity already exists",
                            .context = {}});
        }
        return std::unexpected(database_error(database, "Could not save search"));
    }
    if (sqlite3_changes(database) != 1) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::conflict,
            .message = search.revision == 0
                           ? "At most 256 searches can be saved"
                           : "Saved search changed or was deleted; reopen Search to reload it",
            .context = {}});
    }
    return {};
}

core::Result<void> ListRepository::remove_search(const SavedSearch& expected) {
    auto* database = implementation_->database;
    auto statement = prepare(database, "DELETE FROM saved_searches WHERE id=?1 AND revision=?2");
    if (!statement || !bind_text(statement->get(), 1, expected.id.to_string()) ||
        expected.revision == 0 ||
        expected.revision > static_cast<std::uint64_t>(std::numeric_limits<sqlite3_int64>::max()) ||
        sqlite3_bind_int64(statement->get(), 2, static_cast<sqlite3_int64>(expected.revision)) !=
            SQLITE_OK) {
        return std::unexpected(database_error(database, "Invalid saved search deletion"));
    }
    if (auto done = step_done(database, statement->get(), "Could not delete saved search"); !done) {
        return done;
    }
    if (sqlite3_changes(database) != 1) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::conflict,
            .message = "Saved search changed or was deleted; reopen Search to reload it",
            .context = {}});
    }
    return {};
}

core::Result<std::string> ListRepository::local_listening_key(const ListItem& source) {
    if (!valid_listening_source(source)) {
        return std::unexpected(
            core::Error{.code = core::ErrorCode::invalid_argument,
                        .message = "Listening history requires a qualified local source",
                        .context = {}});
    }
    auto id =
        listening_source(implementation_->database,
                         listening_observation(source.source_reference, *source.source_revision));
    if (!id)
        return std::unexpected(std::move(id.error()));
    // Identity follows playable content, not a CUE filename or list occurrence.
    return listening_track_key(*id, source);
}

core::Result<std::string> local_listening_observation(const ListItem& source) {
    if (!valid_listening_source(source))
        return std::unexpected(
            core::Error{.code = core::ErrorCode::invalid_argument,
                        .message = "Listening history requires a qualified local source",
                        .context = {}});
    return listening_observation(source.source_reference, *source.source_revision);
}

std::string local_listening_track_hash(const std::string& source_id, const ListItem& source) {
    return listening_track_key(source_id, source);
}

core::Result<std::optional<LocalListeningHistory>>
ListRepository::lookup_local_listening_history(const ListItem& source) const {
    if (!valid_listening_source(source))
        return std::unexpected(
            core::Error{.code = core::ErrorCode::invalid_argument,
                        .message = "Listening history requires a qualified local source",
                        .context = {}});
    auto* database = implementation_->database;
    auto query =
        prepare(database, "SELECT source_id FROM local_listening_sources WHERE observation_hash=?");
    if (!query ||
        !bind_text(query->get(), 1,
                   listening_observation(source.source_reference, *source.source_revision)))
        return std::unexpected(database_error(database, "Could not query listening identity"));
    const auto step = sqlite3_step(query->get());
    if (step == SQLITE_DONE)
        return std::optional<LocalListeningHistory>{};
    if (step != SQLITE_ROW)
        return std::unexpected(database_error(database, "Could not read listening identity"));
    return load_local_listening_history(listening_track_key(column_text(query->get(), 0), source));
}

core::Result<void> ListRepository::record_local_listen(const ListItem& source,
                                                       const core::StableId occurrence_id,
                                                       const std::int64_t played_at_ms) {
    if (occurrence_id.is_nil() || played_at_ms <= 0)
        return std::unexpected(
            core::Error{.code = core::ErrorCode::invalid_argument,
                        .message = "A listen requires an occurrence and timestamp",
                        .context = {}});
    auto* database = implementation_->database;
    if (auto begun = execute(database, "BEGIN IMMEDIATE"); !begun)
        return begun;
    const auto finish = [database](core::Result<void> result) {
        if (result)
            result = execute(database, "COMMIT");
        if (!result)
            static_cast<void>(execute(database, "ROLLBACK"));
        return result;
    };
    auto key = local_listening_key(source);
    if (!key)
        return finish(std::unexpected(std::move(key.error())));
    auto event = prepare(
        database, "INSERT INTO local_listening_occurrences(occurrence_id,track_hash,played_at_ms) "
                  "VALUES(?,?,?) ON CONFLICT(occurrence_id) DO NOTHING");
    if (!event || !bind_text(event->get(), 1, occurrence_id.to_string()) ||
        !bind_text(event->get(), 2, *key) ||
        sqlite3_bind_int64(event->get(), 3, played_at_ms) != SQLITE_OK ||
        sqlite3_step(event->get()) != SQLITE_DONE)
        return finish(
            std::unexpected(database_error(database, "Could not store listening occurrence")));
    if (sqlite3_changes(database) != 0) {
        // Qualification can occur halfway through a track. It is not completion
        // and must not clear a saved resume position.
        auto increment = prepare(
            database,
            "INSERT INTO local_listening_history(track_hash,play_count,last_played_ms) "
            "VALUES(?,1,?) ON CONFLICT(track_hash) DO UPDATE SET "
            "play_count=play_count+1,last_played_ms=max(last_played_ms,excluded.last_played_ms)");
        if (!increment || !bind_text(increment->get(), 1, *key) ||
            sqlite3_bind_int64(increment->get(), 2, played_at_ms) != SQLITE_OK)
            return finish(
                std::unexpected(database_error(database, "Could not bind qualified listen")));
        return finish(step_done(database, increment->get(), "Could not count qualified listen"));
    }
    auto existing =
        prepare(database, "SELECT track_hash,played_at_ms FROM local_listening_occurrences "
                          "WHERE occurrence_id=?");
    if (!existing || !bind_text(existing->get(), 1, occurrence_id.to_string()) ||
        sqlite3_step(existing->get()) != SQLITE_ROW)
        return finish(
            std::unexpected(database_error(database, "Could not read listening occurrence")));
    if (column_text(existing->get(), 0) != *key ||
        sqlite3_column_int64(existing->get(), 1) != played_at_ms)
        return finish(std::unexpected(core::Error{.code = core::ErrorCode::conflict,
                                                  .message = "Listening occurrence replay differs",
                                                  .context = {}}));
    return finish({});
}

core::Result<std::optional<LocalListeningHistory>>
ListRepository::load_local_listening_history(const std::string_view track_hash) const {
    if (track_hash.size() != 64U ||
        track_hash.find_first_not_of("0123456789abcdef") != std::string_view::npos) {
        return std::unexpected(core::Error{.code = core::ErrorCode::invalid_argument,
                                           .message = "Invalid listening-history track identity",
                                           .context = {}});
    }
    auto* database = implementation_->database;
    auto statement = prepare(database, "SELECT play_count,last_played_ms,resume_position_ms,"
                                       "resume_updated_at_ms FROM local_listening_history "
                                       "WHERE track_hash=?");
    if (!statement || !bind_text(statement->get(), 1, track_hash)) {
        return std::unexpected(statement
                                   ? database_error(database, "Could not bind listening history")
                                   : std::move(statement.error()));
    }
    const auto result = sqlite3_step(statement->get());
    if (result == SQLITE_DONE)
        return std::optional<LocalListeningHistory>{};
    if (result != SQLITE_ROW)
        return std::unexpected(database_error(database, "Could not load listening history"));
    const auto count = sqlite3_column_int64(statement->get(), 0);
    const auto played = sqlite3_column_int64(statement->get(), 1);
    const auto position = sqlite3_column_int64(statement->get(), 2);
    const auto updated = sqlite3_column_int64(statement->get(), 3);
    if (count < 0 || played < 0 || position < 0 || updated < 0) {
        return std::unexpected(database_error(database, "Invalid listening history record"));
    }
    return std::optional{LocalListeningHistory{
        .track_hash = std::string{track_hash},
        .play_count = static_cast<std::uint64_t>(count),
        .last_played_ms = played,
        .resume_position_ms = position,
        .updated_at_ms = updated,
    }};
}

core::Result<void> ListRepository::record_local_play(const std::string_view track_hash,
                                                     const std::int64_t played_at_ms) {
    if (played_at_ms <= 0) {
        return std::unexpected(core::Error{.code = core::ErrorCode::invalid_argument,
                                           .message = "A completed play requires a timestamp",
                                           .context = {}});
    }
    if (auto loaded = load_local_listening_history(track_hash); !loaded)
        return std::unexpected(std::move(loaded.error()));
    auto* database = implementation_->database;
    auto statement =
        prepare(database,
                "INSERT INTO local_listening_history(track_hash,play_count,last_played_ms,"
                "resume_position_ms,resume_updated_at_ms) VALUES(?,1,?,0,?) "
                "ON CONFLICT(track_hash) DO UPDATE SET play_count=play_count+1,"
                "last_played_ms=max(last_played_ms,excluded.last_played_ms),"
                "resume_position_ms=CASE WHEN excluded.resume_updated_at_ms>=resume_updated_at_ms "
                "THEN 0 ELSE resume_position_ms END,"
                "resume_updated_at_ms=max(resume_updated_at_ms,excluded.resume_updated_at_ms)");
    if (!statement || !bind_text(statement->get(), 1, track_hash) ||
        sqlite3_bind_int64(statement->get(), 2, played_at_ms) != SQLITE_OK ||
        sqlite3_bind_int64(statement->get(), 3, played_at_ms) != SQLITE_OK) {
        return std::unexpected(statement ? database_error(database, "Could not bind completed play")
                                         : std::move(statement.error()));
    }
    return step_done(database, statement->get(), "Could not record completed play");
}

core::Result<void> ListRepository::save_engine_state(const std::string_view key,
                                                     const std::string_view value,
                                                     const std::int64_t updated_at_ms) {
    if (key.empty() || updated_at_ms <= 0) {
        return std::unexpected(core::Error{.code = core::ErrorCode::invalid_argument,
                                           .message = "Invalid engine state write",
                                           .context = {}});
    }
    auto* database = implementation_->database;
    auto statement =
        prepare(database, "INSERT INTO engine_state(key,value,updated_at_ms) VALUES(?,?,?) "
                          "ON CONFLICT(key) DO UPDATE SET value=excluded.value,"
                          "updated_at_ms=excluded.updated_at_ms");
    if (!statement || !bind_text(statement->get(), 1, key) ||
        !bind_text(statement->get(), 2, value) ||
        sqlite3_bind_int64(statement->get(), 3, updated_at_ms) != SQLITE_OK) {
        return std::unexpected(statement ? database_error(database, "Could not bind engine state")
                                         : std::move(statement.error()));
    }
    return step_done(database, statement->get(), "Could not save engine state");
}

core::Result<std::optional<std::string>>
ListRepository::load_engine_state(const std::string_view key) const {
    auto* database = implementation_->database;
    auto statement = prepare(database, "SELECT value FROM engine_state WHERE key = ?");
    if (!statement || !bind_text(statement->get(), 1, key)) {
        return std::unexpected(statement ? database_error(database, "Could not bind engine state")
                                         : std::move(statement.error()));
    }
    const auto stepped = sqlite3_step(statement->get());
    if (stepped == SQLITE_ROW) {
        return std::optional{column_text(statement->get(), 0)};
    }
    if (stepped != SQLITE_DONE) {
        return std::unexpected(database_error(database, "Could not read engine state"));
    }
    return std::optional<std::string>{};
}

core::Result<void> ListRepository::save_local_resume(const std::string_view track_hash,
                                                     const std::int64_t position_ms,
                                                     const std::int64_t updated_at_ms) {
    if (position_ms < 0 || updated_at_ms <= 0) {
        return std::unexpected(core::Error{.code = core::ErrorCode::invalid_argument,
                                           .message = "Invalid local resume observation",
                                           .context = {}});
    }
    if (auto loaded = load_local_listening_history(track_hash); !loaded)
        return std::unexpected(std::move(loaded.error()));
    auto* database = implementation_->database;
    auto statement = prepare(
        database, "INSERT INTO local_listening_history(track_hash,resume_position_ms,"
                  "resume_updated_at_ms) VALUES(?,?,?) ON CONFLICT(track_hash) DO UPDATE SET "
                  "resume_position_ms=excluded.resume_position_ms,"
                  "resume_updated_at_ms=excluded.resume_updated_at_ms "
                  "WHERE excluded.resume_updated_at_ms>resume_updated_at_ms");
    if (!statement || !bind_text(statement->get(), 1, track_hash) ||
        sqlite3_bind_int64(statement->get(), 2, position_ms) != SQLITE_OK ||
        sqlite3_bind_int64(statement->get(), 3, updated_at_ms) != SQLITE_OK) {
        return std::unexpected(statement ? database_error(database, "Could not bind local resume")
                                         : std::move(statement.error()));
    }
    return step_done(database, statement->get(), "Could not save local resume");
}

} // namespace trackknife::persistence

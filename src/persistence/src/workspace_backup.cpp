// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/persistence/workspace_backup.hpp"

#include <sqlite3.h>

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace trackknife::persistence {
namespace {

struct DatabaseCloser {
    void operator()(sqlite3* database) const noexcept {
        if (database != nullptr) {
            static_cast<void>(sqlite3_close_v2(database));
        }
    }
};
using Database = std::unique_ptr<sqlite3, DatabaseCloser>;

[[nodiscard]] core::Error error(core::ErrorCode code, std::string message) {
    return core::Error{.code = code, .message = std::move(message), .context = {}};
}

[[nodiscard]] core::Result<Database> open_read_only(const std::filesystem::path& path) {
    sqlite3* raw = nullptr;
    if (sqlite3_open_v2(path.c_str(), &raw, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK) {
        const auto detail =
            raw == nullptr ? std::string{} : ": " + std::string{sqlite3_errmsg(raw)};
        Database cleanup{raw};
        return std::unexpected(
            error(core::ErrorCode::database, "Could not open workspace database" + detail));
    }
    return Database{raw};
}

[[nodiscard]] core::Result<WorkspaceDatabaseBackupInfo> inspect(sqlite3* database,
                                                                const std::filesystem::path& path) {
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(database, "PRAGMA integrity_check", -1, &raw, nullptr) != SQLITE_OK) {
        return std::unexpected(
            error(core::ErrorCode::database, "Could not start workspace backup integrity check"));
    }
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement{raw, sqlite3_finalize};
    if (sqlite3_step(statement.get()) != SQLITE_ROW ||
        std::string_view{reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 0))} !=
            "ok") {
        return std::unexpected(
            error(core::ErrorCode::database, "Workspace backup failed SQLite integrity check"));
    }
    statement.reset();
    if (sqlite3_prepare_v2(database, "SELECT version FROM schema_version LIMIT 1", -1, &raw,
                           nullptr) != SQLITE_OK) {
        return std::unexpected(
            error(core::ErrorCode::database, "Workspace backup has no Trackknife schema"));
    }
    statement.reset(raw);
    if (sqlite3_step(statement.get()) != SQLITE_ROW) {
        return std::unexpected(
            error(core::ErrorCode::database, "Workspace backup has no schema version"));
    }
    const auto version = sqlite3_column_int64(statement.get(), 0);
    if (version < 1 || version > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected(
            error(core::ErrorCode::database, "Workspace backup has an invalid schema version"));
    }
    std::error_code filesystem_error;
    const auto size = std::filesystem::file_size(path, filesystem_error);
    if (filesystem_error) {
        return std::unexpected(
            error(core::ErrorCode::io,
                  "Could not inspect workspace backup size: " + filesystem_error.message()));
    }
    return WorkspaceDatabaseBackupInfo{.schema_version = static_cast<std::uint32_t>(version),
                                       .size_bytes = size};
}

} // namespace

core::Result<WorkspaceDatabaseBackupInfo>
inspect_workspace_database_backup(const std::filesystem::path& backup) {
    auto database = open_read_only(backup);
    return database ? inspect(database->get(), backup)
                    : std::unexpected(std::move(database.error()));
}

core::Result<WorkspaceDatabaseBackupInfo>
create_workspace_database_backup(const std::filesystem::path& source,
                                 const std::filesystem::path& destination) {
    const auto descriptor =
        ::open(destination.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, S_IRUSR | S_IWUSR);
    if (descriptor < 0) {
        return std::unexpected(
            error(core::ErrorCode::io, errno == EEXIST ? "Workspace backup destination exists"
                                                       : "Could not reserve workspace backup: " +
                                                             std::string{std::strerror(errno)}));
    }
    static_cast<void>(::close(descriptor));
    const auto remove_failed_destination = [&destination] {
        std::error_code ignored;
        std::filesystem::remove(destination, ignored);
    };
    auto input = open_read_only(source);
    if (!input) {
        remove_failed_destination();
        return std::unexpected(std::move(input.error()));
    }
    sqlite3* output_raw = nullptr;
    if (sqlite3_open_v2(destination.c_str(), &output_raw,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
        Database cleanup{output_raw};
        remove_failed_destination();
        return std::unexpected(
            error(core::ErrorCode::database, "Could not create workspace backup database"));
    }
    Database output{output_raw};
    auto* backup = sqlite3_backup_init(output.get(), "main", input->get(), "main");
    if (backup == nullptr) {
        remove_failed_destination();
        return std::unexpected(
            error(core::ErrorCode::database, "Could not initialize workspace backup"));
    }
    const auto copied = sqlite3_backup_step(backup, -1);
    const auto finished = sqlite3_backup_finish(backup);
    if (copied != SQLITE_DONE || finished != SQLITE_OK) {
        output.reset();
        remove_failed_destination();
        return std::unexpected(
            error(core::ErrorCode::database, "Could not copy the workspace database"));
    }
    output.reset();
    input->reset();
    auto inspected = inspect_workspace_database_backup(destination);
    if (!inspected) {
        remove_failed_destination();
    }
    return inspected;
}

core::Result<WorkspaceDatabaseBackupInfo>
restore_workspace_database_backup(const std::filesystem::path& backup,
                                  const std::filesystem::path& live_database,
                                  const std::filesystem::path& rollback_path) {
    auto source_info = inspect_workspace_database_backup(backup);
    if (!source_info) {
        return std::unexpected(std::move(source_info.error()));
    }
    if (std::filesystem::exists(rollback_path)) {
        return std::unexpected(
            error(core::ErrorCode::io, "Workspace restore rollback destination exists"));
    }
    auto staged = live_database;
    staged += ".restore-staging";
    if (std::filesystem::exists(staged)) {
        return std::unexpected(
            error(core::ErrorCode::io, "Workspace restore staging destination exists"));
    }
    auto copied = create_workspace_database_backup(backup, staged);
    if (!copied) {
        return std::unexpected(std::move(copied.error()));
    }
    std::error_code filesystem_error;
    const auto had_live = std::filesystem::exists(live_database);
    std::vector<std::pair<std::filesystem::path, std::filesystem::path>> retained_sidecars;
    const auto restore_retained = [&] {
        std::error_code ignored;
        for (auto iterator = retained_sidecars.rbegin(); iterator != retained_sidecars.rend();
             ++iterator) {
            std::filesystem::rename(iterator->second, iterator->first, ignored);
        }
        std::filesystem::rename(rollback_path, live_database, ignored);
    };
    if (had_live) {
        std::filesystem::rename(live_database, rollback_path, filesystem_error);
        if (filesystem_error) {
            std::filesystem::remove(staged, filesystem_error);
            return std::unexpected(
                error(core::ErrorCode::io, "Could not retain the pre-restore workspace"));
        }
        for (const auto* suffix : {"-wal", "-shm"}) {
            const auto live_sidecar = std::filesystem::path{live_database.string() + suffix};
            const auto rollback_sidecar = std::filesystem::path{rollback_path.string() + suffix};
            if (std::filesystem::exists(live_sidecar)) {
                std::filesystem::rename(live_sidecar, rollback_sidecar, filesystem_error);
                if (filesystem_error) {
                    restore_retained();
                    std::error_code ignored;
                    std::filesystem::remove(staged, ignored);
                    return std::unexpected(
                        error(core::ErrorCode::io, "Could not retain the pre-restore WAL state"));
                }
                retained_sidecars.emplace_back(live_sidecar, rollback_sidecar);
            }
        }
    }
    std::filesystem::rename(staged, live_database, filesystem_error);
    if (filesystem_error) {
        if (had_live) {
            restore_retained();
        }
        std::filesystem::remove(staged, filesystem_error);
        return std::unexpected(
            error(core::ErrorCode::io, "Could not publish the restored workspace"));
    }
    auto restored = inspect_workspace_database_backup(live_database);
    if (!restored) {
        std::filesystem::remove(live_database, filesystem_error);
        if (had_live) {
            restore_retained();
        }
        return std::unexpected(std::move(restored.error()));
    }
    return restored;
}

} // namespace trackknife::persistence

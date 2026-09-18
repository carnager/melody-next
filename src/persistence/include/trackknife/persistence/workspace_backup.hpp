// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/result.hpp"

#include <cstdint>
#include <filesystem>

namespace trackknife::persistence {

struct WorkspaceDatabaseBackupInfo {
    std::uint32_t schema_version{0};
    std::uintmax_t size_bytes{0};

    friend bool operator==(const WorkspaceDatabaseBackupInfo&,
                           const WorkspaceDatabaseBackupInfo&) = default;
};

// Produces one consistent SQLite image, including committed WAL contents.
// Existing destinations are rejected rather than replaced.
[[nodiscard]] core::Result<WorkspaceDatabaseBackupInfo>
create_workspace_database_backup(const std::filesystem::path& source,
                                 const std::filesystem::path& destination);

// Opens an existing snapshot read-only, runs SQLite's full integrity check,
// and returns its Trackknife schema/version evidence.
[[nodiscard]] core::Result<WorkspaceDatabaseBackupInfo>
inspect_workspace_database_backup(const std::filesystem::path& backup);

// Replaces a closed live database from a validated backup. The previous live
// image is retained at rollback_path; neither it nor the backup is overwritten.
[[nodiscard]] core::Result<WorkspaceDatabaseBackupInfo>
restore_workspace_database_backup(const std::filesystem::path& backup,
                                  const std::filesystem::path& live_database,
                                  const std::filesystem::path& rollback_path);

} // namespace trackknife::persistence

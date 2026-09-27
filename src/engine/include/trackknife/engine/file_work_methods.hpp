// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/error.hpp"
#include "trackknife/engine/catalogue.hpp"
#include "trackknife/protocol/dispatch.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>

namespace trackknife::engine {

// ADR-0237: what the engine reads for the file tools, answered at once --
// the long work (measuring, writing) is jobs.
//
// metadata.read {paths: [encoded]} answers {files: [...]}, one per path in
// order: {read} with the file's tags, revision, adapter and capabilities, or
// {error, revision} when it cannot be read -- the revision present when the
// file exists but has no tags to read, which can still take gains in a
// sidecar. At most metadata_read_limit paths per call.
inline constexpr std::size_t metadata_read_limit = 256U;

// What the engine did at startup about file work a crash interrupted: how
// many operations it finished or rolled back, or why it could not look.
struct FileWorkRecovery {
    std::size_t recovered{0U};
    std::optional<core::Error> error;
};

// Recovers the engine's metadata journal, as Trackknife recovers its own:
// finished or rolled back where that is safe, left for the user otherwise.
// Backups are released, as there is no undo to keep them for. Run once, at
// startup, before clients connect.
[[nodiscard]] FileWorkRecovery recover_file_work(const std::filesystem::path& database,
                                                 LocalCatalogue& catalogue);

// metadata.interrupted answers {recovered, error, interrupted: [{id, path,
// message}]}: what startup recovery did, and each operation it could neither
// finish nor roll back -- what Trackknife's "Interrupted file work" lists.
void register_file_work_methods(protocol::Dispatcher& dispatcher,
                                std::filesystem::path database = {},
                                FileWorkRecovery recovery = {});

} // namespace trackknife::engine

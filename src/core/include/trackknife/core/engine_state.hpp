// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/result.hpp"

#include <cstddef>
#include <filesystem>

namespace trackknife::core {

// ADR-0287: where an engine keeps its state -- its database, id, keys,
// staging and undo copies -- when no folder is named: melodyd's default,
// and the folder the window opens the same database in.
//
// `MELODY_STATE_DIR` when set; else `$XDG_DATA_HOME/melody/engine`, on
// macOS `~/Library/Application Support/melody/engine`, else
// `~/.local/share/melody/engine`.
[[nodiscard]] std::filesystem::path default_engine_state_directory();

// ADR-0339: Melody's own data -- its presets: naming layouts, destinations,
// encoder presets, tagging scripts -- which the window and melody-cli share
// on this computer and no engine keeps. `MELODY_DATA_DIR` when set; else
// Qt's AppDataLocation for Melody: `$XDG_DATA_HOME/melody/melody`, on macOS
// `~/Library/Application Support/melody/melody`, else
// `~/.local/share/melody/melody`.
[[nodiscard]] std::filesystem::path melody_data_directory();

// The presets' database in it.
[[nodiscard]] std::filesystem::path presets_database_path();

// Where it was kept before, under Trackknife's name.
[[nodiscard]] std::filesystem::path legacy_engine_state_directory();

// The folder in use: the default, unless it holds no database yet and the
// old one does -- an engine not yet started since the rename, which moves
// it then. A window opening the database follows the engine this way.
[[nodiscard]] std::filesystem::path engine_state_directory();

// Takes the folder's lock, as an engine running there holds it, for as
// long as the process lives. False when another holds it.
[[nodiscard]] bool hold_engine_lock(const std::filesystem::path& state_directory);

// Moves an engine's own files from the old folder to `to`, once: when `to`
// holds no database and the old folder does, and no engine holds the old
// folder's lock. Only what an engine keeps is moved -- a window's tabs and
// backups in the same folder stay for the window. Answers how many entries
// were moved; nothing moved on an error, which says why.
[[nodiscard]] Result<std::size_t> adopt_legacy_engine_state(const std::filesystem::path& to);

} // namespace trackknife::core

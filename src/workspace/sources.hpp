// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QString>

#include <string>
#include <vector>

namespace trackknife::bench {

// The Sources panel's settings, as the window reads them: its folder
// bookmarks and which source it opens on.

// The bookmarked folders, in order. The first time there are none saved,
// the home directory and the library's old folders become them.
[[nodiscard]] std::vector<std::string> loadFolderBookmarks();
void storeFolderBookmarks(const std::vector<std::string>& raw_paths);
// A bookmark as it is listed: the folder's name; and in full, as its tooltip.
[[nodiscard]] QString folderBookmarkLabel(const std::string& raw_path);
[[nodiscard]] QString folderBookmarkTooltip(const std::string& raw_path);

// ADR-0286: the engine holding the library: "local", or an engine elsewhere
// by its address (Settings › Engines).
[[nodiscard]] QString libraryEngine();
// Whether this computer's library is offered: it holds the library.
[[nodiscard]] bool localLibraryShown();
// Which source the panel opens on, as chosen last: "folders", "library" or
// "remote", or "playlists" -- a library unless another was chosen.
[[nodiscard]] QString preferredSource();
// Remembered when the user picks a source, not when the window switches.
void rememberSource(const QString& kind);

} // namespace trackknife::bench

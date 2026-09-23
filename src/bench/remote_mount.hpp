// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace trackknife::bench {

// ADR-0227: where this computer sees a remote engine's music. The remote
// engine names its files by its own paths; moving a row between an engine's
// tab and this computer's translates the path through this, so a remote
// track in a local tab is a real file here -- one this computer's engine can
// play and the tagger can edit.
//
// Empty is the paths being the same on both machines: the NAS mounted at the
// same place, which is the usual set-up and needs no configuring.
struct RemoteMount final {
    std::string remote_folder; // as the remote engine sees it
    std::string local_folder;  // the same folder on this computer

    // Read from Settings.
    [[nodiscard]] static RemoteMount configured();

    // A remote path as this computer sees it. Nothing when it is outside
    // the configured folder, or not there -- the mount missing, say.
    [[nodiscard]] std::optional<std::string> to_local(const std::string& remote_path) const;

    // A path on this computer as the remote engine sees it. Nothing unless
    // it falls inside one of the remote's library folders (`remote_roots`):
    // those are the only files it is known to have.
    [[nodiscard]] std::optional<std::string>
    to_remote(const std::string& local_path, const std::vector<std::string>& remote_roots) const;
};

// Whether `path` is `folder` or inside it, on a component boundary.
[[nodiscard]] bool path_within(std::string_view path, std::string_view folder);

} // namespace trackknife::bench

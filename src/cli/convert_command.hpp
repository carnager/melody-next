// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/protocol/client.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace trackknife::cli {

// When `--since` means, as Unix seconds: those themselves, a local date or
// date and time, or a time ago (90m, 12h, 3d, 2w).
[[nodiscard]] std::optional<std::int64_t> since_seconds(const std::string& when);

// The raw paths of an album's tracks, by the album's key.
[[nodiscard]] std::vector<std::string> album_track_paths(protocol::Client& client,
                                                         const std::string& album_key);

// melody-cli convert: tracks -- named, in folders named, or the albums added
// since a moment -- converted with Melody's converter on this computer,
// named by one of the engine's naming layouts or mirroring their place in
// the music folder; one already converted is passed over. `words` follow
// "convert". Returns the exit status.
[[nodiscard]] int convert_command(protocol::Client& client, bool json,
                                  const std::vector<std::string>& words);

} // namespace trackknife::cli

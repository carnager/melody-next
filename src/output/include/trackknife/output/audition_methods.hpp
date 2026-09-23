// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/audio/local_audition.hpp"
#include "trackknife/protocol/dispatch.hpp"

#include <filesystem>
#include <optional>

namespace trackknife::output {

// ADR-0228: the agent's side. Serves `audition.*` on this machine's audio.
// Relative paths are under `music_root`; an agent without one plays absolute
// paths and streams only.
void register_audition_methods(protocol::Dispatcher& dispatcher,
                               audio::LocalAuditionService& audition,
                               std::optional<std::filesystem::path> music_root);

} // namespace trackknife::output

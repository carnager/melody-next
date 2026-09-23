// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/result.hpp"
#include "trackknife/protocol/client.hpp"

#include <QString>

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>

namespace trackknife::bench {

// ADR-0226: with no engine configured, the workspace runs one of its own --
// the same tkengine a NAS runs, on this machine's database. It is started
// detached and outlives the window, which reconnects to it next time, the way
// an MPD client finds its MPD.
struct LocalEngine final {
    std::filesystem::path state; // The database directory, Trackknife's own.
    std::filesystem::path socket;
};

// Nothing in test mode: a test must never start a daemon that outlives it,
// and never reach the one already serving the real database.
[[nodiscard]] std::optional<LocalEngine> localEngine();

// Connects to the local engine, starting it first if nothing answers. Waits
// at most timeout for a started engine to listen.
[[nodiscard]] core::Result<std::unique_ptr<protocol::Client>>
connectLocalEngine(const LocalEngine& engine,
                   std::chrono::milliseconds timeout = std::chrono::seconds{10});

// The tkengine beside this executable (installed, or in the build tree), or
// the one on PATH. TRACKKNIFE_ENGINE overrides. Empty when there is none.
[[nodiscard]] QString engineProgram();

} // namespace trackknife::bench

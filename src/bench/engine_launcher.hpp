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
// the same melodyd a NAS runs, on this machine's database. It is started
// detached and outlives the window, which reconnects to it next time, the way
// an MPD client finds its MPD.
struct LocalEngine final {
    std::filesystem::path state; // The database directory, Trackknife's own.
    std::filesystem::path socket;
};

// Starting an engine is the application's decision, made once in main():
// off by default, so no library user and no test ever starts a daemon that
// outlives it, or reaches the one serving the real database. (A test once
// did, through a guard that relied on tests enabling Qt's test mode.)
void allowLocalEngine(bool allowed);

// Nothing unless allowed.
[[nodiscard]] std::optional<LocalEngine> localEngine();

// Connects to the local engine, starting it first if nothing answers. Waits
// at most timeout for a started engine to listen.
[[nodiscard]] core::Result<std::unique_ptr<protocol::Client>>
connectLocalEngine(const LocalEngine& engine,
                   std::chrono::milliseconds timeout = std::chrono::seconds{10});

// The melodyd installed beside this executable, or the build tree's.
// TRACKKNIFE_ENGINE overrides. Never a search of PATH: another program of
// the same name there is not this engine (the Go melodyd was, once, and a
// test started it). Empty when there is none.
[[nodiscard]] QString engineProgram();

} // namespace trackknife::bench

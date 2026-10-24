// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/result.hpp"
#include "trackknife/protocol/client.hpp"

#include <QString>
#include <QStringList>

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

#include <sys/types.h>

namespace trackknife::bench {

// ADR-0226: with no engine configured, the workspace runs one of its own --
// the same melodyd a NAS runs, on this machine's database. It is started
// detached and outlives the window, which reconnects to it next time, the way
// an MPD client finds its MPD.
struct LocalEngine final {
    std::filesystem::path state; // The database directory, Trackknife's own.
    std::filesystem::path socket;
    // ADR-0286: this user's engine -- Trackknife's own folder, the default
    // socket -- which systemd runs on Linux (melodyd.socket). Any other, a
    // test's or a second workspace's, and every one on macOS, is started
    // here, with a melodyd.conf beside its database when not this user's.
    bool standard{false};
};

// Starting an engine is the application's decision, made once in main():
// off by default, so no library user and no test ever starts a daemon that
// outlives it, or reaches the one serving the real database. (A test once
// did, through a guard that relied on tests enabling Qt's test mode.)
void allowLocalEngine(bool allowed);

// Nothing unless allowed.
[[nodiscard]] std::optional<LocalEngine> localEngine();

// Connects to the local engine. When nothing answers, this user's engine on
// Linux is systemd's to start: its socket is started if it is enabled and
// is not yet -- or again, when it runs without its socket file -- else what
// to do is said. Any other is started here, as
// `melodyd` with its socket, folder and file -- every setting is in
// melodyd.conf (ADR-0286). Waits at most timeout for it to listen.
[[nodiscard]] core::Result<std::unique_ptr<protocol::Client>>
connectLocalEngine(const LocalEngine& engine,
                   std::chrono::milliseconds timeout = std::chrono::seconds{10});

// How this computer's engine is shared, from Settings; handed to the engine
// as melodyd.conf settings (sharingChanges), a restart taking them up.
struct LocalEngineSharing final {
    bool share{false};
    QString listen;     // host:port for clients and agents
    int stream_port{0}; // the stream port for agents without the files
    QString password;   // empty: open
    QString music_root; // empty: none
    // The engines elsewhere this one plays for on this computer's speakers
    // (ADR-0228, ADR-0234): each by address, with its password and where its
    // music is reachable here.
    struct PlayFor final {
        QString address;
        QString password; // empty: open
        QString music_root;
        // ADR-0239: kbps of Opus it streams here nearby and away; 0 the
        // original files.
        int nearby_kbps{0};
        int away_kbps{128};
        friend bool operator==(const PlayFor&, const PlayFor&) = default;
    };
    std::vector<PlayFor> play_for;
    // Plays for every engine found on the network, too, streamed as these.
    bool play_for_found{false};
    int found_nearby_kbps{0};
    int found_away_kbps{128};
    bool upnp{false};
    friend bool operator==(const LocalEngineSharing&, const LocalEngineSharing&) = default;
};
[[nodiscard]] LocalEngineSharing localEngineSharing();
// ADR-0286: the config.set changes that share this computer's engine as
// `now` says -- each key it manages set or taken out -- and take out the
// [engine] sections of engines `before` played for and `now` does not.
[[nodiscard]] protocol::Json sharingChanges(const LocalEngineSharing& now,
                                            const LocalEngineSharing& before);
// Hands this computer's engine those changes and restarts it to use them.
[[nodiscard]] core::Result<void> configureLocalEngine(const LocalEngine& engine,
                                                      const protocol::Json& changes);

// Stops this computer's engine for good (engine.stop: under systemd, not
// restarted until a client connects again); one older than that is
// signalled. Its queue is saved, and comes back paused.
[[nodiscard]] core::Result<void> stopLocalEngine(const LocalEngine& engine);

// Starts this computer's engine again, with its settings as they are now:
// a service's restarts itself (engine.restart), one started here is stopped
// and started. Playback is saved and comes back paused.
[[nodiscard]] core::Result<void> restartLocalEngine(const LocalEngine& engine);

// Whether a service manager runs it (engine.info's `service`).
[[nodiscard]] bool localEngineIsService(const LocalEngine& engine);
// ADR-0286: this user's engine on Linux, running, but started by an older
// Trackknife rather than systemd -- with melodyd.socket there to take over.
[[nodiscard]] bool localEngineWantsService(const LocalEngine& engine);
// Stops that engine and starts melodyd.socket, whose engine then answers:
// the one time Trackknife starts anything of systemd's.
[[nodiscard]] core::Result<void> handOverToService(const LocalEngine& engine);

// Whether this computer's engine runs a program other than the one this build
// would start: rebuilt or updated since it started. It outlives the window,
// so without this it would go on answering in yesterday's terms.
[[nodiscard]] bool localEngineOutdated(const LocalEngine& engine);

// Whether a process still runs. One that has exited but not yet been reaped
// is not: the engine is started detached, so it is reaped by whatever
// adopted it, and in a container that may be a PID 1 that never reaps.
[[nodiscard]] bool processRunning(pid_t pid);

// The processes holding a lock file locked: the engine, for its engine.lock.
[[nodiscard]] std::vector<pid_t> lockHolders(const std::filesystem::path& lock);

// The melodyd installed beside this executable, or the build tree's.
// MELODY_ENGINE overrides. Never a search of PATH: another program of
// the same name there is not this engine (the Go melodyd was, once, and a
// test started it). Empty when there is none.
[[nodiscard]] QString engineProgram();

} // namespace trackknife::bench

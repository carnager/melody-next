// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/engine/player.hpp"
#include "trackknife/engine/workspace.hpp"

#include <atomic>
#include <chrono>
#include <thread>

namespace trackknife::engine {

// ADR-0220: the queue belongs to the engine, so the engine remembers it.
//
// A window is a view of what the engine holds. That is only true if the engine
// can bring the queue back by itself -- otherwise the first client to connect
// after a restart is the one that decides what the engine is playing, which is
// the client owning the queue with extra steps.
//
// Two records rather than one. The queue changes when someone edits it; the
// position changes continuously. Writing a thousand-entry queue every few
// seconds to record that playback moved on would be pure waste, so the
// structure is written when the player's revision changes and the position on
// a slow cadence while something is playing.
class PlaybackStore final {
  public:
    static constexpr auto queue_key = "playback.queue.v1";
    static constexpr auto position_key = "playback.position.v1";

    PlaybackStore(Player& player, Workspace& workspace,
                  std::chrono::milliseconds interval = std::chrono::seconds{5});
    PlaybackStore(const PlaybackStore&) = delete;
    PlaybackStore(PlaybackStore&&) = delete;
    PlaybackStore& operator=(const PlaybackStore&) = delete;
    PlaybackStore& operator=(PlaybackStore&&) = delete;
    ~PlaybackStore();

    // Reads what was stored and hands it to the player, paused where it left
    // off. Answers whether anything was restored; an engine that has never run
    // has nothing, and that is ordinary rather than an error.
    [[nodiscard]] bool restore();

    // Writes whatever has changed. Called by the worker; public because a
    // caller that is shutting down wants one last write without waiting for a
    // tick.
    void persist();

    void start();
    void stop();

  private:
    Player* player_;
    Workspace* workspace_;
    std::chrono::milliseconds interval_;
    std::uint64_t written_revision_{0};
    bool written_anything_{false};
    std::atomic_bool running_{false};
    std::thread worker_;
};

} // namespace trackknife::engine

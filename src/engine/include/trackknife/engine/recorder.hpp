// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/engine/player.hpp"
#include "trackknife/engine/workspace.hpp"

#include <atomic>
#include <chrono>
#include <thread>

namespace trackknife::engine {

// Drains the player's observations into the workspace.
//
// Player::observe deliberately pulls rather than pushes, so the player has no
// persistence dependency and can be tested without a database. This is the
// piece that joins them, and it is the reason the engine can record a play
// count at all -- without it the counters are computed and discarded.
//
// It samples rather than waiting on a signal because listening time is an
// accumulation: the accounting needs regular observation, not notification of
// a change. That is the same shape the workspace used with a timer, which is
// where these rules came from.
class Recorder final {
  public:
    Recorder(Player& player, Workspace& workspace,
             std::chrono::milliseconds interval = std::chrono::milliseconds{1000});
    Recorder(const Recorder&) = delete;
    Recorder(Recorder&&) = delete;
    Recorder& operator=(const Recorder&) = delete;
    Recorder& operator=(Recorder&&) = delete;
    ~Recorder();

    void start();
    void stop();

  private:
    void drain(std::int64_t monotonic_ms, std::int64_t wall_ms);

    Player* player_;
    Workspace* workspace_;
    std::chrono::milliseconds interval_;
    std::atomic_bool running_{false};
    std::thread worker_;
};

} // namespace trackknife::engine

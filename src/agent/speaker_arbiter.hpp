// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "agent/agent.hpp"
#include "trackknife/engine/interruptible_pause.hpp"
#include "trackknife/engine/player.hpp"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace trackknife::agent {

// One machine's speakers, several engines wanting them: this machine's own
// engine, if it has one, and every other engine playing here through an
// agent. The newest to start playing gets them; the rest are paused, and
// this machine's engine says who took them.
class SpeakerArbiter final {
  public:
    // `player` is this machine's own engine, or nullptr where there is none
    // (a melody-agent on its own).
    explicit SpeakerArbiter(engine::Player* player);
    SpeakerArbiter(const SpeakerArbiter&) = delete;
    SpeakerArbiter& operator=(const SpeakerArbiter&) = delete;
    ~SpeakerArbiter();

    // Another engine playing here, by the name to show when it takes them.
    void add_guest(std::string name, Agent& guest);
    void remove_guest(const Agent& guest);

    void start();
    void stop();
    // One look at every side: what the watcher does every tenth of a second.
    void settle();

  private:
    struct Party final {
        std::string name;
        Agent* guest{nullptr}; // nullptr: this machine's own engine
        bool was_playing{false};
    };
    [[nodiscard]] bool playing(const Party& party) const;
    void pause(const Party& party) const;

    engine::Player* player_;
    std::mutex mutex_;
    std::vector<Party> parties_;
    std::atomic_bool running_{false};
    engine::InterruptiblePause pause_;
    std::thread watcher_;
};

} // namespace trackknife::agent

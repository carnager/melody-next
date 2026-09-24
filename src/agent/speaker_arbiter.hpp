// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "agent/agent.hpp"
#include "trackknife/engine/interruptible_pause.hpp"
#include "trackknife/engine/player.hpp"

#include <atomic>
#include <string>
#include <thread>

namespace trackknife::agent {

// One machine's speakers, two engines wanting them: this one's own player,
// and another engine playing here through this engine's built-in agent. The
// newest to start playing gets them; the other is paused, and this engine's
// player says who took them.
class SpeakerArbiter final {
  public:
    SpeakerArbiter(engine::Player& player, Agent& guest, std::string guest_name);
    SpeakerArbiter(const SpeakerArbiter&) = delete;
    SpeakerArbiter& operator=(const SpeakerArbiter&) = delete;
    ~SpeakerArbiter();

    void start();
    void stop();
    // One look at both sides: what the watcher does every tenth of a second.
    void settle();

  private:
    [[nodiscard]] bool local_playing() const;
    [[nodiscard]] bool guest_playing();

    engine::Player* player_;
    Agent* guest_;
    const std::string guest_name_;
    bool local_was_playing_{false};
    bool guest_was_playing_{false};
    std::atomic_bool running_{false};
    engine::InterruptiblePause pause_;
    std::thread watcher_;
};

} // namespace trackknife::agent

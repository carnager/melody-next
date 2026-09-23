// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/engine/interruptible_pause.hpp"

#include "trackknife/engine/job_registry.hpp"
#include "trackknife/engine/player.hpp"
#include "trackknife/protocol/dispatch.hpp"

#include <atomic>
#include <chrono>
#include <thread>

namespace trackknife::engine {

// The player's state as a client sees it. Raw paths are base64, because a
// path is bytes and a JSON string must be valid UTF-8.
[[nodiscard]] protocol::Json to_json(const Player::State& state);

// A queue entry as it travels, and back. Shared with the engine's own store
// (playback_store): what the engine remembers between runs is the same entry
// the wire carries, and a second encoding of the same thing is a second thing
// to keep in step.
[[nodiscard]] protocol::Json to_json(const QueueEntry& entry);
[[nodiscard]] core::Result<QueueEntry> queue_entry_from_json(const protocol::Json& value);
[[nodiscard]] protocol::Json to_json(const audio::PlaybackModes& modes);
[[nodiscard]] audio::PlaybackModes modes_from_json(const protocol::Json& value);

// Binds playback.*. The player must outlive the dispatcher.
void register_playback_methods(protocol::Dispatcher& dispatcher, Player& player);

// ADR-0222: pushed state events, so a client learns a track changed without
// asking. The engine samples its own player and emits when something a client
// would render has changed.
//
// Position is deliberately excluded from that comparison: it changes
// continuously, and emitting on every tick would be a broadcast storm carrying
// nothing a client could not compute. A client wanting a live position asks
// for state, or interpolates from the last event.
class PlaybackWatcher final {
  public:
    PlaybackWatcher(Player& player, EventSink sink,
                    std::chrono::milliseconds interval = std::chrono::milliseconds{250});
    PlaybackWatcher(const PlaybackWatcher&) = delete;
    PlaybackWatcher(PlaybackWatcher&&) = delete;
    PlaybackWatcher& operator=(const PlaybackWatcher&) = delete;
    PlaybackWatcher& operator=(PlaybackWatcher&&) = delete;
    ~PlaybackWatcher();

    void start();
    void stop();

  private:
    Player* player_;
    EventSink sink_;
    std::chrono::milliseconds interval_;
    std::atomic_bool running_{false};
    InterruptiblePause pause_;
    std::thread worker_;
};

} // namespace trackknife::engine

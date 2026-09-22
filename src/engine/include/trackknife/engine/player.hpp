// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/audio/listen_observation.hpp"
#include "trackknife/audio/local_audition.hpp"
#include "trackknife/audio/playback_anchors.hpp"
#include "trackknife/audio/playback_modes.hpp"
#include "trackknife/audio/playback_order.hpp"
#include "trackknife/audio/playback_selection.hpp"
#include "trackknife/audio/resume_checkpoint.hpp"
#include "trackknife/core/listen_accounting.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/core/stable_id.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace trackknife::engine {

// One entry in the engine's queue.
//
// Deliberately not the workspace's LocalTrackRow. That is a display
// projection carrying a metadata document, artwork state and probe results --
// everything a table needs and nothing the engine does. The engine needs to
// know what to decode and how to name it, which is this.
struct QueueEntry final {
    // ADR-0221: identity, so the entry stays addressable as the queue is
    // reordered and so a client can name it across the socket.
    core::StableId entry_id{core::StableId::random()};
    audio::TrackSource source;
    std::optional<std::int64_t> duration_ms;

    friend bool operator==(const QueueEntry&, const QueueEntry&) = default;
};

// ADR-0220 Phase 2: playback owned by the engine rather than the window, which
// is what lets the UI exit without the music stopping.
//
// Holds the queue, the modes and order that decide what comes next, and the
// audition service that makes sound. Every method is safe to call from the
// socket threads that serve requests.
class Player final {
  public:
    [[nodiscard]] static core::Result<std::unique_ptr<Player>> create();

    Player(const Player&) = delete;
    Player(Player&&) = delete;
    Player& operator=(const Player&) = delete;
    Player& operator=(Player&&) = delete;
    ~Player();

    // Replaces the queue wholesale. Playback continues if the playing entry
    // is still present; it stops if the entry has gone, rather than jumping
    // to whatever now occupies its row.
    void replace_queue(std::vector<QueueEntry> entries);
    [[nodiscard]] std::vector<QueueEntry> queue() const;

    // Explicit asks, which outrank the queue's own order. ADR-0220 calls this
    // up-next; the workspace has had it locally since ADR-0196 and it has to
    // exist here before playback can move, or switching would silently lose
    // it.
    //
    // A request names an entry already in the queue rather than carrying its
    // own source: a client asking for something the engine does not have is a
    // mistake worth reporting, not a second way to add tracks.
    [[nodiscard]] core::Result<void> request(const core::StableId& entry_id);
    [[nodiscard]] std::vector<core::StableId> requests() const;
    void clear_requests();

    [[nodiscard]] core::Result<void> play_entry(const core::StableId& entry_id);
    [[nodiscard]] core::Result<void> resume();
    [[nodiscard]] core::Result<void> pause();
    [[nodiscard]] core::Result<void> stop();
    [[nodiscard]] core::Result<void> seek_ms(std::int64_t position_ms);
    // Direction is +1 or -1. Answers not_found when the modes and order say
    // there is nowhere to go, which is how repeat-off at the end reports.
    [[nodiscard]] core::Result<void> step(int direction);

    [[nodiscard]] audio::PlaybackModes modes() const;
    void set_modes(audio::PlaybackModes modes);

    // What the engine has decided to record about playback since it was last
    // asked. Pulling rather than pushing keeps the player free of a
    // persistence dependency: whoever owns a database drains this.
    struct Observations final {
        // Set when a track has been listened to long enough to count. The
        // entry is the one it was credited to, which may no longer be playing
        // by the time anyone reads this.
        std::optional<core::StableId> listened_entry;
        audio::TrackSource listened_source;
        // Where playback is, for a resume checkpoint. Absent when there is
        // nothing worth remembering -- see audio::resumable.
        std::optional<core::StableId> resume_entry;
        audio::TrackSource resume_source;
        std::int64_t resume_position_ms{0};
    };

    // Samples the player and accumulates listening time, keeps the gapless
    // continuation current, and notices when the engine has handed over to
    // it. Called on a timer by
    // whoever owns the engine; the counters it keeps need regular observation
    // rather than a callback, which is the same shape the workspace used.
    //
    // `monotonic_ms` must advance monotonically; wall time would credit or
    // lose listening whenever the clock is adjusted.
    [[nodiscard]] Observations observe(std::int64_t monotonic_ms);

    // A snapshot of everything a client needs to render transport.
    struct State final {
        std::string status; // playing | paused | stopped | loading
        core::StableId entry;
        audio::TrackSource source;
        std::int64_t position_ms{0};
        std::int64_t duration_ms{-1};
        std::size_t queue_size{0};
        audio::PlaybackModes modes;
    };
    [[nodiscard]] State state() const;

  private:
    class QueueView;

    explicit Player(std::unique_ptr<audio::LocalAuditionService> audition);

    // Callers already hold the lock.
    [[nodiscard]] core::Result<void> start_locked(std::size_t row);
    void reset_order_locked();
    // Offers the audition service whatever should follow the current track,
    // so an album plays without a gap between its tracks. Recomputed rather
    // than remembered, because a queue edit or a mode change can make the
    // answer different from the one offered a moment ago.
    void refresh_gapless_locked();
    // The engine increments a counter when it actually hands over to the
    // queued continuation. Following that, rather than guessing from
    // position, is what keeps the anchors honest across a gapless boundary.
    void follow_gapless_locked(const audio::LocalAuditionSnapshot& snapshot);

    mutable std::mutex mutex_;
    std::unique_ptr<audio::LocalAuditionService> audition_;
    core::ListenAccounting listening_;
    std::vector<QueueEntry> queue_;
    audio::PlaybackAnchors anchors_;
    audio::PlaybackModes modes_;
    audio::PlaybackOrder order_;
    int row_{-1};
    // Identities rather than sources, so a request survives the queue being
    // reordered for the same reason playback does.
    std::vector<core::StableId> requests_;
    // What was last offered for gapless continuation, so an unchanged
    // decision is not re-sent on every observation.
    std::optional<core::StableId> gapless_entry_;
    std::uint64_t seen_transitions_{0U};
};

} // namespace trackknife::engine

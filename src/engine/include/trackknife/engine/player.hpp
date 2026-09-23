// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/audio/album_grouping.hpp"
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
    // What album this entry belongs to, for album shuffle. Sent by the client
    // because the engine's queue is paths, not tags -- and which release a
    // track belongs to is a tagging decision the client has already made
    // (album artist falling back to artist, and so on).
    audio::AlbumRowKey group;
    // ADR-0139/0141: the explicit playback override, when the client has one.
    // The decoder's own tags apply otherwise and need not be sent -- the
    // engine opens the file and reads them for itself. What it cannot know is
    // a sidecar value or a CUE sheet's REM lines, which is what this carries.
    std::optional<formats::ReplayGainInfo> replay_gain;

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
    // With this machine's audio as its output. Fails without an audio device.
    [[nodiscard]] static core::Result<std::unique_ptr<Player>> create();
    // ADR-0228: without audio of its own -- a server with no sound device,
    // which plays through output agents. It keeps its queue and plays
    // nothing until an output is chosen.
    [[nodiscard]] static std::unique_ptr<Player> create_without_audio();

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

    // Replaces the whole request list in one go. A client that owns the
    // up-next order -- the window's panel does -- has to be able to state it,
    // rather than reproducing it by cancelling and re-asking one at a time and
    // being visibly wrong in between.
    //
    // Refuses the lot if any entry is unknown: a partially applied order is
    // worse than a rejected one, because nothing then says which half took.
    [[nodiscard]] core::Result<void> set_requests(const std::vector<core::StableId>& entries);

    // Holds entries that are not in the queue, by identity, so they can be
    // requested. This is how something that was never in the list -- a track
    // sent to up-next from the library -- becomes playable by an engine that
    // only plays what it holds. They stay out of the queue itself.
    void enqueue(std::vector<QueueEntry> entries);
    void clear_requests();

    [[nodiscard]] core::Result<void> play_entry(const core::StableId& entry_id);
    [[nodiscard]] core::Result<void> resume();
    [[nodiscard]] core::Result<void> pause();
    [[nodiscard]] core::Result<void> stop();
    [[nodiscard]] core::Result<void> seek_ms(std::int64_t position_ms);

    // Output volume in percent [0, 100]. It belongs to the engine because the
    // engine owns the output: two clients watching the same engine must see
    // the same volume, and a client that exits must not take its setting with
    // it.
    [[nodiscard]] core::Result<void> set_volume_percent(int percent);

    // ADR-0138: how a gain is chosen, and the two preamps that apply while it
    // is active. Engine state for the same reason volume is: it decides how
    // loud the output is, and two clients must not disagree about it.
    //
    // "Auto" is not here. Choosing album gain unless shuffling is a client's
    // policy about its own modes, and it resolves to track or album before it
    // is sent -- the engine is told what to do, not what the user picked.
    [[nodiscard]] core::Result<void> set_replay_gain_mode(audio::ReplayGainMode mode);
    [[nodiscard]] core::Result<void> set_replay_gain_preamps(audio::ReplayGainPreamps preamps);

    // ADR-0228: what plays the sound. This machine's audio (`local_output`),
    // an output agent, or nothing. Moving playback to another output carries
    // the playing entry and position across, stops the old one first, and
    // keeps playing if it was: the music moves rooms rather than restarting.
    [[nodiscard]] audio::LocalAuditionService* local_output() const noexcept;
    // Null chooses no output: the queue stays, nothing plays.
    [[nodiscard]] core::Result<void> set_output(audio::Audition* output);
    [[nodiscard]] audio::Audition* current_output() const;
    // Whether a path is in the queue or among the asks: what the engine will
    // stream to an agent, and nothing else (ADR-0228).
    [[nodiscard]] bool holds(const std::string& raw_path) const;
    // Takes up the playing entry again on the current output: an agent that
    // dropped and came back. Without a position, where it was waiting to
    // resume -- a restore that failed because the output was not there yet.
    [[nodiscard]] core::Result<void> resume_output(std::optional<std::int64_t> position_ms,
                                                   std::optional<bool> playing);

    // Which sink plays and how much decoded audio is held ahead of it. The
    // engine's, not a client's: they describe the machine the engine runs on,
    // and a laptop's devices mean nothing to the engine on a NAS (ADR-0226).
    // Nullopt is the system default sink.
    [[nodiscard]] core::Result<void> set_output_target(std::optional<std::string> target);
    // Re-reads the device list; the monitor keeps it current otherwise.
    [[nodiscard]] core::Result<void> refresh_output_devices();
    // Applies from the next track, not mid-stream.
    [[nodiscard]] core::Result<void> set_buffer_config(audio::PlaybackBufferDurationConfig buffer);
    struct Output final {
        std::optional<std::string> target;
        audio::PlaybackBufferDurationConfig buffer;
    };
    [[nodiscard]] Output output() const;
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
        // How many explicit asks are outstanding. A client renders up-next
        // from playback.requests; this is enough to show that there are any.
        std::size_t requests{0};
        audio::PlaybackModes modes;
        int volume_percent{100};
        // What is armed to follow the current track without a gap, if
        // anything. Nil when the continuation was refused or none was offered
        // -- which is the difference between gapless working and the engine
        // merely starting the next track quickly, and it was invisible.
        core::StableId gapless_entry;
        // The entry consume most recently dropped from the queue, which a
        // client mirrors onto its own list. Reported rather than left for the
        // client to deduce: it would have to remember whether consume was
        // active during the track that just ended, and a one-shot has already
        // expired by then.
        core::StableId consumed;
        // Bumped whenever the queue, the modes or the asks change. A client
        // holding a view of the queue watches this to know when what it is
        // showing is out of date -- including when another client is what
        // changed it.
        std::uint64_t queue_revision{0};
        // Which playback this is, not which track: replaying the same file is
        // a new instance. A client that credits listening needs to tell those
        // apart, and a path cannot.
        std::uint64_t instance{0U};
        audio::ReplayGainMode replay_gain_mode{audio::ReplayGainMode::off};
        audio::ReplayGainPreamps replay_gain_preamps;
        // The chosen sink (nullopt: the default), what the default is now, and
        // whether the chosen one exists -- a USB DAC unplugged is chosen but
        // absent, and a client should say so rather than show it as playing.
        std::optional<std::string> output_target;
        std::optional<std::string> default_output;
        bool output_available{true};
        std::vector<audio::PipeWireDevice> devices;
        // Reconnecting after the sink went away and came back.
        bool output_suspended{false};
        audio::PlaybackBufferDurationConfig buffer;
        // A buffer change waits for the next track; this says one is waiting.
        bool buffer_pending{false};
        std::uint64_t underruns{0U};
    };
    [[nodiscard]] State state() const;

    // ADR-0220: the queue is the engine's, so the engine remembers it. This is
    // everything that has to survive a restart -- not the wire's State, which
    // is what a client renders and includes derived things like the status.
    struct Persisted final {
        std::vector<QueueEntry> queue;
        // Requested tracks that are not in the list.
        std::vector<QueueEntry> asks;
        core::StableId entry;
        core::StableId request_return;
        bool playing_request{false};
        std::vector<core::StableId> requests;
        audio::PlaybackModes modes;
        std::int64_t position_ms{0};
        // What the file looked like when the position was taken. Resuming
        // into a file that has changed underneath would seek to a position
        // that no longer means anything.
        std::optional<core::LocalSourceRevision> revision;
    };
    [[nodiscard]] Persisted persisted() const;

    // Comes back holding the queue, paused where it left off. Answers whether
    // the audio was restored: the queue and the modes are restored either way,
    // because a queue whose file has changed is still a queue.
    bool restore(Persisted state);

    // Bumped whenever something worth remembering changes -- the queue, the
    // modes, the asks, which entry is playing. A writer polls this rather than
    // serialising a queue it has already stored.
    [[nodiscard]] std::uint64_t revision() const;

    // A track that ended by itself is followed by the next one. Nothing else
    // does this: a gapless handover covers the case where the continuation was
    // accepted, and everything else -- a format change, a seek, a queue edit,
    // single mode expiring -- leaves the output simply stopped. The window
    // used to notice that and act; an engine that does not notice it stalls at
    // the end of every track.
    //
    // Answers whether it started something, which is only of interest to a
    // caller that wants to push an event immediately rather than at its next
    // sample.
    bool advance_if_ended();

    // Whether the audition service is actually holding a continuation, as
    // opposed to the engine believing it offered one. For tests: the two can
    // disagree, and that disagreement is exactly what silently disables
    // gapless.
    [[nodiscard]] bool armed_continuation() const;

  private:
    class QueueView;

    explicit Player(std::unique_ptr<audio::LocalAuditionService> local);
    // Loads the anchored entry on the current output at `position_ms`, and
    // plays it if asked.
    [[nodiscard]] core::Result<void>
    take_up_locked(std::int64_t position_ms, bool playing,
                   std::optional<core::LocalSourceRevision> revision);

    // Callers already hold the lock. `from_request` records where ordinary
    // list playback was interrupted, so playback returns there afterwards
    // instead of continuing from wherever the request happened to sit.
    [[nodiscard]] core::Result<void> start_locked(std::size_t row, bool from_request = false);
    // The same for any entry the engine holds, in the list or asked for from
    // outside it. Takes a copy: starting consumes, which moves rows.
    [[nodiscard]] core::Result<void> start_entry_locked(QueueEntry entry, bool from_request);
    // Plays the first request that still names something held; false when
    // none does, having dropped the ones that do not.
    [[nodiscard]] std::optional<core::Result<void>> start_next_request_locked();
    // An entry by identity, in the list or among the asks; null when neither.
    [[nodiscard]] const QueueEntry* find_locked(const core::StableId& entry_id) const;
    // Drops asks nothing refers to any more: not requested, not playing.
    void prune_asks_locked();
    // The two facts the advance rules need about the request queue.
    [[nodiscard]] audio::RequestQueueState request_state_locked() const;
    void reset_order_locked();
    // Albums shuffled as units. Answers false when the list cannot be grouped
    // within the budget, having turned the mode off.
    bool reset_album_order_locked();
    // Removes an entry the queue is done with, keeping the playing row
    // derived from its identity rather than from its old position.
    void consume_locked(const core::StableId& entry_id);
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
    // This machine's audio, if it has any; an output that refuses to play,
    // for when nothing is chosen; and whichever one is playing, which is
    // never null.
    std::unique_ptr<audio::LocalAuditionService> local_;
    std::unique_ptr<audio::Audition> silent_;
    audio::Audition* audition_{nullptr};
    // Where playback waits to resume when its output could not take it up.
    struct PendingResume final {
        std::int64_t position_ms{0};
        bool playing{false};
    };
    std::optional<PendingResume> pending_resume_;
    core::ListenAccounting listening_;
    std::vector<QueueEntry> queue_;
    audio::PlaybackAnchors anchors_;
    audio::PlaybackModes modes_;
    audio::PlaybackOrder order_;
    int row_{-1};
    // Identities rather than sources, so a request survives the queue being
    // reordered for the same reason playback does.
    std::vector<core::StableId> requests_;
    // Tracks asked for from outside the list -- up-next fed from the library.
    // Kept apart from the queue: they are not list entries, so they take no
    // part in its order, its consume or what a client shows as the list. They
    // once were appended to it, and a client mirroring the queue then showed
    // every asked-for track as a new row.
    std::vector<QueueEntry> asks_;
    // Whether the continuation armed for gapless is a request, so a handover
    // to it keeps the same books a start would.
    bool gapless_from_request_{false};
    // What was last offered for gapless continuation, so an unchanged
    // decision is not re-sent on every observation.
    std::optional<core::StableId> gapless_entry_;
    // Which entry an automatic advance was already made from. "Ended" persists
    // until the next load, so without this the same finished track would
    // advance on every tick and race through the queue.
    std::optional<core::StableId> advanced_from_;
    // Whether what is playing was an explicit ask rather than the list's own
    // order. The return point only applies while it is.
    bool playing_request_{false};
    core::StableId consumed_;
    std::uint64_t revision_{0};
    std::uint64_t seen_transitions_{0U};
};

} // namespace trackknife::engine

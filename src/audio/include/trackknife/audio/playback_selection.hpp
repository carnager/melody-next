// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/audio/playback_anchors.hpp"
#include "trackknife/audio/playback_modes.hpp"
#include "trackknife/audio/playback_order.hpp"
#include "trackknife/audio/track_source.hpp"
#include "trackknife/core/stable_id.hpp"

#include <optional>

namespace trackknife::audio {

// The part of a playing list the engine needs in order to choose what comes
// next. Deliberately narrow: four questions, no mutation, no notion of views,
// selection or presentation.
//
// ADR-0220 Phase 1 calls for a client-facing API so the UI stops being the
// owner of rows. This is the first slice of the opposite direction -- what the
// engine needs *from* a list -- and it is what lets the advance rules be
// exercised without constructing a window.
class PlaybackList {
  public:
    PlaybackList() = default;
    PlaybackList(const PlaybackList&) = delete;
    PlaybackList(PlaybackList&&) = delete;
    PlaybackList& operator=(const PlaybackList&) = delete;
    PlaybackList& operator=(PlaybackList&&) = delete;
    virtual ~PlaybackList() = default;

    [[nodiscard]] virtual int row_count() const = 0;
    // ADR-0221: an entry may have moved, or left the list entirely; -1 says so.
    // The hint makes the unmoved case cheap.
    [[nodiscard]] virtual int row_of_entry(const core::StableId& entry, int hint_row) const = 0;
    [[nodiscard]] virtual TrackSource source_at(int row) const = 0;
};

// Whether a request queue is in the way. The advance rules care about exactly
// these two facts, so they take them rather than the queue itself.
struct RequestQueueState final {
    bool active{false};
    bool pending_empty{true};

    friend bool operator==(const RequestQueueState&, const RequestQueueState&) = default;
};

struct PlaybackChoice final {
    int row{-1};
    TrackSource source;

    friend bool operator==(const PlaybackChoice&, const PlaybackChoice&) = default;
};

// The row `direction` away from the current one, or nothing when playback
// should stop. An active request queue with a recorded return point overrides
// the order: that is where ordinary list playback resumes.
//
// `order` is advanced-by-reference because PlaybackOrder::adjacent consumes a
// lazily drawn shuffle, which is state, not a query.
[[nodiscard]] std::optional<PlaybackChoice>
adjacent_playback_row(const PlaybackList& list, const PlaybackAnchors& anchors,
                      const PlaybackModes& modes, PlaybackOrder& order,
                      const RequestQueueState& requests, int direction, int current_row_hint);

// What to play when a track ends by itself, as opposed to being asked for.
// Single stops after the current track, unless Repeat turns it into repeating
// that track -- and Consume outranks that, because repeating a row the list is
// about to drop is incoherent.
[[nodiscard]] std::optional<PlaybackChoice>
automatic_playback_row(const PlaybackList& list, const PlaybackAnchors& anchors,
                       const PlaybackModes& modes, PlaybackOrder& order,
                       const RequestQueueState& requests, int current_row_hint);

} // namespace trackknife::audio

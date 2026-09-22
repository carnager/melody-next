// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/audio/playback_anchors.hpp"
#include "trackknife/audio/playback_modes.hpp"
#include "trackknife/audio/playback_order.hpp"
#include "bench/local_list_model.hpp"
#include "trackknife/audio/playback_selection.hpp"
#include "trackknife/audio/request_queue.hpp"

#include <optional>

namespace trackknife::bench {

// ADR-0220 Phase 0: everything needed to decide what plays next, in one object
// rather than scattered across the window that happens to display it. No
// widgets, no QObject, no QWidget parent -- BenchMainWindow holds one and
// delegates to it.
//
// This is the object the rest of Phase 0 migrates into: up-next, resume
// checkpointing and listen qualification have somewhere to go now, and MPRIS
// has something to attach to that is not the window. It is also what Phase 2
// serialises, which is easier to design against than five loose members.
//
// The fields are public because they are genuinely the state, not an
// implementation detail behind an invariant; what this class adds is the
// decisions below, which need all of them at once.
struct LocalPlaybackService final {
    // Where playback is, in a form that survives leaving this process.
    audio::PlaybackAnchors anchors;
    // Repeat, random, album-random, and the single/consume tri-states.
    audio::PlaybackModes modes;
    // The traversal, which advancing consumes -- state, not a query.
    audio::PlaybackOrder order;
    // A client-side projection of anchors.current, and the hint that makes
    // resolving it cheap. Deliberately not part of the anchors: rows move when
    // a list is reordered, which is the whole reason ADR-0221 exists.
    int row{-1};
    // Explicit asks, which outrank the playback order. The up-next panel that
    // displays and edits this still lives in the window, because it also shows
    // the MPD queue and so branches on authority; the queue itself does not.
    audio::RequestQueue<LocalTrackRow> requests;
    // What was last handed to the player for gapless continuation, so an
    // unchanged decision is not re-sent every tick.
    std::optional<audio::TrackSource> last_requested_next;

    // The two facts the advance rules need about the request queue. Derived
    // here now that the queue lives here, rather than assembled by the caller.
    [[nodiscard]] audio::RequestQueueState requestState() const {
        return {.active = requests.active().has_value(),
                .pending_empty = requests.pending().empty()};
    }

    // Resolve the playing entry to its current row in `list`, or -1 when the
    // entry is no longer there.
    [[nodiscard]] int resolveRow(const audio::PlaybackList& list) const {
        return list.row_of_entry(anchors.current, row);
    }

    // The row `direction` away, or nothing when playback should stop.
    [[nodiscard]] std::optional<audio::PlaybackChoice>
    adjacentRow(const audio::PlaybackList& list, const int direction) {
        return audio::adjacent_playback_row(list, anchors, modes, order, requestState(), direction,
                                            row);
    }

    // What to play when a track ends by itself rather than being asked for.
    [[nodiscard]] std::optional<audio::PlaybackChoice>
    automaticRow(const audio::PlaybackList& list) {
        return audio::automatic_playback_row(list, anchors, modes, order, requestState(), row);
    }

    // Playback moved to `entry` at `at_row`, decoding `source`.
    void adopt(const core::StableId& entry, const int at_row, audio::TrackSource source) {
        anchors.current = entry;
        anchors.source = std::move(source);
        row = at_row;
    }

    // Nothing is playing and nothing is in flight. The request return point
    // deliberately survives: it is only consulted while a request is active,
    // and is recomputed when the next one starts.
    void stop() {
        anchors.current = core::StableId{};
        anchors.forget_transition();
        anchors.document = core::StableId{};
        anchors.source = {};
        row = -1;
    }
};

} // namespace trackknife::bench

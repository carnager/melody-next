// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/audio/playback_selection.hpp"

namespace trackknife::audio {

std::optional<PlaybackChoice>
adjacent_playback_row(const PlaybackList& list, const PlaybackAnchors& anchors,
                      const PlaybackModes& modes, PlaybackOrder& order,
                      const RequestQueueState& requests, const int direction,
                      const int current_row_hint) {
    if (anchors.source.empty()) {
        return std::nullopt;
    }
    // Going forward out of a request, the recorded return point wins over the
    // order: it is where ordinary list playback was interrupted. Going back
    // does not use it, and neither does a return point that has since left the
    // list.
    if (direction > 0 && requests.active && !anchors.request_return.is_nil()) {
        if (const auto row = list.row_of_entry(anchors.request_return, -1); row >= 0) {
            return PlaybackChoice{.row = row, .source = list.source_at(row)};
        }
    }
    const auto playing_row = list.row_of_entry(anchors.current, current_row_hint);
    if (playing_row < 0) {
        return std::nullopt;
    }
    const auto adjacent = order.adjacent(direction, modes.repeat);
    // Consume removes the row that just played, so landing back on it would
    // mean playing something the list is about to drop.
    if (!adjacent || (modes.consume_active() && *adjacent == playing_row)) {
        return std::nullopt;
    }
    return PlaybackChoice{.row = *adjacent, .source = list.source_at(*adjacent)};
}

std::optional<PlaybackChoice>
automatic_playback_row(const PlaybackList& list, const PlaybackAnchors& anchors,
                       const PlaybackModes& modes, PlaybackOrder& order,
                       const RequestQueueState& requests, const int current_row_hint) {
    if (modes.single_active()) {
        // Single alone stops. Single with Repeat repeats this track -- but only
        // when nothing else is waiting: a pending or active request is a
        // explicit ask, and Consume is about to remove this row.
        if (modes.repeat && !modes.consume_active() && requests.pending_empty && !requests.active) {
            // An identity can outlive its row, so the lookup is checked.
            if (const auto row = list.row_of_entry(anchors.current, current_row_hint); row >= 0) {
                return PlaybackChoice{.row = row, .source = anchors.source};
            }
        }
        return std::nullopt;
    }
    return adjacent_playback_row(list, anchors, modes, order, requests, 1, current_row_hint);
}

} // namespace trackknife::audio

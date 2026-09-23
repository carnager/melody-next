// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "bench/local_list_model.hpp"
#include "trackknife/audio/playback_anchors.hpp"
#include "trackknife/audio/playback_modes.hpp"
#include "trackknife/audio/playback_selection.hpp"
#include "trackknife/audio/request_queue.hpp"

#include <optional>

namespace trackknife::bench {

// Where this window is in the engine's playback, and the Up Next it edits.
// ADR-0226: the engine decides what plays next -- order, shuffle, gapless,
// consume -- so this holds only what the window renders and sends: the
// anchors naming the playing entry, the modes it shows and forwards, and the
// explicit asks the Up Next panel edits before stating them to the engine.
struct LocalPlaybackService final {
    // Where playback is, in a form that survives leaving this process.
    audio::PlaybackAnchors anchors;
    // Repeat, random, album-random, and the single/consume tri-states: the
    // engine's, mirrored for the buttons.
    audio::PlaybackModes modes;
    // A client-side projection of anchors.current, and the hint that makes
    // resolving it cheap. Deliberately not part of the anchors: rows move when
    // a list is reordered, which is the whole reason ADR-0221 exists.
    int row{-1};
    // Explicit asks. The up-next panel that displays and edits this lives in
    // the window; the engine is told the result.
    audio::RequestQueue<LocalTrackRow> requests;

    // Resolve the playing entry to its current row in `list`, or -1 when the
    // entry is no longer there.
    [[nodiscard]] int resolveRow(const audio::PlaybackList& list) const {
        return list.row_of_entry(anchors.current, row);
    }
};

} // namespace trackknife::bench

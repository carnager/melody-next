// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/audio/track_source.hpp"
#include "trackknife/core/stable_id.hpp"

namespace trackknife::audio {

// Where playback is, named so that the answer survives leaving this process.
//
// ADR-0220 Phase 0 / ADR-0221: every field here is an identity or a decode
// target, never a row and never a pointer into a view model. A row is a
// client-side projection that changes when a list is reordered; these do not.
// This is the state Phase 2 puts on the wire, so it deliberately holds nothing
// a remote client could not reconstruct.
struct PlaybackAnchors final {
    // Which list is playing. Nil means nothing is.
    core::StableId document;
    // The entry currently playing within that list.
    core::StableId current;
    // A transition in flight. `queued` is what the engine was handed for
    // gapless continuation; `requested` is what a request queue asked for.
    // They describe the same in-flight step from two directions and are always
    // abandoned together -- see forget_transition below.
    core::StableId queued;
    core::StableId requested;
    // Where ordinary list playback resumes once the request queue drains.
    core::StableId request_return;
    // What is actually being decoded: path, selection within it, and span.
    TrackSource source;

    [[nodiscard]] bool playing() const noexcept { return !current.is_nil() && !document.is_nil(); }

    // A transition is abandoned as a unit. Keeping the pair together here is
    // what stops one half being cleared without the other.
    constexpr void forget_transition() noexcept {
        queued = core::StableId{};
        requested = core::StableId{};
    }

    // Playback stopped entirely: nothing is playing, nothing is in flight, and
    // there is nowhere to return to.
    void clear() noexcept { *this = PlaybackAnchors{}; }

    friend bool operator==(const PlaybackAnchors&, const PlaybackAnchors&) = default;
};

} // namespace trackknife::audio

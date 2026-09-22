// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/audio/local_audition.hpp"

#include <string>

namespace trackknife::audio {

// What core::ListenAccounting needs to know about a moment of playback,
// projected out of a snapshot.
struct ListenObservation final {
    // Empty when this snapshot cannot be credited to anything. A change of
    // identity is what restarts accounting, so it must be stable for exactly
    // as long as one playthrough of one track.
    std::string identity;
    double duration_seconds{0};
    double position_seconds{0};
    bool playing{false};

    [[nodiscard]] bool qualified() const noexcept {
        return !identity.empty();
    }

    friend bool operator==(const ListenObservation&, const ListenObservation&) = default;
};

// ADR-0220 Phase 0: the rules deciding whether a moment counts as listening,
// separated from the persistence and status reporting around them.
//
// Two are worth stating because they are easy to lose. A snapshot is only
// creditable when it names a revisioned source *and* carries a playback
// instance and a real sample rate -- the instance is what distinguishes
// replaying the same file from continuing it, so without one there is nothing
// to accumulate against. And time only counts while the output is actually
// carrying audio: a suspended or unavailable device means the user is not
// hearing anything, whatever the decoder thinks it is doing.
[[nodiscard]] inline ListenObservation listen_observation(const LocalAuditionSnapshot& snapshot) {
    const bool creditable = !snapshot.raw_path.empty() && snapshot.source_revision.has_value() &&
                            snapshot.playback_instance != 0 && snapshot.format.has_value() &&
                            snapshot.format->sample_rate > 0;
    if (!creditable) {
        return {};
    }
    const auto rate = static_cast<double>(snapshot.format->sample_rate);
    return {
        .identity = std::to_string(snapshot.playback_instance),
        .duration_seconds =
            snapshot.end_sample ? static_cast<double>(*snapshot.end_sample) / rate : 0,
        .position_seconds = static_cast<double>(snapshot.position_sample) / rate,
        .playing = (snapshot.state == LocalAuditionState::playing ||
                    snapshot.state == LocalAuditionState::draining) &&
                   snapshot.output_target_available && !snapshot.output_suspended,
    };
}

} // namespace trackknife::audio

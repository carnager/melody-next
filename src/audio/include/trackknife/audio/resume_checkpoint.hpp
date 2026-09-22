// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/audio/local_audition.hpp"

#include <cstdint>

namespace trackknife::audio {

// Whether a snapshot describes playback worth remembering a position in.
//
// Loading and ended states have no meaningful offset; a source without a
// revision cannot be revalidated on restore, so resuming into it would risk
// seeking into a file that changed underneath. A position at or past the end
// of a segment is a finished track, not a place to come back to.
[[nodiscard]] inline bool resumable(const LocalAuditionSnapshot& snapshot) noexcept {
    const bool live = snapshot.state == LocalAuditionState::paused ||
                      snapshot.state == LocalAuditionState::playing ||
                      snapshot.state == LocalAuditionState::buffering ||
                      snapshot.state == LocalAuditionState::draining;
    return live && snapshot.source_revision.has_value() && snapshot.format.has_value() &&
           snapshot.format->sample_rate > 0 && snapshot.position_sample >= 0 &&
           (!snapshot.end_sample || snapshot.position_sample < *snapshot.end_sample);
}

// The snapshot's offset in milliseconds, truncated toward zero.
//
// Equal to position_sample * 1000 / sample_rate, computed as whole seconds
// plus the remainder so the large intermediate product is never formed. The
// caller must have established sample_rate > 0 -- `resumable` above does.
[[nodiscard]] inline std::int64_t resume_position_ms(const LocalAuditionSnapshot& snapshot) {
    const auto rate = static_cast<std::int64_t>(snapshot.format->sample_rate);
    return snapshot.position_sample / rate * 1000 + snapshot.position_sample % rate * 1000 / rate;
}

} // namespace trackknife::audio

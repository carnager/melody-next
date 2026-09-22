// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/audio/local_audition.hpp"

#include <cstdint>
#include <optional>

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

// A resume position is rewritten continuously while a track plays, so it is
// rate limited rather than saved on every tick.
inline constexpr std::int64_t minimum_resume_interval_ms = 5'000;

// Everything outside the snapshot that bears on whether to write now.
struct ResumeWriteConditions final {
    // Set when the caller needs this write to happen regardless of the rate
    // limit -- shutdown, or the user turning resume on.
    bool forced{false};
    // Whether resume is switched on at all.
    bool enabled{false};
    // A previous write has not completed; a second would race it.
    bool write_in_flight{false};
    // Absent when nothing has been written yet this session.
    std::optional<std::int64_t> since_last_write_ms;
};

// Whether to write a resume checkpoint for this snapshot now.
//
// Note what `forced` does and does not bypass: it skips the enabled check, the
// in-flight guard and the rate limit, but a loading snapshot is still refused,
// because it has no meaningful offset to record yet. Forcing overrides policy,
// not arithmetic.
[[nodiscard]] inline bool should_write_resume(const LocalAuditionSnapshot& snapshot,
                                              const ResumeWriteConditions& conditions) noexcept {
    if (snapshot.state == LocalAuditionState::loading) {
        return false;
    }
    if (conditions.forced) {
        return true;
    }
    if (!conditions.enabled || conditions.write_in_flight) {
        return false;
    }
    return !conditions.since_last_write_ms ||
           *conditions.since_last_write_ms >= minimum_resume_interval_ms;
}

} // namespace trackknife::audio

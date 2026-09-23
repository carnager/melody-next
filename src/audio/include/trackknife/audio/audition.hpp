// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/audio/local_playback.hpp"
#include "trackknife/core/local_sources.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/formats/decoder.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace trackknife::audio {

// Defined with the local service, which is where its fields came from; an
// agent's audition reports the same shape.
struct LocalAuditionSnapshot;

// ADR-0228: what the engine's player plays on. The player decides what plays
// -- queue, order, gapless, consume, requests -- and an audition carries it
// out. This machine's audio service is one; an output agent reached over the
// network is another, and the player cannot tell them apart.
//
// Paths are the engine's own. Translating one into what an agent can open is
// the agent audition's business, not the player's.
class Audition {
  public:
    Audition() = default;
    Audition(const Audition&) = delete;
    Audition& operator=(const Audition&) = delete;
    Audition(Audition&&) = delete;
    Audition& operator=(Audition&&) = delete;
    virtual ~Audition() = default;

    [[nodiscard]] virtual LocalAuditionSnapshot snapshot() const = 0;

    [[nodiscard]] virtual core::Result<void>
    load_selected_and_play(std::string raw_path, formats::AudioSourceSelection selection,
                           std::optional<formats::ReplayGainInfo> replay_gain_override) = 0;
    [[nodiscard]] virtual core::Result<void>
    load_selected_segment_and_play(std::string raw_path, formats::AudioSourceSelection selection,
                                   formats::SampleRange segment,
                                   std::optional<formats::ReplayGainInfo> replay_gain_override) = 0;
    // Opens and seeks without making a sound: coming back from a restart, or
    // moving to another output, is not a request to start playing.
    [[nodiscard]] virtual core::Result<void>
    restore_paused(std::string raw_path, core::LocalSourceRevision expected_revision,
                   formats::AudioSourceSelection selection,
                   std::optional<formats::SampleRange> segment, std::int64_t position_ms,
                   std::optional<formats::ReplayGainInfo> replay_gain_override) = 0;
    [[nodiscard]] virtual core::Result<void>
    queue_gapless_next_selected(std::string raw_path, formats::AudioSourceSelection selection,
                                std::optional<formats::ReplayGainInfo> replay_gain_override,
                                std::uint64_t occurrence_token) = 0;
    [[nodiscard]] virtual core::Result<void> queue_gapless_next_selected_segment(
        std::string raw_path, formats::AudioSourceSelection selection, formats::SampleRange segment,
        std::optional<formats::ReplayGainInfo> replay_gain_override,
        std::uint64_t occurrence_token) = 0;
    [[nodiscard]] virtual core::Result<void> clear_gapless_next() = 0;
    [[nodiscard]] virtual core::Result<void> play() = 0;
    [[nodiscard]] virtual core::Result<void> pause() = 0;
    [[nodiscard]] virtual core::Result<void> stop() = 0;
    [[nodiscard]] virtual core::Result<void> seek_to_seconds(double target_seconds) = 0;
    [[nodiscard]] virtual core::Result<void> set_volume_percent(int percent) = 0;
    [[nodiscard]] virtual core::Result<void> set_replay_gain_mode(ReplayGainMode mode) = 0;
    [[nodiscard]] virtual core::Result<void> set_replay_gain_preamps(ReplayGainPreamps preamps) = 0;
    [[nodiscard]] virtual core::Result<void>
    set_buffer_config(PlaybackBufferDurationConfig buffer_config) = 0;
    [[nodiscard]] virtual core::Result<void> refresh_output_devices() = 0;
    [[nodiscard]] virtual core::Result<void>
    set_output_target(std::optional<std::string> target) = 0;
};

} // namespace trackknife::audio

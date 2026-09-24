// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// ADR-0228: the player plays on whatever output it is given. A fake one
// records what it is asked, which is all an output agent is from the
// player's side: the decisions stay in the player. Shared by the tests that
// need an engine whose output does as they say -- fails, say.

#include "trackknife/audio/audition.hpp"
#include "trackknife/audio/local_audition.hpp"
#include "trackknife/core/error.hpp"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace trackknife::testing {

namespace audio = trackknife::audio;
namespace core = trackknife::core;

class RecordingAudition final : public audio::Audition {
  public:
    [[nodiscard]] audio::LocalAuditionSnapshot snapshot() const override {
        const std::lock_guard guard{mutex_};
        audio::LocalAuditionSnapshot current;
        current.state = state_;
        current.raw_path = loaded_;
        // Milliseconds as samples at 1 kHz: exact, and what the player's
        // arithmetic expects.
        current.format = trackknife::formats::PcmFormat{1000, 2, "stereo"};
        current.position_sample = position_ms_;
        current.replay_gain_mode = gain_mode_;
        current.output_target = target_;
        current.playback_instance = instance_;
        current.error = error_;
        return current;
    }
    [[nodiscard]] core::Result<void>
    load_selected_and_play(std::string raw_path, trackknife::formats::AudioSourceSelection,
                           std::optional<trackknife::formats::ReplayGainInfo>) override {
        const std::lock_guard guard{mutex_};
        if (deferred_) {
            pending_ = std::move(raw_path);
            return {};
        }
        load_locked(std::move(raw_path));
        return {};
    }
    [[nodiscard]] core::Result<void>
    load_selected_segment_and_play(std::string raw_path, trackknife::formats::AudioSourceSelection,
                                   trackknife::formats::SampleRange,
                                   std::optional<trackknife::formats::ReplayGainInfo>) override {
        return load_selected_and_play(std::move(raw_path), {}, {});
    }
    [[nodiscard]] core::Result<void>
    restore_paused(std::string raw_path, core::LocalSourceRevision,
                   trackknife::formats::AudioSourceSelection,
                   std::optional<trackknife::formats::SampleRange>, const std::int64_t position_ms,
                   std::optional<trackknife::formats::ReplayGainInfo>) override {
        const std::lock_guard guard{mutex_};
        loaded_ = std::move(raw_path);
        position_ms_ = position_ms;
        state_ = audio::LocalAuditionState::paused;
        return {};
    }
    [[nodiscard]] core::Result<void>
    queue_gapless_next_selected(std::string, trackknife::formats::AudioSourceSelection,
                                std::optional<trackknife::formats::ReplayGainInfo>,
                                std::uint64_t) override {
        return {};
    }
    [[nodiscard]] core::Result<void> queue_gapless_next_selected_segment(
        std::string, trackknife::formats::AudioSourceSelection, trackknife::formats::SampleRange,
        std::optional<trackknife::formats::ReplayGainInfo>, std::uint64_t) override {
        return {};
    }
    [[nodiscard]] core::Result<void> clear_gapless_next() override { return {}; }
    [[nodiscard]] core::Result<void> play() override {
        const std::lock_guard guard{mutex_};
        state_ = audio::LocalAuditionState::playing;
        return {};
    }
    [[nodiscard]] core::Result<void> pause() override {
        const std::lock_guard guard{mutex_};
        state_ = audio::LocalAuditionState::paused;
        return {};
    }
    [[nodiscard]] core::Result<void> stop() override {
        const std::lock_guard guard{mutex_};
        state_ = audio::LocalAuditionState::empty;
        loaded_.clear();
        return {};
    }
    [[nodiscard]] core::Result<void> seek_to_seconds(double) override { return {}; }
    [[nodiscard]] core::Result<void> set_volume_percent(int) override { return {}; }
    [[nodiscard]] core::Result<void>
    set_replay_gain_mode(const audio::ReplayGainMode mode) override {
        const std::lock_guard guard{mutex_};
        gain_mode_ = mode;
        return {};
    }
    [[nodiscard]] core::Result<void> set_replay_gain_preamps(audio::ReplayGainPreamps) override {
        return {};
    }
    [[nodiscard]] core::Result<void>
    set_buffer_config(audio::PlaybackBufferDurationConfig) override {
        return {};
    }
    [[nodiscard]] core::Result<void> refresh_output_devices() override { return {}; }
    [[nodiscard]] core::Result<void>
    set_output_target(std::optional<std::string> target) override {
        const std::lock_guard guard{mutex_};
        target_ = std::move(target);
        return {};
    }

    void advance_to(const std::int64_t position_ms) {
        const std::lock_guard guard{mutex_};
        position_ms_ = position_ms;
    }

    // As a real output does: loads are taken up later, on its own thread,
    // and may fail -- no speakers, say.
    void defer_loads(const bool deferred) {
        const std::lock_guard guard{mutex_};
        deferred_ = deferred;
    }
    void fail_loads(std::optional<std::string> reason) {
        const std::lock_guard guard{mutex_};
        failing_ = std::move(reason);
    }
    void take_up() {
        const std::lock_guard guard{mutex_};
        if (pending_) {
            load_locked(*std::exchange(pending_, std::nullopt));
        }
    }

  private:
    void load_locked(std::string raw_path) {
        ++instance_;
        position_ms_ = 0;
        if (failing_) {
            loaded_.clear();
            error_ = core::Error{
                .code = core::ErrorCode::backend, .message = *failing_, .context = {}};
            state_ = audio::LocalAuditionState::failed;
            return;
        }
        loaded_ = std::move(raw_path);
        error_.reset();
        state_ = audio::LocalAuditionState::playing;
    }

    mutable std::mutex mutex_;
    bool deferred_{false};
    std::optional<std::string> pending_;
    std::optional<std::string> failing_;
    std::optional<core::Error> error_;
    std::uint64_t instance_{0U};
    audio::LocalAuditionState state_{audio::LocalAuditionState::empty};
    std::string loaded_;
    std::int64_t position_ms_{0};
    audio::ReplayGainMode gain_mode_{audio::ReplayGainMode::off};
    std::optional<std::string> target_;
};

} // namespace trackknife::testing

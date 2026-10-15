// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// An output with no device: the source is rendered on a thread of its own,
// PipeWireOutputConfig::offline_speed times as fast as it would play. What a
// test needs to play minutes of audio in seconds, with no audio server.

#include "trackknife/audio/pipewire_output.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace trackknife::audio {

class OfflineOutput final {
  public:
    [[nodiscard]] static core::Result<std::unique_ptr<OfflineOutput>>
    connect(LocalPlayback& source, const PipeWireOutputConfig& config);
    OfflineOutput(LocalPlayback& source, double speed, int sample_rate, int channels);
    OfflineOutput(const OfflineOutput&) = delete;
    OfflineOutput& operator=(const OfflineOutput&) = delete;
    ~OfflineOutput();

    [[nodiscard]] PipeWireOutputSnapshot snapshot() const;
    [[nodiscard]] core::Result<void> activate();
    [[nodiscard]] core::Result<void> set_volume(double volume);
    [[nodiscard]] core::Result<void> quiesce();
    // Nothing is queued beyond what was rendered, so there is nothing to wait
    // for.
    [[nodiscard]] core::Result<void> drain();

  private:
    void run(const std::stop_token& stop);

    LocalPlayback* source_;
    std::size_t channels_;
    std::size_t frames_per_tick_;
    std::vector<float> samples_;
    // Rendering happens under the mutex, so quiesce() returning means no
    // render is in flight, as the device outputs promise.
    mutable std::mutex mutex_;
    std::condition_variable_any woken_;
    bool active_{false};
    double volume_{1.0};
    std::atomic<std::uint64_t> callback_count_{0U};
    std::atomic<std::uint64_t> device_frames_{0U};
    std::atomic<std::uint64_t> source_frames_{0U};
    std::jthread worker_;
};

} // namespace trackknife::audio

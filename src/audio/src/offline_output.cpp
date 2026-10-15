// SPDX-License-Identifier: GPL-3.0-only
#include "offline_output.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <span>
#include <utility>

namespace trackknife::audio {

namespace {

// A device asks for audio every few milliseconds; so does this, only for
// `speed` times as much each time.
constexpr auto tick = std::chrono::milliseconds{5};

[[nodiscard]] core::Error invalid(std::string message) {
    return core::Error{
        .code = core::ErrorCode::invalid_argument, .message = std::move(message), .context = {}};
}

} // namespace

core::Result<std::unique_ptr<OfflineOutput>>
OfflineOutput::connect(LocalPlayback& source, const PipeWireOutputConfig& config) {
    if (!std::isfinite(config.offline_speed) || config.offline_speed <= 0.0) {
        return std::unexpected(invalid("an offline output needs a positive speed"));
    }
    const auto& format = source.output_format();
    if (format.sample_rate <= 0 || format.channels <= 0) {
        return std::unexpected(invalid("an offline output needs positive-rate source PCM"));
    }
    return std::make_unique<OfflineOutput>(source, config.offline_speed, format.sample_rate,
                                           format.channels);
}

OfflineOutput::OfflineOutput(LocalPlayback& source, const double speed, const int sample_rate,
                             const int channels)
    : source_(&source), channels_(static_cast<std::size_t>(channels)),
      frames_per_tick_(std::max<std::size_t>(
          1U, static_cast<std::size_t>(static_cast<double>(sample_rate) * speed *
                                       std::chrono::duration<double>(tick).count()))),
      samples_(frames_per_tick_ * channels_),
      worker_([this](const std::stop_token& stop) { run(stop); }) {}

OfflineOutput::~OfflineOutput() {
    worker_.request_stop();
    woken_.notify_all();
}

void OfflineOutput::run(const std::stop_token& stop) {
    std::unique_lock lock{mutex_};
    while (!stop.stop_requested()) {
        if (!woken_.wait(lock, stop, [this] { return active_; })) {
            return;
        }
        callback_count_.fetch_add(1U, std::memory_order_relaxed);
        const auto copied = source_->render(std::span<float>{samples_});
        device_frames_.fetch_add(frames_per_tick_, std::memory_order_relaxed);
        source_frames_.fetch_add(copied, std::memory_order_relaxed);
        static_cast<void>(woken_.wait_for(lock, stop, tick, [] { return false; }));
    }
}

PipeWireOutputSnapshot OfflineOutput::snapshot() const {
    const std::lock_guard guard{mutex_};
    return PipeWireOutputSnapshot{.state = active_ ? PipeWireOutputState::streaming
                                                   : PipeWireOutputState::paused,
                                  .node_id = std::nullopt,
                                  .callback_count = callback_count_.load(std::memory_order_relaxed),
                                  .device_frames = device_frames_.load(std::memory_order_relaxed),
                                  .source_frames = source_frames_.load(std::memory_order_relaxed),
                                  .invalid_buffer_count = 0U,
                                  .volume = volume_,
                                  .server_volume = volume_,
                                  .error_message = {}};
}

core::Result<void> OfflineOutput::activate() {
    {
        const std::lock_guard guard{mutex_};
        active_ = true;
    }
    woken_.notify_all();
    return {};
}

core::Result<void> OfflineOutput::set_volume(const double volume) {
    if (!std::isfinite(volume) || volume < 0.0 || volume > 1.0) {
        return std::unexpected(invalid("volume must be between 0 and 1"));
    }
    const std::lock_guard guard{mutex_};
    volume_ = volume;
    return {};
}

core::Result<void> OfflineOutput::quiesce() {
    const std::lock_guard guard{mutex_};
    active_ = false;
    return {};
}

core::Result<void> OfflineOutput::drain() { return {}; }

} // namespace trackknife::audio

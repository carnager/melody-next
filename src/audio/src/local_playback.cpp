// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/audio/local_playback.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace trackknife::audio {
namespace {

static_assert(std::atomic_size_t::is_always_lock_free);
static_assert(std::atomic<std::int64_t>::is_always_lock_free);
static_assert(std::atomic_uint64_t::is_always_lock_free);
static_assert(std::atomic<LocalPlaybackState>::is_always_lock_free);

struct alignas(64) CacheAlignedFrameIndex {
    std::atomic_size_t value{0U};
    std::array<std::byte, 64U - sizeof(std::atomic_size_t)> explicit_padding{};
};

class PcmRingBuffer final {
  public:
    PcmRingBuffer(const std::size_t capacity_frames, const std::size_t channels)
        : capacity_frames_(capacity_frames), channels_(channels),
          samples_(capacity_frames * channels),
          read_frame_(std::make_unique<CacheAlignedFrameIndex>()),
          write_frame_(std::make_unique<CacheAlignedFrameIndex>()) {}

    [[nodiscard]] std::size_t capacity_frames() const noexcept { return capacity_frames_; }

    [[nodiscard]] std::size_t size_frames() const noexcept {
        const auto write = write_frame_->value.load(std::memory_order_acquire);
        const auto read = read_frame_->value.load(std::memory_order_acquire);
        return write - read;
    }

    [[nodiscard]] std::size_t write(std::span<const float> input) noexcept {
        const auto requested_frames = input.size() / channels_;
        const auto write_frame = write_frame_->value.load(std::memory_order_relaxed);
        const auto read_frame = read_frame_->value.load(std::memory_order_acquire);
        const auto writable_frames = capacity_frames_ - (write_frame - read_frame);
        const auto frames = std::min(requested_frames, writable_frames);
        const auto first_frames =
            std::min(frames, capacity_frames_ - (write_frame % capacity_frames_));
        const auto first_samples = first_frames * channels_;
        const auto start_sample = (write_frame % capacity_frames_) * channels_;
        std::copy_n(input.begin(), static_cast<std::ptrdiff_t>(first_samples),
                    samples_.begin() + static_cast<std::ptrdiff_t>(start_sample));
        const auto remaining_samples = (frames - first_frames) * channels_;
        std::copy_n(input.begin() + static_cast<std::ptrdiff_t>(first_samples),
                    static_cast<std::ptrdiff_t>(remaining_samples), samples_.begin());
        write_frame_->value.store(write_frame + frames, std::memory_order_release);
        return frames;
    }

    [[nodiscard]] std::size_t read(std::span<float> output) noexcept {
        const auto requested_frames = output.size() / channels_;
        const auto read_frame = read_frame_->value.load(std::memory_order_relaxed);
        const auto write_frame = write_frame_->value.load(std::memory_order_acquire);
        const auto frames = std::min(requested_frames, write_frame - read_frame);
        const auto first_frames =
            std::min(frames, capacity_frames_ - (read_frame % capacity_frames_));
        const auto first_samples = first_frames * channels_;
        const auto start_sample = (read_frame % capacity_frames_) * channels_;
        std::copy_n(samples_.begin() + static_cast<std::ptrdiff_t>(start_sample),
                    static_cast<std::ptrdiff_t>(first_samples), output.begin());
        const auto remaining_samples = (frames - first_frames) * channels_;
        std::copy_n(samples_.begin(), static_cast<std::ptrdiff_t>(remaining_samples),
                    output.begin() + static_cast<std::ptrdiff_t>(first_samples));
        read_frame_->value.store(read_frame + frames, std::memory_order_release);
        return frames;
    }

    // Producer and consumer must be quiesced before reset.
    void reset() noexcept {
        read_frame_->value.store(0U, std::memory_order_relaxed);
        write_frame_->value.store(0U, std::memory_order_relaxed);
    }

  private:
    std::size_t capacity_frames_{0U};
    std::size_t channels_{0U};
    std::vector<float> samples_;
    std::unique_ptr<CacheAlignedFrameIndex> read_frame_;
    std::unique_ptr<CacheAlignedFrameIndex> write_frame_;
};

[[nodiscard]] core::Error invalid_buffer_config(const PlaybackBufferConfig config) {
    return core::Error{
        .code = core::ErrorCode::invalid_argument,
        .message = "local playback buffer capacity and start threshold must be positive, with the "
                   "threshold no larger than capacity",
        .context = {{.key = "capacity_frames", .value = std::to_string(config.capacity_frames)},
                    {.key = "start_threshold_frames",
                     .value = std::to_string(config.start_threshold_frames)}},
    };
}

[[nodiscard]] core::Error
invalid_buffer_duration_config(const PlaybackBufferDurationConfig config) {
    return core::Error{
        .code = core::ErrorCode::invalid_argument,
        .message = "local playback buffer duration and start threshold must be positive, with "
                   "the threshold no larger than the duration",
        .context = {{.key = "capacity_ms", .value = std::to_string(config.capacity.count())},
                    {.key = "start_threshold_ms",
                     .value = std::to_string(config.start_threshold.count())}},
    };
}

[[nodiscard]] core::Result<PlaybackBufferConfig>
frame_buffer_config(const PlaybackBufferDurationConfig config, const int sample_rate) {
    if (config.capacity <= std::chrono::milliseconds::zero() ||
        config.start_threshold <= std::chrono::milliseconds::zero() ||
        config.start_threshold > config.capacity) {
        return std::unexpected(invalid_buffer_duration_config(config));
    }
    if (sample_rate <= 0) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::invariant,
            .message = "local decoder reported a non-positive sample rate",
            .context = {{.key = "sample_rate", .value = std::to_string(sample_rate)}},
        });
    }
    const auto frames_for =
        [sample_rate](const std::chrono::milliseconds duration) -> core::Result<std::size_t> {
        const auto rate = static_cast<std::uint64_t>(sample_rate);
        const auto milliseconds = static_cast<std::uint64_t>(duration.count());
        if (milliseconds > (std::numeric_limits<std::uint64_t>::max() - 999U) / rate) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::limit_exceeded,
                .message = "local playback buffer duration exceeds addressable frames",
                .context = {{.key = "duration_ms", .value = std::to_string(duration.count())},
                            {.key = "sample_rate", .value = std::to_string(sample_rate)}},
            });
        }
        const auto frames = (rate * milliseconds + 999U) / 1'000U;
        if (frames > std::numeric_limits<std::size_t>::max()) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::limit_exceeded,
                .message = "local playback buffer duration exceeds addressable memory",
                .context = {{.key = "duration_ms", .value = std::to_string(duration.count())},
                            {.key = "sample_rate", .value = std::to_string(sample_rate)}},
            });
        }
        return static_cast<std::size_t>(std::max<std::uint64_t>(1U, frames));
    };
    auto capacity = frames_for(config.capacity);
    if (!capacity) {
        return std::unexpected(std::move(capacity.error()));
    }
    auto threshold = frames_for(config.start_threshold);
    if (!threshold) {
        return std::unexpected(std::move(threshold.error()));
    }
    return PlaybackBufferConfig{.capacity_frames = *capacity, .start_threshold_frames = *threshold};
}

} // namespace

float replay_gain_multiplier(const formats::ReplayGainInfo& info, const ReplayGainMode mode,
                             const ReplayGainPreamps& preamps) noexcept {
    if (mode == ReplayGainMode::off) {
        return 1.0F;
    }
    const bool album = mode == ReplayGainMode::album && info.album_gain_db.has_value();
    const auto gain = album ? info.album_gain_db : info.track_gain_db;
    const auto peak = album ? info.album_peak : info.track_peak;
    if (!gain || !std::isfinite(*gain) || *gain < -60.0 || *gain > 60.0) {
        // No usable loudness data: the without-data preamp levels such
        // sources against normalized material (ADR-0138).
        return static_cast<float>(
            std::pow(10.0, static_cast<double>(preamps.without_gain_db) / 20.0));
    }
    auto multiplier = std::pow(10.0, (*gain + static_cast<double>(preamps.with_gain_db)) / 20.0);
    if (peak && std::isfinite(*peak) && *peak > 0.0) {
        multiplier = std::min(multiplier, 1.0 / *peak);
    }
    return static_cast<float>(multiplier);
}

struct LocalPlayback::Impl {
    ReplayGainMode replay_gain_mode{ReplayGainMode::off};
    ReplayGainPreamps replay_gain_preamps{};
    PcmRingBuffer ring;
    formats::AudioDecoder decoder;
    formats::ReplayGainInfo replay_gain;
    std::size_t pending_frame_offset{0U};
    std::int64_t next_decode_sample{0};
    std::atomic<std::int64_t> position_sample{0};
    std::atomic_uint64_t underrun_count{0U};
    std::atomic<float> decoded_peak_before_gain{0.0F};
    std::atomic<float> decoded_peak_after_gain{0.0F};
    PlaybackBufferConfig config;
    formats::SampleRange range;
    std::vector<float> pending_samples;
    formats::PcmFormat output;
    std::atomic<LocalPlaybackState> state{LocalPlaybackState::stopped};
    std::atomic_bool source_ended{false};

    // Gapless continuation: the queued decoder takes over in the same ring at
    // decode end. next_decode_sample and position_sample then live in the
    // produced domain (they keep increasing past the boundary), and
    // chain_offset maps produced samples onto the active decoder's own
    // sample domain.
    std::optional<formats::AudioDecoder> next_decoder;
    std::optional<formats::ReplayGainInfo> next_replay_gain_override;
    std::int64_t chain_offset{0};
    std::atomic<std::int64_t> chain_boundary{-1};
    std::atomic_bool chain_crossed{false};

    Impl(formats::AudioDecoder source_decoder, const PlaybackBufferConfig buffer_config,
         const std::optional<formats::ReplayGainInfo>& replay_gain_override = {})
        : ring(buffer_config.capacity_frames,
               static_cast<std::size_t>(source_decoder.output_format().channels)),
          decoder(std::move(source_decoder)),
          replay_gain(replay_gain_override ? *replay_gain_override : decoder.replay_gain()),
          next_decode_sample(decoder.sample_range().start_sample),
          position_sample(decoder.sample_range().start_sample), config(buffer_config),
          range(decoder.sample_range()), output(decoder.output_format()) {}

    [[nodiscard]] std::optional<std::int64_t> end_sample() const noexcept {
        const auto source_end = range.end_sample ? range.end_sample : decoder.duration_samples();
        if (!source_end) {
            return std::nullopt;
        }
        return chain_offset + *source_end;
    }

    void clear_buffer_quiesced() noexcept {
        ring.reset();
        pending_samples.clear();
        pending_frame_offset = 0U;
        source_ended.store(false, std::memory_order_release);
    }

    void clear_chain_quiesced() noexcept {
        next_decoder.reset();
        next_replay_gain_override.reset();
        chain_offset = 0;
        chain_boundary.store(-1, std::memory_order_release);
        chain_crossed.store(false, std::memory_order_release);
    }

    [[nodiscard]] std::size_t write_pending(const std::size_t frame_budget) noexcept {
        if (pending_samples.empty() || frame_budget == 0U) {
            return 0U;
        }
        const auto channels = static_cast<std::size_t>(output.channels);
        const auto sample_offset = pending_frame_offset * channels;
        const auto pending = std::span<const float>{pending_samples}.subspan(sample_offset);
        const auto pending_frames = pending.size() / channels;
        const auto frames_to_offer = std::min(frame_budget, pending_frames);
        const auto written = ring.write(pending.first(frames_to_offer * channels));
        pending_frame_offset += written;
        if (pending_frame_offset == pending_samples.size() / channels) {
            pending_samples.clear();
            pending_frame_offset = 0U;
        }
        return written;
    }
};

LocalPlayback::LocalPlayback(std::unique_ptr<Impl> implementation)
    : implementation_(std::move(implementation)) {}

LocalPlayback::LocalPlayback(LocalPlayback&&) noexcept = default;
LocalPlayback& LocalPlayback::operator=(LocalPlayback&&) noexcept = default;
LocalPlayback::~LocalPlayback() = default;

core::Result<LocalPlayback> LocalPlayback::open(std::string raw_path,
                                                const PlaybackBufferConfig buffer_config,
                                                core::CancellationToken cancellation) {
    if (buffer_config.capacity_frames == 0U || buffer_config.start_threshold_frames == 0U ||
        buffer_config.start_threshold_frames > buffer_config.capacity_frames) {
        return std::unexpected(invalid_buffer_config(buffer_config));
    }
    auto decoder = formats::AudioDecoder::open(std::move(raw_path), std::move(cancellation));
    if (!decoder) {
        return std::unexpected(std::move(decoder.error()));
    }
    const auto channels = static_cast<std::size_t>(decoder->output_format().channels);
    if (channels == 0U ||
        buffer_config.capacity_frames > std::vector<float>{}.max_size() / channels) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::limit_exceeded,
            .message = "local playback buffer size exceeds addressable memory",
            .context = {},
        });
    }
    return LocalPlayback{std::make_unique<Impl>(std::move(*decoder), buffer_config)};
}

core::Result<LocalPlayback> LocalPlayback::open(std::string raw_path,
                                                const PlaybackBufferDurationConfig buffer_config,
                                                core::CancellationToken cancellation) {
    return open_selected(std::move(raw_path), {}, buffer_config, std::move(cancellation));
}

core::Result<LocalPlayback>
LocalPlayback::open_selected(std::string raw_path, formats::AudioSourceSelection selection,
                             const PlaybackBufferDurationConfig buffer_config,
                             core::CancellationToken cancellation,
                             std::optional<formats::ReplayGainInfo> replay_gain_override) {
    if (buffer_config.capacity <= std::chrono::milliseconds::zero() ||
        buffer_config.start_threshold <= std::chrono::milliseconds::zero() ||
        buffer_config.start_threshold > buffer_config.capacity) {
        return std::unexpected(invalid_buffer_duration_config(buffer_config));
    }
    auto decoder = formats::AudioDecoder::open_selected(std::move(raw_path), selection,
                                                        std::move(cancellation));
    if (!decoder) {
        return std::unexpected(std::move(decoder.error()));
    }
    auto frames = frame_buffer_config(buffer_config, decoder->output_format().sample_rate);
    if (!frames) {
        return std::unexpected(std::move(frames.error()));
    }
    const auto channels = static_cast<std::size_t>(decoder->output_format().channels);
    if (channels == 0U || frames->capacity_frames > std::vector<float>{}.max_size() / channels) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::limit_exceeded,
            .message = "local playback buffer size exceeds addressable memory",
            .context = {},
        });
    }
    return LocalPlayback{
        std::make_unique<Impl>(std::move(*decoder), *frames, replay_gain_override)};
}

core::Result<LocalPlayback> LocalPlayback::open_segment(std::string raw_path,
                                                        const formats::SampleRange range,
                                                        const PlaybackBufferConfig buffer_config,
                                                        core::CancellationToken cancellation) {
    if (buffer_config.capacity_frames == 0U || buffer_config.start_threshold_frames == 0U ||
        buffer_config.start_threshold_frames > buffer_config.capacity_frames) {
        return std::unexpected(invalid_buffer_config(buffer_config));
    }
    auto decoder =
        formats::AudioDecoder::open_segment(std::move(raw_path), range, std::move(cancellation));
    if (!decoder) {
        return std::unexpected(std::move(decoder.error()));
    }
    const auto channels = static_cast<std::size_t>(decoder->output_format().channels);
    if (channels == 0U ||
        buffer_config.capacity_frames > std::vector<float>{}.max_size() / channels) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::limit_exceeded,
            .message = "local playback buffer size exceeds addressable memory",
            .context = {},
        });
    }
    return LocalPlayback{std::make_unique<Impl>(std::move(*decoder), buffer_config)};
}

core::Result<LocalPlayback>
LocalPlayback::open_segment(std::string raw_path, const formats::SampleRange range,
                            const PlaybackBufferDurationConfig buffer_config,
                            core::CancellationToken cancellation) {
    return open_selected_segment(std::move(raw_path), {}, range, buffer_config,
                                 std::move(cancellation));
}

core::Result<LocalPlayback> LocalPlayback::open_selected_segment(
    std::string raw_path, formats::AudioSourceSelection selection, const formats::SampleRange range,
    const PlaybackBufferDurationConfig buffer_config, core::CancellationToken cancellation,
    std::optional<formats::ReplayGainInfo> replay_gain_override) {
    if (buffer_config.capacity <= std::chrono::milliseconds::zero() ||
        buffer_config.start_threshold <= std::chrono::milliseconds::zero() ||
        buffer_config.start_threshold > buffer_config.capacity) {
        return std::unexpected(invalid_buffer_duration_config(buffer_config));
    }
    auto decoder = formats::AudioDecoder::open_selected_segment(std::move(raw_path), selection,
                                                                range, std::move(cancellation));
    if (!decoder) {
        return std::unexpected(std::move(decoder.error()));
    }
    auto frames = frame_buffer_config(buffer_config, decoder->output_format().sample_rate);
    if (!frames) {
        return std::unexpected(std::move(frames.error()));
    }
    const auto channels = static_cast<std::size_t>(decoder->output_format().channels);
    if (channels == 0U || frames->capacity_frames > std::vector<float>{}.max_size() / channels) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::limit_exceeded,
            .message = "local playback buffer size exceeds addressable memory",
            .context = {},
        });
    }
    return LocalPlayback{
        std::make_unique<Impl>(std::move(*decoder), *frames, replay_gain_override)};
}

void LocalPlayback::set_replay_gain_mode(const ReplayGainMode mode) noexcept {
    implementation_->replay_gain_mode = mode;
}

void LocalPlayback::set_replay_gain_preamps(const ReplayGainPreamps preamps) noexcept {
    implementation_->replay_gain_preamps = preamps;
}

void LocalPlayback::set_replay_gain_info(formats::ReplayGainInfo info) noexcept {
    implementation_->replay_gain = std::move(info);
}

const formats::PcmFormat& LocalPlayback::output_format() const noexcept {
    return implementation_->output;
}

const formats::SampleRange& LocalPlayback::sample_range() const noexcept {
    return implementation_->range;
}

LocalPlaybackSnapshot LocalPlayback::snapshot() const noexcept {
    const auto& playback = *implementation_;
    const auto boundary = playback.chain_boundary.load(std::memory_order_acquire);
    return LocalPlaybackSnapshot{
        .state = playback.state.load(std::memory_order_acquire),
        .position_sample = playback.position_sample.load(std::memory_order_acquire),
        .end_sample = playback.end_sample(),
        .buffered_frames = playback.ring.size_frames(),
        .underrun_count = playback.underrun_count.load(std::memory_order_acquire),
        .replay_gain_info = playback.replay_gain,
        .effective_replay_gain_multiplier = replay_gain_multiplier(
            playback.replay_gain, playback.replay_gain_mode, playback.replay_gain_preamps),
        .decoded_peak_before_gain =
            playback.decoded_peak_before_gain.load(std::memory_order_acquire),
        .decoded_peak_after_gain = playback.decoded_peak_after_gain.load(std::memory_order_acquire),
        .next_queued = playback.next_decoder.has_value(),
        .chain_boundary_sample = boundary >= 0 ? std::optional{boundary} : std::nullopt,
        .chain_crossed = playback.chain_crossed.load(std::memory_order_acquire),
    };
}

core::Result<void> LocalPlayback::play() {
    auto& playback = *implementation_;
    const auto current = playback.state.load(std::memory_order_acquire);
    if (current == LocalPlaybackState::failed) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::conflict,
            .message = "failed local playback must be reloaded before it can play",
            .context = {},
        });
    }
    if (current == LocalPlaybackState::buffering || current == LocalPlaybackState::playing ||
        current == LocalPlaybackState::draining) {
        return {};
    }
    if (current == LocalPlaybackState::ended) {
        auto stopped = stop();
        if (!stopped) {
            return stopped;
        }
    }
    if (playback.source_ended.load(std::memory_order_acquire)) {
        playback.state.store(playback.ring.size_frames() == 0U ? LocalPlaybackState::ended
                                                               : LocalPlaybackState::draining,
                             std::memory_order_release);
    } else {
        playback.state.store(playback.ring.size_frames() >= playback.config.start_threshold_frames
                                 ? LocalPlaybackState::playing
                                 : LocalPlaybackState::buffering,
                             std::memory_order_release);
    }
    return {};
}

void LocalPlayback::pause() noexcept {
    auto& state = implementation_->state;
    auto current = state.load(std::memory_order_acquire);
    while ((current == LocalPlaybackState::buffering || current == LocalPlaybackState::playing ||
            current == LocalPlaybackState::draining) &&
           !state.compare_exchange_weak(current, LocalPlaybackState::paused,
                                        std::memory_order_release, std::memory_order_acquire)) {
    }
}

core::Result<void> LocalPlayback::stop() {
    auto& playback = *implementation_;
    playback.state.store(LocalPlaybackState::stopped, std::memory_order_release);
    playback.clear_buffer_quiesced();
    // Stop collapses a chain back to plain single-source semantics of the
    // active decoder.
    playback.clear_chain_quiesced();
    auto seek = playback.decoder.seek_to_sample(playback.range.start_sample);
    if (!seek) {
        playback.state.store(LocalPlaybackState::failed, std::memory_order_release);
        return seek;
    }
    playback.next_decode_sample = playback.range.start_sample;
    playback.position_sample.store(playback.range.start_sample, std::memory_order_release);
    return {};
}

core::Result<void> LocalPlayback::seek_to_sample(const std::int64_t target_sample) {
    auto& playback = *implementation_;
    if (playback.state.load(std::memory_order_acquire) == LocalPlaybackState::failed) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::conflict,
            .message = "failed local playback must be reloaded before it can seek",
            .context = {},
        });
    }
    const auto end_sample = playback.end_sample();
    if (target_sample < playback.chain_offset + playback.range.start_sample ||
        (end_sample && target_sample > *end_sample)) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::invalid_argument,
            .message = "local playback seek target is outside the source sample range",
            .context = {{.key = "target_sample", .value = std::to_string(target_sample)}},
        });
    }
    const auto previous_state = playback.state.load(std::memory_order_acquire);
    const auto seek_state =
        previous_state == LocalPlaybackState::paused
            ? LocalPlaybackState::paused
            : (previous_state == LocalPlaybackState::stopped ? LocalPlaybackState::stopped
                                                             : LocalPlaybackState::buffering);
    playback.state.store(seek_state, std::memory_order_release);
    playback.clear_buffer_quiesced();
    // A queued continuation and any unacknowledged crossing latch cannot
    // survive the flush; the caller re-queues after the seek.
    playback.next_decoder.reset();
    playback.chain_boundary.store(-1, std::memory_order_release);
    playback.chain_crossed.store(false, std::memory_order_release);
    auto seek = playback.decoder.seek_to_sample(target_sample - playback.chain_offset);
    if (!seek) {
        playback.state.store(LocalPlaybackState::failed, std::memory_order_release);
        return seek;
    }
    playback.next_decode_sample = target_sample;
    playback.position_sample.store(target_sample, std::memory_order_release);
    return {};
}

core::Result<void> LocalPlayback::queue_next(std::string raw_path,
                                             core::CancellationToken cancellation) {
    return queue_next_selected_segment(std::move(raw_path), {}, formats::SampleRange{},
                                       std::move(cancellation));
}

core::Result<void>
LocalPlayback::queue_next_selected(std::string raw_path, formats::AudioSourceSelection selection,
                                   core::CancellationToken cancellation,
                                   std::optional<formats::ReplayGainInfo> replay_gain_override) {
    return queue_next_selected_segment(std::move(raw_path), selection, formats::SampleRange{},
                                       std::move(cancellation), replay_gain_override);
}

core::Result<void> LocalPlayback::queue_next_segment(std::string raw_path,
                                                     const formats::SampleRange range,
                                                     core::CancellationToken cancellation) {
    return queue_next_selected_segment(std::move(raw_path), {}, range, std::move(cancellation));
}

core::Result<void> LocalPlayback::queue_next_selected_segment(
    std::string raw_path, formats::AudioSourceSelection selection, const formats::SampleRange range,
    core::CancellationToken cancellation,
    std::optional<formats::ReplayGainInfo> replay_gain_override) {
    auto& playback = *implementation_;
    if (playback.state.load(std::memory_order_acquire) == LocalPlaybackState::failed) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::conflict,
            .message = "failed local playback cannot queue a continuation",
            .context = {},
        });
    }
    if (playback.next_decoder || playback.chain_boundary.load(std::memory_order_acquire) >= 0) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::conflict,
            .message = "a gapless continuation is already queued or in flight",
            .context = {},
        });
    }
    if (playback.source_ended.load(std::memory_order_acquire)) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::conflict,
            .message = "the active source already ended; load the next source instead",
            .context = {},
        });
    }
    auto decoder = formats::AudioDecoder::open_selected_segment(std::move(raw_path), selection,
                                                                range, std::move(cancellation));
    if (!decoder) {
        return std::unexpected(std::move(decoder.error()));
    }
    if (!(decoder->output_format() == playback.output)) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::unsupported,
            .message = "gapless continuation requires an identical PCM format",
            .context = {{.key = "active_rate",
                         .value = std::to_string(playback.output.sample_rate)},
                        {.key = "next_rate",
                         .value = std::to_string(decoder->output_format().sample_rate)},
                        {.key = "active_channels",
                         .value = std::to_string(playback.output.channels)},
                        {.key = "next_channels",
                         .value = std::to_string(decoder->output_format().channels)}},
        });
    }
    playback.next_decoder = std::move(*decoder);
    playback.next_replay_gain_override = replay_gain_override;
    return {};
}

void LocalPlayback::clear_next() noexcept {
    implementation_->next_decoder.reset();
    implementation_->next_replay_gain_override.reset();
}

std::optional<std::int64_t> LocalPlayback::take_chain_crossing() noexcept {
    auto& playback = *implementation_;
    if (!playback.chain_crossed.load(std::memory_order_acquire)) {
        return std::nullopt;
    }
    const auto boundary = playback.chain_boundary.load(std::memory_order_acquire);
    playback.chain_crossed.store(false, std::memory_order_release);
    playback.chain_boundary.store(-1, std::memory_order_release);
    return boundary >= 0 ? std::optional{boundary} : std::nullopt;
}

core::Result<void> LocalPlayback::fill_buffer() {
    auto& playback = *implementation_;
    const auto initial_state = playback.state.load(std::memory_order_acquire);
    if (initial_state == LocalPlaybackState::stopped ||
        initial_state == LocalPlaybackState::paused || initial_state == LocalPlaybackState::ended ||
        initial_state == LocalPlaybackState::failed) {
        return {};
    }

    auto remaining_budget = playback.ring.capacity_frames();
    while (remaining_budget > 0U && playback.ring.size_frames() < playback.ring.capacity_frames()) {
        const auto written = playback.write_pending(remaining_budget);
        remaining_budget -= written;
        if (!playback.pending_samples.empty()) {
            break;
        }
        if (playback.source_ended.load(std::memory_order_acquire)) {
            break;
        }

        auto chunk = playback.decoder.next_chunk();
        if (!chunk) {
            playback.state.store(LocalPlaybackState::failed, std::memory_order_release);
            return std::unexpected(std::move(chunk.error()));
        }
        if (!*chunk) {
            if (playback.next_decoder) {
                // Seamless takeover: the queued decoder continues into the
                // same ring, positions keep increasing in the produced
                // domain, and the boundary is published for the consumer's
                // crossing latch.
                const auto boundary = playback.next_decode_sample;
                playback.decoder = std::move(*playback.next_decoder);
                playback.next_decoder.reset();
                playback.replay_gain = playback.next_replay_gain_override
                                           ? *playback.next_replay_gain_override
                                           : playback.decoder.replay_gain();
                playback.next_replay_gain_override.reset();
                playback.range = playback.decoder.sample_range();
                playback.chain_offset = boundary - playback.range.start_sample;
                playback.chain_boundary.store(boundary, std::memory_order_release);
                continue;
            }
            playback.source_ended.store(true, std::memory_order_release);
            break;
        }
        if ((*chunk)->start_sample != playback.next_decode_sample - playback.chain_offset) {
            playback.state.store(LocalPlaybackState::failed, std::memory_order_release);
            return std::unexpected(core::Error{
                .code = core::ErrorCode::invariant,
                .message = "local decoder produced a non-contiguous PCM chunk",
                .context =
                    {{.key = "expected_start",
                      .value = std::to_string(playback.next_decode_sample - playback.chain_offset)},
                     {.key = "actual_start", .value = std::to_string((*chunk)->start_sample)}},
            });
        }
        const auto chunk_frames =
            static_cast<std::int64_t>((*chunk)->frame_count(playback.output.channels));
        playback.next_decode_sample += chunk_frames;
        playback.pending_samples = std::move((*chunk)->interleaved_samples);
        playback.pending_frame_offset = 0U;
        float before_peak = 0.0F;
        for (const auto sample : playback.pending_samples) {
            before_peak = std::max(before_peak, std::abs(sample));
        }
        const auto gain = replay_gain_multiplier(playback.replay_gain, playback.replay_gain_mode,
                                                 playback.replay_gain_preamps);
        if (gain != 1.0F) {
            for (auto& sample : playback.pending_samples) {
                sample *= gain;
            }
        }
        float after_peak = 0.0F;
        for (const auto sample : playback.pending_samples) {
            after_peak = std::max(after_peak, std::abs(sample));
        }
        playback.decoded_peak_before_gain.store(before_peak, std::memory_order_release);
        playback.decoded_peak_after_gain.store(after_peak, std::memory_order_release);
    }

    auto current = playback.state.load(std::memory_order_acquire);
    if (playback.source_ended.load(std::memory_order_acquire)) {
        // Recompute the target inside the loop and never regress "ended":
        // the real-time renderer may drain the final frames and finish the
        // stream between the ring read and the exchange.
        while (current != LocalPlaybackState::paused && current != LocalPlaybackState::stopped &&
               current != LocalPlaybackState::failed && current != LocalPlaybackState::ended) {
            const auto next_state = playback.ring.size_frames() == 0U
                                        ? LocalPlaybackState::ended
                                        : LocalPlaybackState::draining;
            if (playback.state.compare_exchange_weak(current, next_state, std::memory_order_release,
                                                     std::memory_order_acquire)) {
                break;
            }
        }
    } else if (playback.ring.size_frames() >= playback.config.start_threshold_frames) {
        current = LocalPlaybackState::buffering;
        playback.state.compare_exchange_strong(current, LocalPlaybackState::playing,
                                               std::memory_order_release,
                                               std::memory_order_acquire);
    }
    return {};
}

std::size_t LocalPlayback::render(std::span<float> interleaved_output) noexcept {
    auto& playback = *implementation_;
    std::ranges::fill(interleaved_output, 0.0F);
    const auto channels = static_cast<std::size_t>(playback.output.channels);
    const auto requested_frames = interleaved_output.size() / channels;
    if (requested_frames == 0U) {
        return 0U;
    }
    const auto state = playback.state.load(std::memory_order_acquire);
    if (state != LocalPlaybackState::playing && state != LocalPlaybackState::draining) {
        return 0U;
    }

    const auto copied_frames = playback.ring.read(interleaved_output);
    const auto previous_position = playback.position_sample.fetch_add(
        static_cast<std::int64_t>(copied_frames), std::memory_order_release);
    const auto boundary = playback.chain_boundary.load(std::memory_order_acquire);
    if (boundary >= 0 && previous_position + static_cast<std::int64_t>(copied_frames) >= boundary) {
        playback.chain_crossed.store(true, std::memory_order_release);
    }
    if (copied_frames < requested_frames) {
        if (state == LocalPlaybackState::playing) {
            playback.underrun_count.fetch_add(1U, std::memory_order_relaxed);
            auto expected = LocalPlaybackState::playing;
            playback.state.compare_exchange_strong(expected, LocalPlaybackState::buffering,
                                                   std::memory_order_release,
                                                   std::memory_order_acquire);
        } else if (playback.ring.size_frames() == 0U) {
            auto expected = LocalPlaybackState::draining;
            playback.state.compare_exchange_strong(expected, LocalPlaybackState::ended,
                                                   std::memory_order_release,
                                                   std::memory_order_acquire);
        }
    } else if (state == LocalPlaybackState::draining && playback.ring.size_frames() == 0U) {
        auto expected = LocalPlaybackState::draining;
        playback.state.compare_exchange_strong(expected, LocalPlaybackState::ended,
                                               std::memory_order_release,
                                               std::memory_order_acquire);
    }
    return copied_frames;
}

} // namespace trackknife::audio

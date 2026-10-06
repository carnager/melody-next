// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace trackknife::formats {

struct ReplayGainInfo {
    std::optional<double> track_gain_db;
    std::optional<double> track_peak;
    std::optional<double> album_gain_db;
    std::optional<double> album_peak;

    friend bool operator==(const ReplayGainInfo&, const ReplayGainInfo&) = default;
};

// Lenient ReplayGain text parsing shared by every textual carrier (tags,
// CUE REM lines, staged drafts): optional leading +, optional dB suffix
// on gains, ±60 dB gain and positive peak sanity ranges.
[[nodiscard]] std::optional<double> parse_replay_gain_decibels(std::string_view value) noexcept;
[[nodiscard]] std::optional<double> parse_replay_gain_peak(std::string_view value) noexcept;

// RFC 7845 R128 loudness for Opus: R128_TRACK_GAIN/R128_ALBUM_GAIN hold a
// Q7.8 fixed-point decibel value (signed 16-bit integer, dB = value / 256)
// relative to the OpusHead output gain and referenced to -23 LUFS.
// ReplayGain 2.0 normalizes 5 dB higher (ADR-0149).
inline constexpr double opus_r128_reference_shift_db = 5.0;
// Strict integer parse of the Q7.8 comment value into RFC-referenced
// decibels; anything but an exact in-range integer is rejected.
[[nodiscard]] std::optional<double> parse_r128_gain_decibels(std::string_view value) noexcept;
// Formats RFC-referenced decibels as the Q7.8 comment value, clamped to
// the signed 16-bit domain.
[[nodiscard]] std::string r128_gain_text(double r128_decibels);

struct PcmFormat {
    int sample_rate{0};
    int channels{0};
    std::string channel_layout;

    friend bool operator==(const PcmFormat&, const PcmFormat&) = default;
};

struct PcmChunk {
    std::int64_t start_sample{0};
    std::vector<float> interleaved_samples;

    [[nodiscard]] std::size_t frame_count(const int channels) const noexcept {
        return channels > 0 ? interleaved_samples.size() / static_cast<std::size_t>(channels) : 0U;
    }
};

struct SampleRange {
    std::int64_t start_sample{0};
    std::optional<std::int64_t> end_sample;

    friend bool operator==(const SampleRange&, const SampleRange&) = default;
};

// Selects one logical decoder source inside a physical file. Stream indexes
// are FFmpeg container-stream indexes, while subsong indexes are interpreted
// only by demuxers that advertise a codec-native subsong option. Empty means
// the ordinary best audio stream and the demuxer's default song.
struct AudioSourceSelection {
    std::optional<int> stream_index;
    std::optional<int> subsong_index;

    friend bool operator==(const AudioSourceSelection&, const AudioSourceSelection&) = default;
};

class AudioDecoder final {
  public:
    AudioDecoder(AudioDecoder&&) noexcept;
    AudioDecoder& operator=(AudioDecoder&&) noexcept;
    AudioDecoder(const AudioDecoder&) = delete;
    AudioDecoder& operator=(const AudioDecoder&) = delete;
    ~AudioDecoder();

    [[nodiscard]] static core::Result<AudioDecoder> open(std::string raw_path,
                                                         core::CancellationToken cancellation = {});
    [[nodiscard]] static core::Result<AudioDecoder>
    open_selected(std::string raw_path, AudioSourceSelection selection,
                  core::CancellationToken cancellation = {});
    [[nodiscard]] static core::Result<AudioDecoder>
    open_segment(std::string raw_path, SampleRange range,
                 core::CancellationToken cancellation = {});
    [[nodiscard]] static core::Result<AudioDecoder>
    open_selected_segment(std::string raw_path, AudioSourceSelection selection, SampleRange range,
                          core::CancellationToken cancellation = {});

    // Fresh container/selected-stream values; does not apply gain to decoded PCM.
    [[nodiscard]] ReplayGainInfo replay_gain() const noexcept;
    // The selected stream decodes as Opus and therefore carries loudness
    // via RFC 7845 R128 comments rather than REPLAYGAIN_* tags (ADR-0149).
    [[nodiscard]] bool opus_stream() const noexcept;
    [[nodiscard]] const PcmFormat& output_format() const noexcept;
    [[nodiscard]] std::optional<std::int64_t> duration_samples() const noexcept;
    [[nodiscard]] const SampleRange& sample_range() const noexcept;
    [[nodiscard]] const AudioSourceSelection& source_selection() const noexcept;
    [[nodiscard]] core::Result<void> seek_to_sample(std::int64_t target_sample);
    [[nodiscard]] core::Result<std::optional<PcmChunk>> next_chunk();

  private:
    struct Impl;
    explicit AudioDecoder(std::unique_ptr<Impl> implementation);
    // The file at the path is another one than was opened -- replaced, as a
    // tag write does, by a rename over it: reopened and taken on at the
    // next sample. False when it is the same file or cannot be.
    [[nodiscard]] bool reopen_if_replaced();

    std::unique_ptr<Impl> implementation_;
};

} // namespace trackknife::formats

// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/formats/decoder.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/replaygain.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace trackknife::formats {
namespace {

[[nodiscard]] std::string ffmpeg_error(const int code) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
    if (av_strerror(code, buffer.data(), buffer.size()) < 0) {
        return "unknown FFmpeg error " + std::to_string(code);
    }
    return buffer.data();
}

[[nodiscard]] core::Error decoder_error(const int code, const std::string& operation,
                                        const std::string& raw_path,
                                        const core::CancellationToken& cancellation) {
    if (cancellation.is_cancellation_requested() || code == AVERROR_EXIT) {
        return core::Error{.code = core::ErrorCode::cancelled,
                           .message = operation + " was cancelled",
                           .context = {{.key = "path", .value = raw_path}}};
    }
    return core::Error{
        .code = code == AVERROR(ENOENT) ? core::ErrorCode::not_found : core::ErrorCode::backend,
        .message = operation + " failed: " + ffmpeg_error(code),
        .context = {{.key = "path", .value = raw_path},
                    {.key = "ffmpeg_error", .value = std::to_string(code)}},
    };
}

[[nodiscard]] std::string describe_layout(const AVChannelLayout& layout) {
    std::array<char, 128> description{};
    const auto length = av_channel_layout_describe(&layout, description.data(), description.size());
    return length < 0 ? std::string{} : std::string{description.data()};
}

} // namespace

struct AudioDecoder::Impl {
    std::string raw_path;
    core::CancellationToken cancellation;
    AVFormatContext* format{nullptr};
    AVCodecContext* codec{nullptr};
    SwrContext* resampler{nullptr};
    AVPacket* packet{nullptr};
    AVFrame* frame{nullptr};
    int stream_index{-1};
    AudioSourceSelection selection;
    PcmFormat output;
    std::optional<std::int64_t> total_samples;
    SampleRange range;
    std::int64_t seek_preroll_samples{0};
    std::int64_t discard_before_sample{0};
    std::int64_t decoded_samples{0};
    std::optional<std::int64_t> timeline_origin_sample;
    std::optional<std::int64_t> next_decoded_sample;
    std::int64_t next_output_sample{0};
    // Which file was opened, to know it from one put in its place: a tag
    // write renames a new file over it. Read through another machine's
    // mount, the one opened is gone with it -- NFS keeps a replaced file
    // only for the machine that replaced it -- and reading fails mid-track.
    std::optional<std::pair<dev_t, ino_t>> identity;
    unsigned reopened{0};
    bool input_finished{false};
    bool drain_sent{false};
    bool range_finished{false};

    ~Impl() {
        av_frame_free(&frame);
        av_packet_free(&packet);
        swr_free(&resampler);
        avcodec_free_context(&codec);
        avformat_close_input(&format);
    }

    static int interrupt(void* opaque) {
        const auto* decoder = static_cast<const Impl*>(opaque);
        return decoder != nullptr && decoder->cancellation.is_cancellation_requested() ? 1 : 0;
    }

    [[nodiscard]] core::Result<std::optional<PcmChunk>> convert_frame() {
        std::uint32_t skip_start = 0U;
        std::uint32_t skip_end = 0U;
        const auto* skip_samples = av_frame_get_side_data(frame, AV_FRAME_DATA_SKIP_SAMPLES);
        if (skip_samples != nullptr) {
            constexpr std::size_t skip_sample_data_size = 10U;
            if (skip_samples->size < skip_sample_data_size) {
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::backend,
                    .message = "decoded audio has malformed skip-sample metadata",
                    .context = {{.key = "path", .value = raw_path}},
                });
            }
            skip_start = AV_RL32(skip_samples->data);
            skip_end = AV_RL32(skip_samples->data + 4);
        }

        const auto capacity = swr_get_out_samples(resampler, frame->nb_samples);
        if (capacity < 0 || output.channels <= 0) {
            return std::unexpected(decoder_error(capacity < 0 ? capacity : AVERROR(EINVAL),
                                                 "sizing decoded PCM", raw_path, cancellation));
        }
        const auto sample_capacity =
            static_cast<std::size_t>(capacity) * static_cast<std::size_t>(output.channels);
        std::vector<float> samples(sample_capacity);
        std::array<std::uint8_t*, 1> output_planes{reinterpret_cast<std::uint8_t*>(samples.data())};
        const auto converted =
            swr_convert(resampler, output_planes.data(), capacity,
                        const_cast<const std::uint8_t**>(frame->extended_data), frame->nb_samples);
        if (converted < 0) {
            return std::unexpected(
                decoder_error(converted, "converting decoded PCM", raw_path, cancellation));
        }
        samples.resize(static_cast<std::size_t>(converted) *
                       static_cast<std::size_t>(output.channels));

        const auto samples_to_skip = static_cast<std::uint64_t>(skip_start) + skip_end;
        if (samples_to_skip > static_cast<std::uint64_t>(converted)) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::backend,
                .message = "decoded audio skip metadata exceeds its PCM frame",
                .context = {{.key = "path", .value = raw_path},
                            {.key = "decoded_samples", .value = std::to_string(converted)},
                            {.key = "skip_start", .value = std::to_string(skip_start)},
                            {.key = "skip_end", .value = std::to_string(skip_end)}},
            });
        }

        auto raw_start_sample = decoded_samples;
        if (frame->best_effort_timestamp != AV_NOPTS_VALUE) {
            const auto* stream = format->streams[static_cast<unsigned>(stream_index)];
            auto timestamp = frame->best_effort_timestamp;
            if (stream->start_time != AV_NOPTS_VALUE) {
                timestamp -= stream->start_time;
            }
            raw_start_sample = av_rescale_q(timestamp, stream->time_base,
                                            AVRational{.num = 1, .den = output.sample_rate});
        }
        const auto channels = static_cast<std::size_t>(output.channels);
        const auto drop_front = static_cast<std::size_t>(skip_start) * channels;
        if (drop_front > 0U) {
            samples.erase(samples.begin(),
                          samples.begin() + static_cast<std::ptrdiff_t>(drop_front));
        }
        samples.resize(samples.size() - (static_cast<std::size_t>(skip_end) * channels));
        const auto raw_output_start = raw_start_sample + skip_start;
        if (!timeline_origin_sample) {
            timeline_origin_sample = raw_output_start;
        }
        auto output_start = raw_output_start - *timeline_origin_sample;
        if (next_decoded_sample) {
            output_start = *next_decoded_sample;
        }
        const auto output_frames = static_cast<std::int64_t>(samples.size() / channels);
        next_decoded_sample = output_start + output_frames;
        decoded_samples = raw_start_sample + converted;
        av_frame_unref(frame);
        return std::optional{PcmChunk{
            .start_sample = output_start,
            .interleaved_samples = std::move(samples),
        }};
    }

    [[nodiscard]] bool trim_to_range(PcmChunk& chunk) {
        const auto channels = static_cast<std::size_t>(output.channels);
        const auto frame_count = static_cast<std::int64_t>(chunk.frame_count(output.channels));
        const auto chunk_end = chunk.start_sample + frame_count;
        if (chunk_end <= discard_before_sample) {
            return false;
        }
        if (range.end_sample && chunk.start_sample >= *range.end_sample) {
            range_finished = true;
            return false;
        }

        const auto keep_start = std::max(chunk.start_sample, discard_before_sample);
        const auto keep_end = range.end_sample ? std::min(chunk_end, *range.end_sample) : chunk_end;
        if (keep_end <= keep_start) {
            range_finished = range.end_sample && keep_end >= *range.end_sample;
            return false;
        }
        const auto drop_front =
            static_cast<std::size_t>(keep_start - chunk.start_sample) * channels;
        const auto keep_samples = static_cast<std::size_t>(keep_end - keep_start) * channels;
        if (drop_front > 0U) {
            chunk.interleaved_samples.erase(chunk.interleaved_samples.begin(),
                                            chunk.interleaved_samples.begin() +
                                                static_cast<std::ptrdiff_t>(drop_front));
        }
        chunk.interleaved_samples.resize(keep_samples);
        chunk.start_sample = keep_start;
        if (range.end_sample && keep_end >= *range.end_sample) {
            range_finished = true;
        }
        return !chunk.interleaved_samples.empty();
    }
};

AudioDecoder::AudioDecoder(std::unique_ptr<Impl> implementation)
    : implementation_(std::move(implementation)) {}

AudioDecoder::AudioDecoder(AudioDecoder&&) noexcept = default;
AudioDecoder& AudioDecoder::operator=(AudioDecoder&&) noexcept = default;
AudioDecoder::~AudioDecoder() = default;

core::Result<AudioDecoder> AudioDecoder::open(std::string raw_path,
                                              core::CancellationToken cancellation) {
    return open_selected(std::move(raw_path), {}, std::move(cancellation));
}

core::Result<AudioDecoder> AudioDecoder::open_selected(std::string raw_path,
                                                       AudioSourceSelection selection,
                                                       core::CancellationToken cancellation) {
    if (raw_path.empty()) {
        return std::unexpected(core::Error{.code = core::ErrorCode::invalid_argument,
                                           .message = "local audio path is empty",
                                           .context = {}});
    }
    if (cancellation.is_cancellation_requested()) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::cancelled,
            .message = "opening local audio was cancelled",
            .context = {{.key = "path", .value = raw_path}},
        });
    }
    if ((selection.stream_index && *selection.stream_index < 0) ||
        (selection.subsong_index && *selection.subsong_index < 0)) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::invalid_argument,
            .message = "audio source selection indexes must be non-negative",
            .context = {{.key = "path", .value = raw_path}},
        });
    }

    auto decoder = std::make_unique<Impl>();
    decoder->raw_path = std::move(raw_path);
    decoder->cancellation = std::move(cancellation);
    decoder->selection = selection;
    decoder->format = avformat_alloc_context();
    if (decoder->format == nullptr) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::backend,
            .message = "FFmpeg could not allocate a format context",
            .context = {{.key = "path", .value = decoder->raw_path}},
        });
    }
    decoder->format->interrupt_callback = AVIOInterruptCB{
        .callback = Impl::interrupt,
        .opaque = decoder.get(),
    };
    AVDictionary* input_options = nullptr;
    if (selection.subsong_index) {
        const auto option_value = std::to_string(*selection.subsong_index);
        if (av_dict_set(&input_options, "subsong", option_value.c_str(), 0) < 0) {
            av_dict_free(&input_options);
            return std::unexpected(core::Error{
                .code = core::ErrorCode::backend,
                .message = "FFmpeg could not configure the codec-native subsong",
                .context = {{.key = "path", .value = decoder->raw_path}},
            });
        }
    }
    if (struct stat opened{}; ::stat(decoder->raw_path.c_str(), &opened) == 0) {
        decoder->identity = std::pair{opened.st_dev, opened.st_ino};
    }
    const auto open_result =
        avformat_open_input(&decoder->format, decoder->raw_path.c_str(), nullptr, &input_options);
    const bool unused_subsong_option = av_dict_get(input_options, "subsong", nullptr, 0) != nullptr;
    av_dict_free(&input_options);
    if (open_result < 0) {
        return std::unexpected(decoder_error(open_result, "opening local audio", decoder->raw_path,
                                             decoder->cancellation));
    }
    if (unused_subsong_option) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::unsupported,
            .message = "the selected input format does not expose codec-native subsongs",
            .context = {{.key = "path", .value = decoder->raw_path}},
        });
    }
    const auto info_result = avformat_find_stream_info(decoder->format, nullptr);
    if (info_result < 0) {
        return std::unexpected(decoder_error(info_result, "reading local audio streams",
                                             decoder->raw_path, decoder->cancellation));
    }
    if (selection.stream_index) {
        const auto selected = static_cast<unsigned>(*selection.stream_index);
        if (selected >= decoder->format->nb_streams ||
            decoder->format->streams[selected] == nullptr ||
            decoder->format->streams[selected]->codecpar == nullptr ||
            decoder->format->streams[selected]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::invalid_argument,
                .message = "selected audio stream index is unavailable",
                .context = {{.key = "path", .value = decoder->raw_path},
                            {.key = "stream_index",
                             .value = std::to_string(*selection.stream_index)}},
            });
        }
        decoder->stream_index = *selection.stream_index;
    } else {
        decoder->stream_index =
            av_find_best_stream(decoder->format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    }
    if (decoder->stream_index < 0) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::unsupported,
            .message = "local media contains no decodable audio stream",
            .context = {{.key = "path", .value = decoder->raw_path}},
        });
    }
    const auto* parameters =
        decoder->format->streams[static_cast<unsigned>(decoder->stream_index)]->codecpar;
    const auto* codec = avcodec_find_decoder(parameters->codec_id);
    if (codec == nullptr) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::unsupported,
            .message = "FFmpeg has no decoder for the selected audio stream",
            .context = {{.key = "path", .value = decoder->raw_path}},
        });
    }
    decoder->codec = avcodec_alloc_context3(codec);
    if (decoder->codec == nullptr) {
        return std::unexpected(
            core::Error{.code = core::ErrorCode::backend,
                        .message = "FFmpeg could not allocate a decoder",
                        .context = {{.key = "path", .value = decoder->raw_path}}});
    }
    auto codec_result = avcodec_parameters_to_context(decoder->codec, parameters);
    if (codec_result >= 0) {
        decoder->codec->thread_count = 1;
        decoder->codec->flags2 |= AV_CODEC_FLAG2_SKIP_MANUAL;
        codec_result = avcodec_open2(decoder->codec, codec, nullptr);
    }
    if (codec_result < 0) {
        return std::unexpected(decoder_error(codec_result, "opening the audio decoder",
                                             decoder->raw_path, decoder->cancellation));
    }
    if (decoder->codec->sample_rate <= 0 || decoder->codec->ch_layout.nb_channels <= 0) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::unsupported,
            .message = "audio stream has no usable sample rate or channel layout",
            .context = {{.key = "path", .value = decoder->raw_path}},
        });
    }
    decoder->seek_preroll_samples =
        std::max({std::int64_t{0}, static_cast<std::int64_t>(parameters->initial_padding),
                  static_cast<std::int64_t>(parameters->seek_preroll),
                  static_cast<std::int64_t>(decoder->codec->delay),
                  static_cast<std::int64_t>(decoder->codec->sample_rate)});

    AVChannelLayout output_layout{};
    if (av_channel_layout_copy(&output_layout, &decoder->codec->ch_layout) < 0) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::backend,
            .message = "FFmpeg could not copy the audio channel layout",
            .context = {{.key = "path", .value = decoder->raw_path}},
        });
    }
    const auto resampler_result =
        swr_alloc_set_opts2(&decoder->resampler, &output_layout, AV_SAMPLE_FMT_FLT,
                            decoder->codec->sample_rate, &decoder->codec->ch_layout,
                            decoder->codec->sample_fmt, decoder->codec->sample_rate, 0, nullptr);
    decoder->output = PcmFormat{
        .sample_rate = decoder->codec->sample_rate,
        .channels = decoder->codec->ch_layout.nb_channels,
        .channel_layout = describe_layout(output_layout),
    };
    av_channel_layout_uninit(&output_layout);
    if (resampler_result < 0 || decoder->resampler == nullptr) {
        return std::unexpected(
            decoder_error(resampler_result < 0 ? resampler_result : AVERROR(ENOMEM),
                          "configuring PCM conversion", decoder->raw_path, decoder->cancellation));
    }
    const auto initialize_result = swr_init(decoder->resampler);
    if (initialize_result < 0) {
        return std::unexpected(decoder_error(initialize_result, "initializing PCM conversion",
                                             decoder->raw_path, decoder->cancellation));
    }
    decoder->packet = av_packet_alloc();
    decoder->frame = av_frame_alloc();
    if (decoder->packet == nullptr || decoder->frame == nullptr) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::backend,
            .message = "FFmpeg could not allocate decode buffers",
            .context = {{.key = "path", .value = decoder->raw_path}},
        });
    }
    if (decoder->format->duration != AV_NOPTS_VALUE) {
        decoder->total_samples =
            av_rescale_q(decoder->format->duration, AV_TIME_BASE_Q,
                         AVRational{.num = 1, .den = decoder->output.sample_rate});
    }
    return AudioDecoder{std::move(decoder)};
}

core::Result<AudioDecoder> AudioDecoder::open_segment(std::string raw_path, SampleRange range,
                                                      core::CancellationToken cancellation) {
    return open_selected_segment(std::move(raw_path), {}, range, std::move(cancellation));
}

core::Result<AudioDecoder>
AudioDecoder::open_selected_segment(std::string raw_path, AudioSourceSelection selection,
                                    SampleRange range, core::CancellationToken cancellation) {
    if (range.start_sample < 0 || (range.end_sample && *range.end_sample <= range.start_sample)) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::invalid_argument,
            .message = "audio sample range must be non-negative and end after start",
            .context = {{.key = "path", .value = raw_path},
                        {.key = "start_sample", .value = std::to_string(range.start_sample)},
                        {.key = "end_sample",
                         .value = range.end_sample ? std::to_string(*range.end_sample) : "none"}},
        });
    }
    auto opened = open_selected(std::move(raw_path), selection, std::move(cancellation));
    if (!opened) {
        return std::unexpected(std::move(opened.error()));
    }
    auto& decoder = *opened;
    if (decoder.duration_samples() &&
        (range.start_sample > *decoder.duration_samples() ||
         (range.end_sample && *range.end_sample > *decoder.duration_samples()))) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::invalid_argument,
            .message = "audio sample range exceeds the known source duration",
            .context = {{.key = "path", .value = decoder.implementation_->raw_path},
                        {.key = "duration_samples",
                         .value = std::to_string(*decoder.duration_samples())}},
        });
    }
    if (range.start_sample > 0) {
        auto seek_result = decoder.seek_to_sample(range.start_sample);
        if (!seek_result) {
            return std::unexpected(std::move(seek_result.error()));
        }
    }
    decoder.implementation_->range = range;
    decoder.implementation_->discard_before_sample = range.start_sample;
    return opened;
}

namespace {

[[nodiscard]] std::optional<double> parse_replay_gain_number(std::string_view value,
                                                             const bool gain) noexcept {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    if (!value.empty() && value.front() == '+') {
        value.remove_prefix(1);
    }
    double result = 0.0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || !std::isfinite(result)) {
        return std::nullopt;
    }
    auto suffix = value.substr(static_cast<std::size_t>(parsed.ptr - value.data()));
    while (!suffix.empty() && (suffix.front() == ' ' || suffix.front() == '\t')) {
        suffix.remove_prefix(1);
    }
    if (gain &&
        (suffix.starts_with("dB") || suffix.starts_with("db") || suffix.starts_with("DB"))) {
        suffix.remove_prefix(2);
    }
    while (!suffix.empty() && (suffix.front() == ' ' || suffix.front() == '\t')) {
        suffix.remove_prefix(1);
    }
    if (!suffix.empty() || (gain ? result < -60.0 || result > 60.0 : result <= 0.0)) {
        return std::nullopt;
    }
    return result;
}

} // namespace

std::optional<double> parse_replay_gain_decibels(const std::string_view value) noexcept {
    return parse_replay_gain_number(value, true);
}

std::optional<double> parse_replay_gain_peak(const std::string_view value) noexcept {
    return parse_replay_gain_number(value, false);
}

std::optional<double> parse_r128_gain_decibels(const std::string_view value) noexcept {
    const auto* begin = value.data();
    const auto* end = value.data() + value.size();
    if (begin != end && *begin == '+') {
        ++begin;
    }
    int parsed{};
    const auto ends = std::from_chars(begin, end, parsed);
    if (ends.ec != std::errc{} || ends.ptr != end) {
        return std::nullopt;
    }
    if (parsed < -32'768 || parsed > 32'767) {
        return std::nullopt;
    }
    return static_cast<double>(parsed) / 256.0;
}

std::string r128_gain_text(const double r128_decibels) {
    const auto scaled = std::llround(r128_decibels * 256.0);
    return std::to_string(std::clamp<long long>(scaled, -32'768LL, 32'767LL));
}

ReplayGainInfo AudioDecoder::replay_gain() const noexcept {
    const auto& decoder = *implementation_;
    const auto* stream = decoder.format->streams[static_cast<unsigned>(decoder.stream_index)];
    const auto number = [&](const char* key, const bool gain) -> std::optional<double> {
        const auto* entry = av_dict_get(stream->metadata, key, nullptr, 0);
        if (entry == nullptr) {
            entry = av_dict_get(decoder.format->metadata, key, nullptr, 0);
        }
        if (entry == nullptr) {
            return std::nullopt;
        }
        return parse_replay_gain_number(entry->value, gain);
    };
    const auto present = [&](const char* key) {
        return av_dict_get(stream->metadata, key, nullptr, 0) != nullptr ||
               av_dict_get(decoder.format->metadata, key, nullptr, 0) != nullptr;
    };
    // ADR-0149: Opus loudness lives in RFC 7845 R128 comments — Q7.8 dB
    // relative to the output gain libopus already applied, referenced to
    // -23 LUFS. When present they own the result; REPLAYGAIN_* remnants
    // on the same file are ignored rather than mixed. No peak exists.
    if (stream->codecpar->codec_id == AV_CODEC_ID_OPUS) {
        const auto r128 = [&](const char* key) -> std::optional<double> {
            const auto* entry = av_dict_get(stream->metadata, key, nullptr, 0);
            if (entry == nullptr) {
                entry = av_dict_get(decoder.format->metadata, key, nullptr, 0);
            }
            if (entry == nullptr) {
                return std::nullopt;
            }
            const auto parsed = parse_r128_gain_decibels(entry->value);
            if (!parsed) {
                return std::nullopt;
            }
            return *parsed + opus_r128_reference_shift_db;
        };
        const auto track = r128("R128_TRACK_GAIN");
        const auto album = r128("R128_ALBUM_GAIN");
        if (track || album) {
            return ReplayGainInfo{
                .track_gain_db = track,
                .track_peak = std::nullopt,
                .album_gain_db = album,
                .album_peak = std::nullopt,
            };
        }
    }
    ReplayGainInfo result{
        .track_gain_db = number("REPLAYGAIN_TRACK_GAIN", true),
        .track_peak = number("REPLAYGAIN_TRACK_PEAK", false),
        .album_gain_db = number("REPLAYGAIN_ALBUM_GAIN", true),
        .album_peak = number("REPLAYGAIN_ALBUM_PEAK", false),
    };
    // Some demuxers expose native gain solely as coded stream side data.
    // Opus R128 has a distinct reference policy and is deliberately excluded.
    const auto* parameters = stream->codecpar;
    const auto* side = av_packet_side_data_get(
        parameters->coded_side_data, parameters->nb_coded_side_data, AV_PKT_DATA_REPLAYGAIN);
    if (parameters->codec_id != AV_CODEC_ID_OPUS && side != nullptr &&
        side->size >= sizeof(AVReplayGain)) {
        AVReplayGain gain{};
        std::memcpy(&gain, side->data, sizeof(gain));
        const auto fallback_gain = [](std::optional<double>& target, const std::int32_t raw) {
            const auto db = static_cast<double>(raw) / 100'000.0;
            if (!target && raw != INT32_MIN && db >= -60.0 && db <= 60.0) {
                target = db;
            }
        };
        if (!present("REPLAYGAIN_TRACK_GAIN")) {
            fallback_gain(result.track_gain_db, gain.track_gain);
        }
        if (!present("REPLAYGAIN_ALBUM_GAIN")) {
            fallback_gain(result.album_gain_db, gain.album_gain);
        }
        if (!present("REPLAYGAIN_TRACK_PEAK") && gain.track_peak != 0U) {
            result.track_peak = static_cast<double>(gain.track_peak) / 100'000.0;
        }
        if (!present("REPLAYGAIN_ALBUM_PEAK") && gain.album_peak != 0U) {
            result.album_peak = static_cast<double>(gain.album_peak) / 100'000.0;
        }
    }
    return result;
}

bool AudioDecoder::opus_stream() const noexcept {
    const auto& decoder = *implementation_;
    return decoder.format->streams[static_cast<unsigned>(decoder.stream_index)]
               ->codecpar->codec_id == AV_CODEC_ID_OPUS;
}

const PcmFormat& AudioDecoder::output_format() const noexcept { return implementation_->output; }

std::optional<std::int64_t> AudioDecoder::duration_samples() const noexcept {
    return implementation_->total_samples;
}

const SampleRange& AudioDecoder::sample_range() const noexcept { return implementation_->range; }

const AudioSourceSelection& AudioDecoder::source_selection() const noexcept {
    return implementation_->selection;
}

core::Result<void> AudioDecoder::seek_to_sample(const std::int64_t target_sample) {
    auto& decoder = *implementation_;
    if (target_sample < decoder.range.start_sample ||
        (decoder.range.end_sample && target_sample > *decoder.range.end_sample) ||
        (decoder.total_samples && target_sample > *decoder.total_samples)) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::invalid_argument,
            .message = "audio seek target is outside the decoder sample range",
            .context = {{.key = "path", .value = decoder.raw_path},
                        {.key = "target_sample", .value = std::to_string(target_sample)}},
        });
    }
    if (decoder.cancellation.is_cancellation_requested()) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::cancelled,
            .message = "seeking local audio was cancelled",
            .context = {{.key = "path", .value = decoder.raw_path}},
        });
    }
    // Establish the physical stream's decoded origin before a first seek.
    // Some containers expose coarse timestamps; later frames are counted in
    // contiguous PCM rather than independently rounded from those stamps.
    if (target_sample > decoder.seek_preroll_samples && !decoder.timeline_origin_sample) {
        auto primed = next_chunk();
        if (!primed) {
            return std::unexpected(std::move(primed.error()));
        }
    }
    decoder.range_finished =
        (decoder.range.end_sample && target_sample == *decoder.range.end_sample) ||
        (decoder.total_samples && target_sample == *decoder.total_samples);
    if (decoder.range_finished) {
        decoder.discard_before_sample = target_sample;
        return {};
    }

    decoder.discard_before_sample = target_sample;

    auto* stream = decoder.format->streams[static_cast<unsigned>(decoder.stream_index)];
    const auto seek_sample = target_sample - decoder.seek_preroll_samples;
    const auto index_count = avformat_index_get_entries_count(stream);
    const auto* first_index = index_count > 0 ? avformat_index_get_entry(stream, 0) : nullptr;
    auto timestamp = first_index == nullptr ? AV_NOPTS_VALUE : first_index->timestamp;
    if (seek_sample >= 0 || timestamp == AV_NOPTS_VALUE) {
        timestamp = av_rescale_q(std::max(std::int64_t{0}, seek_sample),
                                 AVRational{.num = 1, .den = decoder.output.sample_rate},
                                 stream->time_base);
        if (stream->start_time != AV_NOPTS_VALUE && seek_sample >= 0) {
            timestamp += stream->start_time;
        }
    }
    const auto seek_result = avformat_seek_file(decoder.format, decoder.stream_index,
                                                std::numeric_limits<std::int64_t>::min(), timestamp,
                                                timestamp, AVSEEK_FLAG_BACKWARD);
    if (seek_result < 0) {
        return std::unexpected(decoder_error(seek_result, "seeking local audio", decoder.raw_path,
                                             decoder.cancellation));
    }
    avcodec_flush_buffers(decoder.codec);
    av_packet_unref(decoder.packet);
    av_frame_unref(decoder.frame);
    swr_close(decoder.resampler);
    const auto resampler_result = swr_init(decoder.resampler);
    if (resampler_result < 0) {
        return std::unexpected(decoder_error(resampler_result,
                                             "resetting PCM conversion after seek",
                                             decoder.raw_path, decoder.cancellation));
    }
    decoder.decoded_samples = target_sample + decoder.timeline_origin_sample.value_or(0);
    decoder.next_decoded_sample.reset();
    decoder.next_output_sample = target_sample;
    decoder.input_finished = false;
    decoder.drain_sent = false;
    return {};
}

bool AudioDecoder::reopen_if_replaced() {
    auto& decoder = *implementation_;
    // A few times a track: a file rewritten again and again is not chased.
    constexpr unsigned most_reopens = 3U;
    if (!decoder.identity || decoder.reopened >= most_reopens ||
        decoder.cancellation.is_cancellation_requested()) {
        return false;
    }
    struct stat now{};
    if (::stat(decoder.raw_path.c_str(), &now) != 0 ||
        (now.st_dev == decoder.identity->first && now.st_ino == decoder.identity->second)) {
        return false;
    }
    const auto resume = decoder.next_output_sample;
    const bool whole = decoder.range.start_sample == 0 && !decoder.range.end_sample;
    auto fresh = whole ? open_selected(decoder.raw_path, decoder.selection, decoder.cancellation)
                       : open_selected_segment(decoder.raw_path, decoder.selection, decoder.range,
                                               decoder.cancellation);
    if (!fresh) {
        return false;
    }
    // The same audio, its tags rewritten: on from the next sample.
    if (resume > fresh->implementation_->range.start_sample && !fresh->seek_to_sample(resume)) {
        return false;
    }
    const auto times = decoder.reopened + 1U;
    implementation_ = std::move(fresh->implementation_);
    implementation_->reopened = times;
    return true;
}

core::Result<std::optional<PcmChunk>> AudioDecoder::next_chunk() {
    auto& decoder = *implementation_;
    while (true) {
        if (decoder.cancellation.is_cancellation_requested()) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::cancelled,
                .message = "decoding local audio was cancelled",
                .context = {{.key = "path", .value = decoder.raw_path}},
            });
        }
        if (decoder.range_finished) {
            return std::optional<PcmChunk>{};
        }
        const auto receive_result = avcodec_receive_frame(decoder.codec, decoder.frame);
        if (receive_result >= 0) {
            auto converted = decoder.convert_frame();
            if (!converted || !*converted) {
                return converted;
            }
            if (decoder.trim_to_range(**converted)) {
                auto& chunk = **converted;
                const auto frame_count =
                    static_cast<std::int64_t>(chunk.frame_count(decoder.output.channels));
                // Frame timestamps locate seek preroll but are not a PCM
                // sequence. Vorbis in particular can expose adjacent
                // best-effort timestamps that overlap or leave a gap while
                // their decoded samples remain continuous.
                chunk.start_sample = decoder.next_output_sample;
                decoder.next_output_sample += frame_count;
                return converted;
            }
            if (decoder.range_finished) {
                return std::optional<PcmChunk>{};
            }
            continue;
        }
        if (receive_result == AVERROR_EOF) {
            return std::optional<PcmChunk>{};
        }
        if (receive_result != AVERROR(EAGAIN)) {
            if (reopen_if_replaced()) {
                return next_chunk();
            }
            return std::unexpected(decoder_error(receive_result, "receiving decoded audio",
                                                 decoder.raw_path, decoder.cancellation));
        }

        if (decoder.input_finished) {
            if (!decoder.drain_sent) {
                const auto send_result = avcodec_send_packet(decoder.codec, nullptr);
                decoder.drain_sent = true;
                if (send_result < 0 && send_result != AVERROR_EOF) {
                    return std::unexpected(decoder_error(send_result, "draining the audio decoder",
                                                         decoder.raw_path, decoder.cancellation));
                }
                continue;
            }
            return std::optional<PcmChunk>{};
        }

        auto read_result = 0;
        do {
            av_packet_unref(decoder.packet);
            read_result = av_read_frame(decoder.format, decoder.packet);
        } while (read_result >= 0 && decoder.packet->stream_index != decoder.stream_index);
        if (read_result == AVERROR_EOF) {
            // A read cut off by the file going reads as its end, too early.
            if (reopen_if_replaced()) {
                return next_chunk();
            }
            decoder.input_finished = true;
            continue;
        }
        if (read_result < 0) {
            if (reopen_if_replaced()) {
                return next_chunk();
            }
            return std::unexpected(decoder_error(read_result, "reading encoded audio",
                                                 decoder.raw_path, decoder.cancellation));
        }
        const auto send_result = avcodec_send_packet(decoder.codec, decoder.packet);
        av_packet_unref(decoder.packet);
        if (send_result < 0 && send_result != AVERROR(EAGAIN)) {
            if (reopen_if_replaced()) {
                return next_chunk();
            }
            return std::unexpected(decoder_error(send_result, "submitting encoded audio",
                                                 decoder.raw_path, decoder.cancellation));
        }
    }
}

} // namespace trackknife::formats

// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/convert/convert.hpp"

#include "trackknife/core/atomic_rename.hpp"
#include "trackknife/core/stable_id.hpp"
#include "trackknife/formats/artwork.hpp"
#include "trackknife/formats/probe.hpp"
#include "trackknife/metadata/local_reader.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

#include <libavutil/audio_fifo.h>
#include <libavutil/base64.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#include <taglib/fileref.h>
#include <taglib/tpropertymap.h>

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace trackknife::convert {
namespace {

[[nodiscard]] std::string ffmpeg_error(const int code) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
    if (av_strerror(code, buffer.data(), buffer.size()) < 0) {
        return "unknown FFmpeg error " + std::to_string(code);
    }
    return buffer.data();
}

[[nodiscard]] core::Error convert_error(const int code, const std::string& operation,
                                        const std::string& raw_path) {
    return core::Error{
        .code = core::ErrorCode::backend,
        .message = operation + " failed: " + ffmpeg_error(code),
        .context = {{.key = "path", .value = raw_path},
                    {.key = "ffmpeg_error", .value = std::to_string(code)}},
    };
}

[[nodiscard]] core::Error cancelled_error(const std::string& raw_path) {
    return core::Error{.code = core::ErrorCode::cancelled,
                       .message = "audio conversion was cancelled",
                       .context = {{.key = "path", .value = raw_path}}};
}

// Removes the hidden temporary on every non-renamed exit so failure and
// cancellation leave nothing behind.
class TemporaryOutputGuard final {
  public:
    explicit TemporaryOutputGuard(std::filesystem::path path) : path_{std::move(path)} {}
    TemporaryOutputGuard(const TemporaryOutputGuard&) = delete;
    TemporaryOutputGuard& operator=(const TemporaryOutputGuard&) = delete;
    ~TemporaryOutputGuard() {
        if (armed_) {
            std::error_code error;
            std::filesystem::remove(path_, error);
        }
    }
    void disarm() noexcept { armed_ = false; }

  private:
    std::filesystem::path path_;
    bool armed_{true};
};

struct EncoderPipeline {
    AVFormatContext* format{nullptr};
    AVCodecContext* codec{nullptr};
    SwrContext* resampler{nullptr};
    AVAudioFifo* fifo{nullptr};
    AVFrame* encode_frame{nullptr};
    AVFrame* convert_frame{nullptr};
    AVPacket* packet{nullptr};
    AVStream* stream{nullptr};
    AVStream* picture_stream{nullptr};
    std::int64_t next_pts{0};

    EncoderPipeline() = default;
    EncoderPipeline(const EncoderPipeline&) = delete;
    EncoderPipeline& operator=(const EncoderPipeline&) = delete;
    ~EncoderPipeline() {
        av_packet_free(&packet);
        av_frame_free(&convert_frame);
        av_frame_free(&encode_frame);
        if (fifo != nullptr) {
            av_audio_fifo_free(fifo);
        }
        swr_free(&resampler);
        avcodec_free_context(&codec);
        if (format != nullptr) {
            if (format->pb != nullptr) {
                avio_closep(&format->pb);
            }
            avformat_free_context(format);
        }
    }
};

[[nodiscard]] const AVSampleFormat* supported_sample_formats(const AVCodec* codec) {
    const void* configs = nullptr;
    int count = 0;
    if (avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0U, &configs,
                                     &count) < 0) {
        return nullptr;
    }
    return static_cast<const AVSampleFormat*>(configs);
}

[[nodiscard]] const int* supported_sample_rates(const AVCodec* codec) {
    const void* configs = nullptr;
    int count = 0;
    if (avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_SAMPLE_RATE, 0U, &configs,
                                     &count) < 0) {
        return nullptr;
    }
    return static_cast<const int*>(configs);
}

[[nodiscard]] AVSampleFormat choose_sample_format(const AVCodec* codec,
                                                  const std::string& hint_name,
                                                  const std::optional<int>& target_bit_depth) {
    const auto* const supported = supported_sample_formats(codec);
    const auto hint = hint_name.empty() ? AV_SAMPLE_FMT_NONE : av_get_sample_fmt(hint_name.c_str());
    if (supported == nullptr) {
        return hint != AV_SAMPLE_FMT_NONE ? hint : AV_SAMPLE_FMT_FLT;
    }
    const auto is_supported = [supported](const AVSampleFormat format) {
        for (const auto* entry = supported; *entry != AV_SAMPLE_FMT_NONE; ++entry) {
            if (*entry == format) {
                return true;
            }
        }
        return false;
    };
    // A requested bit depth outranks the preset hint where the encoder
    // stores integer PCM; encoders without integer formats fall through.
    if (target_bit_depth) {
        const auto packed = *target_bit_depth == 16 ? AV_SAMPLE_FMT_S16 : AV_SAMPLE_FMT_S32;
        const auto planar = *target_bit_depth == 16 ? AV_SAMPLE_FMT_S16P : AV_SAMPLE_FMT_S32P;
        if (is_supported(packed)) {
            return packed;
        }
        if (is_supported(planar)) {
            return planar;
        }
    }
    if (hint != AV_SAMPLE_FMT_NONE && is_supported(hint)) {
        return hint;
    }
    for (const auto preferred : {AV_SAMPLE_FMT_FLT, AV_SAMPLE_FMT_FLTP, AV_SAMPLE_FMT_S32,
                                 AV_SAMPLE_FMT_S32P, AV_SAMPLE_FMT_S16, AV_SAMPLE_FMT_S16P}) {
        if (is_supported(preferred)) {
            return preferred;
        }
    }
    return supported[0];
}

// The source rate when the encoder accepts it, otherwise the smallest
// supported rate at or above the source (so Opus resamples 44.1 kHz
// material to 48 kHz, never down).
[[nodiscard]] int choose_sample_rate(const AVCodec* codec, const int source_rate) {
    const auto* const supported = supported_sample_rates(codec);
    if (supported == nullptr) {
        return source_rate;
    }
    int best_above = 0;
    int best_any = 0;
    for (const auto* entry = supported; *entry != 0; ++entry) {
        const auto rate = *entry;
        if (rate == source_rate) {
            return rate;
        }
        if (rate > source_rate && (best_above == 0 || rate < best_above)) {
            best_above = rate;
        }
        best_any = std::max(best_any, rate);
    }
    return best_above != 0 ? best_above : best_any;
}

[[nodiscard]] core::Result<void> drain_packets(EncoderPipeline& pipeline,
                                               const std::string& raw_path) {
    while (true) {
        const auto receive = avcodec_receive_packet(pipeline.codec, pipeline.packet);
        if (receive == AVERROR(EAGAIN) || receive == AVERROR_EOF) {
            return {};
        }
        if (receive < 0) {
            return std::unexpected(convert_error(receive, "receiving encoded audio", raw_path));
        }
        pipeline.packet->stream_index = pipeline.stream->index;
        av_packet_rescale_ts(pipeline.packet, pipeline.codec->time_base,
                             pipeline.stream->time_base);
        const auto write = av_interleaved_write_frame(pipeline.format, pipeline.packet);
        if (write < 0) {
            return std::unexpected(convert_error(write, "writing encoded audio", raw_path));
        }
    }
}

// Encodes buffered samples in encoder-frame-sized chunks; when finishing,
// the remainder becomes one final short frame.
[[nodiscard]] core::Result<void> encode_buffered(EncoderPipeline& pipeline,
                                                 const std::string& raw_path, const int frame_size,
                                                 const bool finishing) {
    while (true) {
        const auto buffered = av_audio_fifo_size(pipeline.fifo);
        if (buffered < frame_size && !(finishing && buffered > 0)) {
            return {};
        }
        const auto take = std::min(buffered, frame_size);
        if (const auto writable = av_frame_make_writable(pipeline.encode_frame); writable < 0) {
            return std::unexpected(convert_error(writable, "preparing an encoder frame", raw_path));
        }
        pipeline.encode_frame->nb_samples = take;
        const auto read = av_audio_fifo_read(
            pipeline.fifo, reinterpret_cast<void**>(pipeline.encode_frame->data), take);
        if (read < take) {
            return std::unexpected(convert_error(read < 0 ? read : AVERROR(EINVAL),
                                                 "reading buffered audio", raw_path));
        }
        pipeline.encode_frame->pts = pipeline.next_pts;
        pipeline.next_pts += take;
        const auto sent = avcodec_send_frame(pipeline.codec, pipeline.encode_frame);
        if (sent < 0) {
            return std::unexpected(convert_error(sent, "encoding audio", raw_path));
        }
        if (auto drained = drain_packets(pipeline, raw_path); !drained) {
            return drained;
        }
    }
}

// Runs converted samples through the FIFO so encoders with fixed frame
// sizes always see whole frames.
[[nodiscard]] core::Result<void>
buffer_converted(EncoderPipeline& pipeline, const std::string& raw_path, const int converted) {
    if (converted <= 0) {
        return {};
    }
    const auto written = av_audio_fifo_write(
        pipeline.fifo, reinterpret_cast<void**>(pipeline.convert_frame->data), converted);
    if (written < converted) {
        return std::unexpected(convert_error(written < 0 ? written : AVERROR(EINVAL),
                                             "buffering converted audio", raw_path));
    }
    return {};
}

[[nodiscard]] core::Result<void>
ensure_convert_capacity(EncoderPipeline& pipeline, const std::string& raw_path, const int needed) {
    if (pipeline.convert_frame->nb_samples >= needed) {
        return {};
    }
    av_frame_unref(pipeline.convert_frame);
    pipeline.convert_frame->format = pipeline.codec->sample_fmt;
    pipeline.convert_frame->sample_rate = pipeline.codec->sample_rate;
    if (const auto copied =
            av_channel_layout_copy(&pipeline.convert_frame->ch_layout, &pipeline.codec->ch_layout);
        copied < 0) {
        return std::unexpected(convert_error(copied, "sizing the conversion buffer", raw_path));
    }
    pipeline.convert_frame->nb_samples = std::max(needed, 8192);
    if (const auto allocated = av_frame_get_buffer(pipeline.convert_frame, 0); allocated < 0) {
        return std::unexpected(convert_error(allocated, "sizing the conversion buffer", raw_path));
    }
    return {};
}

[[nodiscard]] core::Result<void> synchronize_and_rename(const std::filesystem::path& temporary,
                                                        const std::filesystem::path& destination,
                                                        const std::string& raw_path) {
    const auto file = ::open(temporary.c_str(), O_RDONLY | O_CLOEXEC);
    if (file < 0 || ::fsync(file) != 0) {
        const auto saved = errno;
        if (file >= 0) {
            ::close(file);
        }
        return std::unexpected(
            core::Error{.code = core::ErrorCode::io,
                        .message = std::string{"synchronizing the converted file failed: "} +
                                   std::generic_category().message(saved),
                        .context = {{.key = "path", .value = raw_path}}});
    }
    ::close(file);
    if (auto published = core::publish_no_replace_at(AT_FDCWD, temporary.native(), AT_FDCWD,
                                                     destination.native());
        !published) {
        return std::unexpected(std::move(published.error()).with_context("path", raw_path));
    }
    const auto directory = ::open(destination.parent_path().c_str(), O_RDONLY | O_DIRECTORY);
    if (directory >= 0) {
        ::fsync(directory);
        ::close(directory);
    }
    return {};
}

// Loudness claims measured against the source signal are stale after any
// re-encode; conversion strips them instead of transferring them
// (ADR-0133). Canonical names have separators removed and are lowercase.
[[nodiscard]] bool is_loudness_field(const std::string_view canonical_name) {
    return canonical_name.starts_with("replaygain") || canonical_name == "r128trackgain" ||
           canonical_name == "r128albumgain";
}

[[nodiscard]] metadata::MetadataDocument
strip_loudness_fields(const metadata::MetadataDocument& document) {
    metadata::MetadataDocument stripped;
    stripped.fields.reserve(document.fields.size());
    for (const auto& field : document.fields) {
        if (!is_loudness_field(field.canonical_name)) {
            stripped.fields.push_back(field);
        }
    }
    stripped.unsupported_native_objects = document.unsupported_native_objects;
    return stripped;
}

// FFmpeg's shared FLAC/ID3v2 picture-type vocabulary: the muxers map an
// attached-picture stream's "comment" metadata onto the numeric type
// through exactly these strings.
[[nodiscard]] const char* picture_type_name(const std::uint32_t picture_type) {
    static constexpr std::array names{"Other",
                                      "32x32 pixels 'file icon'",
                                      "Other file icon",
                                      "Cover (front)",
                                      "Cover (back)",
                                      "Leaflet page",
                                      "Media (e.g. label side of CD)",
                                      "Lead artist/lead performer/soloist",
                                      "Artist/performer",
                                      "Conductor",
                                      "Band/Orchestra",
                                      "Composer",
                                      "Lyricist/text writer",
                                      "Recording Location",
                                      "During recording",
                                      "During performance",
                                      "Movie/video screen capture",
                                      "A bright coloured fish",
                                      "Illustration",
                                      "Band/artist logotype",
                                      "Publisher/Studio logotype"};
    return picture_type < names.size() ? names[picture_type] : names[3];
}

// The FLAC PICTURE structure that Vorbis comments carry base64-encoded as
// METADATA_BLOCK_PICTURE (the standard Ogg cover-art mapping).
[[nodiscard]] std::string metadata_block_picture(const ConversionArtwork& artwork) {
    std::vector<unsigned char> block;
    block.reserve(32U + artwork.mime_type.size() + artwork.description.size() +
                  artwork.bytes.size());
    const auto append_be32 = [&block](const std::uint32_t value) {
        block.push_back(static_cast<unsigned char>(value >> 24U));
        block.push_back(static_cast<unsigned char>(value >> 16U));
        block.push_back(static_cast<unsigned char>(value >> 8U));
        block.push_back(static_cast<unsigned char>(value));
    };
    append_be32(artwork.picture_type);
    append_be32(static_cast<std::uint32_t>(artwork.mime_type.size()));
    block.insert(block.end(), artwork.mime_type.begin(), artwork.mime_type.end());
    append_be32(static_cast<std::uint32_t>(artwork.description.size()));
    block.insert(block.end(), artwork.description.begin(), artwork.description.end());
    append_be32(artwork.width);
    append_be32(artwork.height);
    append_be32(0U); // Color depth and palette size are unknown for
    append_be32(0U); // encoded PNG/JPEG payloads; zero is the defined "n/a".
    append_be32(static_cast<std::uint32_t>(artwork.bytes.size()));
    block.insert(block.end(), artwork.bytes.begin(), artwork.bytes.end());

    std::string encoded;
    encoded.resize(static_cast<std::size_t>(AV_BASE64_SIZE(block.size())));
    av_base64_encode(encoded.data(), static_cast<int>(encoded.size()), block.data(),
                     static_cast<int>(block.size()));
    encoded.resize(std::strlen(encoded.c_str()));
    return encoded;
}

// Declares the cover image before the header is written: FLAC and MP3 get a
// real attached-picture stream (their muxers turn it into a PICTURE block or
// APIC frame), Opus and Vorbis get the METADATA_BLOCK_PICTURE comment.
[[nodiscard]] core::Result<void> apply_request_artwork(EncoderPipeline& pipeline,
                                                       const EncoderPreset& preset,
                                                       const ConversionArtwork& artwork,
                                                       const std::string& raw_path) {
    if (artwork.bytes.empty() ||
        (artwork.mime_type != "image/png" && artwork.mime_type != "image/jpeg")) {
        return std::unexpected(
            core::Error{.code = core::ErrorCode::invalid_argument,
                        .message = "conversion artwork requires PNG or JPEG bytes",
                        .context = {{.key = "path", .value = raw_path}}});
    }
    if (preset.container_name == "opus" || preset.container_name == "ogg") {
        if (const auto set = av_dict_set(&pipeline.format->metadata, "METADATA_BLOCK_PICTURE",
                                         metadata_block_picture(artwork).c_str(), 0);
            set < 0) {
            return std::unexpected(convert_error(set, "recording the cover comment", raw_path));
        }
        return {};
    }

    auto* stream = avformat_new_stream(pipeline.format, nullptr);
    if (stream == nullptr) {
        return std::unexpected(
            convert_error(AVERROR(ENOMEM), "creating the cover stream", raw_path));
    }
    stream->disposition |= AV_DISPOSITION_ATTACHED_PIC;
    stream->time_base = AVRational{1, 90'000};
    stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    stream->codecpar->codec_id =
        artwork.mime_type == "image/png" ? AV_CODEC_ID_PNG : AV_CODEC_ID_MJPEG;
    stream->codecpar->width = static_cast<int>(artwork.width);
    stream->codecpar->height = static_cast<int>(artwork.height);
    if (av_dict_set(&stream->metadata, "comment", picture_type_name(artwork.picture_type), 0) < 0) {
        return std::unexpected(
            convert_error(AVERROR(ENOMEM), "recording the cover picture type", raw_path));
    }
    if (!artwork.description.empty() &&
        av_dict_set(&stream->metadata, "title", artwork.description.c_str(), 0) < 0) {
        return std::unexpected(
            convert_error(AVERROR(ENOMEM), "recording the cover description", raw_path));
    }
    pipeline.picture_stream = stream;
    return {};
}

// Sends the single cover packet right after the header so the muxer sees it
// before any audio.
[[nodiscard]] core::Result<void> write_attached_picture(EncoderPipeline& pipeline,
                                                        const ConversionArtwork& artwork,
                                                        const std::string& raw_path) {
    AVPacket* packet = av_packet_alloc();
    if (packet == nullptr || av_new_packet(packet, static_cast<int>(artwork.bytes.size())) < 0) {
        av_packet_free(&packet);
        return std::unexpected(
            convert_error(AVERROR(ENOMEM), "allocating the cover packet", raw_path));
    }
    std::memcpy(packet->data, artwork.bytes.data(), artwork.bytes.size());
    packet->stream_index = pipeline.picture_stream->index;
    packet->pts = 0;
    packet->dts = 0;
    packet->duration = 0;
    packet->flags |= AV_PKT_FLAG_KEY;
    const auto written = av_interleaved_write_frame(pipeline.format, packet);
    av_packet_free(&packet);
    if (written < 0) {
        return std::unexpected(convert_error(written, "writing the cover image", raw_path));
    }
    return {};
}

// The embedded cover must reread from the finished file byte-exactly, the
// same honesty verify_written_metadata proves for text.
[[nodiscard]] core::Result<void> verify_written_artwork(const std::string& temporary_path,
                                                        const ConversionArtwork& artwork,
                                                        const std::string& raw_path) {
    auto reread = formats::load_embedded_artwork(temporary_path);
    if (!reread) {
        return std::unexpected(
            std::move(reread.error()).with_context("verify", "rereading converted artwork failed"));
    }
    if (*reread != artwork.bytes) {
        return std::unexpected(
            core::Error{.code = core::ErrorCode::invariant,
                        .message = "converted artwork reread differs from the request",
                        .context = {{.key = "path", .value = raw_path}}});
    }
    return {};
}

// ADR-0155: FFmpeg writes audio; the finished file's text tags come
// from the tag layer as one property map — every effective field under
// its exact native name with all values — so no muxer key-conversion
// table can rename or drop anything. Pictures live outside the
// property view and stay untouched.
[[nodiscard]] core::Result<void> write_transfer_tags(const std::string& temporary_path,
                                                     const metadata::MetadataDocument& document,
                                                     const std::string& raw_path) {
    TagLib::FileRef file{temporary_path.c_str()};
    if (file.isNull()) {
        return std::unexpected(
            core::Error{.code = core::ErrorCode::io,
                        .message = "reopening the converted file for tag writing failed",
                        .context = {{.key = "path", .value = raw_path}}});
    }
    TagLib::PropertyMap properties;
    for (const auto& field : document.effective_fields()) {
        if (field.values.empty()) {
            continue;
        }
        const auto& name = field.native_name.empty() ? field.canonical_name : field.native_name;
        TagLib::StringList values;
        for (const auto& value : field.values) {
            values.append(TagLib::String{value, TagLib::String::UTF8});
        }
        properties.insert(TagLib::String{name, TagLib::String::UTF8}.upper(), values);
    }
    file.setProperties(properties);
    if (!file.save()) {
        return std::unexpected(core::Error{.code = core::ErrorCode::io,
                                           .message = "writing the converted tags failed",
                                           .context = {{.key = "path", .value = raw_path}}});
    }
    return {};
}

// Every requested field must reread from the finished file with exactly the
// requested values — the same honesty the qualified tag writers prove.
[[nodiscard]] core::Result<void> verify_written_metadata(const std::string& temporary_path,
                                                         const metadata::MetadataDocument& document,
                                                         const std::string& raw_path) {
    const auto reread = metadata::read_local_metadata(temporary_path);
    if (!reread) {
        return std::unexpected(std::move(core::Error{reread.error()})
                                   .with_context("verify", "rereading converted metadata failed"));
    }
    for (const auto& field : document.effective_fields()) {
        const auto values = reread->document.effective_values(field.canonical_name);
        if (values != field.values) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::invariant,
                .message = "converted metadata reread differs for " + field.canonical_name,
                .context = {{.key = "path", .value = raw_path},
                            {.key = "field", .value = field.canonical_name}}});
        }
    }
    // Loudness fields were stripped before the transfer; none may survive.
    for (const auto& field : reread->document.effective_fields()) {
        if (is_loudness_field(field.canonical_name)) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::invariant,
                .message = "stale ReplayGain survived the conversion as " + field.canonical_name,
                .context = {{.key = "path", .value = raw_path},
                            {.key = "field", .value = field.canonical_name}}});
        }
    }
    return {};
}

[[nodiscard]] core::Result<std::unique_ptr<EncoderPipeline>>
open_pipeline(const EncoderPreset& preset, const formats::PcmFormat& source_format,
              const std::optional<int>& target_sample_rate,
              const std::optional<int>& target_bit_depth,
              const std::optional<ConversionArtwork>& artwork, const std::string& temporary_path,
              const std::string& destination_raw_path) {
    const auto* const codec = avcodec_find_encoder_by_name(preset.codec_name.c_str());
    if (codec == nullptr) {
        return std::unexpected(core::Error{.code = core::ErrorCode::unsupported,
                                           .message = "encoder " + preset.codec_name +
                                                      " is not built into this FFmpeg",
                                           .context = {{.key = "preset", .value = preset.id}}});
    }

    auto pipeline = std::make_unique<EncoderPipeline>();
    if (const auto allocated = avformat_alloc_output_context2(
            &pipeline->format, nullptr, preset.container_name.c_str(), temporary_path.c_str());
        allocated < 0) {
        return std::unexpected(
            convert_error(allocated, "opening the output container", destination_raw_path));
    }

    pipeline->codec = avcodec_alloc_context3(codec);
    if (pipeline->codec == nullptr) {
        return std::unexpected(
            convert_error(AVERROR(ENOMEM), "allocating the encoder context", destination_raw_path));
    }
    pipeline->codec->sample_fmt =
        choose_sample_format(codec, preset.sample_format_hint, target_bit_depth);
    pipeline->codec->sample_rate =
        choose_sample_rate(codec, target_sample_rate.value_or(source_format.sample_rate));
    pipeline->codec->time_base = AVRational{1, pipeline->codec->sample_rate};
    av_channel_layout_default(&pipeline->codec->ch_layout, source_format.channels);
    if (preset.bit_rate) {
        pipeline->codec->bit_rate = *preset.bit_rate;
    }
    if (preset.vbr_quality) {
        pipeline->codec->flags |= AV_CODEC_FLAG_QSCALE;
        pipeline->codec->global_quality = *preset.vbr_quality * FF_QP2LAMBDA;
    }
    if ((pipeline->format->oformat->flags & AVFMT_GLOBALHEADER) != 0) {
        pipeline->codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    if (const auto opened = avcodec_open2(pipeline->codec, codec, nullptr); opened < 0) {
        return std::unexpected(convert_error(opened, "opening the encoder", destination_raw_path));
    }

    pipeline->stream = avformat_new_stream(pipeline->format, nullptr);
    if (pipeline->stream == nullptr) {
        return std::unexpected(
            convert_error(AVERROR(ENOMEM), "creating the output stream", destination_raw_path));
    }
    pipeline->stream->time_base = pipeline->codec->time_base;
    if (const auto copied =
            avcodec_parameters_from_context(pipeline->stream->codecpar, pipeline->codec);
        copied < 0) {
        return std::unexpected(
            convert_error(copied, "describing the output stream", destination_raw_path));
    }

    AVChannelLayout source_layout{};
    av_channel_layout_default(&source_layout, source_format.channels);
    const auto swr_configured = swr_alloc_set_opts2(
        &pipeline->resampler, &pipeline->codec->ch_layout, pipeline->codec->sample_fmt,
        pipeline->codec->sample_rate, &source_layout, AV_SAMPLE_FMT_FLT, source_format.sample_rate,
        0, nullptr);
    av_channel_layout_uninit(&source_layout);
    // Quantizing the float pipeline down to 16-bit gets high-passed
    // triangular dither; 24-bit output is exact from 32-bit float.
    if (swr_configured >= 0 &&
        av_get_packed_sample_fmt(pipeline->codec->sample_fmt) == AV_SAMPLE_FMT_S16) {
        av_opt_set(pipeline->resampler, "dither_method", "triangular_hp", 0);
    }
    if (swr_configured < 0 || swr_init(pipeline->resampler) < 0) {
        return std::unexpected(
            convert_error(swr_configured, "configuring the resampler", destination_raw_path));
    }

    pipeline->fifo = av_audio_fifo_alloc(pipeline->codec->sample_fmt,
                                         pipeline->codec->ch_layout.nb_channels, 8192);
    pipeline->encode_frame = av_frame_alloc();
    pipeline->convert_frame = av_frame_alloc();
    pipeline->packet = av_packet_alloc();
    if (pipeline->fifo == nullptr || pipeline->encode_frame == nullptr ||
        pipeline->convert_frame == nullptr || pipeline->packet == nullptr) {
        return std::unexpected(
            convert_error(AVERROR(ENOMEM), "allocating conversion buffers", destination_raw_path));
    }

    const auto frame_size = pipeline->codec->frame_size > 0 ? pipeline->codec->frame_size : 4096;
    pipeline->encode_frame->format = pipeline->codec->sample_fmt;
    pipeline->encode_frame->sample_rate = pipeline->codec->sample_rate;
    pipeline->encode_frame->nb_samples = frame_size;
    if (av_channel_layout_copy(&pipeline->encode_frame->ch_layout, &pipeline->codec->ch_layout) <
            0 ||
        av_frame_get_buffer(pipeline->encode_frame, 0) < 0) {
        return std::unexpected(
            convert_error(AVERROR(ENOMEM), "allocating the encoder frame", destination_raw_path));
    }

    // ADR-0155: text tags are written by the tag layer after encoding;
    // the muxer dictionary stays empty of transfer metadata.
    if (artwork) {
        if (auto cover = apply_request_artwork(*pipeline, preset, *artwork, destination_raw_path);
            !cover) {
            return std::unexpected(cover.error());
        }
    }
    if (const auto opened =
            avio_open(&pipeline->format->pb, temporary_path.c_str(), AVIO_FLAG_WRITE);
        opened < 0) {
        return std::unexpected(
            convert_error(opened, "creating the output file", destination_raw_path));
    }
    if (const auto header = avformat_write_header(pipeline->format, nullptr); header < 0) {
        return std::unexpected(
            convert_error(header, "writing the container header", destination_raw_path));
    }
    if (pipeline->picture_stream != nullptr) {
        if (auto written = write_attached_picture(*pipeline, *artwork, destination_raw_path);
            !written) {
            return std::unexpected(written.error());
        }
    }
    return pipeline;
}

} // namespace

core::Result<ConvertedAudioFile> convert_audio_file(const AudioConversionRequest& request,
                                                    const ConversionProgress& progress,
                                                    const core::CancellationToken& cancellation) {
    const std::filesystem::path destination{request.destination_raw_path};
    std::error_code exists_error;
    if (std::filesystem::exists(destination, exists_error)) {
        return std::unexpected(
            core::Error{.code = core::ErrorCode::conflict,
                        .message = "the conversion destination already exists",
                        .context = {{.key = "path", .value = request.destination_raw_path}}});
    }
    if (request.target_sample_rate && request.sample_rate_cap) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::invalid_argument,
            .message = "a forced sample rate and a downsample-only cap are mutually exclusive",
            .context = {{.key = "path", .value = request.destination_raw_path}}});
    }
    for (const auto& rate : {request.target_sample_rate, request.sample_rate_cap}) {
        if (rate && (*rate < 8'000 || *rate > 768'000)) {
            return std::unexpected(
                core::Error{.code = core::ErrorCode::invalid_argument,
                            .message = "the requested sample rate must be between 8 and 768 kHz",
                            .context = {{.key = "path", .value = request.destination_raw_path}}});
        }
    }
    if (request.target_bit_depth && request.keep_source_bit_depth) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::invalid_argument,
            .message = "a forced bit depth and keep-source depth are mutually exclusive",
            .context = {{.key = "path", .value = request.destination_raw_path}}});
    }
    if (request.target_bit_depth &&
        (*request.target_bit_depth != 16 && *request.target_bit_depth != 24)) {
        return std::unexpected(
            core::Error{.code = core::ErrorCode::invalid_argument,
                        .message = "the requested bit depth must be 16 or 24",
                        .context = {{.key = "path", .value = request.destination_raw_path}}});
    }
    // Keep-source depth reads the stored format from the probe: at most 16
    // stored bits keeps 16, everything else — including float and unknown —
    // keeps 24, the pipeline's maximum stored depth (ADR-0134).
    std::optional<int> effective_bit_depth = request.target_bit_depth;
    if (request.keep_source_bit_depth) {
        auto probed = formats::probe_local_media(request.source_raw_path, cancellation);
        if (!probed) {
            return std::unexpected(
                std::move(probed.error()).with_context("probe", "reading the stored bit depth"));
        }
        std::string stored_format;
        if (probed->best_audio_stream) {
            stored_format =
                probed->audio_streams[static_cast<std::size_t>(*probed->best_audio_stream)]
                    .sample_format;
        }
        effective_bit_depth =
            stored_format.starts_with("u8") || stored_format.starts_with("s16") ? 16 : 24;
    }
    std::error_code parent_error;
    if (!std::filesystem::is_directory(destination.parent_path(), parent_error)) {
        return std::unexpected(
            core::Error{.code = core::ErrorCode::invalid_argument,
                        .message = "the conversion destination directory does not exist",
                        .context = {{.key = "path", .value = request.destination_raw_path}}});
    }

    auto decoder =
        request.source_range
            ? formats::AudioDecoder::open_selected_segment(request.source_raw_path,
                                                           request.source_selection,
                                                           *request.source_range, cancellation)
            : formats::AudioDecoder::open_selected(request.source_raw_path,
                                                   request.source_selection, cancellation);
    if (!decoder) {
        return std::unexpected(decoder.error());
    }
    const auto source_format = decoder->output_format();
    const auto expected_source_frames = decoder->duration_samples();

    const auto temporary =
        destination.parent_path() / ("." + destination.filename().native() + ".tk-part-" +
                                     core::StableId::random().to_string());
    TemporaryOutputGuard guard{temporary};

    // The cap only ever lowers: sources at or below it keep their rate.
    std::optional<int> effective_sample_rate = request.target_sample_rate;
    if (request.sample_rate_cap && source_format.sample_rate > *request.sample_rate_cap) {
        effective_sample_rate = request.sample_rate_cap;
    }

    const auto transfer_metadata = strip_loudness_fields(request.metadata);
    auto pipeline_result =
        open_pipeline(request.preset, source_format, effective_sample_rate, effective_bit_depth,
                      request.artwork, temporary.native(), request.destination_raw_path);
    if (!pipeline_result) {
        return std::unexpected(pipeline_result.error());
    }
    auto& pipeline = **pipeline_result;
    const auto frame_size = pipeline.codec->frame_size > 0 ? pipeline.codec->frame_size : 4096;

    std::uint64_t decoded_frames = 0U;
    while (true) {
        if (cancellation.is_cancellation_requested()) {
            return std::unexpected(cancelled_error(request.source_raw_path));
        }
        auto chunk = decoder->next_chunk();
        if (!chunk) {
            return std::unexpected(chunk.error());
        }
        if (!*chunk) {
            break;
        }
        const auto frames = static_cast<int>((*chunk)->frame_count(source_format.channels));
        if (frames == 0) {
            continue;
        }
        decoded_frames += static_cast<std::uint64_t>(frames);
        const auto capacity = static_cast<int>(swr_get_out_samples(pipeline.resampler, frames));
        if (auto sized = ensure_convert_capacity(pipeline, request.destination_raw_path,
                                                 std::max(capacity, frames));
            !sized) {
            return std::unexpected(sized.error());
        }
        const auto* input_plane =
            reinterpret_cast<const std::uint8_t*>((*chunk)->interleaved_samples.data());
        const auto converted =
            swr_convert(pipeline.resampler, pipeline.convert_frame->data,
                        pipeline.convert_frame->nb_samples, &input_plane, frames);
        if (converted < 0) {
            return std::unexpected(
                convert_error(converted, "resampling audio", request.source_raw_path));
        }
        if (auto buffered = buffer_converted(pipeline, request.destination_raw_path, converted);
            !buffered) {
            return std::unexpected(buffered.error());
        }
        if (auto encoded =
                encode_buffered(pipeline, request.destination_raw_path, frame_size, false);
            !encoded) {
            return std::unexpected(encoded.error());
        }
        if (progress) {
            progress(decoded_frames, expected_source_frames
                                         ? std::optional<std::uint64_t>{static_cast<std::uint64_t>(
                                               std::max<std::int64_t>(*expected_source_frames, 0))}
                                         : std::nullopt);
        }
    }

    // Drain the resampler's tail, then the FIFO remainder, then the encoder.
    while (true) {
        if (auto sized = ensure_convert_capacity(pipeline, request.destination_raw_path, 8192);
            !sized) {
            return std::unexpected(sized.error());
        }
        const auto converted = swr_convert(pipeline.resampler, pipeline.convert_frame->data,
                                           pipeline.convert_frame->nb_samples, nullptr, 0);
        if (converted < 0) {
            return std::unexpected(
                convert_error(converted, "draining the resampler", request.source_raw_path));
        }
        if (converted == 0) {
            break;
        }
        if (auto buffered = buffer_converted(pipeline, request.destination_raw_path, converted);
            !buffered) {
            return std::unexpected(buffered.error());
        }
    }
    if (auto encoded = encode_buffered(pipeline, request.destination_raw_path, frame_size, true);
        !encoded) {
        return std::unexpected(encoded.error());
    }
    if (const auto sent = avcodec_send_frame(pipeline.codec, nullptr); sent < 0) {
        return std::unexpected(
            convert_error(sent, "finishing the encoder", request.destination_raw_path));
    }
    if (auto drained = drain_packets(pipeline, request.destination_raw_path); !drained) {
        return std::unexpected(drained.error());
    }
    if (const auto trailer = av_write_trailer(pipeline.format); trailer < 0) {
        return std::unexpected(
            convert_error(trailer, "writing the container trailer", request.destination_raw_path));
    }
    const auto output_rate = pipeline.codec->sample_rate;
    const auto output_channels = pipeline.codec->ch_layout.nb_channels;
    if (const auto closed = avio_closep(&pipeline.format->pb); closed < 0) {
        return std::unexpected(
            convert_error(closed, "closing the output file", request.destination_raw_path));
    }

    // Verify the temporary end to end with the project decoder before it
    // may become the destination: the whole stream must decode, at the
    // negotiated format, to the expected duration.
    auto verify = formats::AudioDecoder::open(temporary.native(), cancellation);
    if (!verify) {
        return std::unexpected(std::move(verify.error())
                                   .with_context("verify", "reopening the converted file failed"));
    }
    if (verify->output_format().sample_rate != output_rate ||
        verify->output_format().channels != output_channels) {
        return std::unexpected(
            core::Error{.code = core::ErrorCode::invariant,
                        .message = "converted audio reread with an unexpected format",
                        .context = {{.key = "path", .value = request.destination_raw_path}}});
    }
    std::int64_t verified_frames = 0;
    while (true) {
        if (cancellation.is_cancellation_requested()) {
            return std::unexpected(cancelled_error(request.source_raw_path));
        }
        auto chunk = verify->next_chunk();
        if (!chunk) {
            return std::unexpected(
                std::move(chunk.error())
                    .with_context("verify", "decoding the converted file failed"));
        }
        if (!*chunk) {
            break;
        }
        verified_frames += static_cast<std::int64_t>((*chunk)->frame_count(output_channels));
    }
    const auto expected_output_frames = static_cast<std::int64_t>(std::llround(
        static_cast<double>(decoded_frames) * output_rate / source_format.sample_rate));
    const auto tolerance = std::max<std::int64_t>(output_rate / 5, 1);
    if (std::abs(verified_frames - expected_output_frames) > tolerance) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::invariant,
            .message = "converted audio duration differs from the source",
            .context = {{.key = "path", .value = request.destination_raw_path},
                        {.key = "expected_frames", .value = std::to_string(expected_output_frames)},
                        {.key = "verified_frames", .value = std::to_string(verified_frames)}}});
    }

    if (auto written = write_transfer_tags(temporary.native(), transfer_metadata,
                                           request.destination_raw_path);
        !written) {
        return std::unexpected(std::move(written.error()));
    }
    if (auto tags_verified = verify_written_metadata(temporary.native(), transfer_metadata,
                                                     request.destination_raw_path);
        !tags_verified) {
        return std::unexpected(tags_verified.error());
    }
    if (request.artwork) {
        if (auto artwork_verified = verify_written_artwork(temporary.native(), *request.artwork,
                                                           request.destination_raw_path);
            !artwork_verified) {
            return std::unexpected(artwork_verified.error());
        }
    }

    if (auto published =
            synchronize_and_rename(temporary, destination, request.destination_raw_path);
        !published) {
        return std::unexpected(published.error());
    }
    guard.disarm();
    return ConvertedAudioFile{
        .destination_raw_path = request.destination_raw_path,
        .sample_rate = output_rate,
        .channels = output_channels,
        .duration_samples = verified_frames,
    };
}

} // namespace trackknife::convert

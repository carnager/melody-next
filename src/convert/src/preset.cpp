// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/convert/preset.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libswresample/swresample.h>
}

#include <optional>

namespace trackknife::convert {

const std::vector<EncoderPreset>& builtin_encoder_presets() {
    static const std::vector<EncoderPreset> presets{
        {
            .id = "flac",
            .version = 1,
            .display_name = "FLAC (lossless)",
            .codec_name = "flac",
            .container_name = "flac",
            .file_extension = "flac",
            .lossless = true,
            .bit_rate = std::nullopt,
            .vbr_quality = std::nullopt,
            .sample_format_hint = "s32",
        },
        {
            .id = "opus-192",
            .version = 1,
            .display_name = "Opus 192 kbps",
            .codec_name = "libopus",
            .container_name = "opus",
            .file_extension = "opus",
            .lossless = false,
            .bit_rate = 192'000,
            .vbr_quality = std::nullopt,
            .sample_format_hint = {},
        },
        {
            .id = "mp3-v0",
            .version = 1,
            .display_name = "MP3 V0",
            .codec_name = "libmp3lame",
            .container_name = "mp3",
            .file_extension = "mp3",
            .lossless = false,
            .bit_rate = std::nullopt,
            .vbr_quality = 0,
            .sample_format_hint = {},
        },
        {
            .id = "vorbis-q6",
            .version = 1,
            .display_name = "Ogg Vorbis q6",
            .codec_name = "libvorbis",
            .container_name = "ogg",
            .file_extension = "ogg",
            .lossless = false,
            .bit_rate = std::nullopt,
            .vbr_quality = 6,
            .sample_format_hint = {},
        },
    };
    return presets;
}

std::optional<EncoderPreset> find_encoder_preset(const std::string_view id) {
    for (const auto& preset : builtin_encoder_presets()) {
        if (preset.id == id) {
            return preset;
        }
    }
    return std::nullopt;
}

EncoderPresetAvailability probe_encoder_preset(const EncoderPreset& preset) {
    const auto* const codec = avcodec_find_encoder_by_name(preset.codec_name.c_str());
    if (codec == nullptr) {
        return {.available = false,
                .detail = "encoder " + preset.codec_name + " is not built into this FFmpeg"};
    }
    const auto* const muxer = av_guess_format(preset.container_name.c_str(), nullptr, nullptr);
    if (muxer == nullptr) {
        return {.available = false,
                .detail = "muxer " + preset.container_name + " is not built into this FFmpeg"};
    }
    return {.available = true, .detail = {}};
}

std::string conversion_backend_versions() {
    return "ffmpeg=" + std::string{av_version_info()} +
           ";libavcodec=" + std::to_string(avcodec_version()) +
           ";libavformat=" + std::to_string(avformat_version()) +
           ";libswresample=" + std::to_string(swresample_version());
}

} // namespace trackknife::convert

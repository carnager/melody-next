// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace trackknife::convert {

// One versioned encode target (ADR-0105). The identifier is stable across
// releases; `version` increments whenever a preset's produced output would
// change for identical input, so later journaling can record exactly what
// an output file was made with.
struct EncoderPreset {
    std::string id;
    int version{1};
    std::string display_name;
    // FFmpeg encoder and muxer short names plus the destination extension.
    std::string codec_name;
    std::string container_name;
    std::string file_extension;
    bool lossless{false};
    // Exactly one of these is set for lossy presets: a target bit rate in
    // bits per second, or a codec-native VBR quality level.
    std::optional<std::int64_t> bit_rate;
    std::optional<int> vbr_quality;
    // Preferred FFmpeg sample-format name (for example "s32"); empty lets
    // the converter negotiate from the encoder's supported formats.
    std::string sample_format_hint;

    friend bool operator==(const EncoderPreset&, const EncoderPreset&) = default;
};

// Whether this build's FFmpeg can realize a preset, probed at runtime —
// encoders like libopus and libmp3lame are compile-time options of the
// system FFmpeg, never assumed present.
struct EncoderPresetAvailability {
    bool available{false};
    std::string detail;

    friend bool operator==(const EncoderPresetAvailability&,
                           const EncoderPresetAvailability&) = default;
};

[[nodiscard]] const std::vector<EncoderPreset>& builtin_encoder_presets();

[[nodiscard]] std::optional<EncoderPreset> find_encoder_preset(std::string_view id);

[[nodiscard]] EncoderPresetAvailability probe_encoder_preset(const EncoderPreset& preset);

// Exact runtime backend identity recorded with reusable whole-job settings.
[[nodiscard]] std::string conversion_backend_versions();

} // namespace trackknife::convert

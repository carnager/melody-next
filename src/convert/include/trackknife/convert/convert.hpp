// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/convert/preset.hpp"
#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/formats/decoder.hpp"
#include "trackknife/metadata/document.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace trackknife::convert {

enum class ConversionChannelPolicy : std::uint8_t { keep, mono, stereo };
enum class ConversionGainMode : std::uint8_t { none, track, album };

// One encoded cover image to embed into the converted output (ADR-0131).
// Bytes are exact PNG/JPEG data; picture_type uses the shared FLAC/ID3v2
// numbering (3 = front cover).
struct ConversionArtwork {
    std::vector<unsigned char> bytes;
    std::string mime_type;
    std::uint32_t width{0U};
    std::uint32_t height{0U};
    std::uint32_t picture_type{3U};
    std::string description;

    friend bool operator==(const ConversionArtwork&, const ConversionArtwork&) = default;
};

// One source-to-destination conversion. The selection and range address
// logical tracks inside container files exactly as the decoder does, so
// cue-sheet segments convert like whole files.
struct AudioConversionRequest {
    std::string source_raw_path;
    formats::AudioSourceSelection source_selection;
    std::optional<formats::SampleRange> source_range;
    std::string destination_raw_path;
    EncoderPreset preset;
    // Forces the output sample rate; absent keeps the source rate. Either
    // way the encoder's supported-rate constraint applies afterwards, so
    // Opus maps any request into its 48 kHz family.
    std::optional<int> target_sample_rate;
    // Downsample-only cap (ADR-0134): sources above are resampled to the
    // cap, sources at or below keep their rate. Mutually exclusive with
    // target_sample_rate; the encoder constraint still applies afterwards.
    std::optional<int> sample_rate_cap;
    // Forces the stored bit depth (16 or 24) where the encoder keeps integer
    // PCM — FLAC — overriding the preset's sample-format hint; float-based
    // encoders like Opus have no stored depth and ignore it. Quantizing to
    // 16-bit engages high-passed triangular dither in the resampler.
    std::optional<int> target_bit_depth;
    // Keep-source depth (ADR-0134): the probed stored format chooses the
    // depth — at most 16 stored bits keeps 16, everything else (including
    // float and unknown) keeps 24, the pipeline's maximum. Mutually
    // exclusive with target_bit_depth.
    bool keep_source_bit_depth{false};
    // Explicit channel and permanent-gain policies (ADR-0173). FFmpeg's
    // channel-layout-aware resampler performs mono/stereo mixing. Gain is
    // applied to float PCM before resampling; album falls back to track and a
    // matching peak prevents amplification above full scale.
    ConversionChannelPolicy channel_policy{ConversionChannelPolicy::keep};
    ConversionGainMode gain_mode{ConversionGainMode::none};
    float gain_preamp_db{0.0F};
    // Effective text metadata to carry into the output, written at mux time
    // and verified by rereading the finished file with the project metadata
    // reader before it may become the destination. Vorbis-comment containers
    // receive exact native key spellings; MP3 maps the common fields onto
    // proper ID3 frames and passes the rest through as TXXX.
    metadata::MetadataDocument metadata;
    // Optional cover image embedded at mux time — FLAC/MP3 through an
    // attached-picture stream, Opus/Vorbis through a METADATA_BLOCK_PICTURE
    // comment — and verified by rereading the finished file's embedded
    // artwork byte-exactly before it may become the destination.
    std::optional<ConversionArtwork> artwork;

    friend bool operator==(const AudioConversionRequest&, const AudioConversionRequest&) = default;
};

struct ConvertedAudioFile {
    std::string destination_raw_path;
    int sample_rate{0};
    int channels{0};
    // Verified duration of the written output, in output-rate samples.
    std::int64_t duration_samples{0};

    friend bool operator==(const ConvertedAudioFile&, const ConvertedAudioFile&) = default;
};

// Progress in decoded source frames; the total is absent when the source
// container does not declare a duration.
using ConversionProgress =
    std::function<void(std::uint64_t frames_done, std::optional<std::uint64_t> frames_total)>;

// Decodes one source and encodes it with the preset, atomically: the
// encoder writes a hidden temporary beside the destination, the result is
// verified by reopening it with the project decoder (format and duration),
// and only then is it renamed into place without replacing an existing
// file. Failure or cancellation leaves no partial output. This synchronous
// I/O primitive belongs on a bounded worker; it never touches the source.
[[nodiscard]] core::Result<ConvertedAudioFile>
convert_audio_file(const AudioConversionRequest& request, const ConversionProgress& progress = {},
                   const core::CancellationToken& cancellation = {});

} // namespace trackknife::convert

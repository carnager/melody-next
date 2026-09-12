// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/convert/artwork.hpp"
#include "trackknife/convert/convert.hpp"
#include "trackknife/convert/preset.hpp"
#include "trackknife/convert/scan.hpp"
#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/stable_id.hpp"
#include "trackknife/formats/probe.hpp"
#include "trackknife/metadata/document.hpp"
#include "trackknife/metadata/local_reader.hpp"

#include <taglib/attachedpictureframe.h>
#include <taglib/flacfile.h>
#include <taglib/id3v2tag.h>
#include <taglib/mpegfile.h>
#include <taglib/opusfile.h>
#include <taglib/vorbisfile.h>
#include <taglib/xiphcomment.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void check(const bool condition, const std::string_view expression, const int line) {
    if (!condition) {
        std::cerr << "line " << line << ": check failed: " << expression << '\n';
        ++failures;
    }
}

#define CHECK(expression) check((expression), #expression, __LINE__)

constexpr int sample_rate = 44'100;
constexpr double tone_hertz = 997.0;
constexpr double tone_seconds = 2.0;

class TemporaryDirectory final {
  public:
    TemporaryDirectory() {
        path_ = std::filesystem::temp_directory_path() /
                ("trackknife-convert-" + trackknife::core::StableId::random().to_string());
        std::filesystem::create_directory(path_);
    }
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

  private:
    std::filesystem::path path_;
};

void write_sine_wav(const std::filesystem::path& path, const double amplitude,
                    const double seconds) {
    const auto frames = static_cast<std::uint32_t>(seconds * sample_rate);
    const std::uint32_t data_bytes = frames * 2U * 2U;
    std::ofstream output{path, std::ios::binary};
    const auto write_u32 = [&output](const std::uint32_t value) {
        const std::array<char, 4> bytes{
            static_cast<char>(value & 0xFFU), static_cast<char>((value >> 8U) & 0xFFU),
            static_cast<char>((value >> 16U) & 0xFFU), static_cast<char>((value >> 24U) & 0xFFU)};
        output.write(bytes.data(), 4);
    };
    const auto write_u16 = [&output](const std::uint16_t value) {
        const std::array<char, 2> bytes{static_cast<char>(value & 0xFFU),
                                        static_cast<char>((value >> 8U) & 0xFFU)};
        output.write(bytes.data(), 2);
    };
    output.write("RIFF", 4);
    write_u32(36U + data_bytes);
    output.write("WAVE", 4);
    output.write("fmt ", 4);
    write_u32(16U);
    write_u16(1U);
    write_u16(2U);
    write_u32(static_cast<std::uint32_t>(sample_rate));
    write_u32(static_cast<std::uint32_t>(sample_rate) * 4U);
    write_u16(4U);
    write_u16(16U);
    output.write("data", 4);
    write_u32(data_bytes);
    for (std::uint32_t frame = 0U; frame < frames; ++frame) {
        const auto value = amplitude * std::sin(2.0 * std::numbers::pi * tone_hertz *
                                                static_cast<double>(frame) / sample_rate);
        const auto sample = static_cast<std::int16_t>(std::clamp(value, -1.0, 1.0) * 32'767.0);
        write_u16(static_cast<std::uint16_t>(sample));
        write_u16(static_cast<std::uint16_t>(sample));
    }
}

void write_sine_wav_24_96(const std::filesystem::path& path, const double amplitude,
                          const double seconds) {
    constexpr int rate = 96'000;
    const auto frames = static_cast<std::uint32_t>(seconds * rate);
    const std::uint32_t data_bytes = frames * 2U * 3U;
    std::ofstream output{path, std::ios::binary};
    const auto write_u32 = [&output](const std::uint32_t value) {
        const std::array<char, 4> bytes{
            static_cast<char>(value & 0xFFU), static_cast<char>((value >> 8U) & 0xFFU),
            static_cast<char>((value >> 16U) & 0xFFU), static_cast<char>((value >> 24U) & 0xFFU)};
        output.write(bytes.data(), 4);
    };
    const auto write_u16 = [&output](const std::uint16_t value) {
        const std::array<char, 2> bytes{static_cast<char>(value & 0xFFU),
                                        static_cast<char>((value >> 8U) & 0xFFU)};
        output.write(bytes.data(), 2);
    };
    const auto write_s24 = [&output](const std::int32_t value) {
        const std::array<char, 3> bytes{static_cast<char>(value & 0xFF),
                                        static_cast<char>((value >> 8) & 0xFF),
                                        static_cast<char>((value >> 16) & 0xFF)};
        output.write(bytes.data(), 3);
    };
    output.write("RIFF", 4);
    write_u32(36U + data_bytes);
    output.write("WAVE", 4);
    output.write("fmt ", 4);
    write_u32(16U);
    write_u16(1U);
    write_u16(2U);
    write_u32(rate);
    write_u32(rate * 6U);
    write_u16(6U);
    write_u16(24U);
    output.write("data", 4);
    write_u32(data_bytes);
    for (std::uint32_t frame = 0U; frame < frames; ++frame) {
        const auto value = amplitude * std::sin(2.0 * std::numbers::pi * tone_hertz *
                                                static_cast<double>(frame) / rate);
        const auto sample = static_cast<std::int32_t>(std::clamp(value, -1.0, 1.0) * 8'388'607.0);
        write_s24(sample);
        write_s24(sample);
    }
}

[[nodiscard]] std::optional<std::vector<unsigned char>>
decode_base64_file(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        return std::nullopt;
    }
    const std::string encoded{std::istreambuf_iterator<char>{input},
                              std::istreambuf_iterator<char>{}};
    std::array<int, 256> values{};
    values.fill(-1);
    constexpr std::string_view alphabet{
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"};
    for (std::size_t index = 0U; index < alphabet.size(); ++index) {
        values[static_cast<unsigned char>(alphabet[index])] = static_cast<int>(index);
    }
    std::vector<unsigned char> decoded;
    unsigned accumulator = 0U;
    unsigned bits = 0U;
    for (const auto character : encoded) {
        if (character == '=') {
            break;
        }
        const auto byte = static_cast<unsigned char>(character);
        const auto value = values[byte];
        if (value < 0) {
            if (character == '\r' || character == '\n' || character == ' ' || character == '\t') {
                continue;
            }
            return std::nullopt;
        }
        accumulator = (accumulator << 6U) | static_cast<unsigned>(value);
        bits += 6U;
        if (bits >= 8U) {
            bits -= 8U;
            decoded.push_back(static_cast<unsigned char>((accumulator >> bits) & 0xFFU));
        }
    }
    return decoded;
}

[[nodiscard]] std::filesystem::path materialize(const std::filesystem::path& fixture_directory,
                                                const std::string_view fixture,
                                                const std::filesystem::path& destination) {
    const auto decoded = decode_base64_file(fixture_directory / fixture);
    CHECK(decoded.has_value());
    if (decoded) {
        std::ofstream output{destination, std::ios::binary};
        output.write(reinterpret_cast<const char*>(decoded->data()),
                     static_cast<std::streamsize>(decoded->size()));
        CHECK(output.good());
    }
    return destination;
}

struct ReadPicture {
    std::vector<unsigned char> bytes;
    unsigned type{0U};
    std::string mime;
};

[[nodiscard]] std::optional<ReadPicture> from_flac_picture(const TagLib::FLAC::Picture* picture) {
    if (picture == nullptr) {
        return std::nullopt;
    }
    const auto data = picture->data();
    return ReadPicture{
        .bytes = {reinterpret_cast<const unsigned char*>(data.data()),
                  reinterpret_cast<const unsigned char*>(data.data()) + data.size()},
        .type = static_cast<unsigned>(picture->type()),
        .mime = picture->mimeType().to8Bit(true),
    };
}

// Rereads the single embedded cover with TagLib — deliberately independent
// of the FFmpeg code that wrote it.
[[nodiscard]] std::optional<ReadPicture> read_output_picture(const std::filesystem::path& path,
                                                             const std::string& extension) {
    if (extension == "flac") {
        TagLib::FLAC::File file{path.c_str()};
        if (!file.isValid() || file.pictureList().size() != 1U) {
            return std::nullopt;
        }
        return from_flac_picture(file.pictureList().front());
    }
    if (extension == "opus" || extension == "ogg") {
        std::optional<ReadPicture> result;
        const auto from_comment = [&result](TagLib::Ogg::XiphComment* comment) {
            if (comment != nullptr && comment->pictureList().size() == 1U) {
                result = from_flac_picture(comment->pictureList().front());
            }
        };
        if (extension == "opus") {
            TagLib::Ogg::Opus::File file{path.c_str()};
            if (file.isValid()) {
                from_comment(file.tag());
            }
        } else {
            TagLib::Ogg::Vorbis::File file{path.c_str()};
            if (file.isValid()) {
                from_comment(file.tag());
            }
        }
        return result;
    }
    if (extension == "mp3") {
        TagLib::MPEG::File file{path.c_str()};
        auto* tag = file.isValid() ? file.ID3v2Tag(false) : nullptr;
        if (tag == nullptr) {
            return std::nullopt;
        }
        const auto& frames = tag->frameListMap()["APIC"];
        if (frames.size() != 1U) {
            return std::nullopt;
        }
        const auto* frame =
            dynamic_cast<const TagLib::ID3v2::AttachedPictureFrame*>(frames.front());
        if (frame == nullptr) {
            return std::nullopt;
        }
        const auto data = frame->picture();
        return ReadPicture{
            .bytes = {reinterpret_cast<const unsigned char*>(data.data()),
                      reinterpret_cast<const unsigned char*>(data.data()) + data.size()},
            .type = static_cast<unsigned>(frame->type()),
            .mime = frame->mimeType().to8Bit(true),
        };
    }
    return std::nullopt;
}

// Anything besides the named survivors — hidden temporaries above all —
// counts as leftover.
[[nodiscard]] std::size_t entries_besides(const std::filesystem::path& directory,
                                          const std::vector<std::string>& survivors) {
    std::size_t extras = 0U;
    for (const auto& entry : std::filesystem::directory_iterator{directory}) {
        const auto name = entry.path().filename().string();
        if (std::ranges::find(survivors, name) == survivors.end()) {
            std::cerr << "unexpected entry: " << name << '\n';
            ++extras;
        }
    }
    return extras;
}

void builtinPresetsProbeAvailable() {
    const auto& presets = trackknife::convert::builtin_encoder_presets();
    CHECK(presets.size() == 4U);
    for (const auto& preset : presets) {
        const auto availability = trackknife::convert::probe_encoder_preset(preset);
        if (!availability.available) {
            std::cerr << preset.id << ": " << availability.detail << '\n';
        }
        CHECK(availability.available);
    }
    const auto found = trackknife::convert::find_encoder_preset("opus-192");
    CHECK(found.has_value() && found->codec_name == "libopus");
    CHECK(!trackknife::convert::find_encoder_preset("wax-cylinder").has_value());
    trackknife::convert::EncoderPreset unbuildable;
    unbuildable.id = "bogus";
    unbuildable.codec_name = "no-such-encoder";
    unbuildable.container_name = "flac";
    const auto bogus = trackknife::convert::probe_encoder_preset(unbuildable);
    CHECK(!bogus.available && !bogus.detail.empty());
}

void convertsToEveryPresetAtomically() {
    TemporaryDirectory directory;
    const auto source = directory.path() / "tone.wav";
    write_sine_wav(source, 0.6, tone_seconds);
    std::vector<std::string> survivors{"tone.wav"};

    for (const auto& preset : trackknife::convert::builtin_encoder_presets()) {
        const auto destination =
            directory.path() / ("converted-" + preset.id + "." + preset.file_extension);
        std::uint64_t last_frames = 0U;
        bool total_present = false;
        const auto converted = trackknife::convert::convert_audio_file(
            {.source_raw_path = source.native(),
             .source_selection = {},
             .source_range = {},
             .destination_raw_path = destination.native(),
             .preset = preset,
             .target_sample_rate = {},
             .sample_rate_cap = {},
             .target_bit_depth = {},
             .keep_source_bit_depth = false,
             .metadata = {},
             .artwork = {}},
            [&last_frames, &total_present](const std::uint64_t frames_done,
                                           const std::optional<std::uint64_t> frames_total) {
                CHECK(frames_done >= last_frames);
                last_frames = frames_done;
                total_present = total_present || frames_total.has_value();
            });
        if (!converted) {
            std::cerr << preset.id << ": " << converted.error().message << '\n';
        }
        CHECK(converted.has_value());
        if (!converted) {
            continue;
        }
        survivors.push_back(destination.filename().string());
        CHECK(std::filesystem::exists(destination));
        CHECK(converted->channels == 2);
        // Opus only speaks 48 kHz-family rates, so 44.1 kHz material is
        // resampled up; every other preset keeps the source rate.
        const auto expected_rate = preset.id == "opus-192" ? 48'000 : sample_rate;
        CHECK(converted->sample_rate == expected_rate);
        const auto expected_frames = static_cast<std::int64_t>(tone_seconds * expected_rate);
        CHECK(std::abs(converted->duration_samples - expected_frames) <= expected_rate / 5);
        CHECK(last_frames == static_cast<std::uint64_t>(tone_seconds * sample_rate));
        CHECK(total_present);
    }
    CHECK(entries_besides(directory.path(), survivors) == 0U);
}

void carriesMetadataIntoEveryPreset() {
    TemporaryDirectory directory;
    const auto source = directory.path() / "tone.wav";
    write_sine_wav(source, 0.6, 0.5);

    trackknife::metadata::MetadataDocument document;
    const auto field = [](const std::string& canonical, const std::string& native,
                          std::vector<std::string> values) {
        trackknife::metadata::MetadataField result;
        result.canonical_name = canonical;
        result.native_name = native;
        result.values = std::move(values);
        return result;
    };
    document.fields.push_back(field("title", "TITLE", {"Converted Tone"}));
    document.fields.push_back(field("artist", "ARTIST", {"Fixture Band"}));
    document.fields.push_back(field("tracknumber", "TRACKNUMBER", {"7"}));
    // FFmpeg's vorbis writer renames the generic comment key to
    // DESCRIPTION; the post-encode normalization must undo that so the
    // verification and every reader see COMMENT again.
    document.fields.push_back(field("comment", "COMMENT", {"Visit https://example.org"}));
    // Loudness claims measured against the source signal are stale after a
    // re-encode and must be stripped, never transferred (ADR-0133).
    document.fields.push_back(field("replaygaintrackgain", "REPLAYGAIN_TRACK_GAIN", {"-6.50 dB"}));
    document.fields.push_back(field("replaygaintrackpeak", "REPLAYGAIN_TRACK_PEAK", {"0.988547"}));
    document.fields.push_back(field("replaygainalbumgain", "REPLAYGAIN_ALBUM_GAIN", {"-7.10 dB"}));
    document.fields.push_back(field("r128trackgain", "R128_TRACK_GAIN", {"-1536"}));

    for (const auto& preset : trackknife::convert::builtin_encoder_presets()) {
        const auto destination =
            directory.path() / ("tagged-" + preset.id + "." + preset.file_extension);
        const auto converted =
            trackknife::convert::convert_audio_file({.source_raw_path = source.native(),
                                                     .source_selection = {},
                                                     .source_range = {},
                                                     .destination_raw_path = destination.native(),
                                                     .preset = preset,
                                                     .target_sample_rate = {},
                                                     .sample_rate_cap = {},
                                                     .target_bit_depth = {},
                                                     .keep_source_bit_depth = false,
                                                     .metadata = document,
                                                     .artwork = {}});
        if (!converted) {
            std::cerr << preset.id << ": " << converted.error().message << '\n';
        }
        CHECK(converted.has_value());
        if (!converted) {
            continue;
        }
        const auto reread = trackknife::metadata::read_local_metadata(destination.native());
        CHECK(reread.has_value());
        if (!reread) {
            continue;
        }
        CHECK(reread->document.first_effective_value("title") ==
              std::optional<std::string>{"Converted Tone"});
        CHECK(reread->document.first_effective_value("artist") ==
              std::optional<std::string>{"Fixture Band"});
        CHECK(reread->document.first_effective_value("tracknumber") ==
              std::optional<std::string>{"7"});
        CHECK(reread->document.first_effective_value("comment") ==
              std::optional<std::string>{"Visit https://example.org"});
        CHECK(!reread->document.first_effective_value("description").has_value());
        CHECK(!reread->document.first_effective_value("replaygaintrackgain").has_value());
        CHECK(!reread->document.first_effective_value("replaygaintrackpeak").has_value());
        CHECK(!reread->document.first_effective_value("replaygainalbumgain").has_value());
        CHECK(!reread->document.first_effective_value("r128trackgain").has_value());
    }
}

void resamplesOnRequestWithinEncoderConstraints() {
    TemporaryDirectory directory;
    const auto source = directory.path() / "tone.wav";
    write_sine_wav(source, 0.6, 0.5);

    // FLAC accepts any rate, so the requested 96 kHz sticks and the
    // duration scales with it.
    const auto upsampled = trackknife::convert::convert_audio_file(
        {.source_raw_path = source.native(),
         .source_selection = {},
         .source_range = {},
         .destination_raw_path = (directory.path() / "up.flac").native(),
         .preset = *trackknife::convert::find_encoder_preset("flac"),
         .target_sample_rate = 96'000,
         .sample_rate_cap = {},
         .target_bit_depth = {},
         .keep_source_bit_depth = false,
         .metadata = {},
         .artwork = {}});
    CHECK(upsampled.has_value());
    CHECK(upsampled && upsampled->sample_rate == 96'000);
    CHECK(upsampled && std::abs(upsampled->duration_samples - 48'000) <= 96'000 / 5);

    // Opus only speaks the 48 kHz family: the same request lands on 48 kHz
    // instead of failing.
    const auto constrained = trackknife::convert::convert_audio_file(
        {.source_raw_path = source.native(),
         .source_selection = {},
         .source_range = {},
         .destination_raw_path = (directory.path() / "constrained.opus").native(),
         .preset = *trackknife::convert::find_encoder_preset("opus-192"),
         .target_sample_rate = 96'000,
         .sample_rate_cap = {},
         .target_bit_depth = {},
         .keep_source_bit_depth = false,
         .metadata = {},
         .artwork = {}});
    CHECK(constrained.has_value());
    CHECK(constrained && constrained->sample_rate == 48'000);

    // Absurd rates fail typed before any decoding starts.
    const auto absurd = trackknife::convert::convert_audio_file(
        {.source_raw_path = source.native(),
         .source_selection = {},
         .source_range = {},
         .destination_raw_path = (directory.path() / "absurd.flac").native(),
         .preset = *trackknife::convert::find_encoder_preset("flac"),
         .target_sample_rate = 4'000,
         .sample_rate_cap = {},
         .target_bit_depth = {},
         .keep_source_bit_depth = false,
         .metadata = {},
         .artwork = {}});
    CHECK(!absurd.has_value());
    CHECK(!absurd && absurd.error().code == trackknife::core::ErrorCode::invalid_argument);
    CHECK(entries_besides(directory.path(), {"tone.wav", "up.flac", "constrained.opus"}) == 0U);
}

void quantizesHiResToSixteenFortyFourWithDither() {
    TemporaryDirectory directory;
    const auto source = directory.path() / "hires.wav";
    write_sine_wav_24_96(source, 0.6, 0.5);
    const auto probe_format = [](const std::filesystem::path& path) {
        const auto probed = trackknife::formats::probe_local_media(path.native());
        CHECK(probed.has_value());
        if (!probed || !probed->best_audio_stream) {
            return std::pair<std::string, int>{{}, 0};
        }
        const auto& stream =
            probed->audio_streams[static_cast<std::size_t>(*probed->best_audio_stream)];
        return std::pair{stream.sample_format, stream.sample_rate};
    };

    // The headline use case: 24-bit 96 kHz down to a dithered 16/44.1 FLAC.
    const auto cd = directory.path() / "cd.flac";
    const auto quantized = trackknife::convert::convert_audio_file(
        {.source_raw_path = source.native(),
         .source_selection = {},
         .source_range = {},
         .destination_raw_path = cd.native(),
         .preset = *trackknife::convert::find_encoder_preset("flac"),
         .target_sample_rate = 44'100,
         .sample_rate_cap = {},
         .target_bit_depth = 16,
         .keep_source_bit_depth = false,
         .metadata = {},
         .artwork = {}});
    CHECK(quantized.has_value());
    if (!quantized) {
        std::cerr << quantized.error().message << '\n';
        return;
    }
    CHECK(quantized->sample_rate == 44'100);
    CHECK(std::abs(quantized->duration_samples - 22'050) <= 44'100 / 5);
    const auto [cd_format, cd_rate] = probe_format(cd);
    CHECK(cd_format == "s16");
    CHECK(cd_rate == 44'100);

    // Without a depth request the FLAC preset keeps its 24-bit default.
    const auto archive = directory.path() / "archive.flac";
    const auto kept = trackknife::convert::convert_audio_file(
        {.source_raw_path = source.native(),
         .source_selection = {},
         .source_range = {},
         .destination_raw_path = archive.native(),
         .preset = *trackknife::convert::find_encoder_preset("flac"),
         .target_sample_rate = {},
         .sample_rate_cap = {},
         .target_bit_depth = {},
         .keep_source_bit_depth = false,
         .metadata = {},
         .artwork = {}});
    CHECK(kept.has_value());
    const auto [archive_format, archive_rate] = probe_format(archive);
    CHECK(archive_format == "s32");
    CHECK(archive_rate == 96'000);

    // Float-based encoders have no stored depth; the request is inert.
    const auto lossy = trackknife::convert::convert_audio_file(
        {.source_raw_path = source.native(),
         .source_selection = {},
         .source_range = {},
         .destination_raw_path = (directory.path() / "lossy.opus").native(),
         .preset = *trackknife::convert::find_encoder_preset("opus-192"),
         .target_sample_rate = {},
         .sample_rate_cap = {},
         .target_bit_depth = 16,
         .keep_source_bit_depth = false,
         .metadata = {},
         .artwork = {}});
    CHECK(lossy.has_value());
    CHECK(lossy && lossy->sample_rate == 48'000);

    // Unsupported depths fail typed before any decoding.
    const auto odd = trackknife::convert::convert_audio_file(
        {.source_raw_path = source.native(),
         .source_selection = {},
         .source_range = {},
         .destination_raw_path = (directory.path() / "odd.flac").native(),
         .preset = *trackknife::convert::find_encoder_preset("flac"),
         .target_sample_rate = {},
         .sample_rate_cap = {},
         .target_bit_depth = 20,
         .keep_source_bit_depth = false,
         .metadata = {},
         .artwork = {}});
    CHECK(!odd.has_value());
    CHECK(!odd && odd.error().code == trackknife::core::ErrorCode::invalid_argument);
}

void refusesExistingDestinationAndMissingDirectory() {
    TemporaryDirectory directory;
    const auto source = directory.path() / "tone.wav";
    write_sine_wav(source, 0.6, 0.5);
    const auto preset = *trackknife::convert::find_encoder_preset("flac");

    const auto occupied = directory.path() / "occupied.flac";
    std::ofstream{occupied} << "already here";
    const auto conflicting =
        trackknife::convert::convert_audio_file({.source_raw_path = source.native(),
                                                 .source_selection = {},
                                                 .source_range = {},
                                                 .destination_raw_path = occupied.native(),
                                                 .preset = preset,
                                                 .target_sample_rate = {},
                                                 .sample_rate_cap = {},
                                                 .target_bit_depth = {},
                                                 .keep_source_bit_depth = false,
                                                 .metadata = {},
                                                 .artwork = {}});
    CHECK(!conflicting.has_value());
    CHECK(!conflicting && conflicting.error().code == trackknife::core::ErrorCode::conflict);
    {
        std::ifstream input{occupied};
        std::string content;
        std::getline(input, content);
        CHECK(content == "already here");
    }

    const auto orphan = directory.path() / "missing" / "out.flac";
    const auto orphaned =
        trackknife::convert::convert_audio_file({.source_raw_path = source.native(),
                                                 .source_selection = {},
                                                 .source_range = {},
                                                 .destination_raw_path = orphan.native(),
                                                 .preset = preset,
                                                 .target_sample_rate = {},
                                                 .sample_rate_cap = {},
                                                 .target_bit_depth = {},
                                                 .keep_source_bit_depth = false,
                                                 .metadata = {},
                                                 .artwork = {}});
    CHECK(!orphaned.has_value());
    CHECK(!orphaned && orphaned.error().code == trackknife::core::ErrorCode::invalid_argument);

    CHECK(entries_besides(directory.path(), {"tone.wav", "occupied.flac"}) == 0U);
}

void cancellationLeavesNoPartialOutput() {
    TemporaryDirectory directory;
    const auto source = directory.path() / "tone.wav";
    write_sine_wav(source, 0.6, 1.0);
    trackknife::core::CancellationSource cancellation;
    cancellation.request_cancellation();
    const auto cancelled = trackknife::convert::convert_audio_file(
        {.source_raw_path = source.native(),
         .source_selection = {},
         .source_range = {},
         .destination_raw_path = (directory.path() / "cancelled.opus").native(),
         .preset = *trackknife::convert::find_encoder_preset("opus-192"),
         .target_sample_rate = {},
         .sample_rate_cap = {},
         .target_bit_depth = {},
         .keep_source_bit_depth = false,
         .metadata = {},
         .artwork = {}},
        {}, cancellation.token());
    CHECK(!cancelled.has_value());
    CHECK(!cancelled && cancelled.error().code == trackknife::core::ErrorCode::cancelled);
    CHECK(entries_besides(directory.path(), {"tone.wav"}) == 0U);
}

void scansItemsInParallelIsolatingFailures() {
    TemporaryDirectory directory;
    const auto preset = *trackknife::convert::find_encoder_preset("opus-192");
    std::vector<trackknife::convert::ConversionScanItem> items;
    for (std::size_t index = 0U; index < 5U; ++index) {
        const auto source = directory.path() / ("tone-" + std::to_string(index) + ".wav");
        write_sine_wav(source, 0.5, 0.5);
        items.push_back(
            {.item_index = index * 10U,
             .source_raw_path = source.native(),
             .selection = {},
             .range = {},
             .destination_raw_path =
                 (directory.path() / ("out-" + std::to_string(index) + ".opus")).native(),
             .metadata = {}});
    }
    // A missing source and an in-scan destination collision must fail alone.
    items.push_back({.item_index = 60U,
                     .source_raw_path = (directory.path() / "missing.wav").native(),
                     .selection = {},
                     .range = {},
                     .destination_raw_path = (directory.path() / "out-missing.opus").native(),
                     .metadata = {}});
    items.push_back({.item_index = 70U,
                     .source_raw_path = items[0].source_raw_path,
                     .selection = {},
                     .range = {},
                     .destination_raw_path = items[0].destination_raw_path,
                     .metadata = {}});

    std::size_t final_completed = 0U;
    const auto result = trackknife::convert::scan_conversion(
        items,
        {.preset = preset,
         .maximum_parallelism = 3U,
         .target_sample_rate = {},
         .sample_rate_cap = {},
         .target_bit_depth = {},
         .keep_source_bit_depth = false,
         .carry_artwork = false},
        [&final_completed](const trackknife::convert::ConversionScanProgress& update) {
            final_completed = std::max(final_completed, update.completed_items);
            CHECK(update.total_items == 7U);
        });
    CHECK(result.has_value());
    if (!result) {
        std::cerr << result.error().message << '\n';
        return;
    }
    CHECK(result->items.size() == 7U);
    CHECK(result->converted_count() == 5U);
    CHECK(result->failed_count() == 2U);
    CHECK(final_completed == 7U);
    CHECK(!result->cancellation_requested);
    for (std::size_t index = 0U; index < 5U; ++index) {
        const auto& entry = result->items[index];
        CHECK(entry.state == trackknife::convert::ConversionScanState::converted);
        CHECK(entry.source_revision.has_value());
        CHECK(entry.converted.has_value() && entry.converted->sample_rate == 48'000);
        CHECK(std::filesystem::exists(entry.destination_raw_path));
    }
    CHECK(result->items[5].state == trackknife::convert::ConversionScanState::failed);
    CHECK(result->items[5].issue.has_value());
    const auto& collision = result->items[6];
    CHECK(collision.state == trackknife::convert::ConversionScanState::failed);
    CHECK(collision.issue.has_value() &&
          collision.issue->code == trackknife::core::ErrorCode::conflict);

    CHECK(trackknife::convert::scan_conversion(items, {.preset = preset,
                                                       .maximum_parallelism = 0U,
                                                       .target_sample_rate = {},
                                                       .sample_rate_cap = {},
                                                       .target_bit_depth = {},
                                                       .keep_source_bit_depth = false,
                                                       .carry_artwork = false})
              .has_value() == false);

    trackknife::core::CancellationSource cancellation;
    cancellation.request_cancellation();
    const auto cancelled = trackknife::convert::scan_conversion(std::span{items}.subspan(0U, 1U),
                                                                {.preset = preset,
                                                                 .maximum_parallelism = 1U,
                                                                 .target_sample_rate = {},
                                                                 .sample_rate_cap = {},
                                                                 .target_bit_depth = {},
                                                                 .keep_source_bit_depth = false,
                                                                 .carry_artwork = false},
                                                                {}, cancellation.token());
    CHECK(cancelled.has_value());
    CHECK(cancelled && cancelled->cancellation_requested);
    CHECK(cancelled &&
          cancelled->items[0].state == trackknife::convert::ConversionScanState::cancelled);
}

// ADR-0131: the resolved cover must round-trip byte-exactly into every
// qualified preset, with the FLAC source's role-level type preserved.
void carriesArtworkIntoEveryPreset(const std::filesystem::path& fixture_directory) {
    TemporaryDirectory directory;
    const auto source =
        materialize(fixture_directory, "art-tone-flac.b64", directory.path() / "art-tone.flac");

    // The fixture stores its picture with the native type "Other"; the
    // qualified FLAC read must preserve that instead of reclassifying it.
    const auto artwork = trackknife::convert::resolve_conversion_artwork(source.native());
    CHECK(artwork.has_value());
    if (!artwork) {
        return;
    }
    CHECK(artwork->mime_type == "image/png");
    CHECK(artwork->picture_type == 0U);
    CHECK(artwork->width == 64U && artwork->height == 64U);
    CHECK(!artwork->bytes.empty());

    for (const auto& preset : trackknife::convert::builtin_encoder_presets()) {
        const auto destination =
            directory.path() / ("covered-" + preset.id + "." + preset.file_extension);
        const auto converted =
            trackknife::convert::convert_audio_file({.source_raw_path = source.native(),
                                                     .source_selection = {},
                                                     .source_range = {},
                                                     .destination_raw_path = destination.native(),
                                                     .preset = preset,
                                                     .target_sample_rate = {},
                                                     .sample_rate_cap = {},
                                                     .target_bit_depth = {},
                                                     .keep_source_bit_depth = false,
                                                     .metadata = {},
                                                     .artwork = artwork});
        CHECK(converted.has_value());
        if (!converted) {
            std::cerr << preset.id << ": " << converted.error().message << '\n';
            continue;
        }
        const auto reread = read_output_picture(destination, preset.file_extension);
        CHECK(reread.has_value());
        if (reread) {
            CHECK(reread->bytes == artwork->bytes);
            CHECK(reread->type == artwork->picture_type);
            CHECK(reread->mime == artwork->mime_type);
        }
    }

    // A converted Opus is itself a valid source: its embedded cover resolves
    // through the container-agnostic reader, normalized to a front cover.
    const auto opus_output = directory.path() / "covered-opus-192.opus";
    const auto from_opus = trackknife::convert::resolve_conversion_artwork(opus_output.native());
    CHECK(from_opus.has_value());
    if (from_opus) {
        CHECK(from_opus->bytes == artwork->bytes);
        CHECK(from_opus->picture_type == 3U);
    }
}

// A source without embedded pictures falls back to the exact-basename
// sibling cover, carried as a front cover; the parallel scan resolves it
// through its carry_artwork option.
void carriesExternalCoverThroughTheScan(const std::filesystem::path& fixture_directory) {
    TemporaryDirectory directory;
    const auto source = directory.path() / "tone.wav";
    write_sine_wav(source, 0.6, 1.0);
    const auto flac_fixture =
        materialize(fixture_directory, "art-tone-flac.b64", directory.path() / "art-donor.flac");
    const auto donor = trackknife::convert::resolve_conversion_artwork(flac_fixture.native());
    CHECK(donor.has_value());
    if (!donor) {
        return;
    }
    {
        std::ofstream cover{directory.path() / "cover.png", std::ios::binary};
        cover.write(reinterpret_cast<const char*>(donor->bytes.data()),
                    static_cast<std::streamsize>(donor->bytes.size()));
        CHECK(cover.good());
    }

    const std::vector<trackknife::convert::ConversionScanItem> items{
        {.item_index = 0U,
         .source_raw_path = source.native(),
         .selection = {},
         .range = {},
         .destination_raw_path = (directory.path() / "covered.flac").native(),
         .metadata = {}}};
    const auto result = trackknife::convert::scan_conversion(
        items, {.preset = *trackknife::convert::find_encoder_preset("flac"),
                .maximum_parallelism = 1U,
                .target_sample_rate = {},
                .sample_rate_cap = {},
                .target_bit_depth = {},
                .keep_source_bit_depth = false,
                .carry_artwork = true});
    CHECK(result.has_value() && result->converted_count() == 1U);
    const auto reread = read_output_picture(directory.path() / "covered.flac", "flac");
    CHECK(reread.has_value());
    if (reread) {
        CHECK(reread->bytes == donor->bytes);
        CHECK(reread->type == 3U);
    }

    // Without artwork carriage the same conversion embeds nothing.
    const std::vector<trackknife::convert::ConversionScanItem> plain{
        {.item_index = 0U,
         .source_raw_path = source.native(),
         .selection = {},
         .range = {},
         .destination_raw_path = (directory.path() / "plain.flac").native(),
         .metadata = {}}};
    const auto without = trackknife::convert::scan_conversion(
        plain, {.preset = *trackknife::convert::find_encoder_preset("flac"),
                .maximum_parallelism = 1U,
                .target_sample_rate = {},
                .sample_rate_cap = {},
                .target_bit_depth = {},
                .keep_source_bit_depth = false,
                .carry_artwork = false});
    CHECK(without.has_value() && without->converted_count() == 1U);
    CHECK(!read_output_picture(directory.path() / "plain.flac", "flac").has_value());
}

} // namespace

// ADR-0134: the cap only lowers, keep-source depth follows the probed
// stored format, and conflicting option pairs fail before any I/O.
void capsRateDownOnlyAndKeepsSourceDepth() {
    TemporaryDirectory directory;
    const auto hires = directory.path() / "hires.wav";
    write_sine_wav_24_96(hires, 0.6, 0.5);
    const auto cd = directory.path() / "cd.wav";
    write_sine_wav(cd, 0.6, 0.5);
    const auto preset = *trackknife::convert::find_encoder_preset("flac");

    const auto request = [&preset](const std::filesystem::path& source,
                                   const std::filesystem::path& destination) {
        return trackknife::convert::AudioConversionRequest{.source_raw_path = source.native(),
                                                           .source_selection = {},
                                                           .source_range = {},
                                                           .destination_raw_path =
                                                               destination.native(),
                                                           .preset = preset,
                                                           .target_sample_rate = {},
                                                           .sample_rate_cap = 48'000,
                                                           .target_bit_depth = {},
                                                           .keep_source_bit_depth = true,
                                                           .metadata = {},
                                                           .artwork = {}};
    };
    const auto stored_format = [](const std::filesystem::path& path) {
        const auto probed = trackknife::formats::probe_local_media(path.native());
        return probed && probed->best_audio_stream
                   ? probed->audio_streams[static_cast<std::size_t>(*probed->best_audio_stream)]
                         .sample_format
                   : std::string{};
    };

    // 96 kHz / 24-bit lands at the cap and keeps a 24-bit (s32) store.
    const auto capped =
        trackknife::convert::convert_audio_file(request(hires, directory.path() / "capped.flac"));
    CHECK(capped.has_value());
    if (capped) {
        CHECK(capped->sample_rate == 48'000);
        CHECK(stored_format(directory.path() / "capped.flac") == "s32");
    }

    // 44.1 kHz / 16-bit stays untouched below the cap and keeps s16.
    const auto kept =
        trackknife::convert::convert_audio_file(request(cd, directory.path() / "kept.flac"));
    CHECK(kept.has_value());
    if (kept) {
        CHECK(kept->sample_rate == 44'100);
        CHECK(stored_format(directory.path() / "kept.flac") == "s16");
    }

    // Conflicting pairs fail closed before any I/O.
    auto forced_and_capped = request(cd, directory.path() / "conflict-a.flac");
    forced_and_capped.target_sample_rate = 48'000;
    const auto rate_conflict = trackknife::convert::convert_audio_file(forced_and_capped);
    CHECK(!rate_conflict.has_value() &&
          rate_conflict.error().code == trackknife::core::ErrorCode::invalid_argument);
    auto forced_and_kept = request(cd, directory.path() / "conflict-b.flac");
    forced_and_kept.sample_rate_cap = {};
    forced_and_kept.target_bit_depth = 16;
    const auto depth_conflict = trackknife::convert::convert_audio_file(forced_and_kept);
    CHECK(!depth_conflict.has_value() &&
          depth_conflict.error().code == trackknife::core::ErrorCode::invalid_argument);
    CHECK(entries_besides(directory.path(), {"hires.wav", "cd.wav", "capped.flac", "kept.flac"}) ==
          0U);
}

int main(const int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: trackknife_audio_convert_tests <fixture-directory>\n";
        return 1;
    }
    const std::filesystem::path fixture_directory{argv[1]};
    builtinPresetsProbeAvailable();
    convertsToEveryPresetAtomically();
    carriesMetadataIntoEveryPreset();
    carriesArtworkIntoEveryPreset(fixture_directory);
    carriesExternalCoverThroughTheScan(fixture_directory);
    resamplesOnRequestWithinEncoderConstraints();
    quantizesHiResToSixteenFortyFourWithDither();
    capsRateDownOnlyAndKeepsSourceDepth();
    refusesExistingDestinationAndMissingDirectory();
    cancellationLeavesNoPartialOutput();
    scansItemsInParallelIsolatingFailures();
    return failures == 0 ? 0 : 1;
}

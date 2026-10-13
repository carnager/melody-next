// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/stable_id.hpp"
#include "trackknife/formats/artwork.hpp"
#include "trackknife/formats/decoder.hpp"
#include "trackknife/formats/probe.hpp"

extern "C" {
#include <libavformat/avformat.h>
}

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
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

void append_u16(std::array<unsigned char, 44>& header, const std::size_t offset,
                const std::uint16_t value) {
    header.at(offset) = static_cast<unsigned char>(value & 0xFFU);
    header.at(offset + 1U) = static_cast<unsigned char>((value >> 8U) & 0xFFU);
}

void append_u32(std::array<unsigned char, 44>& header, const std::size_t offset,
                const std::uint32_t value) {
    for (std::size_t byte = 0U; byte < 4U; ++byte) {
        header.at(offset + byte) =
            static_cast<unsigned char>((value >> static_cast<unsigned>(byte * 8U)) & 0xFFU);
    }
}

void write_wave(const std::filesystem::path& path, const std::span<const std::int16_t> samples) {
    constexpr std::uint32_t sample_rate = 8'000U;
    constexpr std::uint16_t channels = 1U;
    constexpr std::uint16_t bits_per_sample = 16U;
    const auto data_bytes =
        static_cast<std::uint32_t>(samples.size()) * channels * (bits_per_sample / 8U);
    std::array<unsigned char, 44> header{};
    constexpr std::array riff{'R', 'I', 'F', 'F'};
    constexpr std::array wave{'W', 'A', 'V', 'E'};
    constexpr std::array format{'f', 'm', 't', ' '};
    constexpr std::array data{'d', 'a', 't', 'a'};
    std::ranges::copy(riff, header.begin());
    append_u32(header, 4U, 36U + data_bytes);
    std::ranges::copy(wave, header.begin() + 8);
    std::ranges::copy(format, header.begin() + 12);
    append_u32(header, 16U, 16U);
    append_u16(header, 20U, 1U);
    append_u16(header, 22U, channels);
    append_u32(header, 24U, sample_rate);
    append_u32(header, 28U, sample_rate * channels * (bits_per_sample / 8U));
    append_u16(header, 32U, channels * (bits_per_sample / 8U));
    append_u16(header, 34U, bits_per_sample);
    std::ranges::copy(data, header.begin() + 36);
    append_u32(header, 40U, data_bytes);

    std::ofstream output{path, std::ios::binary};
    output.write(reinterpret_cast<const char*>(header.data()),
                 static_cast<std::streamsize>(header.size()));
    for (const auto sample : samples) {
        const auto bits = static_cast<std::uint16_t>(sample);
        const std::array bytes{static_cast<char>(bits & 0xFFU),
                               static_cast<char>((bits >> 8U) & 0xFFU)};
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
}

[[nodiscard]] std::int16_t pattern_sample(const std::int64_t sample) {
    return static_cast<std::int16_t>(((sample * 37) % 20'001) - 10'000);
}

[[nodiscard]] float expected_float_sample(const std::int64_t sample) {
    return static_cast<float>(pattern_sample(sample)) / 32'768.0F;
}

[[nodiscard]] std::optional<std::vector<unsigned char>>
decode_base64_file(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        return std::nullopt;
    }
    const std::string encoded{std::istreambuf_iterator<char>{input},
                              std::istreambuf_iterator<char>{}};
    constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<unsigned char> decoded;
    decoded.reserve((encoded.size() * 3U) / 4U);
    unsigned accumulator = 0U;
    int bits = -8;
    for (const auto character : encoded) {
        const auto byte = static_cast<unsigned char>(character);
        if (std::isspace(byte) != 0) {
            continue;
        }
        if (character == '=') {
            break;
        }
        const auto value = alphabet.find(character);
        if (value == std::string_view::npos) {
            return std::nullopt;
        }
        accumulator = (accumulator << 6U) | static_cast<unsigned>(value);
        bits += 6;
        if (bits >= 0) {
            decoded.push_back(static_cast<unsigned char>((accumulator >> bits) & 0xFFU));
            bits -= 8;
        }
    }
    return decoded;
}

[[nodiscard]] std::vector<float> decode_all(trackknife::formats::AudioDecoder& decoder) {
    std::vector<float> samples;
    std::int64_t expected_start = decoder.sample_range().start_sample;
    while (true) {
        auto chunk = decoder.next_chunk();
        CHECK(chunk.has_value());
        if (!chunk || !*chunk) {
            break;
        }
        if ((*chunk)->start_sample != expected_start) {
            std::cerr << "non-contiguous chunk: expected " << expected_start << ", got "
                      << (*chunk)->start_sample << ", frames "
                      << (*chunk)->frame_count(decoder.output_format().channels) << '\n';
        }
        CHECK((*chunk)->start_sample == expected_start);
        expected_start +=
            static_cast<std::int64_t>((*chunk)->frame_count(decoder.output_format().channels));
        samples.insert(samples.end(), (*chunk)->interleaved_samples.begin(),
                       (*chunk)->interleaved_samples.end());
    }
    return samples;
}

void probesRealWaveAndPreservesRawPath() {
    const auto root =
        std::filesystem::temp_directory_path() /
        ("trackknife-format-probe-" + trackknife::core::StableId::random().to_string());
    std::error_code error;
    CHECK(std::filesystem::create_directories(root, error));
    const std::string invalid_name{"probe-\xFF.wav", 11U};
    const auto path = root / std::filesystem::path{invalid_name};
    const std::array<std::int16_t, 800> silence{};
    write_wave(path, silence);

    const auto probe = trackknife::formats::probe_local_media(path.native());
    CHECK(probe.has_value());
    CHECK(probe && probe->raw_path == path.native());
    CHECK(probe && probe->container_names.find("wav") != std::string::npos);
    CHECK(probe && probe->duration_ms && *probe->duration_ms == 100);
    CHECK(probe && probe->audio_streams.size() == 1U);
    CHECK(probe && probe->best_audio_stream == 0);
    CHECK(probe && probe->audio_streams.front().codec_name == "pcm_s16le");
    CHECK(probe && probe->audio_streams.front().sample_rate == 8'000);
    CHECK(probe && probe->audio_streams.front().channels == 1);

    auto decoder = trackknife::formats::AudioDecoder::open(path.native());
    CHECK(decoder.has_value());
    CHECK(decoder && decoder->output_format().sample_rate == 8'000);
    CHECK(decoder && decoder->output_format().channels == 1);
    CHECK(decoder && decoder->duration_samples() == 800);
    std::size_t decoded_frames = 0U;
    bool decoded_only_silence = true;
    while (decoder) {
        auto chunk = decoder->next_chunk();
        CHECK(chunk.has_value());
        if (!chunk || !*chunk) {
            break;
        }
        decoded_frames += (*chunk)->frame_count(decoder->output_format().channels);
        decoded_only_silence =
            decoded_only_silence &&
            std::ranges::all_of((*chunk)->interleaved_samples,
                                [](const float sample) { return sample == 0.0F; });
    }
    CHECK(decoded_frames == 800U);
    CHECK(decoded_only_silence);

    trackknife::core::CancellationSource decode_cancellation;
    auto cancelled_decoder =
        trackknife::formats::AudioDecoder::open(path.native(), decode_cancellation.token());
    CHECK(cancelled_decoder.has_value());
    if (cancelled_decoder) {
        decode_cancellation.request_cancellation();
        auto cancelled_chunk = cancelled_decoder->next_chunk();
        CHECK(!cancelled_chunk);
        CHECK(cancelled_chunk.error().code == trackknife::core::ErrorCode::cancelled);
    }

    trackknife::core::CancellationSource cancellation;
    cancellation.request_cancellation();
    const auto cancelled =
        trackknife::formats::probe_local_media(path.native(), cancellation.token());
    CHECK(!cancelled);
    CHECK(cancelled.error().code == trackknife::core::ErrorCode::cancelled);
    std::filesystem::remove_all(root, error);
    CHECK(!error);
}

void seeksAndTrimsAdjacentSampleRanges() {
    const auto root =
        std::filesystem::temp_directory_path() /
        ("trackknife-format-seek-" + trackknife::core::StableId::random().to_string());
    std::error_code error;
    CHECK(std::filesystem::create_directories(root, error));
    const auto path = root / "pattern.wav";
    std::vector<std::int16_t> samples(4'096U);
    for (std::size_t index = 0U; index < samples.size(); ++index) {
        samples[index] = pattern_sample(static_cast<std::int64_t>(index));
    }
    write_wave(path, samples);

    auto decoder = trackknife::formats::AudioDecoder::open(path.native());
    CHECK(decoder.has_value());
    if (decoder) {
        const auto seek = decoder->seek_to_sample(1'234);
        CHECK(seek.has_value());
        auto chunk = decoder->next_chunk();
        CHECK(chunk.has_value());
        CHECK(chunk && *chunk);
        CHECK(chunk && *chunk && (*chunk)->start_sample == 1'234);
        CHECK(chunk && *chunk && !(*chunk)->interleaved_samples.empty());
        CHECK(chunk && *chunk &&
              (*chunk)->interleaved_samples.front() == expected_float_sample(1'234));

        const auto seek_to_end = decoder->seek_to_sample(4'096);
        CHECK(seek_to_end.has_value());
        auto end = decoder->next_chunk();
        CHECK(end.has_value());
        CHECK(end && !*end);
        const auto seek_back = decoder->seek_to_sample(321);
        CHECK(seek_back.has_value());
        auto earlier = decoder->next_chunk();
        CHECK(earlier.has_value());
        CHECK(earlier && *earlier && (*earlier)->start_sample == 321);
        CHECK(earlier && *earlier &&
              (*earlier)->interleaved_samples.front() == expected_float_sample(321));
    }

    const auto decode_range = [&path](const trackknife::formats::SampleRange range) {
        std::vector<float> decoded;
        auto ranged = trackknife::formats::AudioDecoder::open_segment(path.native(), range);
        CHECK(ranged.has_value());
        if (!ranged) {
            return decoded;
        }
        CHECK(ranged->sample_range() == range);
        auto expected_position = range.start_sample;
        while (true) {
            auto chunk = ranged->next_chunk();
            CHECK(chunk.has_value());
            if (!chunk || !*chunk) {
                break;
            }
            CHECK((*chunk)->start_sample == expected_position);
            expected_position +=
                static_cast<std::int64_t>((*chunk)->frame_count(ranged->output_format().channels));
            decoded.insert(decoded.end(), (*chunk)->interleaved_samples.begin(),
                           (*chunk)->interleaved_samples.end());
        }
        CHECK(!range.end_sample || expected_position == *range.end_sample);
        return decoded;
    };

    const auto first = decode_range({.start_sample = 0, .end_sample = 2'048});
    const auto second = decode_range({.start_sample = 2'048, .end_sample = 4'096});
    CHECK(first.size() == 2'048U);
    CHECK(second.size() == 2'048U);
    std::vector<float> joined = first;
    joined.insert(joined.end(), second.begin(), second.end());
    CHECK(joined.size() == samples.size());
    for (std::size_t index = 0U; index < joined.size(); ++index) {
        CHECK(joined[index] == expected_float_sample(static_cast<std::int64_t>(index)));
    }

    const auto invalid = trackknife::formats::AudioDecoder::open_segment(
        path.native(), {.start_sample = 200, .end_sample = 100});
    CHECK(!invalid);
    CHECK(invalid.error().code == trackknife::core::ErrorCode::invalid_argument);
    auto bounded = trackknife::formats::AudioDecoder::open_segment(
        path.native(), {.start_sample = 100, .end_sample = 200});
    CHECK(bounded.has_value());
    if (bounded) {
        const auto outside = bounded->seek_to_sample(99);
        CHECK(!outside);
        CHECK(outside.error().code == trackknife::core::ErrorCode::invalid_argument);
    }

    std::filesystem::remove_all(root, error);
    CHECK(!error);
}

void decodesCompressedGaplessFixtures(const std::filesystem::path& fixture_directory) {
    struct Fixture {
        std::string_view encoded_name;
        std::string_view materialized_name;
        std::string_view codec_name;
    };
    constexpr std::array fixtures{
        Fixture{"gapless-tone-aac-m4a.b64", "gapless-tone.m4a", "aac"},
        Fixture{"gapless-tone-mp3.b64", "gapless-tone.mp3", "mp3"},
        Fixture{"gapless-tone-opus.b64", "gapless-tone.opus", "opus"},
    };
    const auto root =
        std::filesystem::temp_directory_path() /
        ("trackknife-gapless-fixtures-" + trackknife::core::StableId::random().to_string());
    std::error_code error;
    CHECK(std::filesystem::create_directories(root, error));

    for (const auto& fixture : fixtures) {
        const auto binary = decode_base64_file(fixture_directory / fixture.encoded_name);
        CHECK(binary.has_value());
        if (!binary) {
            continue;
        }
        const auto path = root / fixture.materialized_name;
        std::ofstream output{path, std::ios::binary};
        output.write(reinterpret_cast<const char*>(binary->data()),
                     static_cast<std::streamsize>(binary->size()));
        output.close();
        CHECK(output.good());

        const auto probe = trackknife::formats::probe_local_media(path.native());
        CHECK(probe.has_value());
        CHECK(probe && probe->audio_streams.size() == 1U);
        CHECK(probe && probe->audio_streams.front().codec_name == fixture.codec_name);
        CHECK(probe && probe->audio_streams.front().sample_rate == 48'000);
        CHECK(probe && probe->audio_streams.front().channels == 1);

        auto decoder = trackknife::formats::AudioDecoder::open(path.native());
        CHECK(decoder.has_value());
        if (!decoder) {
            continue;
        }
        auto decoded = decode_all(*decoder);
        // Before libavformat 63 (FFmpeg 9) the MP4 demuxer leaves AAC's
        // encoder padding on the end: the tone decodes to 5,120 frames, a few
        // milliseconds of silence that cost only AAC gapless joins. Trackknife
        // relies on FFmpeg for this rather than trimming itself.
        const auto expected_frames =
            fixture.codec_name == "aac" && AV_VERSION_MAJOR(avformat_version()) < 63 ? 5'120U
                                                                                     : 4'800U;
        if (decoded.size() != expected_frames) {
            std::cerr << fixture.materialized_name << " decoded " << decoded.size() << " frames\n";
        }
        CHECK(decoded.size() == expected_frames);
        if (decoded.size() == expected_frames) {
            const auto head_energy = std::ranges::fold_left(
                decoded | std::views::take(128U), 0.0F,
                [](const float sum, const float sample) { return sum + std::abs(sample); });
            // The padding is silent, so the tone's own tail ends at 4,800.
            const auto tail_energy = std::ranges::fold_left(
                decoded | std::views::take(4'800U) | std::views::drop(4'800U - 128U), 0.0F,
                [](const float sum, const float sample) { return sum + std::abs(sample); });
            CHECK(head_energy > 1.0F);
            CHECK(tail_energy > 1.0F);
        }

        auto segment = trackknife::formats::AudioDecoder::open_segment(
            path.native(), {.start_sample = 1'200, .end_sample = 3'600});
        CHECK(segment.has_value());
        if (segment) {
            const auto segment_samples = decode_all(*segment);
            if (segment_samples.size() != 2'400U) {
                std::cerr << fixture.materialized_name << " segment decoded "
                          << segment_samples.size() << " frames\n";
            }
            CHECK(segment_samples.size() == 2'400U);
            if (decoded.size() == expected_frames && segment_samples.size() == 2'400U) {
                const auto matches_full_decode = std::ranges::equal(
                    segment_samples, decoded | std::views::drop(1'200U) | std::views::take(2'400U));
                if (!matches_full_decode) {
                    std::cerr << fixture.materialized_name
                              << " seek output differs from its full-decode slice\n";
                }
                CHECK(matches_full_decode);
            }
        }
    }

    std::filesystem::remove_all(root, error);
    CHECK(!error);
}

void normalizesVorbisFrameTimeline(const std::filesystem::path& fixture_directory) {
    const auto binary = decode_base64_file(fixture_directory / "vorbis-positive-start.b64");
    CHECK(binary.has_value());
    if (!binary) {
        return;
    }
    const auto path =
        std::filesystem::temp_directory_path() /
        ("trackknife-vorbis-timeline-" + trackknife::core::StableId::random().to_string() + ".ogg");
    std::ofstream output{path, std::ios::binary};
    output.write(reinterpret_cast<const char*>(binary->data()),
                 static_cast<std::streamsize>(binary->size()));
    output.close();
    CHECK(output.good());

    auto decoder = trackknife::formats::AudioDecoder::open(path.native());
    CHECK(decoder.has_value());
    if (decoder) {
        CHECK(decoder->output_format().sample_rate == 44'100);
        CHECK(decoder->output_format().channels == 1);
        const auto decoded = decode_all(*decoder);
        CHECK(decoded.size() == 4'282U);

        auto segment = trackknife::formats::AudioDecoder::open_segment(
            path.native(), {.start_sample = 1'000, .end_sample = 3'000});
        CHECK(segment.has_value());
        if (segment) {
            const auto segment_samples = decode_all(*segment);
            CHECK(segment_samples.size() == 2'000U);
            CHECK(std::ranges::equal(segment_samples, decoded | std::views::drop(1'000U) |
                                                          std::views::take(2'000U)));
        }
    }

    std::error_code error;
    std::filesystem::remove(path, error);
    CHECK(!error);
}

void probesTaggedFlacFixture(const std::filesystem::path& fixture_directory) {
    const auto binary = decode_base64_file(fixture_directory / "tagged-tone-flac.b64");
    CHECK(binary.has_value());
    if (!binary) {
        return;
    }
    const auto path =
        std::filesystem::temp_directory_path() /
        ("trackknife-tagged-flac-" + trackknife::core::StableId::random().to_string() + ".flac");
    std::ofstream output{path, std::ios::binary};
    output.write(reinterpret_cast<const char*>(binary->data()),
                 static_cast<std::streamsize>(binary->size()));
    output.close();
    CHECK(output.good());

    const auto probe = trackknife::formats::probe_local_media(path.native());
    CHECK(probe.has_value());
    if (probe) {
        CHECK(probe->audio_streams.size() == 1U);
        CHECK(probe->audio_streams.front().codec_name == "flac");
        CHECK(probe->audio_streams.front().sample_rate == 44'100);
        CHECK(probe->audio_streams.front().channels == 1);
        const auto tag_value = [&probe](const std::string_view name) -> std::string {
            for (const auto& tag : probe->tags) {
                if (tag.name == name) {
                    return tag.value;
                }
            }
            return {};
        };
        CHECK(probe->tags.size() == 5U);
        CHECK(tag_value("title") == "Fixture Tone");
        CHECK(tag_value("artist") == "Trackknife Project");
        CHECK(tag_value("album") == "Trackbench Fixtures");
        CHECK(tag_value("date") == "2026");
        CHECK(tag_value("track") == "3");
    }

    auto decoder = trackknife::formats::AudioDecoder::open(path.native());
    CHECK(decoder.has_value());
    if (decoder) {
        CHECK(decoder->output_format().sample_rate == 44'100);
        CHECK(decoder->output_format().channels == 1);
        const auto decoded = decode_all(*decoder);
        CHECK(decoded.size() == 4'410U);
        const auto loud = [](const float sample) { return std::fabs(sample) > 0.01F; };
        CHECK(std::ranges::any_of(decoded | std::views::take(64U), loud));
        CHECK(std::ranges::any_of(decoded | std::views::drop(decoded.size() - 64U), loud));
    }

    std::error_code error;
    std::filesystem::remove(path, error);
    CHECK(!error);
}

void probesTaggedWavPackFixture(const std::filesystem::path& fixture_directory) {
    const auto binary = decode_base64_file(fixture_directory / "tagged-tone-wavpack.b64");
    CHECK(binary.has_value());
    if (!binary) {
        return;
    }
    const auto path =
        std::filesystem::temp_directory_path() /
        ("trackknife-tagged-wavpack-" + trackknife::core::StableId::random().to_string() + ".wv");
    std::ofstream output{path, std::ios::binary};
    output.write(reinterpret_cast<const char*>(binary->data()),
                 static_cast<std::streamsize>(binary->size()));
    output.close();
    CHECK(output.good());

    const auto probe = trackknife::formats::probe_local_media(path.native());
    CHECK(probe.has_value());
    if (probe) {
        CHECK(probe->audio_streams.size() == 1U);
        CHECK(probe->audio_streams.front().codec_name == "wavpack");
        CHECK(probe->audio_streams.front().sample_rate == 44'100);
        CHECK(probe->audio_streams.front().channels == 1);
        const auto tag_value = [&probe](const std::string_view name) -> std::string {
            for (const auto& tag : probe->tags) {
                if (tag.name == name) {
                    return tag.value;
                }
            }
            return {};
        };
        CHECK(probe->tags.size() == 2U);
        CHECK(tag_value("title") == "Fixture Tone");
        CHECK(tag_value("artist") == "Trackknife Project");
    }

    auto decoder = trackknife::formats::AudioDecoder::open(path.native());
    CHECK(decoder.has_value());
    if (decoder) {
        CHECK(decoder->output_format().sample_rate == 44'100);
        CHECK(decoder->output_format().channels == 1);
        const auto decoded = decode_all(*decoder);
        CHECK(decoded.size() == 4'410U);
        const auto loud = [](const float sample) { return std::fabs(sample) > 0.01F; };
        CHECK(std::ranges::any_of(decoded | std::views::take(64U), loud));
        CHECK(std::ranges::any_of(decoded | std::views::drop(decoded.size() - 64U), loud));
    }

    std::error_code error;
    std::filesystem::remove(path, error);
    CHECK(!error);
}

void probesTaggedAiffFixture(const std::filesystem::path& fixture_directory) {
    const auto binary = decode_base64_file(fixture_directory / "tagged-tone-aiff.b64");
    CHECK(binary.has_value());
    if (!binary) {
        return;
    }
    const auto path =
        std::filesystem::temp_directory_path() /
        ("trackknife-tagged-aiff-" + trackknife::core::StableId::random().to_string() + ".aiff");
    std::ofstream output{path, std::ios::binary};
    output.write(reinterpret_cast<const char*>(binary->data()),
                 static_cast<std::streamsize>(binary->size()));
    output.close();
    CHECK(output.good());

    const auto probe = trackknife::formats::probe_local_media(path.native());
    CHECK(probe.has_value());
    if (probe) {
        CHECK(probe->container_names.find("aiff") != std::string::npos);
        CHECK(probe->audio_streams.size() == 1U);
        CHECK(probe->audio_streams.front().codec_name == "pcm_s24be");
        CHECK(probe->audio_streams.front().sample_rate == 44'100);
        CHECK(probe->audio_streams.front().channels == 1);
        const auto tag_value = [&probe](const std::string_view name) -> std::string {
            for (const auto& tag : probe->tags) {
                if (tag.name == name) {
                    return tag.value;
                }
            }
            return {};
        };
        CHECK(probe->tags.size() == 5U);
        CHECK(tag_value("title") == "Fixture Tone");
        CHECK(tag_value("artist") == "Trackknife Project");
        CHECK(tag_value("album") == "Trackbench Fixtures");
        CHECK(tag_value("date") == "2026");
        CHECK(tag_value("track") == "3");
    }

    auto decoder = trackknife::formats::AudioDecoder::open(path.native());
    CHECK(decoder.has_value());
    if (decoder) {
        CHECK(decoder->output_format().sample_rate == 44'100);
        CHECK(decoder->output_format().channels == 1);
        const auto decoded = decode_all(*decoder);
        CHECK(decoded.size() == 4'410U);
        const auto loud = [](const float sample) { return std::fabs(sample) > 0.01F; };
        CHECK(std::ranges::any_of(decoded | std::views::take(64U), loud));
        CHECK(std::ranges::any_of(decoded | std::views::drop(decoded.size() - 64U), loud));

        auto segment = trackknife::formats::AudioDecoder::open_segment(
            path.native(), {.start_sample = 700, .end_sample = 3'700});
        CHECK(segment.has_value());
        if (segment) {
            const auto segment_samples = decode_all(*segment);
            CHECK(segment_samples.size() == 3'000U);
            CHECK(std::ranges::equal(segment_samples,
                                     decoded | std::views::drop(700U) | std::views::take(3'000U)));
        }
    }

    std::error_code error;
    std::filesystem::remove(path, error);
    CHECK(!error);
}

void probesRf64Fixture(const std::filesystem::path& fixture_directory) {
    const auto binary = decode_base64_file(fixture_directory / "rf64-tone-wav.b64");
    CHECK(binary.has_value());
    if (!binary) {
        return;
    }
    constexpr std::array<unsigned char, 4> rf64_magic{'R', 'F', '6', '4'};
    constexpr std::array<unsigned char, 4> wave_magic{'W', 'A', 'V', 'E'};
    constexpr std::array<unsigned char, 4> ds64_magic{'d', 's', '6', '4'};
    CHECK(binary->size() >= 48U);
    CHECK(std::equal(rf64_magic.begin(), rf64_magic.end(), binary->begin()));
    CHECK(std::equal(wave_magic.begin(), wave_magic.end(), binary->begin() + 8));
    CHECK(std::equal(ds64_magic.begin(), ds64_magic.end(), binary->begin() + 12));

    const auto path =
        std::filesystem::temp_directory_path() /
        ("trackknife-rf64-" + trackknife::core::StableId::random().to_string() + ".wav");
    std::ofstream output{path, std::ios::binary};
    output.write(reinterpret_cast<const char*>(binary->data()),
                 static_cast<std::streamsize>(binary->size()));
    output.close();
    CHECK(output.good());

    const auto probe = trackknife::formats::probe_local_media(path.native());
    CHECK(probe.has_value());
    if (probe) {
        CHECK(probe->container_names.find("wav") != std::string::npos);
        CHECK(probe->duration_ms && *probe->duration_ms == 50);
        CHECK(probe->audio_streams.size() == 1U);
        CHECK(probe->audio_streams.front().codec_name == "pcm_s24le");
        CHECK(probe->audio_streams.front().sample_rate == 48'000);
        CHECK(probe->audio_streams.front().channels == 2);
        const auto tag_value = [&probe](const std::string_view name) -> std::string {
            for (const auto& tag : probe->tags) {
                if (tag.name == name) {
                    return tag.value;
                }
            }
            return {};
        };
        CHECK(tag_value("title") == "RF64 Fixture");
        CHECK(tag_value("artist") == "Trackknife Project");
        CHECK(tag_value("time_reference") == "0");
    }

    auto decoder = trackknife::formats::AudioDecoder::open(path.native());
    CHECK(decoder.has_value());
    if (decoder) {
        CHECK(decoder->output_format().sample_rate == 48'000);
        CHECK(decoder->output_format().channels == 2);
        CHECK(decoder->duration_samples() == 2'400);
        const auto decoded = decode_all(*decoder);
        CHECK(decoded.size() == 4'800U);
        const auto loud = [](const float sample) { return std::fabs(sample) > 0.01F; };
        CHECK(std::ranges::any_of(decoded | std::views::take(128U), loud));
        CHECK(std::ranges::any_of(decoded | std::views::drop(decoded.size() - 128U), loud));

        auto segment = trackknife::formats::AudioDecoder::open_segment(
            path.native(), {.start_sample = 500, .end_sample = 1'900});
        CHECK(segment.has_value());
        if (segment) {
            const auto segment_samples = decode_all(*segment);
            CHECK(segment_samples.size() == 2'800U);
            CHECK(std::ranges::equal(segment_samples, decoded | std::views::drop(1'000U) |
                                                          std::views::take(2'800U)));
        }
    }

    std::error_code error;
    std::filesystem::remove(path, error);
    CHECK(!error);
}

void probesWave64Fixture(const std::filesystem::path& fixture_directory) {
    const auto binary = decode_base64_file(fixture_directory / "wave64-float.b64");
    CHECK(binary.has_value());
    if (!binary) {
        return;
    }
    constexpr std::array<unsigned char, 16> riff_guid{
        'r',   'i',   'f',   'f',   0x2eU, 0x91U, 0xcfU, 0x11U,
        0xa5U, 0xd6U, 0x28U, 0xdbU, 0x04U, 0xc1U, 0x00U, 0x00U,
    };
    constexpr std::array<unsigned char, 16> wave_guid{
        'w',   'a',   'v',   'e',   0xf3U, 0xacU, 0xd3U, 0x11U,
        0x8cU, 0xd1U, 0x00U, 0xc0U, 0x4fU, 0x8eU, 0xdbU, 0x8aU,
    };
    CHECK(binary->size() >= 64U);
    CHECK(std::equal(riff_guid.begin(), riff_guid.end(), binary->begin()));
    CHECK(std::equal(wave_guid.begin(), wave_guid.end(), binary->begin() + 24));

    const auto path =
        std::filesystem::temp_directory_path() /
        ("trackknife-wave64-" + trackknife::core::StableId::random().to_string() + ".w64");
    std::ofstream output{path, std::ios::binary};
    output.write(reinterpret_cast<const char*>(binary->data()),
                 static_cast<std::streamsize>(binary->size()));
    output.close();
    CHECK(output.good());

    const auto probe = trackknife::formats::probe_local_media(path.native());
    CHECK(probe.has_value());
    if (probe) {
        CHECK(probe->container_names.find("w64") != std::string::npos);
        CHECK(probe->duration_ms && *probe->duration_ms == 50);
        CHECK(probe->audio_streams.size() == 1U);
        CHECK(probe->audio_streams.front().codec_name == "pcm_f32le");
        CHECK(probe->audio_streams.front().sample_format == "flt");
        CHECK(probe->audio_streams.front().sample_rate == 48'000);
        CHECK(probe->audio_streams.front().channels == 2);
    }

    auto decoder = trackknife::formats::AudioDecoder::open(path.native());
    CHECK(decoder.has_value());
    if (decoder) {
        CHECK(decoder->output_format().sample_rate == 48'000);
        CHECK(decoder->output_format().channels == 2);
        CHECK(decoder->duration_samples() == 2'400);
        const auto decoded = decode_all(*decoder);
        CHECK(decoded.size() == 4'800U);
        const auto loud = [](const float sample) { return std::fabs(sample) > 0.01F; };
        CHECK(std::ranges::any_of(decoded | std::views::take(128U), loud));
        CHECK(std::ranges::any_of(decoded | std::views::drop(decoded.size() - 128U), loud));

        auto segment = trackknife::formats::AudioDecoder::open_segment(
            path.native(), {.start_sample = 500, .end_sample = 1'900});
        CHECK(segment.has_value());
        if (segment) {
            const auto segment_samples = decode_all(*segment);
            CHECK(segment_samples.size() == 2'800U);
            CHECK(std::ranges::equal(segment_samples, decoded | std::views::drop(1'000U) |
                                                          std::views::take(2'800U)));
        }
    }

    std::error_code error;
    std::filesystem::remove(path, error);
    CHECK(!error);
}

void probesAndDecodesContainerChapters(const std::filesystem::path& fixture_directory) {
    const auto binary = decode_base64_file(fixture_directory / "container-chapters-mka.b64");
    CHECK(binary.has_value());
    if (!binary) {
        return;
    }
    const auto path = std::filesystem::temp_directory_path() /
                      ("trackknife-container-chapters-" +
                       trackknife::core::StableId::random().to_string() + ".mka");
    std::ofstream output{path, std::ios::binary};
    output.write(reinterpret_cast<const char*>(binary->data()),
                 static_cast<std::streamsize>(binary->size()));
    output.close();
    CHECK(output.good());

    const auto probe = trackknife::formats::probe_local_media(path.native());
    CHECK(probe.has_value());
    if (probe) {
        CHECK(probe->best_audio_stream == 0);
        CHECK(probe->audio_streams.size() == 1U);
        CHECK(probe->audio_streams.front().codec_name == "flac");
        CHECK(probe->audio_streams.front().sample_rate == 48'000);
        CHECK(probe->chapters.size() == 2U);
        if (probe->chapters.size() == 2U) {
            CHECK(probe->chapters[0].source_index == 0U);
            CHECK(probe->chapters[0].start_sample == 0);
            CHECK(probe->chapters[0].end_sample == 4'800);
            CHECK(probe->chapters[1].source_index == 1U);
            CHECK(probe->chapters[1].start_sample == 4'800);
            CHECK(probe->chapters[1].end_sample == 9'600);
            const auto tag = [](const trackknife::formats::ProbedChapter& chapter,
                                const std::string_view name) {
                const auto found =
                    std::ranges::find(chapter.tags, name, &trackknife::formats::ProbedTag::name);
                return found == chapter.tags.end() ? std::string{} : found->value;
            };
            CHECK(tag(probe->chapters[0], "title") == "First chapter");
            CHECK(tag(probe->chapters[0], "ARTIST") == "First Artist");
            CHECK(tag(probe->chapters[1], "title") == "Second chapter");
        }
    }

    auto decoder = trackknife::formats::AudioDecoder::open(path.native());
    CHECK(decoder.has_value());
    if (decoder) {
        CHECK(decoder->duration_samples() == 9'600);
        const auto decoded = decode_all(*decoder);
        CHECK(decoded.size() == 9'600U);
        if (probe && probe->chapters.size() == 2U) {
            std::vector<float> joined;
            for (const auto& chapter : probe->chapters) {
                auto segment = trackknife::formats::AudioDecoder::open_segment(
                    path.native(),
                    {.start_sample = chapter.start_sample, .end_sample = chapter.end_sample});
                CHECK(segment.has_value());
                if (segment) {
                    auto samples = decode_all(*segment);
                    if (samples.size() != 4'800U) {
                        std::cerr << "container chapter " << chapter.source_index << " decoded "
                                  << samples.size() << " frames\n";
                    }
                    CHECK(samples.size() == 4'800U);
                    joined.insert(joined.end(), samples.begin(), samples.end());
                }
            }
            CHECK(joined == decoded);
        }
    }

    std::error_code error;
    std::filesystem::remove(path, error);
    CHECK(!error);

    const auto partial_binary = decode_base64_file(fixture_directory / "partial-chapters-mka.b64");
    CHECK(partial_binary.has_value());
    if (partial_binary) {
        const auto partial_path = std::filesystem::temp_directory_path() /
                                  ("trackknife-partial-chapters-" +
                                   trackknife::core::StableId::random().to_string() + ".mka");
        std::ofstream partial_output{partial_path, std::ios::binary};
        partial_output.write(reinterpret_cast<const char*>(partial_binary->data()),
                             static_cast<std::streamsize>(partial_binary->size()));
        partial_output.close();
        CHECK(partial_output.good());
        const auto partial_probe = trackknife::formats::probe_local_media(partial_path.native());
        CHECK(partial_probe.has_value());
        CHECK(partial_probe && partial_probe->duration_ms == 200);
        CHECK(partial_probe && partial_probe->chapters.empty());
        std::filesystem::remove(partial_path, error);
        CHECK(!error);
    }
}

void probesAndDecodesCodecNativeSubsongs(const std::filesystem::path& fixture_directory) {
    const auto binary = decode_base64_file(fixture_directory / "two-subsongs-mod.b64");
    CHECK(binary.has_value());
    if (!binary) {
        return;
    }
    const auto path =
        std::filesystem::temp_directory_path() /
        ("trackknife-codec-subsongs-" + trackknife::core::StableId::random().to_string() + ".mod");
    std::ofstream output{path, std::ios::binary};
    output.write(reinterpret_cast<const char*>(binary->data()),
                 static_cast<std::streamsize>(binary->size()));
    output.close();
    CHECK(output.good());

    const auto probe = trackknife::formats::probe_local_media(path.native());
    CHECK(probe.has_value());
    CHECK(probe && probe->container_names == "libopenmpt");
    CHECK(probe && probe->chapters.empty());
    CHECK(probe && probe->subsongs.size() == 2U);
    if (probe && probe->subsongs.size() == 2U) {
        for (std::size_t index = 0U; index < probe->subsongs.size(); ++index) {
            const auto& subsong = probe->subsongs[index];
            CHECK(subsong.source_index == index);
            CHECK(subsong.selection.stream_index == 0);
            CHECK(subsong.selection.subsong_index == static_cast<int>(index));
            CHECK(subsong.duration_ms == 600);
            CHECK(subsong.duration_samples == 28'800);
        }
    }

    std::array<std::vector<float>, 2> decoded;
    for (std::size_t index = 0U; index < decoded.size(); ++index) {
        const trackknife::formats::AudioSourceSelection selection{
            .stream_index = 0,
            .subsong_index = static_cast<int>(index),
        };
        auto decoder = trackknife::formats::AudioDecoder::open_selected_segment(
            path.native(), selection, {.start_sample = 0, .end_sample = 28'800});
        CHECK(decoder.has_value());
        if (!decoder) {
            continue;
        }
        CHECK(decoder->source_selection() == selection);
        CHECK(decoder->output_format().sample_rate == 48'000);
        CHECK(decoder->output_format().channels == 2);
        CHECK(decoder->duration_samples() == 28'800);
        decoded[index] = decode_all(*decoder);
        CHECK(decoded[index].size() == 57'600U);
        CHECK(std::ranges::any_of(decoded[index],
                                  [](const float sample) { return std::abs(sample) > 0.0001F; }));
    }
    CHECK(decoded[0] != decoded[1]);

    const auto invalid_stream = trackknife::formats::AudioDecoder::open_selected(
        path.native(), {.stream_index = 1, .subsong_index = 0});
    CHECK(!invalid_stream);
    CHECK(!invalid_stream &&
          invalid_stream.error().code == trackknife::core::ErrorCode::invalid_argument);

    std::error_code error;
    std::filesystem::remove(path, error);
    CHECK(!error);
}

void loadsEmbeddedArtworkExactly(const std::filesystem::path& fixture_directory) {
    const auto with_art = decode_base64_file(fixture_directory / "art-tone-flac.b64");
    const auto without_art = decode_base64_file(fixture_directory / "tagged-tone-flac.b64");
    CHECK(with_art.has_value());
    CHECK(without_art.has_value());
    if (!with_art || !without_art) {
        return;
    }
    const auto root = std::filesystem::temp_directory_path() /
                      ("trackknife-artwork-" + trackknife::core::StableId::random().to_string());
    std::error_code error;
    CHECK(std::filesystem::create_directories(root, error));
    const auto art_path = root / "art-tone.flac";
    const auto plain_path = root / "tagged-tone.flac";
    for (const auto& [path, binary] :
         {std::pair{art_path, *with_art}, std::pair{plain_path, *without_art}}) {
        std::ofstream output{path, std::ios::binary};
        output.write(reinterpret_cast<const char*>(binary.data()),
                     static_cast<std::streamsize>(binary.size()));
        output.close();
        CHECK(output.good());
    }

    const auto artwork = trackknife::formats::load_embedded_artwork(art_path.native());
    CHECK(artwork.has_value());
    if (artwork) {
        constexpr std::array<unsigned char, 4> png_magic{0x89U, 'P', 'N', 'G'};
        CHECK(artwork->size() > png_magic.size());
        CHECK(std::equal(png_magic.begin(), png_magic.end(), artwork->begin()));
    }

    const auto absent = trackknife::formats::load_embedded_artwork(plain_path.native());
    CHECK(!absent.has_value());
    CHECK(!absent && absent.error().code == trackknife::core::ErrorCode::not_found);

    trackknife::core::CancellationSource cancellation;
    cancellation.request_cancellation();
    const auto cancelled =
        trackknife::formats::load_embedded_artwork(art_path.native(), cancellation.token());
    CHECK(!cancelled.has_value());
    CHECK(!cancelled && cancelled.error().code == trackknife::core::ErrorCode::cancelled);

    std::filesystem::remove_all(root, error);
    CHECK(!error);
}

} // namespace

int main(const int argc, char** argv) {
    CHECK(argc == 2);
    probesRealWaveAndPreservesRawPath();
    seeksAndTrimsAdjacentSampleRanges();
    if (argc == 2) {
        decodesCompressedGaplessFixtures(argv[1]);
        normalizesVorbisFrameTimeline(argv[1]);
        probesTaggedFlacFixture(argv[1]);
        probesTaggedWavPackFixture(argv[1]);
        probesTaggedAiffFixture(argv[1]);
        probesRf64Fixture(argv[1]);
        probesWave64Fixture(argv[1]);
        probesAndDecodesContainerChapters(argv[1]);
        probesAndDecodesCodecNativeSubsongs(argv[1]);
        loadsEmbeddedArtworkExactly(argv[1]);
    }
    return failures == 0 ? 0 : 1;
}

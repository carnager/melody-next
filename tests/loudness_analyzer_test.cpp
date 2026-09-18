// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/core/stable_id.hpp"
#include "trackknife/formats/decoder.hpp"
#include "trackknife/loudness/analyzer.hpp"
#include "trackknife/loudness/grouping.hpp"
#include "trackknife/loudness/scan.hpp"
#include "trackknife/metadata/document.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numbers>
#include <optional>
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

constexpr int sample_rate = 44'100;
constexpr double tone_hertz = 997.0;

[[nodiscard]] std::vector<float> sine_frames(const double amplitude, const int channels,
                                             const double seconds) {
    const auto frames = static_cast<std::size_t>(seconds * sample_rate);
    std::vector<float> samples;
    samples.reserve(frames * static_cast<std::size_t>(channels));
    for (std::size_t frame = 0U; frame < frames; ++frame) {
        const auto value =
            static_cast<float>(amplitude * std::sin(2.0 * std::numbers::pi * tone_hertz *
                                                    static_cast<double>(frame) / sample_rate));
        for (int channel = 0; channel < channels; ++channel) {
            samples.push_back(value);
        }
    }
    return samples;
}

[[nodiscard]] std::optional<trackknife::loudness::TrackLoudness>
analyze_sine(const double amplitude, const int channels, const bool true_peak,
             trackknife::loudness::LoudnessAnalyzer* keep_alive = nullptr) {
    auto analyzer =
        trackknife::loudness::LoudnessAnalyzer::create(sample_rate, channels, true_peak);
    if (!analyzer) {
        std::cerr << analyzer.error().message << '\n';
        return std::nullopt;
    }
    const auto samples = sine_frames(amplitude, channels, 5.0);
    if (!analyzer->add_frames(samples)) {
        return std::nullopt;
    }
    auto result = analyzer->finish();
    if (!result) {
        std::cerr << result.error().message << '\n';
        return std::nullopt;
    }
    if (keep_alive != nullptr) {
        *keep_alive = std::move(*analyzer);
    }
    return *result;
}

// ITU-R BS.1770 conformance points: a 997 Hz full-scale sine reads
// -3.01 LKFS in a single channel and ~0.00 LKFS when both stereo channels
// carry it; halving the amplitude is exactly -6.02 dB.
void sineLoudnessMatchesTheAnalyticReference() {
    const auto full = analyze_sine(1.0, 2, false);
    CHECK(full.has_value());
    if (!full) {
        return;
    }
    CHECK(std::abs(full->integrated_lufs - 0.0) < 0.1);
    CHECK(std::abs(full->sample_peak - 1.0) < 0.002);
    CHECK(!full->true_peak.has_value());
    CHECK(full->measurable());
    // ReplayGain 2.0: gain aims the track at -18 LUFS.
    CHECK(std::abs(full->track_gain_db() - (-18.0 - full->integrated_lufs)) < 1e-9);

    const auto half = analyze_sine(0.5, 2, false);
    CHECK(half.has_value());
    if (!half) {
        return;
    }
    // Halving the amplitude is exactly -6.02 dB of loudness.
    CHECK(std::abs((full->integrated_lufs - half->integrated_lufs) - 6.0206) < 0.05);
    CHECK(std::abs(half->sample_peak - 0.5) < 0.002);

    const auto mono = analyze_sine(1.0, 1, false);
    CHECK(mono.has_value());
    if (!mono) {
        return;
    }
    CHECK(std::abs(mono->integrated_lufs - (-3.01)) < 0.1);
}

void truePeakIsAtLeastTheSamplePeak() {
    const auto measured = analyze_sine(0.9, 2, true);
    CHECK(measured.has_value());
    if (!measured) {
        return;
    }
    CHECK(measured->true_peak.has_value());
    CHECK(measured->true_peak && *measured->true_peak >= measured->sample_peak);
    CHECK(measured->sample_peak > 0.89);
}

// Album loudness is the gated loudness of the whole programme: with a loud
// and a very quiet track the relative gate keeps the album near the loud
// track instead of averaging the two gains.
void albumReductionIsProgrammeLoudnessNotAnAverage() {
    trackknife::loudness::LoudnessAnalyzer loud{
        *trackknife::loudness::LoudnessAnalyzer::create(sample_rate, 2, false)};
    trackknife::loudness::LoudnessAnalyzer quiet{
        *trackknife::loudness::LoudnessAnalyzer::create(sample_rate, 2, false)};
    const auto loud_result = analyze_sine(1.0, 2, false, &loud);
    const auto quiet_result = analyze_sine(0.05, 2, false, &quiet);
    CHECK(loud_result.has_value() && quiet_result.has_value());
    if (!loud_result || !quiet_result) {
        return;
    }
    const std::array<const trackknife::loudness::LoudnessAnalyzer*, 2> analyzers{&loud, &quiet};
    const auto album = trackknife::loudness::album_integrated_lufs(analyzers);
    CHECK(album.has_value());
    if (!album) {
        std::cerr << album.error().message << '\n';
        return;
    }
    CHECK(*album <= loud_result->integrated_lufs + 0.01);
    CHECK(*album > quiet_result->integrated_lufs + 10.0);
    CHECK(std::abs(*album - loud_result->integrated_lufs) < 3.5);
}

void rejectsInvalidInput() {
    CHECK(!trackknife::loudness::LoudnessAnalyzer::create(0, 2, false).has_value());
    CHECK(!trackknife::loudness::LoudnessAnalyzer::create(sample_rate, 0, false).has_value());
    auto analyzer = trackknife::loudness::LoudnessAnalyzer::create(sample_rate, 2, false);
    CHECK(analyzer.has_value());
    if (analyzer) {
        const std::array<float, 3> uneven{0.0F, 0.0F, 0.0F};
        CHECK(!analyzer->add_frames(uneven).has_value());
    }
    CHECK(!trackknife::loudness::album_integrated_lufs({}).has_value());
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
        const auto value = values[static_cast<unsigned char>(character)];
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

// The real decode integration: the 100 ms fixture tone analyzes cleanly,
// reports its peak, and is honestly unmeasurable for gated loudness — a
// sub-400 ms programme has no integrated value to derive gains from.
void decodedFixtureAnalyzesAndShortMaterialIsUnmeasurable(
    const std::filesystem::path& fixture_directory) {
    const auto decoded = decode_base64_file(fixture_directory / "tagged-tone-wavpack.b64");
    CHECK(decoded.has_value());
    if (!decoded) {
        return;
    }
    const auto path =
        std::filesystem::temp_directory_path() /
        ("trackknife-loudness-" + trackknife::core::StableId::random().to_string() + ".wv");
    {
        std::ofstream output{path, std::ios::binary};
        output.write(reinterpret_cast<const char*>(decoded->data()),
                     static_cast<std::streamsize>(decoded->size()));
    }
    auto decoder = trackknife::formats::AudioDecoder::open(path.native());
    CHECK(decoder.has_value());
    if (decoder) {
        const auto analyzed = trackknife::loudness::analyze_decoded_source(*decoder, true);
        CHECK(analyzed.has_value());
        if (analyzed) {
            CHECK(analyzed->sample_peak > 0.01);
            CHECK(analyzed->true_peak.has_value());
            CHECK(!analyzed->measurable());
        }
    }
    std::error_code remove_error;
    std::filesystem::remove(path, remove_error);
}

// M7 exit gate: every repository audio fixture that the formats adapter
// advertises as decodable must also traverse the bounded ReplayGain graph.
// Short fixtures may be structurally unmeasurable, but decode/analysis itself
// must succeed independently of whether their container has a writable tag map.
void everyDecodableFixtureCanBeScanned(const std::filesystem::path& fixture_directory) {
    struct Fixture {
        std::string_view encoded;
        std::string_view materialized;
    };
    constexpr std::array fixtures{
        Fixture{"art-tone-flac.b64", "art.flac"},
        Fixture{"container-chapters-mka.b64", "chapters.mka"},
        Fixture{"gapless-tone-aac-m4a.b64", "gapless.m4a"},
        Fixture{"gapless-tone-mp3.b64", "gapless.mp3"},
        Fixture{"gapless-tone-opus.b64", "gapless.opus"},
        Fixture{"loudness-chapters-mka.b64", "loudness.mka"},
        Fixture{"loudness-tone-opus.b64", "loudness.opus"},
        Fixture{"partial-chapters-mka.b64", "partial.mka"},
        Fixture{"rf64-tone-wav.b64", "tone.rf64"},
        Fixture{"rich-metadata-flac.b64", "rich.flac"},
        Fixture{"tagged-tone-aiff.b64", "tagged.aiff"},
        Fixture{"tagged-tone-flac.b64", "tagged.flac"},
        Fixture{"tagged-tone-m4a.b64", "tagged.m4a"},
        Fixture{"tagged-tone-mp3.b64", "tagged.mp3"},
        Fixture{"tagged-tone-opus.b64", "tagged.opus"},
        Fixture{"tagged-tone-vorbis.b64", "tagged.ogg"},
        Fixture{"tagged-tone-wavpack.b64", "tagged.wv"},
        Fixture{"two-subsongs-mod.b64", "subsongs.mod"},
        Fixture{"vorbis-positive-start.b64", "positive-start.ogg"},
        Fixture{"wave64-float.b64", "float.w64"},
    };
    const auto root =
        std::filesystem::temp_directory_path() /
        ("trackknife-loudness-formats-" + trackknife::core::StableId::random().to_string());
    std::filesystem::create_directory(root);
    std::vector<trackknife::loudness::LoudnessScanItem> items;
    items.reserve(fixtures.size());
    for (std::size_t index = 0U; index < fixtures.size(); ++index) {
        const auto decoded = decode_base64_file(fixture_directory / fixtures[index].encoded);
        CHECK(decoded.has_value());
        if (!decoded) {
            continue;
        }
        const auto path = root / fixtures[index].materialized;
        std::ofstream output{path, std::ios::binary};
        output.write(reinterpret_cast<const char*>(decoded->data()),
                     static_cast<std::streamsize>(decoded->size()));
        output.close();
        items.push_back(trackknife::loudness::LoudnessScanItem{
            .item_index = index,
            .raw_path = path.native(),
            .selection = {},
            .range = {},
            .album_key = std::nullopt,
        });
    }
    const auto scanned = trackknife::loudness::scan_loudness(
        items, {.measure_true_peak = true, .maximum_parallelism = 4U});
    CHECK(scanned.has_value());
    if (scanned) {
        CHECK(scanned->tracks.size() == fixtures.size());
        CHECK(scanned->analyzed_track_count() == fixtures.size());
        for (const auto& track : scanned->tracks) {
            if (track.state != trackknife::loudness::LoudnessScanState::analyzed && track.issue) {
                std::cerr << track.raw_path << ": " << track.issue->message << '\n';
            }
            CHECK(track.state == trackknife::loudness::LoudnessScanState::analyzed);
            CHECK(track.source_revision.has_value());
            CHECK(track.loudness.has_value());
        }
    }
    std::error_code error;
    std::filesystem::remove_all(root, error);
}

// Minimal PCM16 WAV writer so the scan tests exercise the real decoder.
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

class ScanFixture final {
  public:
    ScanFixture() {
        directory_ =
            std::filesystem::temp_directory_path() /
            ("trackknife-loudness-scan-" + trackknife::core::StableId::random().to_string());
        std::filesystem::create_directory(directory_);
    }
    ~ScanFixture() {
        std::error_code error;
        std::filesystem::remove_all(directory_, error);
    }
    [[nodiscard]] std::string tone(const std::string_view name, const double amplitude) {
        const auto path = directory_ / name;
        write_sine_wav(path, amplitude, 2.0);
        return path.native();
    }
    [[nodiscard]] std::string missing(const std::string_view name) const {
        return (directory_ / name).native();
    }

  private:
    std::filesystem::path directory_;
};

void parallelScanMatchesDirectAnalysisAndReducesAlbums() {
    ScanFixture fixture;
    const std::vector<trackknife::loudness::LoudnessScanItem> items{
        {.item_index = 10U,
         .raw_path = fixture.tone("a1.wav", 0.8),
         .selection = {},
         .range = {},
         .album_key = std::optional<std::string>{"album-a"}},
        {.item_index = 11U,
         .raw_path = fixture.tone("a2.wav", 0.2),
         .selection = {},
         .range = {},
         .album_key = std::optional<std::string>{"album-a"}},
        {.item_index = 12U,
         .raw_path = fixture.tone("solo.wav", 0.4),
         .selection = {},
         .range = {},
         .album_key = std::nullopt},
        {.item_index = 13U,
         .raw_path = fixture.missing("gone.wav"),
         .selection = {},
         .range = {},
         .album_key = std::optional<std::string>{"album-b"}},
    };
    std::size_t final_completed = 0U;
    const auto result = trackknife::loudness::scan_loudness(
        items, {.measure_true_peak = true, .maximum_parallelism = 4U},
        [&final_completed](const trackknife::loudness::LoudnessScanProgress& update) {
            final_completed = std::max(final_completed, update.completed_items);
        });
    CHECK(result.has_value());
    if (!result) {
        std::cerr << result.error().message << '\n';
        return;
    }
    CHECK(result->tracks.size() == 4U);
    CHECK(result->analyzed_track_count() == 3U);
    CHECK(result->failed_track_count() == 1U);
    CHECK(final_completed == 4U);
    CHECK(!result->cancellation_requested);

    // Parallel results equal a direct single-threaded analysis.
    const auto expected = analyze_sine(0.8, 2, true);
    CHECK(expected.has_value());
    const auto& first = result->tracks[0];
    CHECK(first.item_index == 10U);
    CHECK(first.state == trackknife::loudness::LoudnessScanState::analyzed);
    CHECK(first.source_revision.has_value());
    CHECK(first.loudness.has_value());
    if (expected && first.loudness) {
        // PCM16 quantization keeps this within a small tolerance.
        CHECK(std::abs(first.loudness->integrated_lufs - expected->integrated_lufs) < 0.05);
        CHECK(std::abs(first.loudness->sample_peak - 0.8) < 0.01);
        CHECK(first.loudness->true_peak.has_value());
    }
    const auto& failed = result->tracks[3];
    CHECK(failed.state == trackknife::loudness::LoudnessScanState::failed);
    CHECK(failed.issue.has_value());

    // Album A reduces as one gated programme dominated by the loud member;
    // album B is honestly incomplete.
    CHECK(result->albums.size() == 2U);
    const auto& album_a = result->albums[0];
    CHECK(album_a.album_key == "album-a");
    CHECK(album_a.item_indexes == (std::vector<std::size_t>{10U, 11U}));
    CHECK(album_a.integrated_lufs.has_value());
    CHECK(album_a.album_gain_db().has_value());
    if (album_a.integrated_lufs && first.loudness) {
        CHECK(*album_a.integrated_lufs <= first.loudness->integrated_lufs + 0.01);
        CHECK(*album_a.integrated_lufs > first.loudness->integrated_lufs - 3.5);
    }
    CHECK(std::abs(album_a.sample_peak - 0.8) < 0.01);
    const auto& album_b = result->albums[1];
    CHECK(album_b.album_key == "album-b");
    CHECK(!album_b.integrated_lufs.has_value());
    CHECK(album_b.issue.has_value());
    CHECK(!album_b.album_gain_db().has_value());
}

void cancelledScanStopsCleanly() {
    ScanFixture fixture;
    const std::vector<trackknife::loudness::LoudnessScanItem> items{
        {.item_index = 0U,
         .raw_path = fixture.tone("c1.wav", 0.5),
         .selection = {},
         .range = {},
         .album_key = std::nullopt},
    };
    trackknife::core::CancellationSource cancellation;
    cancellation.request_cancellation();
    const auto result = trackknife::loudness::scan_loudness(items, {}, {}, cancellation.token());
    CHECK(result.has_value());
    if (result) {
        CHECK(result->cancellation_requested);
        CHECK(result->tracks[0].state == trackknife::loudness::LoudnessScanState::cancelled);
    }
    CHECK(!trackknife::loudness::scan_loudness(
               items, {.measure_true_peak = true, .maximum_parallelism = 0U})
               .has_value());
}

[[nodiscard]] trackknife::metadata::MetadataDocument
document_with(const std::vector<std::pair<std::string, std::string>>& fields) {
    trackknife::metadata::MetadataDocument document;
    for (const auto& [name, value] : fields) {
        document.fields.push_back(trackknife::metadata::MetadataField{
            .canonical_name = trackknife::metadata::canonicalize_field_name(name),
            .native_name = name,
            .values = {value},
            .qualifier = {},
            .provenance = trackknife::metadata::FieldProvenance::embedded,
        });
    }
    return document;
}

void groupingModesAssignDeterministicKeys() {
    const auto tagged =
        document_with({{"ALBUM", "Alpha"},
                       {"ALBUMARTIST", "Band"},
                       {"MUSICBRAINZ_ALBUMID", "11111111-2222-3333-4444-555555555555"}});
    const auto fallback = document_with({{"ALBUM", "Alpha"}, {"ARTIST", "Band"}});
    const auto bare = document_with({{"TITLE", "Loose"}});
    const std::array<const trackknife::metadata::MetadataDocument*, 3> documents{&tagged, &fallback,
                                                                                 &bare};

    const auto track_keys = trackknife::loudness::assign_loudness_groups(
        {.mode = trackknife::loudness::LoudnessGroupingMode::track, .expression = {}}, documents);
    CHECK(track_keys.has_value());
    CHECK(track_keys &&
          std::ranges::none_of(*track_keys, [](const auto& key) { return key.has_value(); }));

    const auto selection_keys = trackknife::loudness::assign_loudness_groups(
        {.mode = trackknife::loudness::LoudnessGroupingMode::selection_album, .expression = {}},
        documents);
    CHECK(selection_keys.has_value());
    CHECK(selection_keys && (*selection_keys)[0] == (*selection_keys)[2]);
    CHECK(selection_keys && (*selection_keys)[0].has_value());

    // Release-aware: the MusicBrainz id wins, the tag fallback is
    // deterministic, and unidentifiable files stay track-only.
    const auto release_keys = trackknife::loudness::assign_loudness_groups(
        {.mode = trackknife::loudness::LoudnessGroupingMode::release, .expression = {}}, documents);
    CHECK(release_keys.has_value());
    if (release_keys) {
        CHECK((*release_keys)[0] ==
              std::optional<std::string>{"mbid:11111111-2222-3333-4444-555555555555"});
        CHECK((*release_keys)[1].has_value());
        CHECK((*release_keys)[1] != (*release_keys)[0]);
        CHECK(!(*release_keys)[2].has_value());
    }
    // Two files with the same id share one programme.
    const std::array<const trackknife::metadata::MetadataDocument*, 2> same_release{&tagged,
                                                                                    &tagged};
    const auto same_keys = trackknife::loudness::assign_loudness_groups(
        {.mode = trackknife::loudness::LoudnessGroupingMode::release, .expression = {}},
        same_release);
    CHECK(same_keys.has_value());
    CHECK(same_keys && (*same_keys)[0] == (*same_keys)[1]);

    // tkfmt-1 grouping: equal non-empty results group, empty stays
    // track-only, and a broken expression fails typed.
    const auto format_keys = trackknife::loudness::assign_loudness_groups(
        {.mode = trackknife::loudness::LoudnessGroupingMode::format_expression,
         .expression = "%album%"},
        documents);
    CHECK(format_keys.has_value());
    if (format_keys) {
        CHECK((*format_keys)[0] == std::optional<std::string>{"fmt:Alpha"});
        CHECK((*format_keys)[0] == (*format_keys)[1]);
        CHECK(!(*format_keys)[2].has_value());
    }
    CHECK(!trackknife::loudness::assign_loudness_groups(
               {.mode = trackknife::loudness::LoudnessGroupingMode::format_expression,
                .expression = "$unknown(%album%)"},
               documents)
               .has_value());
    CHECK(!trackknife::loudness::assign_loudness_groups(
               {.mode = trackknife::loudness::LoudnessGroupingMode::format_expression,
                .expression = ""},
               documents)
               .has_value());
}

// ADR-0146: the disc-merging release mode strips one trailing disc
// designator from the album fallback key; MusicBrainz ids stay exact.
void discMergedGroupingStripsDesignators() {
    using trackknife::loudness::strip_disc_designator;
    CHECK(strip_disc_designator("Album (Disc 1)") == "Album");
    CHECK(strip_disc_designator("Album [CD 2]") == "Album");
    CHECK(strip_disc_designator("Album - Disc 3") == "Album");
    CHECK(strip_disc_designator("Album CD2") == "Album");
    CHECK(strip_disc_designator("Album, Part 1") == "Album");
    CHECK(strip_disc_designator("Album Vol. 2") == "Album");
    CHECK(strip_disc_designator("Album (disc 12)  ") == "Album");
    // Conservative: no designator word, embedded numbers, or standalone
    // designators keep the original text.
    CHECK(strip_disc_designator("Blink-182") == "Blink-182");
    CHECK(strip_disc_designator("1999") == "1999");
    CHECK(strip_disc_designator("Album (2CD)") == "Album (2CD)");
    CHECK(strip_disc_designator("Disc 1") == "Disc 1");
    CHECK(strip_disc_designator("Album (Live)") == "Album (Live)");

    const auto disc_one = document_with({{"ALBUM", "Alpha (Disc 1)"}, {"ALBUMARTIST", "Band"}});
    const auto disc_two = document_with({{"ALBUM", "Alpha CD2"}, {"ALBUMARTIST", "Band"}});
    const auto other = document_with({{"ALBUM", "Beta"}, {"ALBUMARTIST", "Band"}});
    const std::array<const trackknife::metadata::MetadataDocument*, 3> documents{&disc_one,
                                                                                 &disc_two, &other};
    const auto merged = trackknife::loudness::assign_loudness_groups(
        {.mode = trackknife::loudness::LoudnessGroupingMode::release_merged_discs,
         .expression = {}},
        documents);
    CHECK(merged.has_value());
    if (merged) {
        CHECK((*merged)[0].has_value());
        CHECK((*merged)[0] == (*merged)[1]);
        CHECK((*merged)[2] != (*merged)[0]);
    }
    // The plain release mode keeps the discs separate.
    const auto split = trackknife::loudness::assign_loudness_groups(
        {.mode = trackknife::loudness::LoudnessGroupingMode::release, .expression = {}}, documents);
    CHECK(split.has_value());
    CHECK(split && (*split)[0] != (*split)[1]);
}

} // namespace

int main(const int argc, char** argv) {
    CHECK(argc == 2);
    sineLoudnessMatchesTheAnalyticReference();
    truePeakIsAtLeastTheSamplePeak();
    albumReductionIsProgrammeLoudnessNotAnAverage();
    rejectsInvalidInput();
    parallelScanMatchesDirectAnalysisAndReducesAlbums();
    cancelledScanStopsCleanly();
    groupingModesAssignDeterministicKeys();
    discMergedGroupingStripsDesignators();
    if (argc == 2) {
        decodedFixtureAnalyzesAndShortMaterialIsUnmeasurable(std::filesystem::path{argv[1]});
        everyDecodableFixtureCanBeScanned(std::filesystem::path{argv[1]});
    }
    return failures == 0 ? 0 : 1;
}

// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/stable_id.hpp"
#include "trackknife/metadata/document.hpp"
#include "trackknife/metadata/flac_mapping.hpp"
#include "trackknife/metadata/local_reader.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
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
                                                const std::string_view suffix) {
    const auto encoded = decode_base64_file(fixture_directory / fixture);
    CHECK(encoded.has_value());
    const auto path = std::filesystem::temp_directory_path() /
                      ("trackknife-metadata-" + trackknife::core::StableId::random().to_string() +
                       std::string{suffix});
    if (encoded) {
        std::ofstream output{path, std::ios::binary};
        output.write(reinterpret_cast<const char*>(encoded->data()),
                     static_cast<std::streamsize>(encoded->size()));
        CHECK(output.good());
    }
    return path;
}

void fieldLookupPreservesValuesAndAppliesProvenance() {
    using trackknife::metadata::FieldProvenance;
    using trackknife::metadata::MetadataDocument;
    using trackknife::metadata::MetadataField;

    CHECK(trackknife::metadata::canonicalize_field_name("MusicBrainz_Album-Artist Id") ==
          "musicbrainzalbumartistid");
    CHECK(trackknife::metadata::resolve_text_property_identity("ALBUMARTIST") ==
          (trackknife::metadata::TextPropertyIdentity{.canonical_name = "albumartist",
                                                      .conventional = true}));
    CHECK(trackknife::metadata::resolve_text_property_identity("ALBUM ARTIST") ==
          (trackknife::metadata::TextPropertyIdentity{.canonical_name = "album artist",
                                                      .conventional = false}));
    MetadataDocument document{
        .fields =
            {
                MetadataField{.canonical_name = "artist",
                              .native_name = "Artist",
                              .values = {"Cached"},
                              .qualifier = {},
                              .provenance = FieldProvenance::cached_snapshot},
                MetadataField{.canonical_name = "artist",
                              .native_name = "ARTIST",
                              .values = {"Embedded one", "Embedded two"},
                              .qualifier = {},
                              .provenance = FieldProvenance::embedded},
                MetadataField{.canonical_name = "artist",
                              .native_name = "PERFORMER",
                              .values = {"Cue one"},
                              .qualifier = {},
                              .provenance = FieldProvenance::segment},
                MetadataField{.canonical_name = "artist",
                              .native_name = "PERFORMER",
                              .values = {"Cue two"},
                              .qualifier = {},
                              .provenance = FieldProvenance::segment},
            },
        .unsupported_native_objects = {},
    };
    CHECK(document.effective_values("ART_IST") == (std::vector<std::string>{"Cue one", "Cue two"}));
    CHECK(document.first_effective_value("artist") == std::optional<std::string>{"Cue one"});
    CHECK(document.effective_values("missing").empty());
    CHECK(!document.first_effective_value("missing").has_value());
}

void readsRichFlacAndMusicBrainz(const std::filesystem::path& fixture_directory) {
    const auto path = materialize(fixture_directory, "rich-metadata-flac.b64", ".flac");
    const auto read = trackknife::metadata::read_local_metadata(path.native());
    CHECK(read.has_value());
    if (read) {
        CHECK(read->raw_path == path.native());
        CHECK(read->adapter_name == "taglib-flac-v1");
        CHECK(read->capabilities.fields_readable);
        CHECK(read->capabilities.fields_writable);
        CHECK(read->capabilities.pictures_readable);
        CHECK(read->capabilities.pictures_writable);
        CHECK(read->capabilities.unknown_data_preserved_on_write);
        CHECK(read->source_revision.size == 2'308U);
        CHECK(read->source_revision.inode != 0U);
        CHECK(read->document.effective_values("artist") ==
              (std::vector<std::string>{"First Artist", "Second Artist"}));
        CHECK(read->document.effective_values("custom_field") ==
              (std::vector<std::string>{"first custom value", "second custom value"}));
        CHECK(read->document.first_effective_value("album_artist") ==
              std::optional<std::string>{"Album Credit"});

        const auto artist = std::ranges::find(read->document.fields, "artist",
                                              &trackknife::metadata::MetadataField::canonical_name);
        CHECK(artist != read->document.fields.end());
        CHECK(artist != read->document.fields.end() && artist->native_name == "ARTIST");

        const auto identity = trackknife::metadata::project_musicbrainz(read->document);
        CHECK(identity.recording_ids ==
              (std::vector<std::string>{"11111111-1111-1111-1111-111111111111"}));
        CHECK(identity.release_track_ids ==
              (std::vector<std::string>{"22222222-2222-2222-2222-222222222222"}));
        CHECK(identity.release_ids ==
              (std::vector<std::string>{"33333333-3333-3333-3333-333333333333"}));
        CHECK(identity.release_group_ids ==
              (std::vector<std::string>{"44444444-4444-4444-4444-444444444444"}));
        CHECK(identity.artist_ids ==
              (std::vector<std::string>{"55555555-5555-5555-5555-555555555555",
                                        "66666666-6666-6666-6666-666666666666"}));
        CHECK(identity.album_artist_ids ==
              (std::vector<std::string>{"77777777-7777-7777-7777-777777777777"}));
        CHECK(identity.work_ids ==
              (std::vector<std::string>{"88888888-8888-8888-8888-888888888888"}));
        CHECK(identity.disc_ids == (std::vector<std::string>{"fixture-disc-id"}));
        CHECK(identity.artist_sort_names == (std::vector<std::string>{"Artist, First"}));
        CHECK(identity.album_artist_sort_names == (std::vector<std::string>{"Credit, Album"}));
        CHECK(identity.artist_credits ==
              (std::vector<std::string>{"First Artist", "Second Artist"}));

        const auto second = trackknife::metadata::read_local_metadata(path.native());
        CHECK(second.has_value());
        CHECK(second && second->source_revision == read->source_revision);
    }

    std::error_code remove_error;
    std::filesystem::remove(path, remove_error);
    CHECK(!remove_error);
}

void readsWavPackAndRawBytePath(const std::filesystem::path& fixture_directory) {
    const auto wavpack = materialize(fixture_directory, "tagged-tone-wavpack.b64", ".wv");
    const auto read_wavpack = trackknife::metadata::read_local_metadata(wavpack.native());
    CHECK(read_wavpack.has_value());
    CHECK(read_wavpack && read_wavpack->document.first_effective_value("title") ==
                              std::optional<std::string>{"Fixture Tone"});
    // ADR-0095: native WavPack is a qualified text-write adapter whose
    // prepared-copy writer verifies audio and APEv2 binary preservation.
    CHECK(read_wavpack && read_wavpack->adapter_name == "taglib-wavpack-v1");
    CHECK(read_wavpack && read_wavpack->capabilities.fields_writable);
    CHECK(read_wavpack && read_wavpack->capabilities.unknown_data_preserved_on_write);
    CHECK(read_wavpack && !read_wavpack->capabilities.pictures_writable);

    const auto encoded = decode_base64_file(fixture_directory / "rich-metadata-flac.b64");
    CHECK(encoded.has_value());
    const auto directory =
        std::filesystem::temp_directory_path() /
        ("trackknife-metadata-raw-" + trackknife::core::StableId::random().to_string());
    std::error_code error;
    CHECK(std::filesystem::create_directory(directory, error));
    const std::string raw_name{"rich-\xFF.flac", 11U};
    const auto raw_path = directory / std::filesystem::path{raw_name};
    if (encoded) {
        std::ofstream output{raw_path, std::ios::binary};
        output.write(reinterpret_cast<const char*>(encoded->data()),
                     static_cast<std::streamsize>(encoded->size()));
        CHECK(output.good());
    }
    const auto raw_read = trackknife::metadata::read_local_metadata(raw_path.native());
    CHECK(raw_read.has_value());
    CHECK(raw_read && raw_read->raw_path == raw_path.native());
    CHECK(raw_read && raw_read->document.first_effective_value("title") ==
                          std::optional<std::string>{"Metadata Fixture"});

    trackknife::metadata::StagedMetadataSource cached{.raw_path = raw_path.native(),
                                                      .source_revision = {},
                                                      .baseline = {},
                                                      .needs_metadata_capture = true};
    auto prepared = trackknife::metadata::capture_uncached_metadata_sources({cached, cached});
    CHECK(prepared && prepared->size() == 2);
    CHECK(prepared && prepared->at(0).source_revision == raw_read->source_revision);
    CHECK(prepared && prepared->at(0).baseline == raw_read->document);
    CHECK(prepared && prepared->at(0) == prepared->at(1));
    CHECK(prepared && !prepared->at(0).needs_metadata_capture);
    auto stale = prepared->at(0);
    stale.raw_path = "/already-captured-but-missing.flac";
    auto unchanged = trackknife::metadata::capture_uncached_metadata_sources({stale});
    CHECK(unchanged && unchanged->front() == stale);
    cached.raw_path = "/missing-cache-source.flac";
    CHECK(!trackknife::metadata::capture_uncached_metadata_sources({cached}));
    trackknife::core::CancellationSource cancel;
    cancel.request_cancellation();
    auto stopped =
        trackknife::metadata::capture_uncached_metadata_sources({cached}, cancel.token());
    CHECK(!stopped && stopped.error().code == trackknife::core::ErrorCode::cancelled);

    std::filesystem::remove(wavpack, error);
    CHECK(!error);
    std::filesystem::remove_all(directory, error);
    CHECK(!error);
}

void cancellationAndInvalidSourcesAreTyped() {
    trackknife::core::CancellationSource cancellation;
    cancellation.request_cancellation();
    const auto cancelled =
        trackknife::metadata::read_local_metadata("/not-opened.flac", cancellation.token());
    CHECK(!cancelled.has_value());
    CHECK(cancelled.error().code == trackknife::core::ErrorCode::cancelled);

    const auto missing = trackknife::metadata::read_local_metadata("/definitely/missing.flac");
    CHECK(!missing.has_value());
    CHECK(missing.error().code == trackknife::core::ErrorCode::not_found);

    const auto directory = std::filesystem::temp_directory_path();
    const auto not_regular = trackknife::metadata::read_local_metadata(directory.native());
    CHECK(!not_regular.has_value());
    CHECK(not_regular.error().code == trackknife::core::ErrorCode::unsupported);
}

} // namespace

int main(const int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: metadata-reader-test FIXTURE_DIRECTORY\n";
        return 2;
    }
    const std::filesystem::path fixture_directory{argv[1]};
    fieldLookupPreservesValuesAndAppliesProvenance();
    readsRichFlacAndMusicBrainz(fixture_directory);
    readsWavPackAndRawBytePath(fixture_directory);
    cancellationAndInvalidSourcesAreTyped();
    return failures == 0 ? 0 : 1;
}

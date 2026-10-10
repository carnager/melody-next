// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/core/stable_id.hpp"
#include "trackknife/metadata/artwork.hpp"
#include "trackknife/metadata/artwork_write_plan.hpp"
#include "trackknife/metadata/artwork_writers.hpp"
#include "trackknife/metadata/local_reader.hpp"
#include "trackknife/metadata/lyrics.hpp"
#include "trackknife/metadata/mp3_writer.hpp"
#include "trackknife/metadata/mp4_writer.hpp"
#include "trackknife/metadata/staged_patch.hpp"
#include "trackknife/metadata/staged_selection.hpp"
#include "trackknife/metadata/write_plan.hpp"

#include <taglib/attachedpictureframe.h>
#include <taglib/id3v2tag.h>
#include <taglib/mp4file.h>
#include <taglib/mpegfile.h>
#include <taglib/unsynchronizedlyricsframe.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
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

class TemporaryDirectory final {
  public:
    TemporaryDirectory() {
        path_ = std::filesystem::temp_directory_path() /
                ("trackknife-mp3-writer-" + trackknife::core::StableId::random().to_string());
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

[[nodiscard]] std::filesystem::path materialize(const std::filesystem::path& fixture_directory,
                                                const std::string_view fixture,
                                                const std::filesystem::path& destination) {
    const auto decoded = decode_base64_file(fixture_directory / fixture);
    std::ofstream output{destination, std::ios::binary};
    if (decoded) {
        output.write(reinterpret_cast<const char*>(decoded->data()),
                     static_cast<std::streamsize>(decoded->size()));
    }
    return destination;
}

[[nodiscard]] std::vector<unsigned char> read_bytes(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

// The MPEG audio bytes after the leading ID3v2 tag (syncsafe-sized).
[[nodiscard]] std::vector<unsigned char> audio_region(const std::vector<unsigned char>& bytes) {
    std::size_t begin = 0U;
    if (bytes.size() > 10U && bytes[0] == 'I' && bytes[1] == 'D' && bytes[2] == '3') {
        std::size_t size = 0U;
        for (std::size_t index = 6U; index < 10U; ++index) {
            size = (size << 7U) | (bytes[index] & 0x7FU);
        }
        begin = 10U + size + (((bytes[5] & 0x10U) != 0U) ? 10U : 0U);
    }
    std::size_t end = bytes.size();
    if (end >= 128U && bytes[end - 128U] == 'T' && bytes[end - 127U] == 'A' &&
        bytes[end - 126U] == 'G') {
        end -= 128U;
    }
    return {bytes.begin() + static_cast<std::ptrdiff_t>(begin),
            bytes.begin() + static_cast<std::ptrdiff_t>(end)};
}

[[nodiscard]] std::optional<trackknife::metadata::StagedMetadataSelection>
selection_for(const trackknife::metadata::LocalMetadataRead& read) {
    auto selection = trackknife::metadata::StagedMetadataSelection::create(
        {trackknife::metadata::StagedMetadataSource{
            .raw_path = read.raw_path,
            .source_revision = read.source_revision,
            .baseline = read.document,
        }});
    if (!selection) {
        std::cerr << selection.error().message << '\n';
        return std::nullopt;
    }
    return std::move(*selection);
}

[[nodiscard]] std::optional<trackknife::metadata::MetadataWritePlan>
make_plan(const trackknife::metadata::StagedMetadataSelection& selection,
          const trackknife::metadata::StagedMetadataPatchSet& patches) {
    auto plan = trackknife::metadata::revalidate_metadata_write_plan(selection, patches);
    if (!plan) {
        std::cerr << plan.error().message << '\n';
        return std::nullopt;
    }
    return std::move(*plan);
}

void roundTripsId3TextEditsPreservingAudio(const std::filesystem::path& fixture_directory) {
    TemporaryDirectory directory;
    const auto source =
        materialize(fixture_directory, "tagged-tone-mp3.b64", directory.path() / "roundtrip.mp3");
    const auto source_bytes = read_bytes(source);
    CHECK(!source_bytes.empty());

    const auto read = trackknife::metadata::read_local_metadata(source.native());
    CHECK(read.has_value());
    if (!read) {
        return;
    }
    CHECK(read->adapter_name == "taglib-mpeg-v1");
    CHECK(read->capabilities.fields_writable);
    CHECK(read->capabilities.unknown_data_preserved_on_write);
    CHECK(read->capabilities.pictures_writable);
    CHECK(read->document.first_effective_value("title") ==
          std::optional<std::string>{"Fixture Tone"});

    auto selection = selection_for(*read);
    if (!selection) {
        return;
    }
    const auto title = selection->field_index("title");
    const auto artist = selection->field_index("artist");
    const auto track_id =
        selection->ensure_missing_field("musicbrainztrackid", "MUSICBRAINZ_TRACKID");
    const auto date = selection->ensure_missing_field("date", "Date");
    CHECK(title.has_value() && artist.has_value());
    CHECK(track_id.has_value() && date.has_value());
    if (!title || !artist || !track_id || !date) {
        return;
    }
    trackknife::metadata::StagedMetadataPatchSet patches;
    CHECK(patches.replace_values(*selection, 0U, *title, {"Prepared Tone"}).has_value());
    CHECK(patches.remove_field(*selection, 0U, *artist).has_value());
    CHECK(
        patches.replace_values(*selection, 0U, *track_id, {"01b43c31-1b23-42f0-a5d5-275288538230"})
            .has_value());
    CHECK(patches.replace_values(*selection, 0U, *date, {"1999-09-09"}).has_value());

    auto plan = make_plan(*selection, patches);
    CHECK(plan && plan->ready());
    if (!plan || !plan->ready()) {
        return;
    }
    const auto prepared_path = directory.path() / "roundtrip-prepared.mp3";
    const auto prepared = trackknife::metadata::prepare_mp3_metadata_write_copy(
        plan->sources.front(), prepared_path.native());
    CHECK(prepared.has_value());
    if (!prepared) {
        std::cerr << prepared.error().message << '\n';
        return;
    }
    CHECK(prepared->field_change_count == 4U);

    const auto reread = trackknife::metadata::read_local_metadata(prepared_path.native());
    CHECK(reread.has_value());
    if (!reread) {
        return;
    }
    CHECK(reread->document.first_effective_value("title") ==
          std::optional<std::string>{"Prepared Tone"});
    CHECK(!reread->document.first_effective_value("artist"));
    CHECK(reread->document.first_effective_value("musicbrainztrackid") ==
          std::optional<std::string>{"01b43c31-1b23-42f0-a5d5-275288538230"});
    CHECK(reread->document.first_effective_value("date") ==
          std::optional<std::string>{"1999-09-09"});

    // The MPEG audio frames are byte-identical even though the leading
    // ID3v2 tag resized.
    const auto prepared_bytes = read_bytes(prepared_path);
    CHECK(audio_region(source_bytes) == audio_region(prepared_bytes));

    // The dispatcher routes taglib-mpeg-v1 plans to this writer.
    const auto adapters_ok =
        trackknife::metadata::is_qualified_text_adapter("taglib-mpeg-v1") &&
        trackknife::metadata::is_qualified_text_adapter("taglib-flac-v1") &&
        trackknife::metadata::is_qualified_text_adapter("taglib-wavpack-v1") &&
        !trackknife::metadata::is_qualified_text_adapter("taglib-properties-v1");
    CHECK(adapters_ok);
}

// ADR-0276: lyrics typed in the tagger are one value of many lines. Written,
// they are each format's own lyrics -- ID3 USLT, MP4 \u00a9lyr -- with every
// line, and read back as lyrics.
void roundTripsMultiLineLyrics(const std::filesystem::path& fixture_directory) {
    TemporaryDirectory directory;
    const std::string text{"First line\nSecond line\n\nThird after a verse"};
    struct Case {
        const char* fixture;
        const char* name;
    };
    for (const auto& [fixture, name] :
         {Case{"tagged-tone-mp3.b64", "lyrics.mp3"}, Case{"tagged-tone-m4a.b64", "lyrics.m4a"}}) {
        const auto source = materialize(fixture_directory, fixture, directory.path() / name);
        const auto read = trackknife::metadata::read_local_metadata(source.native());
        CHECK(read.has_value());
        if (!read) {
            continue;
        }
        auto selection = selection_for(*read);
        if (!selection) {
            continue;
        }
        const auto lyrics = selection->ensure_missing_field("lyrics", "Lyrics");
        CHECK(lyrics.has_value());
        if (!lyrics) {
            continue;
        }
        trackknife::metadata::StagedMetadataPatchSet patches;
        CHECK(patches.replace_values(*selection, 0U, *lyrics, {text}).has_value());
        auto plan = make_plan(*selection, patches);
        CHECK(plan && plan->ready());
        if (!plan || !plan->ready()) {
            continue;
        }
        const auto prepared_path = directory.path() / (std::string{"prepared-"} + name);
        const bool mp3 = std::string_view{name}.ends_with(".mp3");
        const auto written =
            mp3 ? trackknife::metadata::prepare_mp3_metadata_write_copy(plan->sources.front(),
                                                                       prepared_path.native())
                      .has_value()
                : trackknife::metadata::prepare_mp4_metadata_write_copy(plan->sources.front(),
                                                                       prepared_path.native())
                      .has_value();
        CHECK(written);
        const auto reread = trackknife::metadata::read_local_metadata(prepared_path.native());
        CHECK(reread && reread->document.first_effective_value("lyrics") ==
                            std::optional<std::string>{text});
        if (mp3) {
            TagLib::MPEG::File file{prepared_path.c_str()};
            CHECK(file.hasID3v2Tag() && !file.ID3v2Tag()->frameList("USLT").isEmpty());
        } else {
            TagLib::MP4::File file{prepared_path.c_str()};
            CHECK(file.hasMP4Tag() && file.tag()->contains(TagLib::String{"\xa9lyr", TagLib::String::Latin1}));
        }
        const auto found = trackknife::metadata::read_lyrics(prepared_path.native());
        CHECK(found && *found && (*found)->source == "tag:LYRICS" && (*found)->lines.size() == 4U);
    }

    // A USLT frame with a description reads as LYRICS:<description>; its
    // lyrics are found all the same.
    const auto described =
        materialize(fixture_directory, "tagged-tone-mp3.b64", directory.path() / "described.mp3");
    {
        TagLib::MPEG::File file{described.c_str()};
        auto* frame = new TagLib::ID3v2::UnsynchronizedLyricsFrame(TagLib::String::UTF8);
        frame->setDescription("Song text");
        frame->setLanguage("eng");
        frame->setText("Sung\nwords");
        file.ID3v2Tag(true)->addFrame(frame);
        CHECK(file.save());
    }
    const auto found = trackknife::metadata::read_lyrics(described.native());
    CHECK(found && *found && (*found)->source == "tag:LYRICS:SONG TEXT" &&
          (*found)->lines.size() == 2U);
}

void toleratesId3v1TrailersUnlikeWavPack(const std::filesystem::path& fixture_directory) {
    TemporaryDirectory directory;
    const auto source =
        materialize(fixture_directory, "tagged-tone-mp3.b64", directory.path() / "id3v1.mp3");
    {
        std::ofstream append{source, std::ios::binary | std::ios::app};
        std::array<char, 128> id3v1{};
        id3v1[0] = 'T';
        id3v1[1] = 'A';
        id3v1[2] = 'G';
        append.write(id3v1.data(), static_cast<std::streamsize>(id3v1.size()));
    }
    const auto source_bytes = read_bytes(source);
    const auto read = trackknife::metadata::read_local_metadata(source.native());
    CHECK(read.has_value());
    if (!read) {
        return;
    }
    auto selection = selection_for(*read);
    if (!selection) {
        return;
    }
    const auto title = selection->field_index("title");
    CHECK(title.has_value());
    if (!title) {
        return;
    }
    trackknife::metadata::StagedMetadataPatchSet patches;
    CHECK(patches.replace_values(*selection, 0U, *title, {"Trailered"}).has_value());
    auto plan = make_plan(*selection, patches);
    CHECK(plan && plan->ready());
    if (!plan || !plan->ready()) {
        return;
    }
    const auto prepared_path = directory.path() / "id3v1-prepared.mp3";
    const auto prepared = trackknife::metadata::prepare_mp3_metadata_write_copy(
        plan->sources.front(), prepared_path.native());
    CHECK(prepared.has_value());
    if (!prepared) {
        std::cerr << prepared.error().message << '\n';
        return;
    }
    const auto reread = trackknife::metadata::read_local_metadata(prepared_path.native());
    CHECK(reread.has_value());
    CHECK(reread && reread->document.first_effective_value("title") ==
                        std::optional<std::string>{"Trailered"});
    // ID3v1 is a rewritable trailer for MP3; the audio between the tags
    // stayed byte-identical.
    CHECK(audio_region(source_bytes) == audio_region(read_bytes(prepared_path)));
}

} // namespace

// ADR-0137: APIC replace mutates the frame in place — position, type, and
// description survive — remove reorders the ordinals, add lands with the
// canonical type and its description, and the MPEG audio region stays
// byte-identical throughout.
void roundTripsApicArtworkEdits(const std::filesystem::path& fixture_directory) {
    using trackknife::metadata::ArtworkWritePlanIntent;
    using trackknife::metadata::ArtworkWritePlanIntentKind;

    TemporaryDirectory directory;
    const auto media =
        materialize(fixture_directory, "tagged-tone-mp3.b64", directory.path() / "artful.mp3");
    const auto replacement = materialize(fixture_directory, "external-blue-jpeg.b64",
                                         directory.path() / "replacement.jpg");
    const std::vector<unsigned char> first_pixels{0xFFU, 0xD8U, 0xFFU, 0xDBU, 0x00U,
                                                  0x04U, 0x01U, 0x02U, 0xFFU, 0xD9U};
    {
        TagLib::MPEG::File file{media.c_str(), false};
        CHECK(file.isValid());
        auto* frame = new TagLib::ID3v2::AttachedPictureFrame();
        frame->setType(TagLib::ID3v2::AttachedPictureFrame::Other);
        frame->setMimeType(TagLib::String{"image/jpeg", TagLib::String::UTF8});
        frame->setDescription(TagLib::String{"Secondary", TagLib::String::UTF8});
        frame->setPicture(TagLib::ByteVector{reinterpret_cast<const char*>(first_pixels.data()),
                                             static_cast<unsigned int>(first_pixels.size())});
        file.ID3v2Tag(true)->addFrame(frame);
        CHECK(file.save());
    }

    auto policy = trackknife::metadata::default_artwork_inventory_policy();
    policy.external_patterns.clear();
    const auto inventory =
        trackknife::metadata::read_local_artwork_inventory(media.native(), policy);
    CHECK(inventory.has_value());
    if (!inventory) {
        return;
    }
    CHECK(inventory->embedded_adapter_name == "taglib-id3v2-apic-v1");
    CHECK(inventory->items.size() == 1U);
    CHECK(inventory->items[0].role == trackknife::metadata::ArtworkRole::other);
    CHECK(inventory->items[0].native_type == "Other");
    CHECK(inventory->items[0].description == "Secondary");
    CHECK(inventory->items[0].mime_type == "image/jpeg");

    // Replace keeps the frame's type and description.
    const ArtworkWritePlanIntent replace_intent{
        .occurrence_index = 0U,
        .raw_media_path = media.native(),
        .expected_media_revision = inventory->media_revision,
        .target_ordinal = 0U,
        .expected_target_fingerprint = inventory->items[0].content_fingerprint,
        .kind = ArtworkWritePlanIntentKind::replace,
        .replacement_raw_path = replacement.native(),
        .added_role = trackknife::metadata::ArtworkRole::front,
        .added_description = {},
        .replacement_embedded_source = std::nullopt,
    };
    const auto replace_plan = trackknife::metadata::revalidate_artwork_write_plan({replace_intent});
    CHECK(replace_plan.has_value() && replace_plan->ready());
    if (!replace_plan || !replace_plan->ready()) {
        return;
    }
    const auto replaced_path = directory.path() / "replaced.mp3";
    const auto replaced = trackknife::metadata::prepare_mp3_artwork_write_copy(
        replace_plan->sources.front(), replaced_path.native());
    if (!replaced) {
        std::cerr << "replace: " << replaced.error().message << '\n';
    }
    CHECK(replaced.has_value());
    if (!replaced) {
        return;
    }
    CHECK(replaced->inventory.items.size() == 1U);
    CHECK(replaced->inventory.items[0].native_type == "Other");
    CHECK(replaced->inventory.items[0].description == "Secondary");
    CHECK(replaced->inventory.items[0].content_fingerprint !=
          inventory->items[0].content_fingerprint);
    CHECK(replaced->document.first_effective_value("title") ==
          std::optional<std::string>{"Fixture Tone"});
    CHECK(audio_region(read_bytes(media)) == audio_region(read_bytes(replaced_path)));

    // Add carries the canonical type and description; remove empties the tag.
    auto add_intent = replace_intent;
    add_intent.kind = ArtworkWritePlanIntentKind::add;
    add_intent.expected_target_fingerprint = {};
    add_intent.added_role = trackknife::metadata::ArtworkRole::front;
    add_intent.added_description = "Chosen front";
    const auto add_plan = trackknife::metadata::revalidate_artwork_write_plan({add_intent});
    CHECK(add_plan.has_value() && add_plan->ready());
    if (add_plan && add_plan->ready()) {
        const auto added_path = directory.path() / "added.mp3";
        const auto added = trackknife::metadata::prepare_mp3_artwork_write_copy(
            add_plan->sources.front(), added_path.native());
        if (!added) {
            std::cerr << "add: " << added.error().message << '\n';
        }
        CHECK(added.has_value());
        if (added) {
            CHECK(added->inventory.items.size() == 2U);
            CHECK(added->inventory.items[1].native_type == "Front Cover");
            CHECK(added->inventory.items[1].description == "Chosen front");
            CHECK(added->inventory.items[1].role == trackknife::metadata::ArtworkRole::front);
            CHECK(audio_region(read_bytes(media)) == audio_region(read_bytes(added_path)));
        }
    }

    auto remove_intent = replace_intent;
    remove_intent.kind = ArtworkWritePlanIntentKind::remove;
    remove_intent.replacement_raw_path.reset();
    const auto remove_plan = trackknife::metadata::revalidate_artwork_write_plan({remove_intent});
    CHECK(remove_plan.has_value() && remove_plan->ready());
    if (remove_plan && remove_plan->ready()) {
        const auto removed_path = directory.path() / "removed.mp3";
        const auto removed = trackknife::metadata::prepare_mp3_artwork_write_copy(
            remove_plan->sources.front(), removed_path.native());
        if (!removed) {
            std::cerr << "remove: " << removed.error().message << '\n';
        }
        CHECK(removed.has_value());
        if (removed) {
            CHECK(removed->inventory.items.empty());
            CHECK(audio_region(read_bytes(media)) == audio_region(read_bytes(removed_path)));
        }
    }
}

int main(const int argc, char** argv) {
    CHECK(argc == 2);
    if (argc == 2) {
        const std::filesystem::path fixture_directory{argv[1]};
        roundTripsId3TextEditsPreservingAudio(fixture_directory);
        toleratesId3v1TrailersUnlikeWavPack(fixture_directory);
        roundTripsMultiLineLyrics(fixture_directory);
        roundTripsApicArtworkEdits(fixture_directory);
    }
    return failures == 0 ? 0 : 1;
}

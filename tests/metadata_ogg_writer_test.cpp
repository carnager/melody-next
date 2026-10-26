// SPDX-License-Identifier: GPL-3.0-only

#include "../src/metadata/src/ogg_stream_detail.hpp"
#include "trackknife/core/stable_id.hpp"
#include "trackknife/formats/decoder.hpp"
#include "trackknife/metadata/artwork.hpp"
#include "trackknife/metadata/artwork_write_plan.hpp"
#include "trackknife/metadata/artwork_writers.hpp"
#include "trackknife/metadata/local_reader.hpp"
#include "trackknife/metadata/mp3_writer.hpp"
#include "trackknife/metadata/ogg_writer.hpp"
#include "trackknife/metadata/staged_patch.hpp"
#include "trackknife/metadata/staged_selection.hpp"
#include "trackknife/metadata/write_plan.hpp"

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <taglib/flacpicture.h>
#include <taglib/opusfile.h>
#include <taglib/vorbisfile.h>
#include <taglib/xiphcomment.h>

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
                ("trackknife-ogg-writer-" + trackknife::core::StableId::random().to_string());
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

// Bit-exact decoded PCM: the packet proof from the writer plus this decode
// comparison together pin that a tag rewrite cannot touch the audio.
[[nodiscard]] bool decoded_pcm_matches(const std::filesystem::path& first,
                                       const std::filesystem::path& second) {
    auto decode_all = [](const std::filesystem::path& path) -> std::optional<std::vector<float>> {
        auto decoder = trackknife::formats::AudioDecoder::open(path.native());
        if (!decoder) {
            return std::nullopt;
        }
        std::vector<float> samples;
        while (true) {
            auto chunk = decoder->next_chunk();
            if (!chunk) {
                return std::nullopt;
            }
            if (!*chunk) {
                break;
            }
            samples.insert(samples.end(), (*chunk)->interleaved_samples.begin(),
                           (*chunk)->interleaved_samples.end());
        }
        return samples;
    };
    const auto first_samples = decode_all(first);
    const auto second_samples = decode_all(second);
    return first_samples && second_samples && *first_samples == *second_samples;
}

void roundTripsCommentEditsPreservingPackets(const std::filesystem::path& fixture_directory,
                                             const std::string_view fixture,
                                             const std::string_view expected_adapter) {
    TemporaryDirectory directory;
    const auto source = materialize(fixture_directory, fixture, directory.path() / "source.ogg");

    const auto read = trackknife::metadata::read_local_metadata(source.native());
    CHECK(read.has_value());
    if (!read) {
        return;
    }
    CHECK(read->adapter_name == expected_adapter);
    CHECK(read->capabilities.fields_writable);
    CHECK(read->capabilities.unknown_data_preserved_on_write);
    CHECK(read->capabilities.pictures_writable);
    CHECK(read->document.first_effective_value("title") ==
          std::optional<std::string>{"Fixture Tone"});

    auto selection = trackknife::metadata::StagedMetadataSelection::create(
        {trackknife::metadata::StagedMetadataSource{
            .raw_path = read->raw_path,
            .source_revision = read->source_revision,
            .baseline = read->document,
        }});
    CHECK(selection.has_value());
    if (!selection) {
        return;
    }
    const auto title = selection->field_index("title");
    const auto artist = selection->field_index("artist");
    const auto track_id =
        selection->ensure_missing_field("musicbrainztrackid", "MUSICBRAINZ_TRACKID");
    const auto totals = selection->ensure_missing_field("totaltracks", "TOTALTRACKS");
    CHECK(title.has_value() && artist.has_value() && track_id.has_value() && totals.has_value());
    if (!title || !artist || !track_id || !totals) {
        return;
    }
    trackknife::metadata::StagedMetadataPatchSet patches;
    CHECK(patches.replace_values(*selection, 0U, *title, {"Prepared Tone"}).has_value());
    CHECK(patches.remove_field(*selection, 0U, *artist).has_value());
    CHECK(
        patches.replace_values(*selection, 0U, *track_id, {"01b43c31-1b23-42f0-a5d5-275288538230"})
            .has_value());
    CHECK(patches.replace_values(*selection, 0U, *totals, {"9"}).has_value());

    auto plan = trackknife::metadata::revalidate_metadata_write_plan(*selection, patches);
    CHECK(plan.has_value() && plan->ready());
    if (!plan || !plan->ready()) {
        return;
    }
    const auto prepared_path = directory.path() / "prepared.ogg";
    const auto prepared = trackknife::metadata::prepare_ogg_metadata_write_copy(
        plan->sources.front(), prepared_path.native());
    if (!prepared) {
        std::cerr << fixture << ": " << prepared.error().message << '\n';
    }
    CHECK(prepared.has_value());
    if (!prepared) {
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
    CHECK(reread->document.first_effective_value("totaltracks") == std::optional<std::string>{"9"});

    CHECK(decoded_pcm_matches(source, prepared_path));

    CHECK(trackknife::metadata::is_qualified_text_adapter(expected_adapter));
}

// The cover entries an Ogg file's comment packet holds, as stored.
[[nodiscard]] std::optional<std::vector<std::string>>
stored_covers(const std::filesystem::path& path, const bool opus) {
    std::ifstream input{path, std::ios::binary};
    const std::vector<unsigned char> bytes{std::istreambuf_iterator<char>{input},
                                           std::istreambuf_iterator<char>{}};
    auto stream = trackknife::metadata::ogg_stream_detail::parse_packet_stream(
        bytes, trackknife::core::Error{});
    if (!stream || stream->packets.size() < 2U) {
        return std::nullopt;
    }
    auto entries = trackknife::metadata::ogg_stream_detail::comment_entries(
        stream->packets[1],
        opus ? std::string_view{"OpusTags"}.size() : std::string_view{"\x03vorbis"}.size());
    if (!entries) {
        return std::nullopt;
    }
    return trackknife::metadata::ogg_stream_detail::cover_entries(*entries);
}

// ADR-0334: a tag write keeps every cover the file carries, as it was --
// a picture TagLib reads, one it cannot read and would not write back, and
// the older COVERART with its mime type.
void keepsCoversThroughTagWrites(const std::filesystem::path& fixture_directory,
                                 const std::string_view fixture, const bool opus) {
    TemporaryDirectory directory;
    const auto source = materialize(fixture_directory, fixture, directory.path() / "covered.ogg");
    {
        std::unique_ptr<TagLib::File> file;
        TagLib::Ogg::XiphComment* comment = nullptr;
        if (opus) {
            auto opened = std::make_unique<TagLib::Ogg::Opus::File>(source.c_str());
            comment = opened->tag();
            file = std::move(opened);
        } else {
            auto opened = std::make_unique<TagLib::Vorbis::File>(source.c_str());
            comment = opened->tag();
            file = std::move(opened);
        }
        CHECK(file->isValid() && comment != nullptr);
        if (comment == nullptr) {
            return;
        }
        auto* picture = new TagLib::FLAC::Picture;
        picture->setType(TagLib::FLAC::Picture::FrontCover);
        picture->setMimeType("image/png");
        picture->setDescription("front");
        picture->setData(TagLib::ByteVector{"\x89PNG\r\n\x1a\nnot really a picture", 28U});
        comment->addPicture(picture);
        // A picture block cut short: TagLib cannot read it, so writes
        // nothing of it back.
        comment->addField("METADATA_BLOCK_PICTURE", "AAAAAwAAAAlpbWFnZS9wbmc=", false);
        comment->addField("COVERART", "/9j/4AAQSkZJRgABAQ==", false);
        comment->addField("COVERARTMIME", "image/jpeg", false);
        CHECK(file->save());
    }
    const auto carried = stored_covers(source, opus);
    CHECK(carried && carried->size() == 4U);
    if (!carried || carried->size() != 4U) {
        return;
    }

    const auto read = trackknife::metadata::read_local_metadata(source.native());
    CHECK(read.has_value());
    if (!read) {
        return;
    }
    auto selection = trackknife::metadata::StagedMetadataSelection::create(
        {trackknife::metadata::StagedMetadataSource{
            .raw_path = read->raw_path,
            .source_revision = read->source_revision,
            .baseline = read->document,
        }});
    CHECK(selection.has_value());
    if (!selection) {
        return;
    }
    // As ReplayGain stages it.
    const auto gain =
        selection->ensure_missing_field(opus ? "r128_track_gain" : "replaygain_track_gain",
                                        opus ? "R128_TRACK_GAIN" : "REPLAYGAIN_TRACK_GAIN");
    CHECK(gain.has_value());
    if (!gain) {
        return;
    }
    trackknife::metadata::StagedMetadataPatchSet patches;
    CHECK(patches.replace_values(*selection, 0U, *gain, {opus ? "-768" : "-3.00 dB"}).has_value());
    auto plan = trackknife::metadata::revalidate_metadata_write_plan(*selection, patches);
    CHECK(plan.has_value() && plan->ready());
    if (!plan || !plan->ready()) {
        return;
    }
    const auto prepared_path = directory.path() / "prepared.ogg";
    const auto prepared = trackknife::metadata::prepare_ogg_metadata_write_copy(
        plan->sources.front(), prepared_path.native());
    if (!prepared) {
        std::cerr << fixture << ": " << prepared.error().message << '\n';
    }
    CHECK(prepared.has_value());
    const auto kept = stored_covers(prepared_path, opus);
    CHECK(kept.has_value());
    if (kept) {
        CHECK(trackknife::metadata::ogg_stream_detail::same_covers(*carried, *kept));
    }
    CHECK(decoded_pcm_matches(source, prepared_path));
}

// ADR-0337: covers in Opus and Vorbis comments are read, replaced, removed
// and added like FLAC's -- each change in its place, the others as they
// were, the audio packets byte-identical; a comment holding covers TagLib
// cannot all read is shown, not written.
void roundTripsOggArtworkEdits(const std::filesystem::path& fixture_directory,
                               const std::string_view fixture, const bool opus) {
    using trackknife::metadata::ArtworkRole;
    using trackknife::metadata::ArtworkWritePlanIntent;
    using trackknife::metadata::ArtworkWritePlanIntentKind;
    const std::string qualified = opus ? "taglib-opus-picture-v1" : "taglib-vorbis-picture-v1";

    TemporaryDirectory directory;
    const auto media = materialize(fixture_directory, fixture, directory.path() / "covered.ogg");
    const auto plain = materialize(fixture_directory, fixture, directory.path() / "plain.ogg");
    const auto replacement = materialize(fixture_directory, "external-blue-jpeg.b64",
                                         directory.path() / "replacement.jpg");
    const auto with_comment = [opus](const std::filesystem::path& path, const auto& edit) {
        std::unique_ptr<TagLib::File> file;
        TagLib::Ogg::XiphComment* comment = nullptr;
        if (opus) {
            auto opened = std::make_unique<TagLib::Ogg::Opus::File>(path.c_str());
            comment = opened->tag();
            file = std::move(opened);
        } else {
            auto opened = std::make_unique<TagLib::Vorbis::File>(path.c_str());
            comment = opened->tag();
            file = std::move(opened);
        }
        CHECK(file->isValid() && comment != nullptr);
        if (comment != nullptr) {
            edit(*comment);
            CHECK(file->save());
        }
    };
    const auto picture = [](const TagLib::FLAC::Picture::Type type, const char* description,
                            const std::vector<unsigned char>& bytes) {
        auto* made = new TagLib::FLAC::Picture;
        made->setType(type);
        made->setMimeType("image/jpeg");
        made->setDescription(description);
        made->setData(TagLib::ByteVector{reinterpret_cast<const char*>(bytes.data()),
                                         static_cast<unsigned int>(bytes.size())});
        return made;
    };
    const std::vector<unsigned char> front{0xFFU, 0xD8U, 0xFFU, 0xDBU, 0x00U,
                                           0x04U, 0x01U, 0x02U, 0xFFU, 0xD9U};
    const std::vector<unsigned char> back{0xFFU, 0xD8U, 0xFFU, 0xDBU, 0x00U,
                                          0x04U, 0x03U, 0x04U, 0xFFU, 0xD9U};
    with_comment(media, [&](TagLib::Ogg::XiphComment& comment) {
        comment.addPicture(picture(TagLib::FLAC::Picture::FrontCover, "the front", front));
        comment.addPicture(picture(TagLib::FLAC::Picture::BackCover, "the back", back));
    });

    auto policy = trackknife::metadata::default_artwork_inventory_policy();
    policy.external_patterns.clear();
    const auto inventory =
        trackknife::metadata::read_local_artwork_inventory(media.native(), policy);
    CHECK(inventory.has_value());
    if (!inventory) {
        return;
    }
    CHECK(inventory->embedded_adapter_name == qualified);
    CHECK(trackknife::metadata::is_qualified_artwork_adapter(inventory->embedded_adapter_name));
    CHECK(inventory->items.size() == 2U);
    if (inventory->items.size() != 2U) {
        return;
    }
    CHECK(inventory->items[0].role == ArtworkRole::front);
    CHECK(inventory->items[0].description == "the front");
    CHECK(inventory->items[1].role == ArtworkRole::back);

    const ArtworkWritePlanIntent replace_intent{
        .occurrence_index = 0U,
        .raw_media_path = media.native(),
        .expected_media_revision = inventory->media_revision,
        .target_ordinal = 0U,
        .expected_target_fingerprint = inventory->items[0].content_fingerprint,
        .kind = ArtworkWritePlanIntentKind::replace,
        .replacement_raw_path = replacement.native(),
        .added_role = ArtworkRole::front,
        .added_description = {},
        .replacement_embedded_source = std::nullopt,
    };
    const auto write =
        [&](const std::vector<ArtworkWritePlanIntent>& intents,
            const std::string& name) -> std::optional<trackknife::metadata::LocalArtworkInventory> {
        const auto plan = trackknife::metadata::revalidate_artwork_write_plan(intents);
        CHECK(plan.has_value() && plan->ready());
        if (!plan || !plan->ready()) {
            return std::nullopt;
        }
        CHECK(plan->sources.front().adapter_name == qualified);
        const auto prepared_path = directory.path() / name;
        const auto prepared = trackknife::metadata::prepare_qualified_artwork_write_copy(
            plan->sources.front(), prepared_path.native());
        if (!prepared) {
            std::cerr << fixture << " " << name << ": " << prepared.error().message << '\n';
            CHECK(false);
            return std::nullopt;
        }
        CHECK(decoded_pcm_matches(std::filesystem::path{intents.front().raw_media_path},
                                  prepared_path));
        auto reread =
            trackknife::metadata::read_local_artwork_inventory(prepared_path.native(), policy);
        CHECK(reread.has_value());
        return reread ? std::optional{std::move(*reread)} : std::nullopt;
    };

    // Replaced in its place: the new picture, the old type and description.
    if (const auto replaced = write({replace_intent}, "replaced.ogg")) {
        CHECK(replaced->items.size() == 2U);
        if (replaced->items.size() == 2U) {
            CHECK(replaced->items[0].content_fingerprint !=
                  inventory->items[0].content_fingerprint);
            CHECK(replaced->items[0].role == ArtworkRole::front);
            CHECK(replaced->items[0].description == "the front");
            CHECK(replaced->items[1].content_fingerprint ==
                  inventory->items[1].content_fingerprint);
            CHECK(replaced->items[1].description == "the back");
        }
    }
    // Removed: the other stays, first now.
    auto remove_intent = replace_intent;
    remove_intent.kind = ArtworkWritePlanIntentKind::remove;
    remove_intent.replacement_raw_path.reset();
    if (const auto removed = write({remove_intent}, "removed.ogg")) {
        CHECK(removed->items.size() == 1U);
        if (removed->items.size() == 1U) {
            CHECK(removed->items[0].content_fingerprint == inventory->items[1].content_fingerprint);
            CHECK(removed->items[0].source_ordinal == 0U);
        }
    }
    // Added to a file without covers: its role's type, the description given.
    auto add_intent = replace_intent;
    add_intent.raw_media_path = plain.native();
    add_intent.expected_media_revision =
        *trackknife::core::observe_local_source_revision(plain.native());
    add_intent.kind = ArtworkWritePlanIntentKind::add;
    add_intent.expected_target_fingerprint = {};
    add_intent.added_role = ArtworkRole::back;
    add_intent.added_description = "added";
    if (const auto added = write({add_intent}, "added.ogg")) {
        CHECK(added->items.size() == 1U);
        if (added->items.size() == 1U) {
            CHECK(added->items[0].role == ArtworkRole::back);
            CHECK(added->items[0].description == "added");
            CHECK(added->items[0].mime_type == "image/jpeg");
        }
    }
    // Two changes to one file at once: replaced, and one more added.
    const auto other =
        materialize(fixture_directory, "cover-64-png.b64", directory.path() / "other.png");
    auto second_add = add_intent;
    second_add.replacement_raw_path = other.native();
    second_add.raw_media_path = media.native();
    second_add.expected_media_revision = inventory->media_revision;
    second_add.target_ordinal = 2U;
    if (const auto both = write({replace_intent, second_add}, "both.ogg")) {
        CHECK(both->items.size() == 3U);
        if (both->items.size() == 3U) {
            CHECK(both->items[1].content_fingerprint == inventory->items[1].content_fingerprint);
            CHECK(both->items[2].description == "added");
        }
    }

    // An older COVERART beside the pictures: read, but not to be written.
    const auto legacy = materialize(fixture_directory, fixture, directory.path() / "legacy.ogg");
    with_comment(legacy, [&](TagLib::Ogg::XiphComment& comment) {
        comment.addPicture(picture(TagLib::FLAC::Picture::FrontCover, "", front));
        comment.addField("COVERART", "/9j/4AAQSkZJRgABAQ==", false);
        comment.addField("COVERARTMIME", "image/jpeg", false);
    });
    const auto legacy_inventory =
        trackknife::metadata::read_local_artwork_inventory(legacy.native(), policy);
    CHECK(legacy_inventory.has_value());
    if (legacy_inventory) {
        CHECK(legacy_inventory->capabilities.embedded_readable);
        CHECK(!trackknife::metadata::is_qualified_artwork_adapter(
            legacy_inventory->embedded_adapter_name));
    }
}

} // namespace

int main(const int argc, char** argv) {
    CHECK(argc == 2);
    if (argc == 2) {
        const std::filesystem::path fixture_directory{argv[1]};
        roundTripsCommentEditsPreservingPackets(fixture_directory, "tagged-tone-vorbis.b64",
                                                "taglib-vorbis-v1");
        roundTripsCommentEditsPreservingPackets(fixture_directory, "tagged-tone-opus.b64",
                                                "taglib-opus-v1");
        keepsCoversThroughTagWrites(fixture_directory, "tagged-tone-vorbis.b64", false);
        keepsCoversThroughTagWrites(fixture_directory, "tagged-tone-opus.b64", true);
        roundTripsOggArtworkEdits(fixture_directory, "tagged-tone-vorbis.b64", false);
        roundTripsOggArtworkEdits(fixture_directory, "tagged-tone-opus.b64", true);
    }
    return failures == 0 ? 0 : 1;
}

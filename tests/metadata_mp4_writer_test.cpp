// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/core/stable_id.hpp"
#include "trackknife/formats/decoder.hpp"
#include "trackknife/metadata/artwork.hpp"
#include "trackknife/metadata/artwork_write_plan.hpp"
#include "trackknife/metadata/artwork_writers.hpp"
#include "trackknife/metadata/local_reader.hpp"
#include "trackknife/metadata/mp3_writer.hpp"
#include "trackknife/metadata/mp4_writer.hpp"
#include "trackknife/metadata/staged_patch.hpp"
#include "trackknife/metadata/staged_selection.hpp"
#include "trackknife/metadata/write_plan.hpp"

#include "../src/metadata/src/container_preservation_detail.hpp"

#include <taglib/mp4coverart.h>
#include <taglib/mp4file.h>
#include <taglib/mp4item.h>
#include <taglib/tpropertymap.h>

#include <array>
#include <cstddef>
#include <cstdint>
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
                ("trackknife-mp4-writer-" + trackknife::core::StableId::random().to_string());
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

// An independent top-level box walk: returns the payload of the first box
// with the requested type, empty when absent.
[[nodiscard]] std::vector<unsigned char> box_bytes(const std::vector<unsigned char>& bytes,
                                                   const std::string_view type) {
    std::size_t offset = 0U;
    while (offset + 8U <= bytes.size()) {
        auto size = (static_cast<std::size_t>(bytes[offset]) << 24U) |
                    (static_cast<std::size_t>(bytes[offset + 1U]) << 16U) |
                    (static_cast<std::size_t>(bytes[offset + 2U]) << 8U) |
                    static_cast<std::size_t>(bytes[offset + 3U]);
        const std::string_view name{reinterpret_cast<const char*>(bytes.data()) + offset + 4U, 4U};
        if (size == 0U) {
            size = bytes.size() - offset;
        }
        if (size < 8U || size > bytes.size() - offset) {
            return {};
        }
        if (name == type) {
            return {bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                    bytes.begin() + static_cast<std::ptrdiff_t>(offset + size)};
        }
        offset += size;
    }
    return {};
}

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

// The first `stco` table's entries in `bytes`: where its count is, and how
// many. ADR-0306's tests change them in place.
struct ChunkTable {
    std::size_t count_at{0U};
    std::size_t count{0U};
};
[[nodiscard]] std::optional<ChunkTable> chunk_table(const std::vector<unsigned char>& bytes) {
    const std::string_view text{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
    const auto at = text.find("stco");
    if (at == std::string_view::npos || at + 12U > bytes.size()) {
        return std::nullopt;
    }
    const auto be32 = [&bytes](const std::size_t from) {
        return (static_cast<std::size_t>(bytes[from]) << 24U) |
               (static_cast<std::size_t>(bytes[from + 1U]) << 16U) |
               (static_cast<std::size_t>(bytes[from + 2U]) << 8U) |
               static_cast<std::size_t>(bytes[from + 3U]);
    };
    return ChunkTable{.count_at = at + 8U, .count = be32(at + 8U)};
}
[[nodiscard]] std::uint32_t chunk_offset(const std::vector<unsigned char>& bytes,
                                         const ChunkTable& table, const std::size_t index) {
    const auto at = table.count_at + 4U + index * 4U;
    return (static_cast<std::uint32_t>(bytes[at]) << 24U) |
           (static_cast<std::uint32_t>(bytes[at + 1U]) << 16U) |
           (static_cast<std::uint32_t>(bytes[at + 2U]) << 8U) |
           static_cast<std::uint32_t>(bytes[at + 3U]);
}
void set_chunk_offset(std::vector<unsigned char>& bytes, const ChunkTable& table,
                      const std::size_t index, const std::uint32_t value) {
    const auto at = table.count_at + 4U + index * 4U;
    bytes[at] = static_cast<unsigned char>(value >> 24U);
    bytes[at + 1U] = static_cast<unsigned char>(value >> 16U);
    bytes[at + 2U] = static_cast<unsigned char>(value >> 8U);
    bytes[at + 3U] = static_cast<unsigned char>(value);
}
void write_bytes(const std::filesystem::path& path, const std::vector<unsigned char>& bytes) {
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
}

// ADR-0306: the audio index. Many real files have `moov` before `mdat`
// ("fast start"), so a tag that grows `moov` moves the audio, and TagLib
// must shift every chunk offset by as much. The audio stays byte-identical
// either way; only this proves it can still be found.
void verifiesTheAudioIndex(const std::filesystem::path& fixture_directory) {
    TemporaryDirectory directory;
    const auto original =
        materialize(fixture_directory, "tagged-tone-m4a.b64", directory.path() / "original.m4a");
    const auto bytes = read_bytes(original);
    const auto ftyp = box_bytes(bytes, "ftyp");
    const auto mdat = box_bytes(bytes, "mdat");
    auto moov = box_bytes(bytes, "moov");
    CHECK(!ftyp.empty() && !mdat.empty() && !moov.empty());
    // Fast start, made here: ftyp, moov, mdat -- every offset moved by the
    // moov now in front of the audio.
    const auto table = chunk_table(moov);
    CHECK(table.has_value() && table->count > 0U);
    if (!table) {
        return;
    }
    const auto old_mdat_at = bytes.size() - mdat.size() - moov.size();
    const auto new_mdat_at = ftyp.size() + moov.size();
    for (std::size_t index = 0U; index < table->count; ++index) {
        set_chunk_offset(moov, *table, index,
                         static_cast<std::uint32_t>(chunk_offset(moov, *table, index) -
                                                    old_mdat_at + new_mdat_at));
    }
    std::vector<unsigned char> fast;
    fast.insert(fast.end(), ftyp.begin(), ftyp.end());
    fast.insert(fast.end(), moov.begin(), moov.end());
    fast.insert(fast.end(), mdat.begin(), mdat.end());
    const auto source = directory.path() / "source.m4a";
    write_bytes(source, fast);
    CHECK(decoded_pcm_matches(original, source));

    // A large tag: moov grows, the audio moves, TagLib shifts the index.
    const auto prepared = directory.path() / "prepared.m4a";
    std::filesystem::copy_file(source, prepared);
    {
        TagLib::MP4::File file{prepared.c_str()};
        CHECK(file.isValid());
        auto properties = file.properties();
        properties.replace("COMMENT",
                           TagLib::String{std::string(20000U, 'x'), TagLib::String::UTF8});
        CHECK(file.setProperties(properties).isEmpty());
        CHECK(file.save());
    }
    auto written = read_bytes(prepared);
    const auto written_table = chunk_table(written);
    CHECK(written_table.has_value() && written_table->count == table->count);
    if (!written_table) {
        return;
    }
    CHECK(chunk_offset(written, *written_table, 0U) != chunk_offset(moov, *table, 0U));
    CHECK(decoded_pcm_matches(original, prepared));
    namespace detail = trackknife::metadata::preservation_detail;
    CHECK(detail::verify_mp4_box_preservation(source.native(), prepared.native(), {}).has_value());

    // An index not shifted -- as if the writer had not -- points beside the
    // audio: refused, though every byte of the audio is the same.
    auto unshifted = written;
    for (std::size_t index = 0U; index < table->count; ++index) {
        set_chunk_offset(unshifted, *written_table, index, chunk_offset(moov, *table, index));
    }
    const auto damaged = directory.path() / "damaged.m4a";
    write_bytes(damaged, unshifted);
    auto refused = detail::verify_mp4_box_preservation(source.native(), damaged.native(), {});
    CHECK(!refused.has_value());
    CHECK(!refused && refused.error().message.find("audio index") != std::string::npos);
    // One chunk off by a byte: refused too.
    auto nudged = written;
    set_chunk_offset(nudged, *written_table, 0U, chunk_offset(written, *written_table, 0U) + 1U);
    write_bytes(damaged, nudged);
    CHECK(!detail::verify_mp4_box_preservation(source.native(), damaged.native(), {}).has_value());
}

// ADR-0136: standard atoms, an exact freeform addition, the combined trkn
// total, and a pre-existing covr atom must all survive a prepared-copy tag
// write while ftyp/mdat stay byte-identical and the PCM is unchanged.
void roundTripsAtomEditsPreservingBoxes(const std::filesystem::path& fixture_directory) {
    TemporaryDirectory directory;
    const auto source =
        materialize(fixture_directory, "tagged-tone-m4a.b64", directory.path() / "source.m4a");

    // Give the source a cover atom and the combined track total up front —
    // the writer must carry both through untouched picture-wise.
    const std::vector<unsigned char> cover_pixels{0xFFU, 0xD8U, 0xFFU, 0xDBU, 0x00U,
                                                  0x04U, 0x01U, 0x02U, 0xFFU, 0xD9U};
    {
        TagLib::MP4::File file{source.c_str()};
        CHECK(file.isValid());
        TagLib::MP4::CoverArtList covers;
        covers.append(TagLib::MP4::CoverArt{
            TagLib::MP4::CoverArt::JPEG,
            TagLib::ByteVector{reinterpret_cast<const char*>(cover_pixels.data()),
                               static_cast<unsigned int>(cover_pixels.size())}});
        file.tag()->setItem("covr", TagLib::MP4::Item{covers});
        auto properties = file.properties();
        properties.replace("TRACKNUMBER", TagLib::String{"3/12", TagLib::String::UTF8});
        CHECK(file.setProperties(properties).isEmpty());
        CHECK(file.save());
    }

    const auto read = trackknife::metadata::read_local_metadata(source.native());
    CHECK(read.has_value());
    if (!read) {
        return;
    }
    CHECK(read->adapter_name == "taglib-mp4-v1");
    CHECK(read->capabilities.fields_writable);
    CHECK(read->capabilities.unknown_data_preserved_on_write);
    CHECK(read->capabilities.pictures_writable);
    CHECK(read->document.first_effective_value("title") ==
          std::optional<std::string>{"Fixture Tone"});
    CHECK(read->document.first_effective_value("tracknumber") ==
          std::optional<std::string>{"3/12"});

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
    const auto album = selection->field_index("album");
    const auto tracknumber = selection->field_index("tracknumber");
    const auto genre = selection->ensure_missing_field("genre", "GENRE");
    const auto freeform = selection->ensure_missing_field("customfield", "CUSTOMFIELD");
    CHECK(title.has_value() && album.has_value() && tracknumber.has_value() && genre.has_value() &&
          freeform.has_value());
    if (!title || !album || !tracknumber || !genre || !freeform) {
        return;
    }
    trackknife::metadata::StagedMetadataPatchSet patches;
    CHECK(patches.replace_values(*selection, 0U, *title, {"Prepared Tone"}).has_value());
    CHECK(patches.remove_field(*selection, 0U, *album).has_value());
    CHECK(patches.replace_values(*selection, 0U, *tracknumber, {"7/12"}).has_value());
    CHECK(patches.replace_values(*selection, 0U, *genre, {"Test Tone"}).has_value());
    CHECK(patches.replace_values(*selection, 0U, *freeform, {"Freeform Value"}).has_value());

    auto plan = trackknife::metadata::revalidate_metadata_write_plan(*selection, patches);
    CHECK(plan.has_value() && plan->ready());
    if (!plan || !plan->ready()) {
        return;
    }
    const auto prepared_path = directory.path() / "prepared.m4a";
    const auto prepared = trackknife::metadata::prepare_mp4_metadata_write_copy(
        plan->sources.front(), prepared_path.native());
    if (!prepared) {
        std::cerr << prepared.error().message << '\n';
    }
    CHECK(prepared.has_value());
    if (!prepared) {
        return;
    }
    CHECK(prepared->field_change_count == 5U);

    const auto reread = trackknife::metadata::read_local_metadata(prepared_path.native());
    CHECK(reread.has_value());
    if (!reread) {
        return;
    }
    CHECK(reread->document.first_effective_value("title") ==
          std::optional<std::string>{"Prepared Tone"});
    CHECK(!reread->document.first_effective_value("album"));
    CHECK(reread->document.first_effective_value("tracknumber") ==
          std::optional<std::string>{"7/12"});
    CHECK(reread->document.first_effective_value("genre") ==
          std::optional<std::string>{"Test Tone"});
    CHECK(reread->document.first_effective_value("customfield") ==
          std::optional<std::string>{"Freeform Value"});

    // The freeform addition and the untouched cover keep their exact native
    // atom identity in the prepared copy.
    {
        TagLib::MP4::File file{prepared_path.c_str()};
        CHECK(file.isValid());
        CHECK(file.tag()->contains("----:com.apple.iTunes:CUSTOMFIELD"));
        CHECK(file.tag()->contains("covr"));
        const auto covers = file.tag()->item("covr").toCoverArtList();
        CHECK(covers.size() == 1U);
        const auto& data = covers.front().data();
        CHECK(std::vector<unsigned char>(data.begin(), data.end()) == cover_pixels);
    }

    const auto source_bytes = read_bytes(source);
    const auto prepared_bytes = read_bytes(prepared_path);
    CHECK(!box_bytes(source_bytes, "ftyp").empty());
    CHECK(box_bytes(source_bytes, "ftyp") == box_bytes(prepared_bytes, "ftyp"));
    CHECK(!box_bytes(source_bytes, "mdat").empty());
    CHECK(box_bytes(source_bytes, "mdat") == box_bytes(prepared_bytes, "mdat"));

    CHECK(decoded_pcm_matches(source, prepared_path));

    CHECK(trackknife::metadata::is_qualified_text_adapter("taglib-mp4-v1"));
    CHECK(!trackknife::metadata::is_qualified_text_adapter("taglib-properties-v1"));
}

// ADR-0137: covr replace/remove/add through the qualified prepared-copy
// writer — untyped front-cover items, unrelated entries preserved by
// fingerprint, ftyp/mdat byte-identical, text and PCM unchanged.
void roundTripsCovrArtworkEdits(const std::filesystem::path& fixture_directory) {
    using trackknife::metadata::ArtworkWritePlanIntent;
    using trackknife::metadata::ArtworkWritePlanIntentKind;

    TemporaryDirectory directory;
    const auto media =
        materialize(fixture_directory, "tagged-tone-m4a.b64", directory.path() / "artful.m4a");
    const auto replacement = materialize(fixture_directory, "external-blue-jpeg.b64",
                                         directory.path() / "replacement.jpg");
    const std::vector<unsigned char> first_pixels{0xFFU, 0xD8U, 0xFFU, 0xDBU, 0x00U,
                                                  0x04U, 0x01U, 0x02U, 0xFFU, 0xD9U};
    const std::vector<unsigned char> second_pixels{0xFFU, 0xD8U, 0xFFU, 0xDBU, 0x00U,
                                                   0x04U, 0x03U, 0x04U, 0xFFU, 0xD9U};
    {
        TagLib::MP4::File file{media.c_str()};
        CHECK(file.isValid());
        TagLib::MP4::CoverArtList covers;
        covers.append(TagLib::MP4::CoverArt{
            TagLib::MP4::CoverArt::JPEG,
            TagLib::ByteVector{reinterpret_cast<const char*>(first_pixels.data()),
                               static_cast<unsigned int>(first_pixels.size())}});
        covers.append(TagLib::MP4::CoverArt{
            TagLib::MP4::CoverArt::JPEG,
            TagLib::ByteVector{reinterpret_cast<const char*>(second_pixels.data()),
                               static_cast<unsigned int>(second_pixels.size())}});
        file.tag()->setItem("covr", TagLib::MP4::Item{covers});
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
    CHECK(inventory->embedded_adapter_name == "taglib-mp4-covr-v1");
    CHECK(inventory->capabilities.embedded_readable);
    CHECK(inventory->items.size() == 2U);
    CHECK(inventory->items[0].role == trackknife::metadata::ArtworkRole::front);
    CHECK(inventory->items[0].native_type.empty());
    CHECK(inventory->items[0].description.empty());
    CHECK(inventory->items[0].mime_type == "image/jpeg");
    CHECK(inventory->items[1].source_ordinal == 1U);

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
    CHECK(replace_plan->sources.front().adapter_name == "taglib-mp4-covr-v1");
    const auto replaced_path = directory.path() / "replaced.m4a";
    const auto replaced = trackknife::metadata::prepare_mp4_artwork_write_copy(
        replace_plan->sources.front(), replaced_path.native());
    if (!replaced) {
        std::cerr << replaced.error().message << '\n';
    }
    CHECK(replaced.has_value());
    if (!replaced) {
        return;
    }
    CHECK(replaced->inventory.items.size() == 2U);
    CHECK(replaced->inventory.items[0].content_fingerprint !=
          inventory->items[0].content_fingerprint);
    CHECK(replaced->inventory.items[0].mime_type == "image/jpeg");
    CHECK(replaced->inventory.items[0].native_type.empty());
    CHECK(replaced->inventory.items[1].content_fingerprint ==
          inventory->items[1].content_fingerprint);
    CHECK(replaced->document.first_effective_value("title") ==
          std::optional<std::string>{"Fixture Tone"});
    const auto media_bytes = read_bytes(media);
    const auto replaced_bytes = read_bytes(replaced_path);
    CHECK(box_bytes(media_bytes, "ftyp") == box_bytes(replaced_bytes, "ftyp"));
    CHECK(box_bytes(media_bytes, "mdat") == box_bytes(replaced_bytes, "mdat"));
    CHECK(decoded_pcm_matches(media, replaced_path));

    auto remove_intent = replace_intent;
    remove_intent.kind = ArtworkWritePlanIntentKind::remove;
    remove_intent.target_ordinal = 1U;
    remove_intent.expected_target_fingerprint = inventory->items[1].content_fingerprint;
    remove_intent.replacement_raw_path.reset();
    const auto remove_plan = trackknife::metadata::revalidate_artwork_write_plan({remove_intent});
    CHECK(remove_plan.has_value() && remove_plan->ready());
    if (remove_plan && remove_plan->ready()) {
        const auto removed_path = directory.path() / "removed.m4a";
        const auto removed = trackknife::metadata::prepare_mp4_artwork_write_copy(
            remove_plan->sources.front(), removed_path.native());
        CHECK(removed.has_value());
        if (removed) {
            CHECK(removed->inventory.items.size() == 1U);
            CHECK(removed->inventory.items[0].content_fingerprint ==
                  inventory->items[0].content_fingerprint);
            CHECK(removed->inventory.items[0].source_ordinal == 0U);
        }
    }

    // Adds land as untyped covr entries; a requested description is
    // unsupported in this container.
    const auto plain =
        materialize(fixture_directory, "tagged-tone-m4a.b64", directory.path() / "plain.m4a");
    auto add_intent = replace_intent;
    add_intent.raw_media_path = plain.native();
    add_intent.expected_media_revision =
        *trackknife::core::observe_local_source_revision(plain.native());
    add_intent.kind = ArtworkWritePlanIntentKind::add;
    add_intent.target_ordinal = 0U;
    add_intent.expected_target_fingerprint = {};
    add_intent.added_role = trackknife::metadata::ArtworkRole::back;
    const auto add_plan = trackknife::metadata::revalidate_artwork_write_plan({add_intent});
    CHECK(add_plan.has_value() && add_plan->ready());
    if (add_plan && add_plan->ready()) {
        const auto added_path = directory.path() / "added.m4a";
        const auto added = trackknife::metadata::prepare_mp4_artwork_write_copy(
            add_plan->sources.front(), added_path.native());
        if (!added) {
            std::cerr << "add: " << added.error().message << '\n';
        }
        CHECK(added.has_value());
        if (added) {
            CHECK(added->inventory.items.size() == 1U);
            CHECK(added->inventory.items[0].role == trackknife::metadata::ArtworkRole::front);
            CHECK(added->inventory.items[0].native_type.empty());
            CHECK(added->inventory.items[0].mime_type == "image/jpeg");
        }

        auto described = add_plan->sources.front();
        described.change.added_description = "Not representable";
        const auto rejected = trackknife::metadata::prepare_mp4_artwork_write_copy(
            described, (directory.path() / "rejected.m4a").native());
        CHECK(!rejected.has_value() &&
              rejected.error().code == trackknife::core::ErrorCode::unsupported);
    }
}

} // namespace

int main(const int argc, char** argv) {
    CHECK(argc == 2);
    if (argc == 2) {
        roundTripsAtomEditsPreservingBoxes(std::filesystem::path{argv[1]});
        roundTripsCovrArtworkEdits(std::filesystem::path{argv[1]});
        verifiesTheAudioIndex(std::filesystem::path{argv[1]});
    }
    return failures == 0 ? 0 : 1;
}

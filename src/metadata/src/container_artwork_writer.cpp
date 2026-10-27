// SPDX-License-Identifier: GPL-3.0-only

#include "mp4_item_factory_detail.hpp"
#include "trackknife/core/posix.hpp"
#include "trackknife/metadata/artwork_writers.hpp"

#include "container_preservation_detail.hpp"
#include "text_writer_detail.hpp"
#include "trackknife/metadata/local_reader.hpp"

#include <attachedpictureframe.h>
#include <flacpicture.h>
#include <id3v2tag.h>
#include <mp4coverart.h>
#include <mp4file.h>
#include <mp4item.h>
#include <mp4tag.h>
#include <mpegfile.h>
#include <opusfile.h>
#include <vorbisfile.h>
#include <xiphcomment.h>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <utility>
#include <vector>

namespace trackknife::metadata {
namespace {

using text_writer_detail::PreparedPathGuard;
using text_writer_detail::system_error;

// One qualified container: the artwork adapter, its text-adapter sibling,
// how a change is applied to the prepared copy, and the container
// preservation proof. The APIC writer keeps types and descriptions; the
// covr writer produces untyped entries (ADR-0137).
struct ContainerArtworkFormat {
    std::string_view label;
    std::string_view artwork_adapter;
    std::string_view text_adapter;
    // Pictures surface as unsupported native objects in the text document
    // ("covr", "APIC[:description]"); the text-unchanged check must ignore
    // exactly these while every other native object stays pinned.
    std::string_view artwork_object_prefix;
    bool supports_descriptions{false};
    std::function<core::Result<void>(const ArtworkWritePlanSource&, const std::string&,
                                     const std::vector<unsigned char>&)>
        apply;
    std::function<core::Result<void>(const std::string&, const std::string&,
                                     const core::CancellationToken&)>
        verify_preservation;
    // Text changes written into the prepared copy, with or without
    // Picard's paired totals spellings (Vorbis comments carry them).
    std::function<core::Result<void>(const MetadataWritePlanSource&, const std::string&,
                                     const core::CancellationToken&)>
        apply_text;
    bool paired_totals{false};
};

[[nodiscard]] core::Error writer_error(const core::ErrorCode code, std::string message,
                                       const std::string& source_raw_path,
                                       const std::string& prepared_raw_path) {
    return text_writer_detail::writer_error(code, std::move(message), source_raw_path,
                                            prepared_raw_path);
}

[[nodiscard]] core::Error cancelled(const ContainerArtworkFormat& format,
                                    const std::string& source_raw_path,
                                    const std::string& prepared_raw_path) {
    return text_writer_detail::cancelled(format.label, source_raw_path, prepared_raw_path);
}

[[nodiscard]] core::Result<LocalArtworkInventory>
read_embedded_inventory(const std::string& raw_path, const core::CancellationToken& cancellation) {
    auto policy = default_artwork_inventory_policy();
    policy.external_patterns.clear();
    return read_local_artwork_inventory(raw_path, policy, cancellation);
}

[[nodiscard]] std::vector<TagLib::ID3v2::AttachedPictureFrame*>
apic_frames(TagLib::ID3v2::Tag* tag) {
    std::vector<TagLib::ID3v2::AttachedPictureFrame*> frames;
    if (tag == nullptr) {
        return frames;
    }
    for (auto* frame : tag->frameListMap()["APIC"]) {
        frames.push_back(dynamic_cast<TagLib::ID3v2::AttachedPictureFrame*>(frame));
    }
    return frames;
}

[[nodiscard]] TagLib::ByteVector to_byte_vector(const std::vector<unsigned char>& bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), static_cast<unsigned int>(bytes.size())};
}

[[nodiscard]] core::Result<void> apply_apic_change(const ArtworkWritePlanSource& source_plan,
                                                   const std::string& prepared_raw_path,
                                                   const std::vector<unsigned char>& replacement) {
    TagLib::MPEG::File file{prepared_raw_path.c_str(), false};
    if (!file.isValid()) {
        return std::unexpected(writer_error(core::ErrorCode::backend,
                                            "TagLib rejected the prepared MP3 copy",
                                            source_plan.raw_media_path, prepared_raw_path));
    }
    auto* tag = file.ID3v2Tag(true);
    auto frames = apic_frames(tag);
    const auto ordinal = source_plan.change.target_ordinal;
    const auto& change = source_plan.change;
    switch (change.kind) {
    case ArtworkWritePlanIntentKind::batch:
        return std::unexpected(writer_error(core::ErrorCode::invalid_argument,
                                            "batch is not an individual picture change",
                                            source_plan.raw_media_path, prepared_raw_path));
    case ArtworkWritePlanIntentKind::remove:
        if (ordinal >= frames.size() || frames[ordinal] == nullptr) {
            return std::unexpected(writer_error(core::ErrorCode::conflict,
                                                "the targeted APIC frame no longer exists",
                                                source_plan.raw_media_path, prepared_raw_path));
        }
        tag->removeFrame(frames[ordinal]);
        break;
    case ArtworkWritePlanIntentKind::replace:
        if (ordinal >= frames.size() || frames[ordinal] == nullptr) {
            return std::unexpected(writer_error(core::ErrorCode::conflict,
                                                "the targeted APIC frame no longer exists",
                                                source_plan.raw_media_path, prepared_raw_path));
        }
        // In-place mutation keeps the frame's position, type, and
        // description.
        frames[ordinal]->setMimeType(
            TagLib::String{change.replacement->mime_type, TagLib::String::UTF8});
        frames[ordinal]->setPicture(to_byte_vector(replacement));
        break;
    case ArtworkWritePlanIntentKind::add: {
        auto* frame = new TagLib::ID3v2::AttachedPictureFrame();
        frame->setType(static_cast<TagLib::ID3v2::AttachedPictureFrame::Type>(
            artwork_detail::canonical_picture_type(change.added_role)));
        frame->setMimeType(TagLib::String{change.replacement->mime_type, TagLib::String::UTF8});
        frame->setDescription(TagLib::String{change.added_description, TagLib::String::UTF8});
        frame->setPicture(to_byte_vector(replacement));
        tag->addFrame(frame);
        break;
    }
    }
    if (!file.save()) {
        return std::unexpected(writer_error(core::ErrorCode::backend,
                                            "TagLib could not save the prepared MP3 copy",
                                            source_plan.raw_media_path, prepared_raw_path));
    }
    return {};
}

[[nodiscard]] core::Result<void> apply_covr_change(const ArtworkWritePlanSource& source_plan,
                                                   const std::string& prepared_raw_path,
                                                   const std::vector<unsigned char>& replacement) {
    TagLib::MP4::File file{prepared_raw_path.c_str(), false, TagLib::AudioProperties::Average,
                           mp4_detail::FreeFormFactory::instance()};
    if (!file.isValid() || file.tag() == nullptr) {
        return std::unexpected(writer_error(core::ErrorCode::backend,
                                            "TagLib rejected the prepared MP4 copy",
                                            source_plan.raw_media_path, prepared_raw_path));
    }
    auto* tag = file.tag();
    auto covers =
        tag->contains("covr") ? tag->item("covr").toCoverArtList() : TagLib::MP4::CoverArtList{};
    const auto ordinal = source_plan.change.target_ordinal;
    const auto& change = source_plan.change;
    const auto replacement_cover = [&] {
        const auto format = change.replacement->mime_type == "image/png"
                                ? TagLib::MP4::CoverArt::PNG
                                : TagLib::MP4::CoverArt::JPEG;
        return TagLib::MP4::CoverArt{format, to_byte_vector(replacement)};
    };
    switch (change.kind) {
    case ArtworkWritePlanIntentKind::batch:
        return std::unexpected(writer_error(core::ErrorCode::invalid_argument,
                                            "batch is not an individual picture change",
                                            source_plan.raw_media_path, prepared_raw_path));
    case ArtworkWritePlanIntentKind::remove:
    case ArtworkWritePlanIntentKind::replace: {
        if (ordinal >= covers.size()) {
            return std::unexpected(writer_error(core::ErrorCode::conflict,
                                                "the targeted covr entry no longer exists",
                                                source_plan.raw_media_path, prepared_raw_path));
        }
        TagLib::MP4::CoverArtList rebuilt;
        for (unsigned int index = 0U; index < covers.size(); ++index) {
            if (index == ordinal) {
                if (change.kind == ArtworkWritePlanIntentKind::replace) {
                    rebuilt.append(replacement_cover());
                }
                continue;
            }
            rebuilt.append(covers[index]);
        }
        covers = rebuilt;
        break;
    }
    case ArtworkWritePlanIntentKind::add:
        covers.append(replacement_cover());
        break;
    }
    if (covers.isEmpty()) {
        tag->removeItem("covr");
    } else {
        tag->setItem("covr", TagLib::MP4::Item{covers});
    }
    if (!file.save()) {
        return std::unexpected(writer_error(core::ErrorCode::backend,
                                            "TagLib could not save the prepared MP4 copy",
                                            source_plan.raw_media_path, prepared_raw_path));
    }
    return {};
}

// ADR-0337: one picture change in an Opus or Vorbis comment -- its pictures
// rebuilt in order, the changed one in its place: a comment has no way to
// change a picture where it stands. A replacement keeps the original's type
// and description; an added picture takes its role's.
[[nodiscard]] core::Result<void>
apply_xiph_picture_change(const bool opus, const ArtworkWritePlanSource& source_plan,
                          const std::string& prepared_raw_path,
                          const std::vector<unsigned char>& replacement) {
    std::unique_ptr<TagLib::File> file;
    TagLib::Ogg::XiphComment* comment = nullptr;
    if (opus) {
        auto opened = std::make_unique<TagLib::Ogg::Opus::File>(prepared_raw_path.c_str(), false);
        comment = opened->isValid() ? opened->tag() : nullptr;
        file = std::move(opened);
    } else {
        auto opened = std::make_unique<TagLib::Vorbis::File>(prepared_raw_path.c_str(), false);
        comment = opened->isValid() ? opened->tag() : nullptr;
        file = std::move(opened);
    }
    if (comment == nullptr) {
        return std::unexpected(writer_error(core::ErrorCode::backend,
                                            "TagLib rejected the prepared Ogg copy",
                                            source_plan.raw_media_path, prepared_raw_path));
    }
    const auto& change = source_plan.change;
    const auto pictures = comment->pictureList();
    // Copies first: taking a picture out of the comment deletes it.
    std::vector<std::unique_ptr<TagLib::FLAC::Picture>> current;
    for (const auto* picture : pictures) {
        auto copy = std::make_unique<TagLib::FLAC::Picture>();
        if (picture == nullptr || !copy->parse(picture->render())) {
            return std::unexpected(writer_error(core::ErrorCode::backend,
                                                "an Ogg picture could not be copied",
                                                source_plan.raw_media_path, prepared_raw_path));
        }
        current.push_back(std::move(copy));
    }
    const auto ordinal = change.target_ordinal;
    const auto add = change.kind == ArtworkWritePlanIntentKind::add;
    if (change.kind == ArtworkWritePlanIntentKind::batch ||
        (add ? ordinal != current.size() : ordinal >= current.size())) {
        return std::unexpected(writer_error(core::ErrorCode::conflict,
                                            add ? "the Ogg picture count differs from the plan"
                                                : "the targeted Ogg picture no longer exists",
                                            source_plan.raw_media_path, prepared_raw_path));
    }
    std::unique_ptr<TagLib::FLAC::Picture> made;
    if (change.kind != ArtworkWritePlanIntentKind::remove) {
        const auto& image = *change.replacement;
        made = std::make_unique<TagLib::FLAC::Picture>();
        made->setType(add ? artwork_detail::canonical_picture_type(change.added_role)
                          : current[ordinal]->type());
        made->setMimeType(TagLib::String{image.mime_type, TagLib::String::UTF8});
        made->setDescription(add ? TagLib::String{change.added_description, TagLib::String::UTF8}
                                 : current[ordinal]->description());
        made->setWidth(static_cast<int>(image.width.value_or(0U)));
        made->setHeight(static_cast<int>(image.height.value_or(0U)));
        made->setColorDepth(0);
        made->setNumColors(0);
        made->setData(to_byte_vector(replacement));
    }
    if (add) {
        current.push_back(std::move(made));
    } else if (made) {
        current[ordinal] = std::move(made);
    } else {
        current.erase(current.begin() + static_cast<std::ptrdiff_t>(ordinal));
    }
    comment->removeAllPictures();
    for (auto& picture : current) {
        comment->addPicture(picture.release());
    }
    if (!file->save()) {
        return std::unexpected(writer_error(core::ErrorCode::backend,
                                            "TagLib could not save the prepared Ogg copy",
                                            source_plan.raw_media_path, prepared_raw_path));
    }
    return {};
}

// Text changes through TagLib's properties, for a container whose file
// type opens the prepared copy.
template <typename File, typename... Arguments>
[[nodiscard]] auto text_writer(const std::string_view label, const bool paired_totals,
                               Arguments... arguments) {
    return [label, paired_totals, arguments...](const MetadataWritePlanSource& plan,
                                                const std::string& prepared,
                                                const core::CancellationToken& cancellation) {
        File file{prepared.c_str(), false, arguments...};
        return text_writer_detail::apply_text_changes_to_properties(label, plan, file, prepared,
                                                                    cancellation, paired_totals);
    };
}

[[nodiscard]] const ContainerArtworkFormat& mp3_format() {
    static const ContainerArtworkFormat format{
        .label = "MP3",
        .artwork_adapter = "taglib-id3v2-apic-v1",
        .text_adapter = "taglib-mpeg-v1",
        .artwork_object_prefix = "APIC",
        .supports_descriptions = true,
        .apply = apply_apic_change,
        .verify_preservation = preservation_detail::verify_mp3_binary_preservation,
        .apply_text = text_writer<TagLib::MPEG::File>("MP3", false),
        .paired_totals = false,
    };
    return format;
}

[[nodiscard]] const ContainerArtworkFormat& mp4_format() {
    static const ContainerArtworkFormat format{
        .label = "MP4",
        .artwork_adapter = "taglib-mp4-covr-v1",
        .text_adapter = "taglib-mp4-v1",
        .artwork_object_prefix = "covr",
        .supports_descriptions = false,
        .apply = apply_covr_change,
        .verify_preservation = preservation_detail::verify_mp4_box_preservation,
        .apply_text = text_writer<TagLib::MP4::File>("MP4", false, TagLib::AudioProperties::Average,
                                                     mp4_detail::FreeFormFactory::instance()),
        .paired_totals = false,
    };
    return format;
}

// ADR-0337: covers in Opus and Vorbis comments.
[[nodiscard]] const ContainerArtworkFormat& ogg_format(const bool opus) {
    const auto make = [](const bool is_opus) {
        return ContainerArtworkFormat{
            .label = is_opus ? "Opus" : "Vorbis",
            .artwork_adapter = is_opus ? "taglib-opus-picture-v1" : "taglib-vorbis-picture-v1",
            .text_adapter = is_opus ? "taglib-opus-v1" : "taglib-vorbis-v1",
            // Pictures are no text field of the comment; nothing to set aside.
            .artwork_object_prefix = "METADATA_BLOCK_PICTURE",
            .supports_descriptions = true,
            .apply =
                [is_opus](const ArtworkWritePlanSource& source_plan, const std::string& prepared,
                          const std::vector<unsigned char>& replacement) {
                    return apply_xiph_picture_change(is_opus, source_plan, prepared, replacement);
                },
            .verify_preservation =
                [is_opus](const std::string& source, const std::string& prepared,
                          const core::CancellationToken& cancellation) {
                    return preservation_detail::verify_ogg_packet_preservation(
                        source, prepared, is_opus, cancellation);
                },
            .apply_text =
                is_opus ? std::function<core::Result<void>(
                              const MetadataWritePlanSource&, const std::string&,
                              const core::CancellationToken&)>{text_writer<TagLib::Ogg::Opus::
                                                                               File>("Ogg", true)}
                        : text_writer<TagLib::Vorbis::File>("Ogg", true),
            .paired_totals = true,
        };
    };
    static const ContainerArtworkFormat opus_format = make(true);
    static const ContainerArtworkFormat vorbis_format = make(false);
    return opus ? opus_format : vorbis_format;
}

// The container whose artwork or text adapter this is; none for FLAC, which
// is written by its own rewriter.
[[nodiscard]] const ContainerArtworkFormat* container_format(const std::string_view adapter) {
    for (const auto* format :
         {&mp3_format(), &mp4_format(), &ogg_format(true), &ogg_format(false)}) {
        if (format->artwork_adapter == adapter || format->text_adapter == adapter) {
            return format;
        }
    }
    return nullptr;
}

// Predicts one resulting item's identity attributes so the reread
// inventory can be compared exactly.
struct ExpectedItem {
    core::ContentFingerprint fingerprint;
    std::string native_type;
    std::string mime_type;
    std::string description;
    ArtworkRole role{ArtworkRole::other};
};

[[nodiscard]] ExpectedItem expected_from(const ArtworkInventoryItem& item) {
    return ExpectedItem{
        .fingerprint = item.content_fingerprint,
        .native_type = item.native_type,
        .mime_type = item.mime_type,
        .description = item.description,
        .role = item.role,
    };
}

[[nodiscard]] core::Result<void> verify_inventory_result(const ContainerArtworkFormat& format,
                                                         const LocalArtworkInventory& before,
                                                         const LocalArtworkInventory& after,
                                                         const ArtworkWritePlanSource& source_plan,
                                                         const std::string& prepared_raw_path) {
    const auto& change = source_plan.change;
    std::vector<ExpectedItem> expected;
    expected.reserve(before.items.size() + 1U);
    for (const auto& item : before.items) {
        expected.push_back(expected_from(item));
    }
    const auto covr = format.artwork_adapter == "taglib-mp4-covr-v1";
    switch (change.kind) {
    case ArtworkWritePlanIntentKind::batch:
        return std::unexpected(writer_error(core::ErrorCode::invalid_argument,
                                            "batch is not an individual picture change",
                                            source_plan.raw_media_path, prepared_raw_path));
    case ArtworkWritePlanIntentKind::remove:
        expected.erase(expected.begin() + static_cast<std::ptrdiff_t>(change.target_ordinal));
        break;
    case ArtworkWritePlanIntentKind::replace: {
        auto& target = expected[change.target_ordinal];
        target.fingerprint = change.replacement->content_fingerprint;
        target.mime_type = change.replacement->mime_type;
        break;
    }
    case ArtworkWritePlanIntentKind::add: {
        const auto type = artwork_detail::canonical_picture_type(change.added_role);
        expected.push_back(ExpectedItem{
            .fingerprint = change.replacement->content_fingerprint,
            .native_type =
                covr ? std::string{} : TagLib::FLAC::Picture::typeToString(type).to8Bit(true),
            .mime_type = change.replacement->mime_type,
            .description = covr ? std::string{} : change.added_description,
            .role = covr ? ArtworkRole::front : change.added_role,
        });
        break;
    }
    }
    const auto mismatch = [&] {
        return std::unexpected(writer_error(
            core::ErrorCode::conflict, "prepared artwork inventory differs from the planned result",
            source_plan.raw_media_path, prepared_raw_path));
    };
    if (after.items.size() != expected.size()) {
        return mismatch();
    }
    for (std::size_t index = 0U; index < expected.size(); ++index) {
        const auto& item = after.items[index];
        const auto& want = expected[index];
        if (item.provenance != ArtworkProvenance::embedded || item.source_ordinal != index ||
            item.content_fingerprint != want.fingerprint || item.native_type != want.native_type ||
            item.mime_type != want.mime_type || item.description != want.description ||
            item.role != want.role) {
            return mismatch();
        }
    }
    return {};
}

[[nodiscard]] core::Result<PreparedArtworkWrite> prepare_container_artwork_write_copy(
    const ContainerArtworkFormat& format, const ArtworkWritePlanSource& source_plan,
    const std::string& prepared_raw_path, const core::CancellationToken& cancellation) {
    if (cancellation.is_cancellation_requested()) {
        return std::unexpected(cancelled(format, source_plan.raw_media_path, prepared_raw_path));
    }
    const auto replace = source_plan.change.kind == ArtworkWritePlanIntentKind::replace;
    const auto add = source_plan.change.kind == ArtworkWritePlanIntentKind::add;
    const auto uses_replacement = replace || add;
    const auto replacement_representable =
        !uses_replacement ||
        (source_plan.change.replacement && source_plan.change.replacement->width &&
         source_plan.change.replacement->height && source_plan.change.replacement->byte_size > 0U &&
         (source_plan.change.replacement->mime_type == "image/png" ||
          source_plan.change.replacement->mime_type == "image/jpeg"));
    if (source_plan.raw_media_path.empty() || prepared_raw_path.empty() ||
        source_plan.raw_media_path.find('\0') != std::string::npos ||
        prepared_raw_path.find('\0') != std::string::npos ||
        source_plan.raw_media_path == prepared_raw_path || !source_plan.ready() ||
        source_plan.adapter_name != format.artwork_adapter ||
        !source_plan.expected_media_revision || !source_plan.observed_media_revision ||
        *source_plan.expected_media_revision != *source_plan.observed_media_revision ||
        source_plan.occurrence_indexes.empty() ||
        uses_replacement != source_plan.change.replacement.has_value() ||
        !replacement_representable || (add && source_plan.change.original) ||
        (!add &&
         (!source_plan.change.original ||
          source_plan.change.original->provenance != ArtworkProvenance::embedded ||
          source_plan.change.original->raw_source_path != source_plan.raw_media_path ||
          !core::same_file(source_plan.raw_media_path, *source_plan.observed_media_revision,
                           source_plan.change.original->source_revision) ||
          source_plan.change.original->source_ordinal != source_plan.change.target_ordinal ||
          source_plan.change.original->content_fingerprint !=
              source_plan.change.expected_target_fingerprint))) {
        return std::unexpected(
            writer_error(core::ErrorCode::invalid_argument,
                         "prepared artwork write requires one complete ready source plan",
                         source_plan.raw_media_path, prepared_raw_path));
    }
    if (add && !format.supports_descriptions && !source_plan.change.added_description.empty()) {
        return std::unexpected(writer_error(core::ErrorCode::unsupported,
                                            "MP4 covr entries cannot carry a picture description",
                                            source_plan.raw_media_path, prepared_raw_path));
    }

    auto observed = core::observe_local_source_revision(source_plan.raw_media_path);
    if (!observed) {
        return std::unexpected(std::move(observed.error()));
    }
    if (*observed != *source_plan.observed_media_revision) {
        return std::unexpected(writer_error(
            core::ErrorCode::conflict, "the source changed after the artwork plan was previewed",
            source_plan.raw_media_path, prepared_raw_path));
    }
    auto before_inventory = read_embedded_inventory(source_plan.raw_media_path, cancellation);
    if (!before_inventory) {
        return std::unexpected(std::move(before_inventory.error()));
    }
    if (!core::same_file(source_plan.raw_media_path, *source_plan.observed_media_revision,
                         before_inventory->media_revision) ||
        before_inventory->embedded_adapter_name != format.artwork_adapter) {
        return std::unexpected(
            writer_error(core::ErrorCode::conflict,
                         "the artwork source no longer matches the previewed adapter and revision",
                         source_plan.raw_media_path, prepared_raw_path));
    }
    if (add) {
        if (source_plan.change.target_ordinal != before_inventory->items.size()) {
            return std::unexpected(
                writer_error(core::ErrorCode::conflict,
                             "the fresh artwork count differs from the previewed insertion ordinal",
                             source_plan.raw_media_path, prepared_raw_path));
        }
        const auto duplicate = std::ranges::find_if(before_inventory->items, [&](const auto& item) {
            return item.content_fingerprint == source_plan.change.replacement->content_fingerprint;
        });
        if (duplicate != before_inventory->items.end()) {
            return std::unexpected(writer_error(core::ErrorCode::conflict,
                                                "the source already contains the added image",
                                                source_plan.raw_media_path, prepared_raw_path));
        }
    } else {
        const auto target = std::ranges::find_if(before_inventory->items, [&](const auto& item) {
            return item.provenance == ArtworkProvenance::embedded &&
                   item.source_ordinal == source_plan.change.target_ordinal;
        });
        if (target == before_inventory->items.end() || *target != *source_plan.change.original ||
            target->content_fingerprint != source_plan.change.expected_target_fingerprint) {
            return std::unexpected(
                writer_error(core::ErrorCode::conflict,
                             "the fresh artwork differs from the previewed target picture",
                             source_plan.raw_media_path, prepared_raw_path));
        }
    }

    std::vector<unsigned char> replacement_bytes;
    if (uses_replacement) {
        auto loaded =
            read_artwork_image_bytes(*source_plan.change.replacement,
                                     source_plan.change.replacement->byte_size, cancellation);
        if (!loaded) {
            auto error = std::move(loaded.error());
            error.context.push_back(
                {.key = "target", .value = core::escape_raw_path(source_plan.raw_media_path)});
            return std::unexpected(std::move(error));
        }
        replacement_bytes = std::move(*loaded);
    }
    auto before_document = read_local_metadata(source_plan.raw_media_path, cancellation);
    if (!before_document) {
        return std::unexpected(std::move(before_document.error()));
    }
    if (!core::same_file(source_plan.raw_media_path, *source_plan.observed_media_revision,
                         before_document->source_revision) ||
        before_document->adapter_name != format.text_adapter) {
        return std::unexpected(writer_error(
            core::ErrorCode::conflict, "the metadata changed after the artwork plan was previewed",
            source_plan.raw_media_path, prepared_raw_path));
    }

    PreparedPathGuard prepared_guard{prepared_raw_path};
    auto source_mode = text_writer_detail::copy_source_exclusively(
        format.label, source_plan.raw_media_path, *source_plan.observed_media_revision,
        prepared_raw_path, cancellation, prepared_guard);
    if (!source_mode) {
        return std::unexpected(std::move(source_mode.error()));
    }
    auto applied = format.apply(source_plan, prepared_raw_path, replacement_bytes);
    if (!applied) {
        return std::unexpected(std::move(applied.error()));
    }
    if (core::keep_mode(prepared_raw_path.c_str(), *source_mode) != 0) {
        return std::unexpected(system_error("restoring prepared artwork permissions failed", errno,
                                            source_plan.raw_media_path, prepared_raw_path));
    }
    if (cancellation.is_cancellation_requested()) {
        return std::unexpected(cancelled(format, source_plan.raw_media_path, prepared_raw_path));
    }

    if (auto settled =
            text_writer_detail::settle_prepared(prepared_raw_path, source_plan.raw_media_path);
        !settled) {
        return std::unexpected(std::move(settled.error()));
    }
    auto after_document = read_local_metadata(prepared_raw_path, cancellation);
    if (!after_document) {
        return std::unexpected(std::move(after_document.error()));
    }
    const auto non_artwork_objects = [&format](const MetadataDocument& document) {
        auto objects = document.unsupported_native_objects;
        std::erase_if(objects, [&format](const NativeObjectIdentity& object) {
            return std::string_view{object.identity}.starts_with(format.artwork_object_prefix);
        });
        return objects;
    };
    if (after_document->document.fields != before_document->document.fields ||
        non_artwork_objects(after_document->document) !=
            non_artwork_objects(before_document->document)) {
        return std::unexpected(
            writer_error(core::ErrorCode::conflict,
                         "the prepared artwork write changed the text metadata document",
                         source_plan.raw_media_path, prepared_raw_path));
    }
    auto after_inventory = read_embedded_inventory(prepared_raw_path, cancellation);
    if (!after_inventory) {
        return std::unexpected(std::move(after_inventory.error()));
    }
    if (after_inventory->media_revision != after_document->source_revision ||
        after_inventory->embedded_adapter_name != format.artwork_adapter) {
        return std::unexpected(writer_error(core::ErrorCode::conflict,
                                            "the prepared artwork and metadata rereads disagree",
                                            source_plan.raw_media_path, prepared_raw_path));
    }
    auto inventory_verified = verify_inventory_result(format, *before_inventory, *after_inventory,
                                                      source_plan, prepared_raw_path);
    if (!inventory_verified) {
        return std::unexpected(std::move(inventory_verified.error()));
    }
    auto preserved =
        format.verify_preservation(source_plan.raw_media_path, prepared_raw_path, cancellation);
    if (!preserved) {
        return std::unexpected(std::move(preserved.error()));
    }
    auto source_after = core::observe_local_source_revision(source_plan.raw_media_path);
    if (!source_after) {
        return std::unexpected(std::move(source_after.error()));
    }
    if (*source_after != *source_plan.observed_media_revision) {
        return std::unexpected(
            writer_error(core::ErrorCode::conflict,
                         "the source changed while the prepared artwork copy was being verified",
                         source_plan.raw_media_path, prepared_raw_path));
    }
    if (cancellation.is_cancellation_requested()) {
        return std::unexpected(cancelled(format, source_plan.raw_media_path, prepared_raw_path));
    }

    prepared_guard.release();
    return PreparedArtworkWrite{
        .source_raw_path = source_plan.raw_media_path,
        .prepared_raw_path = prepared_raw_path,
        .source_revision = *source_after,
        .prepared_revision = after_document->source_revision,
        .document = std::move(after_document->document),
        .inventory = std::move(*after_inventory),
        .kind = source_plan.change.kind,
        .target_ordinal = source_plan.change.target_ordinal,
    };
}

} // namespace

core::Result<PreparedFlacMetadataWrite>
prepare_composed_metadata_write_copy(const MetadataWritePlanSource& plan,
                                     const std::string& prepared,
                                     const core::CancellationToken& cancellation) {
    const auto fail = [&](const std::string& message) {
        return std::unexpected(
            writer_error(core::ErrorCode::conflict, message, plan.raw_path, prepared));
    };
    if (!plan.artwork || !plan.ready() || !plan.observed_revision ||
        plan.expected_revision != plan.observed_revision ||
        plan.raw_path != plan.artwork->raw_media_path ||
        plan.observed_revision != plan.artwork->observed_media_revision || plan.raw_path.empty() ||
        prepared.empty() || plan.raw_path == prepared || prepared.find('\0') != std::string::npos) {
        return fail("invalid composed metadata plan");
    }
    auto before = read_local_metadata(plan.raw_path, cancellation);
    auto inventory = read_embedded_inventory(plan.raw_path, cancellation);
    if (!before || !inventory) {
        return std::unexpected(!before ? before.error() : inventory.error());
    }
    if (!core::same_file(plan.raw_path, *plan.observed_revision, before->source_revision) ||
        !core::same_file(plan.raw_path, *plan.observed_revision, inventory->media_revision) ||
        before->adapter_name != plan.adapter_name ||
        inventory->embedded_adapter_name != plan.artwork->adapter_name) {
        return fail("source changed after review");
    }
    auto expected = project_artwork_inventory(*inventory, *plan.artwork);
    if (!expected) {
        return std::unexpected(expected.error());
    }
    auto originals =
        text_writer_detail::verify_plan_originals("metadata", before->document, plan, prepared);
    if (!originals) {
        return std::unexpected(originals.error());
    }
    text_writer_detail::PreparedPathGuard guard{prepared};
    const auto flac = plan.adapter_name == "taglib-flac-v1";
    const auto* format = flac ? nullptr : container_format(plan.adapter_name);
    if (flac) {
        const text_writer_detail::Descriptor descriptor{
            ::open(prepared.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600)};
        if (!descriptor.valid()) {
            return fail("could not exclusively create prepared file");
        }
        guard.take_ownership();
        auto rewritten = preservation_detail::rewrite_flac_composed(plan, prepared, cancellation);
        if (!rewritten) {
            return std::unexpected(rewritten.error());
        }
        struct stat status{};
        if (::stat(plan.raw_path.c_str(), &status) != 0 ||
            core::keep_mode(prepared.c_str(), status.st_mode & 07777) != 0) {
            return fail("could not preserve file permissions");
        }
    } else {
        if (format == nullptr) {
            return fail("no artwork writer for this container");
        }
        auto copied = text_writer_detail::copy_source_exclusively(
            "metadata", plan.raw_path, *plan.observed_revision, prepared, cancellation, guard);
        if (!copied) {
            return std::unexpected(copied.error());
        }
        for (const auto& change : artwork_changes(*plan.artwork)) {
            if (cancellation.is_cancellation_requested()) {
                return std::unexpected(core::Error{.code = core::ErrorCode::cancelled,
                                                   .message = "artwork preparation cancelled",
                                                   .context = {}});
            }
            auto single = *plan.artwork;
            single.additional_changes.clear();
            single.change = change;
            std::vector<unsigned char> bytes;
            if (change.replacement) {
                auto loaded = read_artwork_image_bytes(*change.replacement,
                                                       change.replacement->byte_size, cancellation);
                if (!loaded) {
                    return std::unexpected(loaded.error());
                }
                bytes = std::move(*loaded);
            }
            if (auto applied = format->apply(single, prepared, bytes); !applied) {
                return std::unexpected(applied.error());
            }
        }
        if (!plan.changes.empty()) {
            if (auto applied = format->apply_text(plan, prepared, cancellation); !applied) {
                return std::unexpected(applied.error());
            }
        }
        if (core::keep_mode(prepared.c_str(), *copied) != 0) {
            return fail("could not preserve file permissions");
        }
    }
    if (auto settled = text_writer_detail::settle_prepared(prepared, plan.raw_path); !settled) {
        return std::unexpected(std::move(settled.error()));
    }
    auto after = read_local_metadata(prepared, cancellation);
    auto resulting = read_embedded_inventory(prepared, cancellation);
    if (!after || !resulting) {
        return std::unexpected(!after ? after.error() : resulting.error());
    }
    auto original_document = before->document;
    auto resulting_document = after->document;
    const auto remove_picture_identities = [](auto& document) {
        std::erase_if(document.unsupported_native_objects, [](const auto& object) {
            return object.identity == "covr" || object.identity == "APIC" ||
                   object.identity.starts_with("APIC:");
        });
    };
    remove_picture_identities(original_document);
    remove_picture_identities(resulting_document);
    auto text =
        text_writer_detail::verify_text_result("metadata", original_document, resulting_document,
                                               plan, prepared, flac || format->paired_totals);
    if (!text) {
        return std::unexpected(text.error());
    }
    const auto expected_hash = fingerprint_embedded_artwork_inventory(*expected);
    const auto actual_hash = fingerprint_embedded_artwork_inventory(resulting->items);
    if (!expected_hash || !actual_hash || *expected_hash != *actual_hash ||
        resulting->media_revision != after->source_revision) {
        return fail("prepared covers differ from review");
    }
    auto preservation = flac ? preservation_detail::verify_flac_composed(
                                   plan.raw_path, prepared, !plan.changes.empty(), cancellation)
                             : format->verify_preservation(plan.raw_path, prepared, cancellation);
    if (!preservation) {
        return std::unexpected(preservation.error());
    }
    auto observed = core::observe_local_source_revision(plan.raw_path);
    if (!observed || *observed != *plan.observed_revision) {
        return fail("source changed while preparing metadata");
    }
    if (cancellation.is_cancellation_requested()) {
        return std::unexpected(core::Error{.code = core::ErrorCode::cancelled,
                                           .message = "metadata preparation cancelled",
                                           .context = {}});
    }
    guard.release();
    return PreparedFlacMetadataWrite{.source_raw_path = plan.raw_path,
                                     .prepared_raw_path = prepared,
                                     .source_revision = *observed,
                                     .prepared_revision = after->source_revision,
                                     .document = std::move(after->document),
                                     .field_change_count = plan.changes.size()};
}

core::Result<PreparedArtworkWrite>
prepare_mp3_artwork_write_copy(const ArtworkWritePlanSource& source_plan,
                               const std::string& prepared_raw_path,
                               const core::CancellationToken& cancellation) {
    return prepare_container_artwork_write_copy(mp3_format(), source_plan, prepared_raw_path,
                                                cancellation);
}

core::Result<PreparedArtworkWrite>
prepare_mp4_artwork_write_copy(const ArtworkWritePlanSource& source_plan,
                               const std::string& prepared_raw_path,
                               const core::CancellationToken& cancellation) {
    return prepare_container_artwork_write_copy(mp4_format(), source_plan, prepared_raw_path,
                                                cancellation);
}

core::Result<PreparedArtworkWrite>
prepare_qualified_artwork_write_copy(const ArtworkWritePlanSource& source_plan,
                                     const std::string& prepared_raw_path,
                                     const core::CancellationToken& cancellation) {
    if (source_plan.folder_image || !source_plan.embed)
        return std::unexpected(core::Error{
            .code = core::ErrorCode::invalid_argument,
            .message = "Folder image policy must execute through the journaled artwork committer",
            .context = {}});
    if (!source_plan.additional_changes.empty()) {
        auto merged = merge_artwork_write_plan(
            {}, ArtworkWritePlan{.sources = {source_plan},
                                 .logical_intent_count = source_plan.occurrence_indexes.size()});
        if (!merged) {
            return std::unexpected(merged.error());
        }
        auto prepared = prepare_composed_metadata_write_copy(merged->sources.front(),
                                                             prepared_raw_path, cancellation);
        if (!prepared) {
            return std::unexpected(prepared.error());
        }
        PreparedPathGuard guard{prepared_raw_path};
        guard.take_ownership();
        auto inventory = read_embedded_inventory(prepared_raw_path, cancellation);
        if (!inventory) {
            return std::unexpected(inventory.error());
        }
        guard.release();
        return PreparedArtworkWrite{.source_raw_path = source_plan.raw_media_path,
                                    .prepared_raw_path = prepared_raw_path,
                                    .source_revision = prepared->source_revision,
                                    .prepared_revision = prepared->prepared_revision,
                                    .document = std::move(prepared->document),
                                    .inventory = std::move(*inventory),
                                    .kind = source_plan.change.kind,
                                    .target_ordinal = source_plan.change.target_ordinal};
    }
    if (const auto* format = container_format(source_plan.adapter_name)) {
        return prepare_container_artwork_write_copy(*format, source_plan, prepared_raw_path,
                                                    cancellation);
    }
    return prepare_flac_artwork_write_copy(source_plan, prepared_raw_path, cancellation);
}

} // namespace trackknife::metadata

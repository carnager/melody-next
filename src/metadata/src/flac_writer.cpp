// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/metadata/flac_writer.hpp"

#include "container_preservation_detail.hpp"
#include "text_writer_detail.hpp"

#include "trackknife/metadata/flac_mapping.hpp"
#include "trackknife/metadata/local_reader.hpp"

#include <flacfile.h>
#include <flacpicture.h>
#include <tpropertymap.h>
#include <tstring.h>
#include <tstringlist.h>
#include <xiphcomment.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <ranges>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace trackknife::metadata {
namespace {

constexpr std::size_t comparison_buffer_size = 64U * 1024U;
constexpr std::size_t maximum_metadata_blocks = 4'096U;
constexpr std::uint8_t padding_block_type = 1U;
constexpr std::uint8_t vorbis_comment_block_type = 4U;
constexpr std::uint8_t picture_block_type = 6U;

using text_writer_detail::Descriptor;
using text_writer_detail::mapping_native_name;
using text_writer_detail::PreparedPathGuard;
using text_writer_detail::system_error;
using text_writer_detail::writer_error;

[[nodiscard]] core::Error cancelled(const std::string& source_raw_path,
                                    const std::string& prepared_raw_path) {
    return text_writer_detail::cancelled("FLAC", source_raw_path, prepared_raw_path);
}

struct FlacBlock {
    std::uint8_t type{0U};
    std::uint64_t data_offset{0U};
    std::uint64_t length{0U};
};

struct FlacLayout {
    std::vector<FlacBlock> blocks;
    std::vector<FlacBlock> preserved_blocks;
    std::uint64_t audio_offset{0U};
    std::uint64_t file_size{0U};
};

[[nodiscard]] core::Result<mode_t> copy_source_exclusively(
    const std::string& source_raw_path, const core::LocalSourceRevision& expected_revision,
    const std::string& prepared_raw_path, const core::CancellationToken& cancellation,
    PreparedPathGuard& prepared_guard) {
    return text_writer_detail::copy_source_exclusively("FLAC", source_raw_path, expected_revision,
                                                       prepared_raw_path, cancellation,
                                                       prepared_guard);
}

[[nodiscard]] core::Result<FlacLayout> parse_flac_layout(const std::string& raw_path,
                                                         const std::string& source_raw_path,
                                                         const std::string& prepared_raw_path,
                                                         const std::uint8_t mutable_block_type) {
    std::ifstream input{std::filesystem::path{raw_path}, std::ios::binary};
    if (!input) {
        return std::unexpected(writer_error(core::ErrorCode::io,
                                            "opening FLAC layout for verification failed",
                                            source_raw_path, prepared_raw_path));
    }
    input.seekg(0, std::ios::end);
    const auto end = input.tellg();
    if (end < 0) {
        return std::unexpected(writer_error(core::ErrorCode::io, "reading FLAC layout size failed",
                                            source_raw_path, prepared_raw_path));
    }
    const auto file_size = static_cast<std::uint64_t>(end);
    input.seekg(0, std::ios::beg);
    std::array<unsigned char, 4> marker{};
    input.read(reinterpret_cast<char*>(marker.data()), static_cast<std::streamsize>(marker.size()));
    if (!input || marker != std::array<unsigned char, 4>{'f', 'L', 'a', 'C'}) {
        return std::unexpected(writer_error(core::ErrorCode::unsupported,
                                            "metadata writer requires native FLAC", source_raw_path,
                                            prepared_raw_path));
    }

    FlacLayout layout{
        .blocks = {}, .preserved_blocks = {}, .audio_offset = 0U, .file_size = file_size};
    std::uint64_t offset = marker.size();
    std::size_t block_count = 0U;
    bool last = false;
    while (!last) {
        if (block_count == maximum_metadata_blocks) {
            return std::unexpected(writer_error(core::ErrorCode::limit_exceeded,
                                                "FLAC metadata block count exceeds the write limit",
                                                source_raw_path, prepared_raw_path));
        }
        ++block_count;
        std::array<unsigned char, 4> header{};
        input.read(reinterpret_cast<char*>(header.data()),
                   static_cast<std::streamsize>(header.size()));
        if (!input) {
            return std::unexpected(writer_error(core::ErrorCode::backend,
                                                "FLAC metadata block header is truncated",
                                                source_raw_path, prepared_raw_path));
        }
        last = (header[0] & 0x80U) != 0U;
        const auto type = static_cast<std::uint8_t>(header[0] & 0x7FU);
        const auto length = (static_cast<std::uint64_t>(header[1]) << 16U) |
                            (static_cast<std::uint64_t>(header[2]) << 8U) |
                            static_cast<std::uint64_t>(header[3]);
        offset += header.size();
        if (offset > file_size || length > file_size - offset) {
            return std::unexpected(writer_error(core::ErrorCode::backend,
                                                "FLAC metadata block payload is truncated",
                                                source_raw_path, prepared_raw_path));
        }
        if (type != padding_block_type && type != mutable_block_type) {
            layout.preserved_blocks.push_back(
                FlacBlock{.type = type, .data_offset = offset, .length = length});
        }
        layout.blocks.push_back(FlacBlock{.type = type, .data_offset = offset, .length = length});
        offset += length;
        input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        if (!input) {
            return std::unexpected(writer_error(core::ErrorCode::backend,
                                                "seeking across FLAC metadata failed",
                                                source_raw_path, prepared_raw_path));
        }
    }
    layout.audio_offset = offset;
    return layout;
}

[[nodiscard]] core::Result<void>
compare_region(std::ifstream& source, const std::uint64_t source_offset, std::ifstream& prepared,
               const std::uint64_t prepared_offset, const std::uint64_t length,
               const std::string_view identity, const std::string& source_raw_path,
               const std::string& prepared_raw_path, const core::CancellationToken& cancellation) {
    source.seekg(static_cast<std::streamoff>(source_offset), std::ios::beg);
    prepared.seekg(static_cast<std::streamoff>(prepared_offset), std::ios::beg);
    if (!source || !prepared) {
        return std::unexpected(writer_error(core::ErrorCode::io,
                                            "seeking FLAC preservation regions failed",
                                            source_raw_path, prepared_raw_path));
    }
    std::array<char, comparison_buffer_size> source_buffer{};
    std::array<char, comparison_buffer_size> prepared_buffer{};
    std::uint64_t compared = 0U;
    while (compared < length) {
        if (cancellation.is_cancellation_requested()) {
            return std::unexpected(cancelled(source_raw_path, prepared_raw_path));
        }
        const auto amount = static_cast<std::size_t>(
            std::min<std::uint64_t>(source_buffer.size(), length - compared));
        source.read(source_buffer.data(), static_cast<std::streamsize>(amount));
        prepared.read(prepared_buffer.data(), static_cast<std::streamsize>(amount));
        if (source.gcount() != static_cast<std::streamsize>(amount) ||
            prepared.gcount() != static_cast<std::streamsize>(amount)) {
            return std::unexpected(writer_error(core::ErrorCode::io,
                                                "reading FLAC preservation regions failed",
                                                source_raw_path, prepared_raw_path));
        }
        if (!std::equal(source_buffer.begin(), source_buffer.begin() + amount,
                        prepared_buffer.begin())) {
            return std::unexpected(
                writer_error(core::ErrorCode::conflict,
                             "prepared FLAC changed preserved " + std::string{identity},
                             source_raw_path, prepared_raw_path));
        }
        compared += amount;
    }
    return {};
}

[[nodiscard]] core::Result<void> verify_flac_binary_preservation(
    const std::string& source_raw_path, const std::string& prepared_raw_path,
    const std::uint8_t mutable_block_type, const core::CancellationToken& cancellation,
    const bool text_changes = false) {
    auto source_layout =
        parse_flac_layout(source_raw_path, source_raw_path, prepared_raw_path, mutable_block_type);
    if (!source_layout) {
        return std::unexpected(std::move(source_layout.error()));
    }
    auto prepared_layout = parse_flac_layout(prepared_raw_path, source_raw_path, prepared_raw_path,
                                             mutable_block_type);
    if (!prepared_layout) {
        return std::unexpected(std::move(prepared_layout.error()));
    }
    if (text_changes) {
        const auto comment = [](const auto& block) {
            return block.type == vorbis_comment_block_type;
        };
        std::erase_if(source_layout->preserved_blocks, comment);
        std::erase_if(prepared_layout->preserved_blocks, comment);
    }
    if (source_layout->preserved_blocks.size() != prepared_layout->preserved_blocks.size()) {
        return std::unexpected(writer_error(core::ErrorCode::conflict,
                                            "prepared FLAC changed preserved metadata blocks",
                                            source_raw_path, prepared_raw_path));
    }

    std::ifstream source{std::filesystem::path{source_raw_path}, std::ios::binary};
    std::ifstream prepared{std::filesystem::path{prepared_raw_path}, std::ios::binary};
    if (!source || !prepared) {
        return std::unexpected(writer_error(core::ErrorCode::io,
                                            "opening FLAC preservation streams failed",
                                            source_raw_path, prepared_raw_path));
    }
    for (std::size_t index = 0U; index < source_layout->preserved_blocks.size(); ++index) {
        const auto& source_block = source_layout->preserved_blocks[index];
        const auto& prepared_block = prepared_layout->preserved_blocks[index];
        if (source_block.type != prepared_block.type ||
            source_block.length != prepared_block.length) {
            return std::unexpected(writer_error(core::ErrorCode::conflict,
                                                "prepared FLAC changed a preserved metadata block",
                                                source_raw_path, prepared_raw_path));
        }
        auto compared =
            compare_region(source, source_block.data_offset, prepared, prepared_block.data_offset,
                           source_block.length, "metadata block", source_raw_path,
                           prepared_raw_path, cancellation);
        if (!compared) {
            return compared;
        }
    }
    const auto source_audio_size = source_layout->file_size - source_layout->audio_offset;
    const auto prepared_audio_size = prepared_layout->file_size - prepared_layout->audio_offset;
    if (source_audio_size != prepared_audio_size) {
        return std::unexpected(writer_error(core::ErrorCode::conflict,
                                            "prepared FLAC changed compressed audio size",
                                            source_raw_path, prepared_raw_path));
    }
    return compare_region(source, source_layout->audio_offset, prepared,
                          prepared_layout->audio_offset, source_audio_size, "compressed audio",
                          source_raw_path, prepared_raw_path, cancellation);
}

[[nodiscard]] core::Result<void> verify_text_result(const MetadataDocument& before,
                                                    const MetadataDocument& after,
                                                    const MetadataWritePlanSource& source_plan,
                                                    const std::string& prepared_raw_path) {
    return text_writer_detail::verify_text_result("FLAC", before, after, source_plan,
                                                  prepared_raw_path);
}

[[nodiscard]] core::Result<void> verify_plan_originals(const MetadataDocument& before,
                                                       const MetadataWritePlanSource& source_plan,
                                                       const std::string& prepared_raw_path) {
    return text_writer_detail::verify_plan_originals("FLAC", before, source_plan,
                                                     prepared_raw_path);
}

[[nodiscard]] core::Result<void> apply_text_changes(const MetadataWritePlanSource& source_plan,
                                                    const std::string& prepared_raw_path,
                                                    const core::CancellationToken& cancellation) {
    TagLib::FLAC::File file{prepared_raw_path.c_str(), false};
    if (!file.isValid()) {
        return std::unexpected(writer_error(core::ErrorCode::backend,
                                            "TagLib rejected the prepared FLAC copy",
                                            source_plan.raw_path, prepared_raw_path));
    }
    return text_writer_detail::apply_text_changes_to_properties("FLAC", source_plan, file,
                                                                prepared_raw_path, cancellation);
}

[[nodiscard]] core::Result<std::vector<unsigned char>>
read_planned_replacement(const ArtworkWritePlanSource& source_plan,
                         const std::string& prepared_raw_path,
                         const core::CancellationToken& cancellation) {
    const auto& replacement = *source_plan.change.replacement;
    auto bytes = read_artwork_image_bytes(replacement, replacement.byte_size, cancellation);
    if (!bytes) {
        auto error = std::move(bytes.error());
        error.context.push_back(
            {.key = "target", .value = core::escape_raw_path(source_plan.raw_media_path)});
        error.context.push_back(
            {.key = "prepared", .value = core::escape_raw_path(prepared_raw_path)});
        return std::unexpected(std::move(error));
    }
    return bytes;
}

[[nodiscard]] core::Result<LocalArtworkInventory>
read_embedded_inventory(const std::string& raw_path, const core::CancellationToken& cancellation) {
    auto policy = default_artwork_inventory_policy();
    policy.external_patterns.clear();
    return read_local_artwork_inventory(raw_path, policy, cancellation);
}

using PicturePayloads = std::vector<std::vector<unsigned char>>;

using artwork_detail::canonical_picture_type;

[[nodiscard]] core::Result<PicturePayloads>
read_picture_payloads(const std::string& raw_path, const ArtworkWritePlanSource& source_plan,
                      const std::string& prepared_raw_path) {
    TagLib::FLAC::File file{raw_path.c_str(), false};
    if (!file.isValid()) {
        return std::unexpected(writer_error(core::ErrorCode::backend,
                                            "TagLib rejected FLAC picture verification",
                                            source_plan.raw_media_path, prepared_raw_path));
    }
    const auto pictures = file.pictureList();
    PicturePayloads result;
    result.reserve(pictures.size());
    for (const auto* picture : pictures) {
        if (picture == nullptr) {
            return std::unexpected(writer_error(core::ErrorCode::backend,
                                                "FLAC picture verification found a null picture",
                                                source_plan.raw_media_path, prepared_raw_path));
        }
        const auto payload = picture->render();
        const auto* begin = reinterpret_cast<const unsigned char*>(payload.data());
        result.emplace_back(begin, begin + payload.size());
    }
    return result;
}

[[nodiscard]] core::Result<void>
rewrite_artwork_change(const ArtworkWritePlanSource& source_plan,
                       const std::string& prepared_raw_path,
                       const std::vector<unsigned char>& replacement_bytes,
                       const core::CancellationToken& cancellation) {
    TagLib::FLAC::File source_file{source_plan.raw_media_path.c_str(), false};
    if (!source_file.isValid()) {
        return std::unexpected(writer_error(core::ErrorCode::backend,
                                            "TagLib rejected the FLAC artwork source",
                                            source_plan.raw_media_path, prepared_raw_path));
    }
    const auto pictures = source_file.pictureList();
    const auto add = source_plan.change.kind == ArtworkWritePlanIntentKind::add;
    if ((!add &&
         (source_plan.change.target_ordinal >= pictures.size() ||
          pictures[static_cast<unsigned int>(source_plan.change.target_ordinal)] == nullptr)) ||
        (add && source_plan.change.target_ordinal != pictures.size())) {
        return std::unexpected(
            writer_error(core::ErrorCode::conflict,
                         add ? "FLAC source picture count differs from the plan"
                             : "FLAC source no longer contains the planned picture",
                         source_plan.raw_media_path, prepared_raw_path));
    }
    const auto remove = source_plan.change.kind == ArtworkWritePlanIntentKind::remove;
    std::vector<unsigned char> replacement_payload;
    if (!remove) {
        const auto* current =
            add ? nullptr : pictures[static_cast<unsigned int>(source_plan.change.target_ordinal)];
        const auto& replacement = *source_plan.change.replacement;
        TagLib::FLAC::Picture picture;
        picture.setType(add ? canonical_picture_type(source_plan.change.added_role)
                            : current->type());
        picture.setMimeType(TagLib::String{replacement.mime_type, TagLib::String::UTF8});
        picture.setDescription(
            add ? TagLib::String{source_plan.change.added_description, TagLib::String::UTF8}
                : current->description());
        picture.setWidth(static_cast<int>(*replacement.width));
        picture.setHeight(static_cast<int>(*replacement.height));
        picture.setColorDepth(0);
        picture.setNumColors(0);
        picture.setData(TagLib::ByteVector{
            reinterpret_cast<const char*>(replacement_bytes.data()),
            static_cast<unsigned int>(replacement_bytes.size()),
        });
        const auto rendered = picture.render();
        const auto* begin = reinterpret_cast<const unsigned char*>(rendered.data());
        replacement_payload.assign(begin, begin + rendered.size());
        if (replacement_payload.size() > 0xFF'FF'FFU) {
            return std::unexpected(writer_error(core::ErrorCode::limit_exceeded,
                                                "replacement FLAC picture block is too large",
                                                source_plan.raw_media_path, prepared_raw_path));
        }
    }

    auto layout = parse_flac_layout(source_plan.raw_media_path, source_plan.raw_media_path,
                                    prepared_raw_path, picture_block_type);
    if (!layout) {
        return std::unexpected(std::move(layout.error()));
    }
    std::size_t picture_ordinal = 0U;
    std::optional<std::size_t> target_block;
    std::size_t insertion_block = layout->blocks.size();
    for (std::size_t index = 0U; index < layout->blocks.size(); ++index) {
        if (layout->blocks[index].type != picture_block_type) {
            if (picture_ordinal == 0U && insertion_block == layout->blocks.size() &&
                layout->blocks[index].type == padding_block_type) {
                insertion_block = index;
            }
            continue;
        }
        insertion_block = index + 1U;
        if (picture_ordinal == source_plan.change.target_ordinal) {
            target_block = index;
        }
        ++picture_ordinal;
    }
    if (!add && !target_block) {
        return std::unexpected(writer_error(core::ErrorCode::conflict,
                                            "FLAC picture block order differs from the plan",
                                            source_plan.raw_media_path, prepared_raw_path));
    }

    std::ifstream input{std::filesystem::path{source_plan.raw_media_path}, std::ios::binary};
    std::ofstream output{std::filesystem::path{prepared_raw_path},
                         std::ios::binary | std::ios::trunc};
    if (!input || !output) {
        return std::unexpected(writer_error(core::ErrorCode::io,
                                            "opening the native FLAC artwork rewrite failed",
                                            source_plan.raw_media_path, prepared_raw_path));
    }
    constexpr std::array<char, 4> marker{'f', 'L', 'a', 'C'};
    output.write(marker.data(), static_cast<std::streamsize>(marker.size()));
    const auto output_block_count = layout->blocks.size() - (remove ? 1U : 0U) + (add ? 1U : 0U);
    if (output_block_count == 0U) {
        return std::unexpected(writer_error(core::ErrorCode::backend,
                                            "FLAC artwork rewrite would remove all metadata",
                                            source_plan.raw_media_path, prepared_raw_path));
    }

    std::array<char, comparison_buffer_size> buffer{};
    std::size_t output_index = 0U;
    const auto copy_region = [&](const std::uint64_t offset,
                                 const std::uint64_t length) -> core::Result<void> {
        input.clear();
        input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        if (!input) {
            return std::unexpected(writer_error(core::ErrorCode::io,
                                                "seeking native FLAC artwork input failed",
                                                source_plan.raw_media_path, prepared_raw_path));
        }
        std::uint64_t copied = 0U;
        while (copied < length) {
            if (cancellation.is_cancellation_requested()) {
                return std::unexpected(cancelled(source_plan.raw_media_path, prepared_raw_path));
            }
            const auto amount =
                static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), length - copied));
            input.read(buffer.data(), static_cast<std::streamsize>(amount));
            if (input.gcount() != static_cast<std::streamsize>(amount)) {
                return std::unexpected(writer_error(core::ErrorCode::io,
                                                    "reading native FLAC artwork input failed",
                                                    source_plan.raw_media_path, prepared_raw_path));
            }
            output.write(buffer.data(), static_cast<std::streamsize>(amount));
            if (!output) {
                return std::unexpected(writer_error(core::ErrorCode::io,
                                                    "writing native FLAC artwork output failed",
                                                    source_plan.raw_media_path, prepared_raw_path));
            }
            copied += amount;
        }
        return {};
    };

    const auto write_block_header = [&](const std::uint8_t type,
                                        const std::size_t length) -> core::Result<void> {
        const auto last = output_index + 1U == output_block_count;
        std::array<unsigned char, 4> header{
            static_cast<unsigned char>(type | (last ? 0x80U : 0U)),
            static_cast<unsigned char>((length >> 16U) & 0xFFU),
            static_cast<unsigned char>((length >> 8U) & 0xFFU),
            static_cast<unsigned char>(length & 0xFFU),
        };
        output.write(reinterpret_cast<const char*>(header.data()),
                     static_cast<std::streamsize>(header.size()));
        if (!output) {
            return std::unexpected(writer_error(core::ErrorCode::io,
                                                "writing native FLAC metadata header failed",
                                                source_plan.raw_media_path, prepared_raw_path));
        }
        ++output_index;
        return {};
    };
    const auto write_added_picture = [&]() -> core::Result<void> {
        auto header = write_block_header(picture_block_type, replacement_payload.size());
        if (!header) {
            return header;
        }
        output.write(reinterpret_cast<const char*>(replacement_payload.data()),
                     static_cast<std::streamsize>(replacement_payload.size()));
        if (!output) {
            return std::unexpected(writer_error(core::ErrorCode::io,
                                                "writing added native FLAC picture failed",
                                                source_plan.raw_media_path, prepared_raw_path));
        }
        return {};
    };

    for (std::size_t index = 0U; index < layout->blocks.size(); ++index) {
        if (add && index == insertion_block) {
            auto inserted = write_added_picture();
            if (!inserted) {
                return inserted;
            }
        }
        const auto& block = layout->blocks[index];
        if (remove && index == *target_block) {
            continue;
        }
        const auto replacement = !add && !remove && index == *target_block;
        const auto length = replacement ? replacement_payload.size() : block.length;
        auto header = write_block_header(block.type, length);
        if (!header) {
            return header;
        }
        if (replacement) {
            output.write(reinterpret_cast<const char*>(replacement_payload.data()),
                         static_cast<std::streamsize>(replacement_payload.size()));
        } else {
            auto copied = copy_region(block.data_offset, block.length);
            if (!copied) {
                return copied;
            }
        }
        if (!output) {
            return std::unexpected(writer_error(core::ErrorCode::io,
                                                "writing native FLAC metadata block failed",
                                                source_plan.raw_media_path, prepared_raw_path));
        }
    }
    if (add && insertion_block == layout->blocks.size()) {
        auto inserted = write_added_picture();
        if (!inserted) {
            return inserted;
        }
    }
    auto audio_copied = copy_region(layout->audio_offset, layout->file_size - layout->audio_offset);
    if (!audio_copied) {
        return audio_copied;
    }
    output.flush();
    if (!output) {
        return std::unexpected(writer_error(core::ErrorCode::io,
                                            "flushing native FLAC artwork output failed",
                                            source_plan.raw_media_path, prepared_raw_path));
    }
    return {};
}

[[nodiscard]] core::Result<void>
verify_picture_payload_result(const PicturePayloads& before, const PicturePayloads& after,
                              const ArtworkWritePlanSource& source_plan,
                              const std::string& prepared_raw_path) {
    const auto target = source_plan.change.target_ordinal;
    const auto remove = source_plan.change.kind == ArtworkWritePlanIntentKind::remove;
    const auto add = source_plan.change.kind == ArtworkWritePlanIntentKind::add;
    if ((!remove && !add && before.size() != after.size()) ||
        (add && before.size() + 1U != after.size()) ||
        (remove && (before.empty() || before.size() - 1U != after.size()))) {
        return std::unexpected(writer_error(core::ErrorCode::conflict,
                                            "prepared FLAC picture count differs from the plan",
                                            source_plan.raw_media_path, prepared_raw_path));
    }
    if (add && target != before.size()) {
        return std::unexpected(writer_error(core::ErrorCode::conflict,
                                            "prepared FLAC added picture at an unexpected ordinal",
                                            source_plan.raw_media_path, prepared_raw_path));
    }
    for (std::size_t after_index = 0U; after_index < after.size(); ++after_index) {
        if (add && after_index == target) {
            continue;
        }
        const auto before_index = remove && after_index >= target ? after_index + 1U : after_index;
        if ((!remove && !add && after_index == target) ||
            before[before_index] == after[after_index]) {
            continue;
        }
        return std::unexpected(
            writer_error(core::ErrorCode::conflict,
                         "prepared FLAC changed an unrelated serialized picture block",
                         source_plan.raw_media_path, prepared_raw_path));
    }
    return {};
}

[[nodiscard]] core::Result<void> verify_artwork_inventory_result(
    const LocalArtworkInventory& before, const LocalArtworkInventory& after,
    const ArtworkWritePlanSource& source_plan, const std::string& prepared_raw_path) {
    const auto target = source_plan.change.target_ordinal;
    const auto remove = source_plan.change.kind == ArtworkWritePlanIntentKind::remove;
    const auto add = source_plan.change.kind == ArtworkWritePlanIntentKind::add;
    if ((!remove && !add && before.items.size() != after.items.size()) ||
        (add && before.items.size() + 1U != after.items.size()) ||
        (remove && (before.items.empty() || before.items.size() - 1U != after.items.size()))) {
        return std::unexpected(writer_error(core::ErrorCode::conflict,
                                            "prepared FLAC artwork inventory differs from the plan",
                                            source_plan.raw_media_path, prepared_raw_path));
    }
    if (remove) {
        return {};
    }
    if (target >= after.items.size()) {
        return std::unexpected(writer_error(core::ErrorCode::conflict,
                                            "prepared FLAC replacement picture is missing",
                                            source_plan.raw_media_path, prepared_raw_path));
    }
    const auto& replacement = *source_plan.change.replacement;
    const auto& result = after.items[target];
    const auto expected_role =
        add ? source_plan.change.added_role : source_plan.change.original->role;
    const auto expected_description =
        add ? source_plan.change.added_description : source_plan.change.original->description;
    const auto expected_native_type =
        TagLib::FLAC::Picture::typeToString(canonical_picture_type(expected_role)).to8Bit(true);
    const auto native_type_matches =
        add ? result.native_type ==
                  std::string{expected_native_type.data(), expected_native_type.size()}
            : result.native_type == source_plan.change.original->native_type;
    if (result.role != expected_role || !native_type_matches ||
        result.description != expected_description || result.mime_type != replacement.mime_type ||
        result.width != replacement.width || result.height != replacement.height ||
        result.byte_size != replacement.byte_size ||
        result.content_fingerprint != replacement.content_fingerprint ||
        result.provenance != ArtworkProvenance::embedded || result.source_ordinal != target) {
        return std::unexpected(
            writer_error(core::ErrorCode::conflict,
                         "prepared FLAC replacement picture differs from the reviewed result",
                         source_plan.raw_media_path, prepared_raw_path));
    }
    return {};
}

} // namespace

core::Result<PreparedFlacMetadataWrite>
prepare_flac_metadata_write_copy(const MetadataWritePlanSource& source_plan,
                                 const std::string& prepared_raw_path,
                                 const core::CancellationToken& cancellation) {
    if (cancellation.is_cancellation_requested()) {
        return std::unexpected(cancelled(source_plan.raw_path, prepared_raw_path));
    }
    if (source_plan.raw_path.empty() || prepared_raw_path.empty() ||
        source_plan.raw_path.find('\0') != std::string::npos ||
        prepared_raw_path.find('\0') != std::string::npos) {
        return std::unexpected(writer_error(core::ErrorCode::invalid_argument,
                                            "prepared FLAC write paths must be nonempty raw paths",
                                            source_plan.raw_path, prepared_raw_path));
    }
    if (source_plan.raw_path == prepared_raw_path) {
        return std::unexpected(writer_error(core::ErrorCode::invalid_argument,
                                            "prepared FLAC path must differ from the source",
                                            source_plan.raw_path, prepared_raw_path));
    }
    if (!source_plan.ready() || source_plan.adapter_name != "taglib-flac-v1" ||
        !source_plan.expected_revision || !source_plan.observed_revision ||
        *source_plan.expected_revision != *source_plan.observed_revision ||
        source_plan.changes.empty()) {
        return std::unexpected(
            writer_error(core::ErrorCode::invalid_argument,
                         "prepared FLAC write requires one ready taglib-flac-v1 source plan",
                         source_plan.raw_path, prepared_raw_path));
    }
    for (const auto& change : source_plan.changes) {
        if (change.intents.empty() || change.conflicting_intents ||
            change.unresolved_non_embedded_target) {
            return std::unexpected(writer_error(core::ErrorCode::invalid_argument,
                                                "prepared FLAC plan contains an unresolved change",
                                                source_plan.raw_path, prepared_raw_path));
        }
        const auto& first = change.intents.front();
        if (!std::ranges::all_of(change.intents, [&first](const auto& intent) {
                return intent.kind == first.kind && intent.values == first.values;
            })) {
            return std::unexpected(writer_error(core::ErrorCode::invalid_argument,
                                                "prepared FLAC plan contains conflicting intents",
                                                source_plan.raw_path, prepared_raw_path));
        }
        auto mapping = map_flac_text_field(change.canonical_name, change.display_name,
                                           mapping_native_name(change), first.kind, first.values);
        if (!mapping) {
            return std::unexpected(std::move(mapping.error()));
        }
    }

    auto observed = core::observe_local_source_revision(source_plan.raw_path);
    if (!observed) {
        return std::unexpected(std::move(observed.error()));
    }
    if (*observed != *source_plan.observed_revision) {
        return std::unexpected(writer_error(
            core::ErrorCode::conflict, "FLAC source changed after the write plan was previewed",
            source_plan.raw_path, prepared_raw_path));
    }
    auto before = read_local_metadata(source_plan.raw_path, cancellation);
    if (!before) {
        return std::unexpected(std::move(before.error()));
    }
    if (before->source_revision != *source_plan.observed_revision ||
        before->adapter_name != "taglib-flac-v1") {
        return std::unexpected(
            writer_error(core::ErrorCode::conflict,
                         "FLAC source no longer matches the previewed adapter and revision",
                         source_plan.raw_path, prepared_raw_path));
    }
    auto originals_verified =
        verify_plan_originals(before->document, source_plan, prepared_raw_path);
    if (!originals_verified) {
        return std::unexpected(std::move(originals_verified.error()));
    }

    PreparedPathGuard prepared_guard{prepared_raw_path};
    auto source_mode = copy_source_exclusively(source_plan.raw_path, *source_plan.observed_revision,
                                               prepared_raw_path, cancellation, prepared_guard);
    if (!source_mode) {
        return std::unexpected(std::move(source_mode.error()));
    }
    auto applied = apply_text_changes(source_plan, prepared_raw_path, cancellation);
    if (!applied) {
        return std::unexpected(std::move(applied.error()));
    }
    if (::chmod(prepared_raw_path.c_str(), *source_mode) != 0) {
        return std::unexpected(system_error("restoring prepared FLAC permissions failed", errno,
                                            source_plan.raw_path, prepared_raw_path));
    }
    if (cancellation.is_cancellation_requested()) {
        return std::unexpected(cancelled(source_plan.raw_path, prepared_raw_path));
    }
    auto after = read_local_metadata(prepared_raw_path, cancellation);
    if (!after) {
        return std::unexpected(std::move(after.error()));
    }
    auto text_verified =
        verify_text_result(before->document, after->document, source_plan, prepared_raw_path);
    if (!text_verified) {
        return std::unexpected(std::move(text_verified.error()));
    }
    auto binary_verified = verify_flac_binary_preservation(source_plan.raw_path, prepared_raw_path,
                                                           vorbis_comment_block_type, cancellation);
    if (!binary_verified) {
        return std::unexpected(std::move(binary_verified.error()));
    }
    auto source_after = core::observe_local_source_revision(source_plan.raw_path);
    if (!source_after) {
        return std::unexpected(std::move(source_after.error()));
    }
    if (*source_after != *source_plan.observed_revision) {
        return std::unexpected(
            writer_error(core::ErrorCode::conflict,
                         "FLAC source changed while the prepared copy was being verified",
                         source_plan.raw_path, prepared_raw_path));
    }
    if (cancellation.is_cancellation_requested()) {
        return std::unexpected(cancelled(source_plan.raw_path, prepared_raw_path));
    }

    prepared_guard.release();
    return PreparedFlacMetadataWrite{
        .source_raw_path = source_plan.raw_path,
        .prepared_raw_path = prepared_raw_path,
        .source_revision = *source_after,
        .prepared_revision = after->source_revision,
        .document = std::move(after->document),
        .field_change_count = source_plan.changes.size(),
    };
}

core::Result<PreparedFlacArtworkWrite>
prepare_flac_artwork_write_copy(const ArtworkWritePlanSource& source_plan,
                                const std::string& prepared_raw_path,
                                const core::CancellationToken& cancellation) {
    if (cancellation.is_cancellation_requested()) {
        return std::unexpected(cancelled(source_plan.raw_media_path, prepared_raw_path));
    }
    const auto replace = source_plan.change.kind == ArtworkWritePlanIntentKind::replace;
    const auto add = source_plan.change.kind == ArtworkWritePlanIntentKind::add;
    const auto uses_replacement = replace || add;
    const auto replacement_representable =
        !uses_replacement ||
        (source_plan.change.replacement && source_plan.change.replacement->width &&
         source_plan.change.replacement->height && source_plan.change.replacement->byte_size > 0U &&
         (*source_plan.change.replacement->width <= static_cast<std::uint32_t>(INT_MAX)) &&
         (*source_plan.change.replacement->height <= static_cast<std::uint32_t>(INT_MAX)) &&
         (source_plan.change.replacement->mime_type == "image/png" ||
          source_plan.change.replacement->mime_type == "image/jpeg"));
    if (source_plan.raw_media_path.empty() || prepared_raw_path.empty() ||
        source_plan.raw_media_path.find('\0') != std::string::npos ||
        prepared_raw_path.find('\0') != std::string::npos ||
        source_plan.raw_media_path == prepared_raw_path || !source_plan.ready() ||
        source_plan.adapter_name != "taglib-flac-picture-v1" ||
        !source_plan.expected_media_revision || !source_plan.observed_media_revision ||
        *source_plan.expected_media_revision != *source_plan.observed_media_revision ||
        source_plan.occurrence_indexes.empty() ||
        uses_replacement != source_plan.change.replacement.has_value() ||
        !replacement_representable || (add && source_plan.change.original) ||
        (!add &&
         (!source_plan.change.original ||
          source_plan.change.original->provenance != ArtworkProvenance::embedded ||
          source_plan.change.original->raw_source_path != source_plan.raw_media_path ||
          source_plan.change.original->source_revision != *source_plan.observed_media_revision ||
          source_plan.change.original->source_ordinal != source_plan.change.target_ordinal ||
          source_plan.change.original->content_fingerprint !=
              source_plan.change.expected_target_fingerprint))) {
        return std::unexpected(
            writer_error(core::ErrorCode::invalid_argument,
                         "prepared FLAC artwork write requires one complete ready source plan",
                         source_plan.raw_media_path, prepared_raw_path));
    }

    auto observed = core::observe_local_source_revision(source_plan.raw_media_path);
    if (!observed) {
        return std::unexpected(std::move(observed.error()));
    }
    if (*observed != *source_plan.observed_media_revision) {
        return std::unexpected(writer_error(
            core::ErrorCode::conflict, "FLAC source changed after the artwork plan was previewed",
            source_plan.raw_media_path, prepared_raw_path));
    }
    auto before_inventory = read_embedded_inventory(source_plan.raw_media_path, cancellation);
    if (!before_inventory) {
        return std::unexpected(std::move(before_inventory.error()));
    }
    if (before_inventory->media_revision != *source_plan.observed_media_revision ||
        before_inventory->embedded_adapter_name != "taglib-flac-picture-v1") {
        return std::unexpected(
            writer_error(core::ErrorCode::conflict,
                         "FLAC artwork source no longer matches the previewed adapter and revision",
                         source_plan.raw_media_path, prepared_raw_path));
    }
    if (add) {
        if (source_plan.change.target_ordinal != before_inventory->items.size()) {
            return std::unexpected(writer_error(
                core::ErrorCode::conflict,
                "fresh FLAC artwork count differs from the previewed insertion ordinal",
                source_plan.raw_media_path, prepared_raw_path));
        }
        const auto duplicate = std::ranges::find_if(before_inventory->items, [&](const auto& item) {
            return item.content_fingerprint == source_plan.change.replacement->content_fingerprint;
        });
        if (duplicate != before_inventory->items.end()) {
            return std::unexpected(writer_error(core::ErrorCode::conflict,
                                                "fresh FLAC already contains the added image",
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
                             "fresh FLAC artwork differs from the previewed target picture",
                             source_plan.raw_media_path, prepared_raw_path));
        }
    }

    std::vector<unsigned char> replacement_bytes;
    if (uses_replacement) {
        auto loaded = read_planned_replacement(source_plan, prepared_raw_path, cancellation);
        if (!loaded) {
            return std::unexpected(std::move(loaded.error()));
        }
        replacement_bytes = std::move(*loaded);
    }
    auto before_document = read_local_metadata(source_plan.raw_media_path, cancellation);
    if (!before_document) {
        return std::unexpected(std::move(before_document.error()));
    }
    if (before_document->source_revision != *source_plan.observed_media_revision ||
        before_document->adapter_name != "taglib-flac-v1") {
        return std::unexpected(writer_error(
            core::ErrorCode::conflict, "FLAC metadata changed after the artwork plan was previewed",
            source_plan.raw_media_path, prepared_raw_path));
    }
    auto before_payloads =
        read_picture_payloads(source_plan.raw_media_path, source_plan, prepared_raw_path);
    if (!before_payloads) {
        return std::unexpected(std::move(before_payloads.error()));
    }

    PreparedPathGuard prepared_guard{prepared_raw_path};
    auto source_mode =
        copy_source_exclusively(source_plan.raw_media_path, *source_plan.observed_media_revision,
                                prepared_raw_path, cancellation, prepared_guard);
    if (!source_mode) {
        return std::unexpected(std::move(source_mode.error()));
    }
    auto applied =
        rewrite_artwork_change(source_plan, prepared_raw_path, replacement_bytes, cancellation);
    if (!applied) {
        return std::unexpected(std::move(applied.error()));
    }
    if (::chmod(prepared_raw_path.c_str(), *source_mode) != 0) {
        return std::unexpected(system_error("restoring prepared FLAC permissions failed", errno,
                                            source_plan.raw_media_path, prepared_raw_path));
    }
    if (cancellation.is_cancellation_requested()) {
        return std::unexpected(cancelled(source_plan.raw_media_path, prepared_raw_path));
    }

    auto after_document = read_local_metadata(prepared_raw_path, cancellation);
    if (!after_document) {
        return std::unexpected(std::move(after_document.error()));
    }
    if (after_document->document != before_document->document) {
        return std::unexpected(
            writer_error(core::ErrorCode::conflict,
                         "prepared FLAC artwork write changed the text metadata document",
                         source_plan.raw_media_path, prepared_raw_path));
    }
    auto after_inventory = read_embedded_inventory(prepared_raw_path, cancellation);
    if (!after_inventory) {
        return std::unexpected(std::move(after_inventory.error()));
    }
    if (after_inventory->media_revision != after_document->source_revision ||
        after_inventory->embedded_adapter_name != "taglib-flac-picture-v1") {
        return std::unexpected(writer_error(core::ErrorCode::conflict,
                                            "prepared FLAC artwork and metadata rereads disagree",
                                            source_plan.raw_media_path, prepared_raw_path));
    }
    auto inventory_verified = verify_artwork_inventory_result(*before_inventory, *after_inventory,
                                                              source_plan, prepared_raw_path);
    if (!inventory_verified) {
        return std::unexpected(std::move(inventory_verified.error()));
    }
    auto after_payloads = read_picture_payloads(prepared_raw_path, source_plan, prepared_raw_path);
    if (!after_payloads) {
        return std::unexpected(std::move(after_payloads.error()));
    }
    auto pictures_verified = verify_picture_payload_result(*before_payloads, *after_payloads,
                                                           source_plan, prepared_raw_path);
    if (!pictures_verified) {
        return std::unexpected(std::move(pictures_verified.error()));
    }
    auto binary_verified = verify_flac_binary_preservation(
        source_plan.raw_media_path, prepared_raw_path, picture_block_type, cancellation);
    if (!binary_verified) {
        return std::unexpected(std::move(binary_verified.error()));
    }
    auto source_after = core::observe_local_source_revision(source_plan.raw_media_path);
    if (!source_after) {
        return std::unexpected(std::move(source_after.error()));
    }
    if (*source_after != *source_plan.observed_media_revision) {
        return std::unexpected(
            writer_error(core::ErrorCode::conflict,
                         "FLAC source changed while the prepared artwork copy was being verified",
                         source_plan.raw_media_path, prepared_raw_path));
    }
    if (cancellation.is_cancellation_requested()) {
        return std::unexpected(cancelled(source_plan.raw_media_path, prepared_raw_path));
    }

    prepared_guard.release();
    return PreparedFlacArtworkWrite{
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

core::Result<void>
preservation_detail::verify_flac_composed(const std::string& source, const std::string& prepared,
                                          const bool text_changes,
                                          const core::CancellationToken& cancellation) {
    return verify_flac_binary_preservation(source, prepared, picture_block_type, cancellation,
                                           text_changes);
}

core::Result<void>
preservation_detail::rewrite_flac_composed(const MetadataWritePlanSource& plan,
                                           const std::string& prepared,
                                           const core::CancellationToken& cancellation) {
    const auto& artwork = *plan.artwork;
    auto layout = parse_flac_layout(plan.raw_path, plan.raw_path, prepared, picture_block_type);
    if (!layout) {
        return std::unexpected(layout.error());
    }
    TagLib::FLAC::File file{plan.raw_path.c_str(), false};
    if (!file.isValid()) {
        return std::unexpected(
            writer_error(core::ErrorCode::backend, "invalid FLAC source", plan.raw_path, prepared));
    }
    // Apply properties in memory. The native stream writer below owns serialization.
    struct MemoryProperties {
        TagLib::FLAC::File& file;
        auto properties() { return file.properties(); }
        auto setProperties(const TagLib::PropertyMap& properties) {
            return file.setProperties(properties);
        }
        bool save() { return true; }
    } memory{file};
    if (!plan.changes.empty()) {
        auto changed = text_writer_detail::apply_text_changes_to_properties("FLAC", plan, memory,
                                                                            prepared, cancellation);
        if (!changed) {
            return changed;
        }
    }
    struct Block {
        std::uint8_t type;
        std::uint64_t offset;
        std::uint64_t length;
        std::optional<TagLib::ByteVector> bytes;
    };
    std::vector<Block> blocks;
    const auto changes = artwork_changes(artwork);
    const auto pictures = file.pictureList();
    const auto payload =
        [&](const ArtworkWritePlanChange& change) -> core::Result<TagLib::ByteVector> {
        if (!change.replacement || !change.replacement->width || !change.replacement->height) {
            return std::unexpected(writer_error(core::ErrorCode::invalid_argument,
                                                "missing replacement dimensions", plan.raw_path,
                                                prepared));
        }
        auto bytes = read_artwork_image_bytes(*change.replacement, change.replacement->byte_size,
                                              cancellation);
        if (!bytes) {
            return std::unexpected(bytes.error());
        }
        const auto add = change.kind == ArtworkWritePlanIntentKind::add;
        const auto* original =
            add ? nullptr : pictures[static_cast<unsigned int>(change.target_ordinal)];
        TagLib::FLAC::Picture picture;
        picture.setType(add ? canonical_picture_type(change.added_role) : original->type());
        picture.setDescription(add ? TagLib::String{change.added_description, TagLib::String::UTF8}
                                   : original->description());
        picture.setMimeType(TagLib::String{change.replacement->mime_type, TagLib::String::UTF8});
        picture.setWidth(static_cast<int>(*change.replacement->width));
        picture.setHeight(static_cast<int>(*change.replacement->height));
        picture.setData(TagLib::ByteVector{reinterpret_cast<const char*>(bytes->data()),
                                           static_cast<unsigned int>(bytes->size())});
        return picture.render();
    };
    std::size_t ordinal = 0;
    bool comment_written = false;
    for (const auto& block : layout->blocks) {
        if (cancellation.is_cancellation_requested()) {
            return std::unexpected(cancelled(plan.raw_path, prepared));
        }
        if (block.type == vorbis_comment_block_type && !plan.changes.empty()) {
            if (comment_written) {
                return std::unexpected(writer_error(core::ErrorCode::unsupported,
                                                    "multiple FLAC comment blocks", plan.raw_path,
                                                    prepared));
            }
            blocks.push_back({block.type, 0, 0, file.xiphComment()->render(false)});
            comment_written = true;
            continue;
        }
        if (block.type == picture_block_type) {
            const auto change = std::ranges::find_if(changes, [&](const auto& candidate) {
                return candidate.kind != ArtworkWritePlanIntentKind::add &&
                       candidate.target_ordinal == ordinal;
            });
            ++ordinal;
            if (change != changes.end()) {
                if (change->kind == ArtworkWritePlanIntentKind::remove) {
                    continue;
                }
                auto rendered = payload(*change);
                if (!rendered) {
                    return std::unexpected(rendered.error());
                }
                blocks.push_back({block.type, 0, 0, std::move(*rendered)});
                continue;
            }
        }
        blocks.push_back({block.type, block.data_offset, block.length, std::nullopt});
    }
    if (!plan.changes.empty() && !comment_written) {
        blocks.push_back({vorbis_comment_block_type, 0, 0, file.xiphComment()->render(false)});
    }
    for (const auto& change : changes) {
        if (change.kind != ArtworkWritePlanIntentKind::add) {
            continue;
        }
        auto rendered = payload(change);
        if (!rendered) {
            return std::unexpected(rendered.error());
        }
        blocks.push_back({picture_block_type, 0, 0, std::move(*rendered)});
    }
    std::ifstream input{std::filesystem::path{plan.raw_path}, std::ios::binary};
    std::ofstream output{std::filesystem::path{prepared}, std::ios::binary | std::ios::trunc};
    if (!input || !output) {
        return std::unexpected(writer_error(core::ErrorCode::io, "opening FLAC streams failed",
                                            plan.raw_path, prepared));
    }
    output.write("fLaC", 4);
    std::array<char, comparison_buffer_size> buffer{};
    const auto copy = [&](std::uint64_t offset, std::uint64_t length) -> core::Result<void> {
        input.seekg(static_cast<std::streamoff>(offset));
        while (length > 0) {
            if (cancellation.is_cancellation_requested()) {
                return std::unexpected(cancelled(plan.raw_path, prepared));
            }
            const auto amount =
                static_cast<std::streamsize>(std::min<std::uint64_t>(length, buffer.size()));
            input.read(buffer.data(), amount);
            output.write(buffer.data(), amount);
            if (!input || !output) {
                return std::unexpected(writer_error(core::ErrorCode::io, "copying FLAC data failed",
                                                    plan.raw_path, prepared));
            }
            length -= static_cast<std::uint64_t>(amount);
        }
        return {};
    };
    for (std::size_t index = 0; index < blocks.size(); ++index) {
        const auto& block = blocks[index];
        const auto length =
            block.bytes ? static_cast<std::uint64_t>(block.bytes->size()) : block.length;
        if (length > 0xffffffU) {
            return std::unexpected(writer_error(core::ErrorCode::limit_exceeded,
                                                "FLAC metadata block too large", plan.raw_path,
                                                prepared));
        }
        const std::array<unsigned char, 4> header{
            static_cast<unsigned char>(block.type | (index + 1 == blocks.size() ? 0x80U : 0)),
            static_cast<unsigned char>(length >> 16U), static_cast<unsigned char>(length >> 8U),
            static_cast<unsigned char>(length)};
        output.write(reinterpret_cast<const char*>(header.data()), 4);
        if (block.bytes) {
            output.write(block.bytes->data(), static_cast<std::streamsize>(block.bytes->size()));
        } else {
            auto copied = copy(block.offset, length);
            if (!copied) {
                return copied;
            }
        }
    }
    auto copied = copy(layout->audio_offset, layout->file_size - layout->audio_offset);
    if (!copied) {
        return copied;
    }
    output.close();
    if (!output) {
        return std::unexpected(writer_error(core::ErrorCode::io, "finishing FLAC output failed",
                                            plan.raw_path, prepared));
    }
    return {};
}

TagLib::FLAC::Picture::Type artwork_detail::canonical_picture_type(const ArtworkRole role) {
    using Picture = TagLib::FLAC::Picture;
    switch (role) {
    case ArtworkRole::front:
        return Picture::FrontCover;
    case ArtworkRole::back:
        return Picture::BackCover;
    case ArtworkRole::artist:
        return Picture::Artist;
    case ArtworkRole::disc:
        return Picture::Media;
    case ArtworkRole::icon:
        return Picture::FileIcon;
    case ArtworkRole::other:
        return Picture::Other;
    }
    return Picture::Other;
}

} // namespace trackknife::metadata

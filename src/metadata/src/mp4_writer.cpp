// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/metadata/mp4_writer.hpp"

#include "container_preservation_detail.hpp"
#include "text_writer_detail.hpp"
#include "trackknife/metadata/local_reader.hpp"
#include "trackknife/metadata/ratings.hpp"

#include <mp4file.h>
#include <mp4tag.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <utility>
#include <vector>

namespace trackknife::metadata {
namespace {

using text_writer_detail::PreparedPathGuard;
using text_writer_detail::system_error;
using text_writer_detail::writer_error;

constexpr std::string_view mp4_label = "MP4";
constexpr std::string_view mp4_adapter = "taglib-mp4-v1";

[[nodiscard]] core::Error cancelled(const std::string& source_raw_path,
                                    const std::string& prepared_raw_path) {
    return text_writer_detail::cancelled(mp4_label, source_raw_path, prepared_raw_path);
}

[[nodiscard]] core::Result<std::vector<unsigned char>>
read_file_bytes(const std::string& raw_path, const std::string& source_raw_path,
                const std::string& prepared_raw_path) {
    std::ifstream input{raw_path, std::ios::binary | std::ios::ate};
    if (!input) {
        return std::unexpected(writer_error(core::ErrorCode::io,
                                            "opening an MP4 file for verification failed",
                                            source_raw_path, prepared_raw_path));
    }
    const auto size = input.tellg();
    if (size < 0) {
        return std::unexpected(writer_error(core::ErrorCode::io,
                                            "observing an MP4 file size failed", source_raw_path,
                                            prepared_raw_path));
    }
    std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
    input.seekg(0);
    if (!bytes.empty() && !input.read(reinterpret_cast<char*>(bytes.data()),
                                      static_cast<std::streamsize>(bytes.size()))) {
        return std::unexpected(writer_error(core::ErrorCode::io,
                                            "reading an MP4 file for verification failed",
                                            source_raw_path, prepared_raw_path));
    }
    return bytes;
}

struct TopLevelBox {
    std::array<unsigned char, 4> type{};
    std::size_t offset{0U};
    std::size_t size{0U};

    [[nodiscard]] bool is(const std::string_view name) const {
        return name.size() == 4U && std::equal(name.begin(), name.end(), type.begin());
    }
};

// Top-level ISO-BMFF box walk, including 64-bit largesize and the
// size-0 "extends to end of file" form.
[[nodiscard]] core::Result<std::vector<TopLevelBox>>
parse_top_level_boxes(const std::vector<unsigned char>& bytes, const core::Error& malformed) {
    std::vector<TopLevelBox> boxes;
    std::size_t offset = 0U;
    const auto read_be32 = [&bytes](const std::size_t at) {
        return (static_cast<std::uint64_t>(bytes[at]) << 24U) |
               (static_cast<std::uint64_t>(bytes[at + 1U]) << 16U) |
               (static_cast<std::uint64_t>(bytes[at + 2U]) << 8U) |
               static_cast<std::uint64_t>(bytes[at + 3U]);
    };
    while (offset < bytes.size()) {
        if (bytes.size() - offset < 8U) {
            return std::unexpected(malformed);
        }
        auto box_size = read_be32(offset);
        TopLevelBox box;
        box.offset = offset;
        std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset) + 4, 4U, box.type.begin());
        if (box_size == 1U) {
            if (bytes.size() - offset < 16U) {
                return std::unexpected(malformed);
            }
            box_size = (read_be32(offset + 8U) << 32U) | read_be32(offset + 12U);
        } else if (box_size == 0U) {
            box_size = bytes.size() - offset;
        }
        if (box_size < 8U || box_size > bytes.size() - offset) {
            return std::unexpected(malformed);
        }
        box.size = static_cast<std::size_t>(box_size);
        boxes.push_back(box);
        offset += box.size;
    }
    if (boxes.empty()) {
        return std::unexpected(malformed);
    }
    return boxes;
}

} // namespace

// The qualification proof (ADR-0136), shared with the covr artwork writer
// (ADR-0137): TagLib rewrites `moov` and may grow or shrink `free`
// padding, so those two may differ. Everything else — `ftyp`, every
// `mdat`, and any other top-level box — must be byte-identical and appear
// in the same order.
core::Result<void>
preservation_detail::verify_mp4_box_preservation(const std::string& source_raw_path,
                                                 const std::string& prepared_raw_path,
                                                 const core::CancellationToken& cancellation) {
    auto source_bytes = read_file_bytes(source_raw_path, source_raw_path, prepared_raw_path);
    if (!source_bytes) {
        return std::unexpected(std::move(source_bytes.error()));
    }
    if (cancellation.is_cancellation_requested()) {
        return std::unexpected(cancelled(source_raw_path, prepared_raw_path));
    }
    auto prepared_bytes = read_file_bytes(prepared_raw_path, source_raw_path, prepared_raw_path);
    if (!prepared_bytes) {
        return std::unexpected(std::move(prepared_bytes.error()));
    }
    const auto malformed =
        writer_error(core::ErrorCode::backend, "the MP4 container is not a well-formed box stream",
                     source_raw_path, prepared_raw_path);
    auto source_boxes = parse_top_level_boxes(*source_bytes, malformed);
    if (!source_boxes) {
        return std::unexpected(std::move(source_boxes.error()));
    }
    auto prepared_boxes = parse_top_level_boxes(*prepared_bytes, malformed);
    if (!prepared_boxes) {
        return std::unexpected(std::move(prepared_boxes.error()));
    }

    const auto mutable_box = [](const TopLevelBox& box) {
        return box.is("moov") || box.is("free") || box.is("skip");
    };
    std::vector<const TopLevelBox*> source_fixed;
    std::vector<const TopLevelBox*> prepared_fixed;
    bool source_moov = false;
    bool prepared_moov = false;
    for (const auto& box : *source_boxes) {
        source_moov = source_moov || box.is("moov");
        if (!mutable_box(box)) {
            source_fixed.push_back(&box);
        }
    }
    for (const auto& box : *prepared_boxes) {
        prepared_moov = prepared_moov || box.is("moov");
        if (!mutable_box(box)) {
            prepared_fixed.push_back(&box);
        }
    }
    if (!source_moov || !prepared_moov) {
        return std::unexpected(malformed);
    }
    if (source_fixed.size() != prepared_fixed.size()) {
        return std::unexpected(writer_error(core::ErrorCode::conflict,
                                            "prepared MP4 box sequence differs from the source",
                                            source_raw_path, prepared_raw_path));
    }
    for (std::size_t index = 0U; index < source_fixed.size(); ++index) {
        const auto& source_box = *source_fixed[index];
        const auto& prepared_box = *prepared_fixed[index];
        const auto identical =
            source_box.type == prepared_box.type && source_box.size == prepared_box.size &&
            std::equal(source_bytes->begin() + static_cast<std::ptrdiff_t>(source_box.offset),
                       source_bytes->begin() +
                           static_cast<std::ptrdiff_t>(source_box.offset + source_box.size),
                       prepared_bytes->begin() + static_cast<std::ptrdiff_t>(prepared_box.offset));
        if (!identical) {
            return std::unexpected(
                writer_error(core::ErrorCode::conflict,
                             "a prepared MP4 box differs from the source outside the metadata",
                             source_raw_path, prepared_raw_path));
        }
    }
    return {};
}

namespace {

// ADR-0237 stage 2: the generic mapping names the rating's freeform atom
// in capitals; it is named as the FMPS specification spells it, which is
// what the players that read it look for.
[[nodiscard]] core::Result<void> spell_rating_atom(const MetadataWritePlanSource& source_plan,
                                                   const std::string& prepared_raw_path) {
    const auto canonical = canonicalize_field_name(fmps_rating_field);
    if (std::ranges::none_of(source_plan.changes, [&canonical](const auto& change) {
            return change.canonical_name == canonical;
        })) {
        return {};
    }
    TagLib::MP4::File file{prepared_raw_path.c_str(), false};
    if (!file.isValid() || file.tag() == nullptr) {
        return std::unexpected(writer_error(core::ErrorCode::backend,
                                            "TagLib rejected the prepared MP4 copy",
                                            source_plan.raw_path, prepared_raw_path));
    }
    const TagLib::String spelled{"----:com.apple.iTunes:" + std::string{fmps_rating_spelling}};
    auto* tag = file.tag();
    const auto items = tag->itemMap();
    bool renamed = false;
    for (const auto& [key, item] : items) {
        if (key != spelled && key.upper() == spelled.upper()) {
            tag->removeItem(key);
            tag->setItem(spelled, item);
            renamed = true;
        }
    }
    if (renamed && !file.save()) {
        return std::unexpected(writer_error(core::ErrorCode::backend,
                                            "TagLib failed to save the prepared MP4 rating",
                                            source_plan.raw_path, prepared_raw_path));
    }
    return {};
}

[[nodiscard]] core::Result<void> apply_text_changes(const MetadataWritePlanSource& source_plan,
                                                    const std::string& prepared_raw_path,
                                                    const core::CancellationToken& cancellation) {
    {
        // Closed before the rating atom is renamed: TagLib finishes its
        // writes only when the file is closed.
        TagLib::MP4::File file{prepared_raw_path.c_str(), false};
        if (!file.isValid()) {
            return std::unexpected(writer_error(core::ErrorCode::backend,
                                                "TagLib rejected the prepared MP4 copy",
                                                source_plan.raw_path, prepared_raw_path));
        }
        // `trkn` carries number and total inside one atom (the combined
        // TRACKNUMBER value), so the FLAC-style paired-totals expansion stays
        // off exactly like MP3.
        auto applied = text_writer_detail::apply_text_changes_to_properties(
            mp4_label, source_plan, file, prepared_raw_path, cancellation, false);
        if (!applied) {
            return applied;
        }
    }
    return spell_rating_atom(source_plan, prepared_raw_path);
}

} // namespace

core::Result<PreparedMp4MetadataWrite>
prepare_mp4_metadata_write_copy(const MetadataWritePlanSource& source_plan,
                                const std::string& prepared_raw_path,
                                const core::CancellationToken& cancellation) {
    if (cancellation.is_cancellation_requested()) {
        return std::unexpected(cancelled(source_plan.raw_path, prepared_raw_path));
    }
    if (source_plan.raw_path.empty() || prepared_raw_path.empty() ||
        source_plan.raw_path.find('\0') != std::string::npos ||
        prepared_raw_path.find('\0') != std::string::npos) {
        return std::unexpected(writer_error(core::ErrorCode::invalid_argument,
                                            "prepared MP4 write paths must be nonempty raw paths",
                                            source_plan.raw_path, prepared_raw_path));
    }
    if (source_plan.raw_path == prepared_raw_path) {
        return std::unexpected(writer_error(core::ErrorCode::invalid_argument,
                                            "prepared MP4 path must differ from the source",
                                            source_plan.raw_path, prepared_raw_path));
    }
    if (!source_plan.ready() || source_plan.adapter_name != mp4_adapter ||
        !source_plan.expected_revision || !source_plan.observed_revision ||
        *source_plan.expected_revision != *source_plan.observed_revision ||
        source_plan.changes.empty()) {
        return std::unexpected(
            writer_error(core::ErrorCode::invalid_argument,
                         "prepared MP4 write requires one ready taglib-mp4-v1 source plan",
                         source_plan.raw_path, prepared_raw_path));
    }
    for (const auto& change : source_plan.changes) {
        if (change.intents.empty() || change.conflicting_intents ||
            change.unresolved_non_embedded_target) {
            return std::unexpected(writer_error(core::ErrorCode::invalid_argument,
                                                "prepared MP4 plan contains an unresolved change",
                                                source_plan.raw_path, prepared_raw_path));
        }
        const auto& first = change.intents.front();
        if (!std::ranges::all_of(change.intents, [&first](const auto& intent) {
                return intent.kind == first.kind && intent.values == first.values;
            })) {
            return std::unexpected(writer_error(core::ErrorCode::invalid_argument,
                                                "prepared MP4 plan contains conflicting intents",
                                                source_plan.raw_path, prepared_raw_path));
        }
        auto mapping = map_flac_text_field(change.canonical_name, change.display_name,
                                           text_writer_detail::mapping_native_name(change),
                                           first.kind, first.values);
        if (!mapping) {
            return std::unexpected(std::move(mapping.error()));
        }
    }

    auto observed = core::observe_local_source_revision(source_plan.raw_path);
    if (!observed) {
        return std::unexpected(std::move(observed.error()));
    }
    if (*observed != *source_plan.observed_revision) {
        return std::unexpected(writer_error(core::ErrorCode::conflict,
                                            "MP4 source changed after the write plan was previewed",
                                            source_plan.raw_path, prepared_raw_path));
    }
    auto before = read_local_metadata(source_plan.raw_path, cancellation);
    if (!before) {
        return std::unexpected(std::move(before.error()));
    }
    if (before->source_revision != *source_plan.observed_revision ||
        before->adapter_name != source_plan.adapter_name) {
        return std::unexpected(
            writer_error(core::ErrorCode::conflict,
                         "MP4 source no longer matches the previewed adapter and revision",
                         source_plan.raw_path, prepared_raw_path));
    }
    auto originals_verified = text_writer_detail::verify_plan_originals(
        mp4_label, before->document, source_plan, prepared_raw_path);
    if (!originals_verified) {
        return std::unexpected(std::move(originals_verified.error()));
    }

    PreparedPathGuard prepared_guard{prepared_raw_path};
    auto source_mode = text_writer_detail::copy_source_exclusively(
        mp4_label, source_plan.raw_path, *source_plan.observed_revision, prepared_raw_path,
        cancellation, prepared_guard);
    if (!source_mode) {
        return std::unexpected(std::move(source_mode.error()));
    }
    auto applied = apply_text_changes(source_plan, prepared_raw_path, cancellation);
    if (!applied) {
        return std::unexpected(std::move(applied.error()));
    }
    if (::chmod(prepared_raw_path.c_str(), *source_mode) != 0) {
        return std::unexpected(system_error("restoring prepared MP4 permissions failed", errno,
                                            source_plan.raw_path, prepared_raw_path));
    }
    if (cancellation.is_cancellation_requested()) {
        return std::unexpected(cancelled(source_plan.raw_path, prepared_raw_path));
    }
    auto after = read_local_metadata(prepared_raw_path, cancellation);
    if (!after) {
        return std::unexpected(std::move(after.error()));
    }
    auto text_verified = text_writer_detail::verify_text_result(
        mp4_label, before->document, after->document, source_plan, prepared_raw_path, false);
    if (!text_verified) {
        return std::unexpected(std::move(text_verified.error()));
    }
    auto boxes_verified = preservation_detail::verify_mp4_box_preservation(
        source_plan.raw_path, prepared_raw_path, cancellation);
    if (!boxes_verified) {
        return std::unexpected(std::move(boxes_verified.error()));
    }
    auto source_after = core::observe_local_source_revision(source_plan.raw_path);
    if (!source_after) {
        return std::unexpected(std::move(source_after.error()));
    }
    if (*source_after != *source_plan.observed_revision) {
        return std::unexpected(
            writer_error(core::ErrorCode::conflict,
                         "MP4 source changed while the prepared copy was being verified",
                         source_plan.raw_path, prepared_raw_path));
    }
    if (cancellation.is_cancellation_requested()) {
        return std::unexpected(cancelled(source_plan.raw_path, prepared_raw_path));
    }

    prepared_guard.release();
    return PreparedMp4MetadataWrite{
        .source_raw_path = source_plan.raw_path,
        .prepared_raw_path = prepared_raw_path,
        .source_revision = *source_after,
        .prepared_revision = after->source_revision,
        .document = std::move(after->document),
        .field_change_count = source_plan.changes.size(),
    };
}

} // namespace trackknife::metadata

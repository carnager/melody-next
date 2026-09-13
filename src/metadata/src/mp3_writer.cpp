// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/metadata/mp3_writer.hpp"
#include "trackknife/metadata/artwork_writers.hpp"

#include "apev2_trailer_detail.hpp"
#include "container_preservation_detail.hpp"
#include "text_writer_detail.hpp"
#include "trackknife/metadata/local_reader.hpp"
#include "trackknife/metadata/mp4_writer.hpp"
#include "trackknife/metadata/ogg_writer.hpp"
#include "trackknife/metadata/wavpack_writer.hpp"

#include <mpegfile.h>

#include <algorithm>
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

constexpr std::string_view mp3_label = "MP3";
constexpr std::string_view mp3_adapter = "taglib-mpeg-v1";
constexpr std::size_t id3v2_header_size = 10U;

[[nodiscard]] core::Error cancelled(const std::string& source_raw_path,
                                    const std::string& prepared_raw_path) {
    return text_writer_detail::cancelled(mp3_label, source_raw_path, prepared_raw_path);
}

[[nodiscard]] core::Result<std::vector<unsigned char>>
read_file_bytes(const std::string& raw_path, const std::string& source_raw_path,
                const std::string& prepared_raw_path) {
    std::ifstream input{raw_path, std::ios::binary | std::ios::ate};
    if (!input) {
        return std::unexpected(writer_error(core::ErrorCode::io,
                                            "opening an MP3 file for verification failed",
                                            source_raw_path, prepared_raw_path));
    }
    const auto size = input.tellg();
    if (size < 0) {
        return std::unexpected(writer_error(core::ErrorCode::io,
                                            "observing an MP3 file size failed", source_raw_path,
                                            prepared_raw_path));
    }
    std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
    input.seekg(0);
    if (!bytes.empty() && !input.read(reinterpret_cast<char*>(bytes.data()),
                                      static_cast<std::streamsize>(bytes.size()))) {
        return std::unexpected(writer_error(core::ErrorCode::io,
                                            "reading an MP3 file for verification failed",
                                            source_raw_path, prepared_raw_path));
    }
    return bytes;
}

// The MPEG audio region starts after the optional leading ID3v2 tag, whose
// length is carried in its own syncsafe-size header (plus a footer when the
// footer flag is set).
[[nodiscard]] core::Result<std::size_t> audio_begin(const std::vector<unsigned char>& bytes,
                                                    const std::string& source_raw_path,
                                                    const std::string& prepared_raw_path) {
    if (bytes.size() < 2U) {
        return std::unexpected(writer_error(core::ErrorCode::backend,
                                            "the file is too small to be an MP3", source_raw_path,
                                            prepared_raw_path));
    }
    if (!(bytes.size() >= 3U && bytes[0] == 'I' && bytes[1] == 'D' && bytes[2] == '3')) {
        return std::size_t{0U};
    }
    if (bytes.size() < id3v2_header_size) {
        return std::unexpected(writer_error(core::ErrorCode::backend,
                                            "the leading ID3v2 header is truncated",
                                            source_raw_path, prepared_raw_path));
    }
    const auto flags = bytes[5];
    std::size_t size = 0U;
    for (std::size_t index = 6U; index < 10U; ++index) {
        if ((bytes[index] & 0x80U) != 0U) {
            return std::unexpected(writer_error(core::ErrorCode::backend,
                                                "the leading ID3v2 size is not syncsafe",
                                                source_raw_path, prepared_raw_path));
        }
        size = (size << 7U) | (bytes[index] & 0x7FU);
    }
    auto total = id3v2_header_size + size;
    if ((flags & 0x10U) != 0U) {
        total += id3v2_header_size;
    }
    if (total > bytes.size()) {
        return std::unexpected(writer_error(core::ErrorCode::backend,
                                            "the leading ID3v2 tag exceeds the file",
                                            source_raw_path, prepared_raw_path));
    }
    return total;
}

} // namespace

// The qualification proof, shared with the APIC artwork writer
// (ADR-0137): the MPEG audio region — between the leading ID3v2 tag and
// any trailing ID3v1/APEv2 tags — is byte-identical, and every trailing
// APEv2 binary item survives byte-exactly.
core::Result<void>
preservation_detail::verify_mp3_binary_preservation(const std::string& source_raw_path,
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
    auto source_begin = audio_begin(*source_bytes, source_raw_path, prepared_raw_path);
    if (!source_begin) {
        return std::unexpected(std::move(source_begin.error()));
    }
    auto prepared_begin = audio_begin(*prepared_bytes, source_raw_path, prepared_raw_path);
    if (!prepared_begin) {
        return std::unexpected(std::move(prepared_begin.error()));
    }
    const auto malformed = [&] {
        return writer_error(core::ErrorCode::backend, "the MP3 APEv2 trailer is malformed",
                            source_raw_path, prepared_raw_path);
    };
    auto source_layout = apev2_trailer_detail::parse_trailer_layout(*source_bytes, malformed());
    if (!source_layout) {
        return std::unexpected(std::move(source_layout.error()));
    }
    auto prepared_layout = apev2_trailer_detail::parse_trailer_layout(*prepared_bytes, malformed());
    if (!prepared_layout) {
        return std::unexpected(std::move(prepared_layout.error()));
    }
    if (source_layout->audio_end < *source_begin || prepared_layout->audio_end < *prepared_begin) {
        return std::unexpected(writer_error(core::ErrorCode::backend,
                                            "the MP3 tag regions overlap the audio",
                                            source_raw_path, prepared_raw_path));
    }
    const auto source_audio = source_layout->audio_end - *source_begin;
    const auto prepared_audio = prepared_layout->audio_end - *prepared_begin;
    if (source_audio != prepared_audio ||
        !std::equal(source_bytes->begin() + static_cast<std::ptrdiff_t>(*source_begin),
                    source_bytes->begin() + static_cast<std::ptrdiff_t>(source_layout->audio_end),
                    prepared_bytes->begin() + static_cast<std::ptrdiff_t>(*prepared_begin))) {
        return std::unexpected(writer_error(core::ErrorCode::conflict,
                                            "prepared MP3 audio frames differ from the source",
                                            source_raw_path, prepared_raw_path));
    }
    if (!apev2_trailer_detail::preserved_items_match(*source_layout, *prepared_layout)) {
        return std::unexpected(writer_error(
            core::ErrorCode::conflict, "a prepared MP3 APEv2 binary item differs from the source",
            source_raw_path, prepared_raw_path));
    }
    return {};
}

namespace {

[[nodiscard]] core::Result<void> apply_text_changes(const MetadataWritePlanSource& source_plan,
                                                    const std::string& prepared_raw_path,
                                                    const core::CancellationToken& cancellation) {
    TagLib::MPEG::File file{prepared_raw_path.c_str(), false};
    if (!file.isValid()) {
        return std::unexpected(writer_error(core::ErrorCode::backend,
                                            "TagLib rejected the prepared MP3 copy",
                                            source_plan.raw_path, prepared_raw_path));
    }
    // ID3 carries no Picard-paired totals spellings; every field writes
    // exactly one mapped frame.
    return text_writer_detail::apply_text_changes_to_properties(
        mp3_label, source_plan, file, prepared_raw_path, cancellation, false);
}

} // namespace

core::Result<PreparedMp3MetadataWrite>
prepare_mp3_metadata_write_copy(const MetadataWritePlanSource& source_plan,
                                const std::string& prepared_raw_path,
                                const core::CancellationToken& cancellation) {
    if (cancellation.is_cancellation_requested()) {
        return std::unexpected(cancelled(source_plan.raw_path, prepared_raw_path));
    }
    if (source_plan.raw_path.empty() || prepared_raw_path.empty() ||
        source_plan.raw_path.find('\0') != std::string::npos ||
        prepared_raw_path.find('\0') != std::string::npos) {
        return std::unexpected(writer_error(core::ErrorCode::invalid_argument,
                                            "prepared MP3 write paths must be nonempty raw paths",
                                            source_plan.raw_path, prepared_raw_path));
    }
    if (source_plan.raw_path == prepared_raw_path) {
        return std::unexpected(writer_error(core::ErrorCode::invalid_argument,
                                            "prepared MP3 path must differ from the source",
                                            source_plan.raw_path, prepared_raw_path));
    }
    if (!source_plan.ready() || source_plan.adapter_name != mp3_adapter ||
        !source_plan.expected_revision || !source_plan.observed_revision ||
        *source_plan.expected_revision != *source_plan.observed_revision ||
        source_plan.changes.empty()) {
        return std::unexpected(
            writer_error(core::ErrorCode::invalid_argument,
                         "prepared MP3 write requires one ready taglib-mpeg-v1 source plan",
                         source_plan.raw_path, prepared_raw_path));
    }
    for (const auto& change : source_plan.changes) {
        if (change.intents.empty() || change.conflicting_intents ||
            change.unresolved_non_embedded_target) {
            return std::unexpected(writer_error(core::ErrorCode::invalid_argument,
                                                "prepared MP3 plan contains an unresolved change",
                                                source_plan.raw_path, prepared_raw_path));
        }
        const auto& first = change.intents.front();
        if (!std::ranges::all_of(change.intents, [&first](const auto& intent) {
                return intent.kind == first.kind && intent.values == first.values;
            })) {
            return std::unexpected(writer_error(core::ErrorCode::invalid_argument,
                                                "prepared MP3 plan contains conflicting intents",
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
                                            "MP3 source changed after the write plan was previewed",
                                            source_plan.raw_path, prepared_raw_path));
    }
    auto before = read_local_metadata(source_plan.raw_path, cancellation);
    if (!before) {
        return std::unexpected(std::move(before.error()));
    }
    if (before->source_revision != *source_plan.observed_revision ||
        before->adapter_name != mp3_adapter) {
        return std::unexpected(
            writer_error(core::ErrorCode::conflict,
                         "MP3 source no longer matches the previewed adapter and revision",
                         source_plan.raw_path, prepared_raw_path));
    }
    auto originals_verified = text_writer_detail::verify_plan_originals(
        mp3_label, before->document, source_plan, prepared_raw_path);
    if (!originals_verified) {
        return std::unexpected(std::move(originals_verified.error()));
    }

    PreparedPathGuard prepared_guard{prepared_raw_path};
    auto source_mode = text_writer_detail::copy_source_exclusively(
        mp3_label, source_plan.raw_path, *source_plan.observed_revision, prepared_raw_path,
        cancellation, prepared_guard);
    if (!source_mode) {
        return std::unexpected(std::move(source_mode.error()));
    }
    auto applied = apply_text_changes(source_plan, prepared_raw_path, cancellation);
    if (!applied) {
        return std::unexpected(std::move(applied.error()));
    }
    if (::chmod(prepared_raw_path.c_str(), *source_mode) != 0) {
        return std::unexpected(system_error("restoring prepared MP3 permissions failed", errno,
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
        mp3_label, before->document, after->document, source_plan, prepared_raw_path, false);
    if (!text_verified) {
        return std::unexpected(std::move(text_verified.error()));
    }
    auto binary_verified = preservation_detail::verify_mp3_binary_preservation(
        source_plan.raw_path, prepared_raw_path, cancellation);
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
                         "MP3 source changed while the prepared copy was being verified",
                         source_plan.raw_path, prepared_raw_path));
    }
    if (cancellation.is_cancellation_requested()) {
        return std::unexpected(cancelled(source_plan.raw_path, prepared_raw_path));
    }

    prepared_guard.release();
    return PreparedMp3MetadataWrite{
        .source_raw_path = source_plan.raw_path,
        .prepared_raw_path = prepared_raw_path,
        .source_revision = *source_after,
        .prepared_revision = after->source_revision,
        .document = std::move(after->document),
        .field_change_count = source_plan.changes.size(),
    };
}

core::Result<PreparedFlacMetadataWrite>
prepare_qualified_metadata_write_copy(const MetadataWritePlanSource& source_plan,
                                      const std::string& prepared_raw_path,
                                      const core::CancellationToken& cancellation) {
    if (source_plan.artwork) {
        return prepare_composed_metadata_write_copy(source_plan, prepared_raw_path, cancellation);
    }
    if (source_plan.adapter_name == "taglib-wavpack-v1") {
        return prepare_wavpack_metadata_write_copy(source_plan, prepared_raw_path, cancellation);
    }
    if (source_plan.adapter_name == mp3_adapter) {
        return prepare_mp3_metadata_write_copy(source_plan, prepared_raw_path, cancellation);
    }
    if (source_plan.adapter_name == "taglib-vorbis-v1" ||
        source_plan.adapter_name == "taglib-opus-v1") {
        return prepare_ogg_metadata_write_copy(source_plan, prepared_raw_path, cancellation);
    }
    if (source_plan.adapter_name == "taglib-mp4-v1") {
        return prepare_mp4_metadata_write_copy(source_plan, prepared_raw_path, cancellation);
    }
    return prepare_flac_metadata_write_copy(source_plan, prepared_raw_path, cancellation);
}

bool is_qualified_text_adapter(const std::string_view adapter_name) {
    return adapter_name == "taglib-flac-v1" || adapter_name == "taglib-wavpack-v1" ||
           adapter_name == mp3_adapter || adapter_name == "taglib-vorbis-v1" ||
           adapter_name == "taglib-opus-v1" || adapter_name == "taglib-mp4-v1";
}

} // namespace trackknife::metadata

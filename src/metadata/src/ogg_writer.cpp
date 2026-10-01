// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/metadata/ogg_writer.hpp"

#include "ogg_stream_detail.hpp"
#include "text_writer_detail.hpp"
#include "trackknife/metadata/local_reader.hpp"

#include <opusfile.h>
#include <vorbisfile.h>

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <memory>
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

constexpr std::string_view ogg_label = "Ogg";
constexpr std::string_view vorbis_adapter = "taglib-vorbis-v1";
constexpr std::string_view opus_adapter = "taglib-opus-v1";
constexpr std::string_view vorbis_comment_magic = "\x03vorbis";
constexpr std::string_view opus_comment_magic = "OpusTags";

[[nodiscard]] core::Error cancelled(const std::string& source_raw_path,
                                    const std::string& prepared_raw_path) {
    return text_writer_detail::cancelled(ogg_label, source_raw_path, prepared_raw_path);
}

[[nodiscard]] core::Result<std::vector<unsigned char>>
read_file_bytes(const std::string& raw_path, const std::string& source_raw_path,
                const std::string& prepared_raw_path) {
    std::ifstream input{raw_path, std::ios::binary | std::ios::ate};
    if (!input) {
        return std::unexpected(writer_error(core::ErrorCode::io,
                                            "opening an Ogg file for verification failed",
                                            source_raw_path, prepared_raw_path));
    }
    const auto size = input.tellg();
    if (size < 0) {
        return std::unexpected(writer_error(core::ErrorCode::io,
                                            "observing an Ogg file size failed", source_raw_path,
                                            prepared_raw_path));
    }
    std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
    input.seekg(0);
    if (!bytes.empty() && !input.read(reinterpret_cast<char*>(bytes.data()),
                                      static_cast<std::streamsize>(bytes.size()))) {
        return std::unexpected(writer_error(core::ErrorCode::io,
                                            "reading an Ogg file for verification failed",
                                            source_raw_path, prepared_raw_path));
    }
    return bytes;
}

[[nodiscard]] bool packet_starts_with(const std::vector<unsigned char>& packet,
                                      const std::string_view magic) {
    return packet.size() >= magic.size() && std::equal(magic.begin(), magic.end(), packet.begin());
}

// The qualification proof: every logical packet except the comment packet
// (index 1 in both Vorbis and Opus streams) is byte-identical, the comment
// packet keeps its codec magic on both sides, and the packet counts match.
[[nodiscard]] core::Result<void>
verify_ogg_packet_preservation(const std::string& source_raw_path,
                               const std::string& prepared_raw_path, const bool opus,
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
    const auto malformed = writer_error(core::ErrorCode::backend,
                                        "the Ogg stream is not a single well-formed bitstream",
                                        source_raw_path, prepared_raw_path);
    auto source_stream = ogg_stream_detail::parse_packet_stream(*source_bytes, malformed);
    if (!source_stream) {
        return std::unexpected(std::move(source_stream.error()));
    }
    auto prepared_stream = ogg_stream_detail::parse_packet_stream(*prepared_bytes, malformed);
    if (!prepared_stream) {
        return std::unexpected(std::move(prepared_stream.error()));
    }
    const auto minimum_packets = opus ? 2U : 3U;
    if (source_stream->packets.size() < minimum_packets ||
        source_stream->packets.size() != prepared_stream->packets.size()) {
        return std::unexpected(writer_error(core::ErrorCode::conflict,
                                            "prepared Ogg packet count differs from the source",
                                            source_raw_path, prepared_raw_path));
    }
    const auto comment_magic = opus ? opus_comment_magic : vorbis_comment_magic;
    if (!packet_starts_with(source_stream->packets[1], comment_magic) ||
        !packet_starts_with(prepared_stream->packets[1], comment_magic)) {
        return std::unexpected(writer_error(core::ErrorCode::backend,
                                            "the Ogg comment packet is not where expected",
                                            source_raw_path, prepared_raw_path));
    }
    for (std::size_t index = 0U; index < source_stream->packets.size(); ++index) {
        if (index == 1U) {
            continue;
        }
        if (source_stream->packets[index] != prepared_stream->packets[index]) {
            return std::unexpected(
                writer_error(core::ErrorCode::conflict,
                             "a prepared Ogg packet differs from the source outside the comment",
                             source_raw_path, prepared_raw_path));
        }
    }
    return {};
}

[[nodiscard]] core::Result<void> apply_text_changes(const MetadataWritePlanSource& source_plan,
                                                    const std::string& prepared_raw_path,
                                                    const bool opus,
                                                    const core::CancellationToken& cancellation) {
    std::unique_ptr<TagLib::File> file;
    if (opus) {
        file = std::make_unique<TagLib::Ogg::Opus::File>(prepared_raw_path.c_str(), false);
    } else {
        file = std::make_unique<TagLib::Vorbis::File>(prepared_raw_path.c_str(), false);
    }
    if (!file->isValid()) {
        return std::unexpected(writer_error(core::ErrorCode::backend,
                                            "TagLib rejected the prepared Ogg copy",
                                            source_plan.raw_path, prepared_raw_path));
    }
    // Vorbis comments carry the Picard-paired totals spellings exactly like
    // native FLAC, so paired writing stays on.
    return text_writer_detail::apply_text_changes_to_properties(
        ogg_label, source_plan, *file, prepared_raw_path, cancellation, true);
}

} // namespace

core::Result<PreparedOggMetadataWrite>
prepare_ogg_metadata_write_copy(const MetadataWritePlanSource& source_plan,
                                const std::string& prepared_raw_path,
                                const core::CancellationToken& cancellation) {
    if (cancellation.is_cancellation_requested()) {
        return std::unexpected(cancelled(source_plan.raw_path, prepared_raw_path));
    }
    if (source_plan.raw_path.empty() || prepared_raw_path.empty() ||
        source_plan.raw_path.find('\0') != std::string::npos ||
        prepared_raw_path.find('\0') != std::string::npos) {
        return std::unexpected(writer_error(core::ErrorCode::invalid_argument,
                                            "prepared Ogg write paths must be nonempty raw paths",
                                            source_plan.raw_path, prepared_raw_path));
    }
    if (source_plan.raw_path == prepared_raw_path) {
        return std::unexpected(writer_error(core::ErrorCode::invalid_argument,
                                            "prepared Ogg path must differ from the source",
                                            source_plan.raw_path, prepared_raw_path));
    }
    const auto opus = source_plan.adapter_name == opus_adapter;
    if (!source_plan.ready() ||
        (source_plan.adapter_name != vorbis_adapter && source_plan.adapter_name != opus_adapter) ||
        !source_plan.expected_revision || !source_plan.observed_revision ||
        *source_plan.expected_revision != *source_plan.observed_revision ||
        source_plan.changes.empty()) {
        return std::unexpected(writer_error(
            core::ErrorCode::invalid_argument,
            "prepared Ogg write requires one ready taglib-vorbis-v1 or taglib-opus-v1 source plan",
            source_plan.raw_path, prepared_raw_path));
    }
    for (const auto& change : source_plan.changes) {
        if (change.intents.empty() || change.conflicting_intents ||
            change.unresolved_non_embedded_target) {
            return std::unexpected(writer_error(core::ErrorCode::invalid_argument,
                                                "prepared Ogg plan contains an unresolved change",
                                                source_plan.raw_path, prepared_raw_path));
        }
        const auto& first = change.intents.front();
        if (!std::ranges::all_of(change.intents, [&first](const auto& intent) {
                return intent.kind == first.kind && intent.values == first.values;
            })) {
            return std::unexpected(writer_error(core::ErrorCode::invalid_argument,
                                                "prepared Ogg plan contains conflicting intents",
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
                                            "Ogg source changed after the write plan was previewed",
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
                         "Ogg source no longer matches the previewed adapter and revision",
                         source_plan.raw_path, prepared_raw_path));
    }
    auto originals_verified = text_writer_detail::verify_plan_originals(
        ogg_label, before->document, source_plan, prepared_raw_path);
    if (!originals_verified) {
        return std::unexpected(std::move(originals_verified.error()));
    }

    PreparedPathGuard prepared_guard{prepared_raw_path};
    auto source_mode = text_writer_detail::copy_source_exclusively(
        ogg_label, source_plan.raw_path, *source_plan.observed_revision, prepared_raw_path,
        cancellation, prepared_guard);
    if (!source_mode) {
        return std::unexpected(std::move(source_mode.error()));
    }
    auto applied = apply_text_changes(source_plan, prepared_raw_path, opus, cancellation);
    if (!applied) {
        return std::unexpected(std::move(applied.error()));
    }
    if (::chmod(prepared_raw_path.c_str(), *source_mode) != 0) {
        return std::unexpected(system_error("restoring prepared Ogg permissions failed", errno,
                                            source_plan.raw_path, prepared_raw_path));
    }
    if (cancellation.is_cancellation_requested()) {
        return std::unexpected(cancelled(source_plan.raw_path, prepared_raw_path));
    }
    if (auto settled = text_writer_detail::settle_prepared(prepared_raw_path, source_plan.raw_path); !settled) {
        return std::unexpected(std::move(settled.error()));
    }
    auto after = read_local_metadata(prepared_raw_path, cancellation);
    if (!after) {
        return std::unexpected(std::move(after.error()));
    }
    auto text_verified = text_writer_detail::verify_text_result(
        ogg_label, before->document, after->document, source_plan, prepared_raw_path, true);
    if (!text_verified) {
        return std::unexpected(std::move(text_verified.error()));
    }
    auto packets_verified =
        verify_ogg_packet_preservation(source_plan.raw_path, prepared_raw_path, opus, cancellation);
    if (!packets_verified) {
        return std::unexpected(std::move(packets_verified.error()));
    }
    auto source_after = core::observe_local_source_revision(source_plan.raw_path);
    if (!source_after) {
        return std::unexpected(std::move(source_after.error()));
    }
    if (*source_after != *source_plan.observed_revision) {
        return std::unexpected(
            writer_error(core::ErrorCode::conflict,
                         "Ogg source changed while the prepared copy was being verified",
                         source_plan.raw_path, prepared_raw_path));
    }
    if (cancellation.is_cancellation_requested()) {
        return std::unexpected(cancelled(source_plan.raw_path, prepared_raw_path));
    }

    prepared_guard.release();
    return PreparedOggMetadataWrite{
        .source_raw_path = source_plan.raw_path,
        .prepared_raw_path = prepared_raw_path,
        .source_revision = *source_after,
        .prepared_revision = after->source_revision,
        .document = std::move(after->document),
        .field_change_count = source_plan.changes.size(),
    };
}

} // namespace trackknife::metadata

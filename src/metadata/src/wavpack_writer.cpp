// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/metadata/wavpack_writer.hpp"

#include "apev2_trailer_detail.hpp"
#include "text_writer_detail.hpp"
#include "trackknife/metadata/local_reader.hpp"

#include <wavpackfile.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <map>
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

constexpr std::string_view wavpack_label = "WavPack";
constexpr std::string_view wavpack_adapter = "taglib-wavpack-v1";

[[nodiscard]] core::Error cancelled(const std::string& source_raw_path,
                                    const std::string& prepared_raw_path) {
    return text_writer_detail::cancelled(wavpack_label, source_raw_path, prepared_raw_path);
}

[[nodiscard]] core::Result<std::vector<unsigned char>>
read_file_bytes(const std::string& raw_path, const std::string& source_raw_path,
                const std::string& prepared_raw_path) {
    std::ifstream input{raw_path, std::ios::binary | std::ios::ate};
    if (!input) {
        return std::unexpected(writer_error(core::ErrorCode::io,
                                            "opening a WavPack file for verification failed",
                                            source_raw_path, prepared_raw_path));
    }
    const auto size = input.tellg();
    if (size < 0) {
        return std::unexpected(writer_error(core::ErrorCode::io,
                                            "observing a WavPack file size failed", source_raw_path,
                                            prepared_raw_path));
    }
    std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
    input.seekg(0);
    if (!bytes.empty() && !input.read(reinterpret_cast<char*>(bytes.data()),
                                      static_cast<std::streamsize>(bytes.size()))) {
        return std::unexpected(writer_error(core::ErrorCode::io,
                                            "reading a WavPack file for verification failed",
                                            source_raw_path, prepared_raw_path));
    }
    return bytes;
}

// The qualification proof: audio bytes identical, every binary/external
// APEv2 item carried over byte-exactly, and none invented. ID3v1 trailers
// are rejected rather than half-preserved.
[[nodiscard]] core::Result<void>
verify_wavpack_binary_preservation(const std::string& source_raw_path,
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
    const auto wvpk_marker = [](const std::vector<unsigned char>& bytes) {
        return bytes.size() >= 4U && bytes[0] == 'w' && bytes[1] == 'v' && bytes[2] == 'p' &&
               bytes[3] == 'k';
    };
    if (!wvpk_marker(*source_bytes) || !wvpk_marker(*prepared_bytes)) {
        return std::unexpected(writer_error(core::ErrorCode::backend,
                                            "the file does not start with a WavPack block",
                                            source_raw_path, prepared_raw_path));
    }
    const auto malformed = [&] {
        return writer_error(core::ErrorCode::backend, "the WavPack APEv2 trailer is malformed",
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
    if (source_layout->id3v1_present || prepared_layout->id3v1_present) {
        return std::unexpected(
            writer_error(core::ErrorCode::unsupported,
                         "WavPack sources carrying an ID3v1 trailer are not qualified",
                         source_raw_path, prepared_raw_path));
    }
    if (source_layout->audio_end != prepared_layout->audio_end ||
        !std::equal(source_bytes->begin(),
                    source_bytes->begin() + static_cast<std::ptrdiff_t>(source_layout->audio_end),
                    prepared_bytes->begin())) {
        return std::unexpected(writer_error(core::ErrorCode::conflict,
                                            "prepared WavPack audio blocks differ from the source",
                                            source_raw_path, prepared_raw_path));
    }
    if (!apev2_trailer_detail::preserved_items_match(*source_layout, *prepared_layout)) {
        return std::unexpected(
            writer_error(core::ErrorCode::conflict,
                         "a prepared WavPack APEv2 binary item differs from the source",
                         source_raw_path, prepared_raw_path));
    }
    return {};
}

[[nodiscard]] core::Result<void> apply_text_changes(const MetadataWritePlanSource& source_plan,
                                                    const std::string& prepared_raw_path,
                                                    const core::CancellationToken& cancellation) {
    TagLib::WavPack::File file{prepared_raw_path.c_str(), false};
    if (!file.isValid()) {
        return std::unexpected(writer_error(core::ErrorCode::backend,
                                            "TagLib rejected the prepared WavPack copy",
                                            source_plan.raw_path, prepared_raw_path));
    }
    return text_writer_detail::apply_text_changes_to_properties(wavpack_label, source_plan, file,
                                                                prepared_raw_path, cancellation);
}

} // namespace

core::Result<PreparedWavPackMetadataWrite>
prepare_wavpack_metadata_write_copy(const MetadataWritePlanSource& source_plan,
                                    const std::string& prepared_raw_path,
                                    const core::CancellationToken& cancellation) {
    if (cancellation.is_cancellation_requested()) {
        return std::unexpected(cancelled(source_plan.raw_path, prepared_raw_path));
    }
    if (source_plan.raw_path.empty() || prepared_raw_path.empty() ||
        source_plan.raw_path.find('\0') != std::string::npos ||
        prepared_raw_path.find('\0') != std::string::npos) {
        return std::unexpected(
            writer_error(core::ErrorCode::invalid_argument,
                         "prepared WavPack write paths must be nonempty raw paths",
                         source_plan.raw_path, prepared_raw_path));
    }
    if (source_plan.raw_path == prepared_raw_path) {
        return std::unexpected(writer_error(core::ErrorCode::invalid_argument,
                                            "prepared WavPack path must differ from the source",
                                            source_plan.raw_path, prepared_raw_path));
    }
    if (!source_plan.ready() || source_plan.adapter_name != wavpack_adapter ||
        !source_plan.expected_revision || !source_plan.observed_revision ||
        *source_plan.expected_revision != *source_plan.observed_revision ||
        source_plan.changes.empty()) {
        return std::unexpected(
            writer_error(core::ErrorCode::invalid_argument,
                         "prepared WavPack write requires one ready taglib-wavpack-v1 source plan",
                         source_plan.raw_path, prepared_raw_path));
    }
    for (const auto& change : source_plan.changes) {
        if (change.intents.empty() || change.conflicting_intents ||
            change.unresolved_non_embedded_target) {
            return std::unexpected(
                writer_error(core::ErrorCode::invalid_argument,
                             "prepared WavPack plan contains an unresolved change",
                             source_plan.raw_path, prepared_raw_path));
        }
        const auto& first = change.intents.front();
        if (!std::ranges::all_of(change.intents, [&first](const auto& intent) {
                return intent.kind == first.kind && intent.values == first.values;
            })) {
            return std::unexpected(
                writer_error(core::ErrorCode::invalid_argument,
                             "prepared WavPack plan contains conflicting intents",
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
        return std::unexpected(writer_error(
            core::ErrorCode::conflict, "WavPack source changed after the write plan was previewed",
            source_plan.raw_path, prepared_raw_path));
    }
    auto before = read_local_metadata(source_plan.raw_path, cancellation);
    if (!before) {
        return std::unexpected(std::move(before.error()));
    }
    if (before->source_revision != *source_plan.observed_revision ||
        before->adapter_name != wavpack_adapter) {
        return std::unexpected(
            writer_error(core::ErrorCode::conflict,
                         "WavPack source no longer matches the previewed adapter and revision",
                         source_plan.raw_path, prepared_raw_path));
    }
    auto originals_verified = text_writer_detail::verify_plan_originals(
        wavpack_label, before->document, source_plan, prepared_raw_path);
    if (!originals_verified) {
        return std::unexpected(std::move(originals_verified.error()));
    }

    PreparedPathGuard prepared_guard{prepared_raw_path};
    auto source_mode = text_writer_detail::copy_source_exclusively(
        wavpack_label, source_plan.raw_path, *source_plan.observed_revision, prepared_raw_path,
        cancellation, prepared_guard);
    if (!source_mode) {
        return std::unexpected(std::move(source_mode.error()));
    }
    auto applied = apply_text_changes(source_plan, prepared_raw_path, cancellation);
    if (!applied) {
        return std::unexpected(std::move(applied.error()));
    }
    if (::chmod(prepared_raw_path.c_str(), *source_mode) != 0) {
        return std::unexpected(system_error("restoring prepared WavPack permissions failed", errno,
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
        wavpack_label, before->document, after->document, source_plan, prepared_raw_path);
    if (!text_verified) {
        return std::unexpected(std::move(text_verified.error()));
    }
    auto binary_verified =
        verify_wavpack_binary_preservation(source_plan.raw_path, prepared_raw_path, cancellation);
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
                         "WavPack source changed while the prepared copy was being verified",
                         source_plan.raw_path, prepared_raw_path));
    }
    if (cancellation.is_cancellation_requested()) {
        return std::unexpected(cancelled(source_plan.raw_path, prepared_raw_path));
    }

    prepared_guard.release();
    return PreparedWavPackMetadataWrite{
        .source_raw_path = source_plan.raw_path,
        .prepared_raw_path = prepared_raw_path,
        .source_revision = *source_after,
        .prepared_revision = after->source_revision,
        .document = std::move(after->document),
        .field_change_count = source_plan.changes.size(),
    };
}

} // namespace trackknife::metadata

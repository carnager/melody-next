// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/metadata/local_reader.hpp"

#include "trackknife/metadata/flac_mapping.hpp"
#include "trackknife/metadata/ratings.hpp"

#include <fileref.h>
#include <flacfile.h>
#include <id3v2tag.h>
#include <mp4file.h>
#include <mpegfile.h>
#include <opusfile.h>
#include <popularimeterframe.h>
#include <tfile.h>
#include <tfilestream.h>
#include <tpropertymap.h>
#include <vorbisfile.h>
#include <wavpackfile.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace trackknife::metadata {
namespace {

constexpr std::size_t maximum_fields = 4'096U;
constexpr std::size_t maximum_values = 16'384U;
constexpr std::size_t maximum_text_bytes = 4U * 1024U * 1024U;

[[nodiscard]] core::Error error(const core::ErrorCode code, std::string message,
                                const std::string& raw_path) {
    return core::Error{
        .code = code,
        .message = std::move(message),
        .context = {{.key = "path", .value = core::escape_raw_path(raw_path)}},
    };
}

[[nodiscard]] core::Error cancelled(const std::string& raw_path) {
    return error(core::ErrorCode::cancelled, "metadata read was cancelled", raw_path);
}

[[nodiscard]] bool add_text_bytes(std::size_t& total, const std::size_t amount) {
    if (amount > maximum_text_bytes - total) {
        return false;
    }
    total += amount;
    return true;
}

[[nodiscard]] bool has_leading_marker(const std::string& raw_path,
                                      const std::array<char, 4>& expected) {
    std::ifstream input{std::filesystem::path{raw_path}, std::ios::binary};
    std::array<char, 4> marker{};
    return input.read(marker.data(), static_cast<std::streamsize>(marker.size())) &&
           marker == expected;
}

[[nodiscard]] bool has_native_flac_marker(const std::string& raw_path) {
    return has_leading_marker(raw_path, {'f', 'L', 'a', 'C'});
}

[[nodiscard]] bool has_native_wavpack_marker(const std::string& raw_path) {
    return has_leading_marker(raw_path, {'w', 'v', 'p', 'k'});
}

[[nodiscard]] bool has_native_mpeg_marker(const std::string& raw_path) {
    std::ifstream input{std::filesystem::path{raw_path}, std::ios::binary};
    std::array<char, 3> marker{};
    if (!input.read(marker.data(), static_cast<std::streamsize>(marker.size()))) {
        return false;
    }
    if (marker == std::array<char, 3>{'I', 'D', '3'}) {
        return true;
    }
    const auto first = static_cast<unsigned char>(marker[0]);
    const auto second = static_cast<unsigned char>(marker[1]);
    return first == 0xFFU && (second & 0xE0U) == 0xE0U;
}

[[nodiscard]] bool has_native_ogg_marker(const std::string& raw_path) {
    std::ifstream input{std::filesystem::path{raw_path}, std::ios::binary};
    std::array<char, 4> marker{};
    return input.read(marker.data(), static_cast<std::streamsize>(marker.size())) &&
           marker == std::array<char, 4>{'O', 'g', 'g', 'S'};
}

// The MP4 brand marker sits after the leading box-size word.
[[nodiscard]] bool has_native_mp4_marker(const std::string& raw_path) {
    std::ifstream input{std::filesystem::path{raw_path}, std::ios::binary};
    std::array<char, 8> header{};
    return input.read(header.data(), static_cast<std::streamsize>(header.size())) &&
           header[4] == 'f' && header[5] == 't' && header[6] == 'y' && header[7] == 'p';
}

} // namespace

core::Result<LocalMetadataRead> read_local_metadata(const std::string& raw_path,
                                                    const core::CancellationToken& cancellation) {
    if (cancellation.is_cancellation_requested()) {
        return std::unexpected(cancelled(raw_path));
    }
    auto revision_before = core::observe_local_source_revision(raw_path);
    if (!revision_before) {
        return std::unexpected(std::move(revision_before.error()));
    }

    // Read-only. TagLib opens for writing when it may, and a file opened for
    // writing reads as changed when closed -- to inotify, and so to
    // melody-watch, which would send it back to be read again (ADR-0232).
    TagLib::FileStream stream{raw_path.c_str(), true};
    TagLib::FileRef reference{&stream, false};
    if (reference.isNull() || reference.file() == nullptr) {
        return std::unexpected(error(core::ErrorCode::unsupported,
                                     "TagLib has no metadata reader for this source", raw_path));
    }
    if (!reference.file()->isValid()) {
        return std::unexpected(
            error(core::ErrorCode::backend, "TagLib rejected the metadata source", raw_path));
    }
    const bool native_flac = dynamic_cast<TagLib::FLAC::File*>(reference.file()) != nullptr &&
                             has_native_flac_marker(raw_path);
    const bool native_wavpack = !native_flac &&
                                dynamic_cast<TagLib::WavPack::File*>(reference.file()) != nullptr &&
                                has_native_wavpack_marker(raw_path);
    const bool native_mpeg = !native_flac && !native_wavpack &&
                             dynamic_cast<TagLib::MPEG::File*>(reference.file()) != nullptr &&
                             has_native_mpeg_marker(raw_path);
    const bool native_vorbis = !native_flac && !native_wavpack && !native_mpeg &&
                               dynamic_cast<TagLib::Vorbis::File*>(reference.file()) != nullptr &&
                               has_native_ogg_marker(raw_path);
    const bool native_opus = !native_flac && !native_wavpack && !native_mpeg && !native_vorbis &&
                             dynamic_cast<TagLib::Ogg::Opus::File*>(reference.file()) != nullptr &&
                             has_native_ogg_marker(raw_path);
    const bool native_mp4 = !native_flac && !native_wavpack && !native_mpeg && !native_vorbis &&
                            !native_opus &&
                            dynamic_cast<TagLib::MP4::File*>(reference.file()) != nullptr &&
                            has_native_mp4_marker(raw_path);
    const auto properties = reference.file()->properties();
    std::optional<std::uint8_t> popularimeter;
    if (native_mpeg) {
        auto* mpeg = dynamic_cast<TagLib::MPEG::File*>(reference.file());
        if (mpeg != nullptr && mpeg->hasID3v2Tag()) {
            for (auto* frame : mpeg->ID3v2Tag()->frameList("POPM")) {
                const auto* popm = dynamic_cast<TagLib::ID3v2::PopularimeterFrame*>(frame);
                if (popm == nullptr) {
                    continue;
                }
                const auto byte = static_cast<std::uint8_t>(std::clamp(popm->rating(), 0, 255));
                if (popm->email() == TagLib::String{std::string{popularimeter_owner}}) {
                    popularimeter = byte;
                    break;
                }
                if (!popularimeter) {
                    popularimeter = byte;
                }
            }
        }
    }
    if (cancellation.is_cancellation_requested()) {
        return std::unexpected(cancelled(raw_path));
    }

    MetadataDocument document;
    if (properties.size() > maximum_fields) {
        return std::unexpected(error(core::ErrorCode::limit_exceeded,
                                     "metadata field count exceeds the read limit", raw_path));
    }
    document.fields.reserve(properties.size());
    std::size_t value_count = 0U;
    std::size_t text_bytes = 0U;
    for (auto property = properties.cbegin(); property != properties.cend(); ++property) {
        MetadataField field;
        field.native_name = property->first.to8Bit(true);
        field.canonical_name = resolve_text_property_identity(field.native_name).canonical_name;
        field.provenance = FieldProvenance::embedded;
        if (field.canonical_name.empty() || !add_text_bytes(text_bytes, field.native_name.size())) {
            return std::unexpected(error(core::ErrorCode::limit_exceeded,
                                         "metadata field text exceeds the read limit", raw_path));
        }
        field.values.reserve(property->second.size());
        for (const auto& property_value : property->second) {
            if (value_count == maximum_values) {
                return std::unexpected(error(core::ErrorCode::limit_exceeded,
                                             "metadata value count exceeds the read limit",
                                             raw_path));
            }
            auto value = property_value.to8Bit(true);
            if (!add_text_bytes(text_bytes, value.size())) {
                return std::unexpected(error(core::ErrorCode::limit_exceeded,
                                             "metadata value text exceeds the read limit",
                                             raw_path));
            }
            field.values.push_back(std::move(value));
            ++value_count;
        }
        document.fields.push_back(std::move(field));
    }

    // Picard load rule for paired totals spellings: both natives resolve to
    // one canonical identity, and when a file carries both, the primary
    // spelling wins and the secondary is not surfaced twice. The secondary
    // comment stays byte-preserved in the file; writes refresh both.
    const auto drop_paired_secondary = [&document](const std::string_view primary,
                                                   const std::string_view secondary) {
        const auto has_primary = std::ranges::any_of(document.fields, [primary](const auto& field) {
            return canonicalize_native_field_name(field.native_name) == primary;
        });
        if (has_primary) {
            std::erase_if(document.fields, [secondary](const auto& field) {
                return canonicalize_native_field_name(field.native_name) == secondary;
            });
        }
    };
    drop_paired_secondary("totaltracks", "tracktotal");
    drop_paired_secondary("totaldiscs", "disctotal");

    const auto& unsupported = properties.unsupportedData();
    if (unsupported.size() > maximum_fields) {
        return std::unexpected(error(core::ErrorCode::limit_exceeded,
                                     "native metadata object count exceeds the read limit",
                                     raw_path));
    }
    document.unsupported_native_objects.reserve(unsupported.size());
    for (const auto& item : unsupported) {
        auto identity = item.to8Bit(true);
        if (!add_text_bytes(text_bytes, identity.size())) {
            return std::unexpected(error(core::ErrorCode::limit_exceeded,
                                         "native metadata identity text exceeds the read limit",
                                         raw_path));
        }
        document.unsupported_native_objects.push_back(
            NativeObjectIdentity{.identity = std::move(identity)});
    }

    auto revision_after = core::observe_local_source_revision(raw_path);
    if (!revision_after) {
        return std::unexpected(std::move(revision_after.error()));
    }
    if (*revision_before != *revision_after) {
        // Said in full: on a network mount, which value moved is the clue.
        // Ending in the name of the file read: shown cut in the middle, the
        // end still says whether it was the file or a copy prepared from it.
        const auto slash = raw_path.find_last_of('/');
        return std::unexpected(error(
            core::ErrorCode::conflict,
            "local source changed while metadata was being read (" +
                core::describe_revision_change(*revision_before, *revision_after) + ") in " +
                core::display_raw_path(slash == std::string::npos ? raw_path
                                                                  : raw_path.substr(slash + 1U)),
            raw_path));
    }
    if (cancellation.is_cancellation_requested()) {
        return std::unexpected(cancelled(raw_path));
    }
    // Native FLAC proves preservation only with no unsupported objects; the
    // native WavPack writer instead enumerates APEv2 binary items and
    // verifies them byte-exactly on every prepared copy, so their presence
    // does not block writes.
    const bool preservation_supported =
        (native_flac && document.unsupported_native_objects.empty()) || native_wavpack ||
        native_mpeg || native_vorbis || native_opus || native_mp4;
    return LocalMetadataRead{
        .raw_path = raw_path,
        .source_revision = *revision_after,
        .document = std::move(document),
        .adapter_name = native_flac      ? "taglib-flac-v1"
                        : native_wavpack ? "taglib-wavpack-v1"
                        : native_mpeg    ? "taglib-mpeg-v1"
                        : native_vorbis  ? "taglib-vorbis-v1"
                        : native_opus    ? "taglib-opus-v1"
                        : native_mp4     ? "taglib-mp4-v1"
                                         : "taglib-properties-v1",
        .capabilities =
            MetadataCapabilities{
                .fields_readable = true,
                .fields_writable = native_flac || native_wavpack || native_mpeg || native_vorbis ||
                                   native_opus || native_mp4,
                .pictures_readable = native_flac || native_mpeg || native_mp4,
                .pictures_writable = native_flac || native_mpeg || native_mp4,
                .unknown_data_preserved_on_write = preservation_supported,
            },
        .popularimeter = popularimeter,
    };
}

core::Result<std::vector<StagedMetadataSource>>
capture_uncached_metadata_sources(std::vector<StagedMetadataSource> sources,
                                  const core::CancellationToken& cancellation) {
    return capture_uncached_metadata_sources(std::move(sources), local_metadata_file_access(),
                                             cancellation);
}

MetadataFileAccess local_metadata_file_access() {
    return MetadataFileAccess{
        .read =
            [](const std::string& raw_path, const core::CancellationToken& cancellation) {
                return read_local_metadata(raw_path, cancellation);
            },
        .revision =
            [](const std::string& raw_path) {
                return core::observe_local_source_revision(raw_path);
            },
        .read_many = {}};
}

core::Result<std::vector<StagedMetadataSource>>
capture_uncached_metadata_sources(std::vector<StagedMetadataSource> sources,
                                  const MetadataFileAccess& access,
                                  const core::CancellationToken& cancellation) {
    std::map<std::string, LocalMetadataRead> captured;
    for (auto& source : sources) {
        if (cancellation.is_cancellation_requested()) {
            return std::unexpected(cancelled(source.raw_path));
        }
        if (!source.needs_metadata_capture || source.source_revision || source.logical_track) {
            continue;
        }
        auto found = captured.find(source.raw_path);
        if (found == captured.end()) {
            auto read = access.read(source.raw_path, cancellation);
            if (!read) {
                // Tagless decodable formats can still store gains in a sidecar.
                // No native metadata baseline is claimed for these sources.
                if (read.error().code != core::ErrorCode::unsupported) {
                    return std::unexpected(read.error());
                }
                auto revision = access.revision(source.raw_path);
                if (!revision)
                    return std::unexpected(revision.error());
                source.source_revision = *revision;
                source.needs_metadata_capture = false;
                continue;
            }
            found = captured.emplace(source.raw_path, std::move(*read)).first;
        }
        source.source_revision = found->second.source_revision;
        source.baseline = found->second.document;
        source.needs_metadata_capture = false;
    }
    return sources;
}

core::Result<CapturedMetadataSources>
capture_metadata_sources(std::vector<StagedMetadataSource> sources,
                         const MetadataFileAccess& access,
                         const core::CancellationToken& cancellation,
                         const MetadataCaptureProgress& progress) {
    // Each file once, however often it is listed.
    std::vector<std::string> wanted;
    {
        std::set<std::string> seen;
        for (const auto& source : sources) {
            if (source.needs_metadata_capture && !source.source_revision &&
                !source.logical_track && seen.insert(source.raw_path).second) {
                wanted.push_back(source.raw_path);
            }
        }
    }
    std::map<std::string, core::Result<LocalMetadataRead>> read;
    std::map<std::string, core::LocalSourceRevision> tagless;
    std::vector<std::pair<std::string, core::Error>> unreadable;
    if (progress) {
        progress(0U, wanted.size());
    }
    for (std::size_t first = 0; first < wanted.size(); first += metadata_capture_batch) {
        if (cancellation.is_cancellation_requested()) {
            return std::unexpected(cancelled(wanted[first]));
        }
        const auto last = std::min(wanted.size(), first + metadata_capture_batch);
        const std::vector<std::string> batch{wanted.begin() + static_cast<std::ptrdiff_t>(first),
                                             wanted.begin() + static_cast<std::ptrdiff_t>(last)};
        std::vector<core::Result<LocalMetadataRead>> results;
        if (access.read_many) {
            auto asked = access.read_many(batch, cancellation);
            if (!asked) {
                return std::unexpected(std::move(asked.error()));
            }
            results = std::move(*asked);
        } else {
            results.reserve(batch.size());
            for (const auto& path : batch) {
                results.push_back(access.read(path, cancellation));
            }
        }
        if (results.size() != batch.size()) {
            return std::unexpected(core::Error{.code = core::ErrorCode::backend,
                                               .message = "a batch of files was answered in part",
                                               .context = {}});
        }
        for (std::size_t index = 0; index < batch.size(); ++index) {
            auto& result = results[index];
            if (result) {
                read.emplace(batch[index], std::move(result));
                continue;
            }
            switch (result.error().code) {
            case core::ErrorCode::cancelled:
                return std::unexpected(std::move(result.error()));
            case core::ErrorCode::unsupported: {
                // Tagless decodable formats can still store gains in a
                // sidecar. No native metadata baseline is claimed for them.
                auto revision = access.revision(batch[index]);
                if (revision) {
                    tagless.emplace(batch[index], *revision);
                } else {
                    unreadable.emplace_back(batch[index], std::move(revision.error()));
                }
                break;
            }
            default:
                unreadable.emplace_back(batch[index], std::move(result.error()));
                break;
            }
        }
        if (progress) {
            progress(last, wanted.size());
        }
    }
    CapturedMetadataSources captured;
    captured.unreadable = std::move(unreadable);
    captured.sources.reserve(sources.size());
    for (auto& source : sources) {
        if (!source.needs_metadata_capture || source.source_revision || source.logical_track) {
            captured.sources.push_back(std::move(source));
            continue;
        }
        if (const auto found = read.find(source.raw_path); found != read.end()) {
            source.source_revision = found->second->source_revision;
            source.baseline = found->second->document;
            source.needs_metadata_capture = false;
            captured.sources.push_back(std::move(source));
        } else if (const auto revision = tagless.find(source.raw_path);
                   revision != tagless.end()) {
            source.source_revision = revision->second;
            source.needs_metadata_capture = false;
            captured.sources.push_back(std::move(source));
        } else {
            // Unreadable: kept as cached, with no revision -- shown, never
            // written from.
            source.needs_metadata_capture = false;
            captured.sources.push_back(std::move(source));
        }
    }
    return captured;
}

} // namespace trackknife::metadata

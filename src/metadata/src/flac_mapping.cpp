// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/metadata/flac_mapping.hpp"

#include "trackknife/metadata/document.hpp"

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#include <tstring.h>
#include <xiphcomment.h>

namespace trackknife::metadata {
namespace {

struct ConventionalMapping {
    std::string_view canonical_name;
    std::string_view property_name;
};

constexpr std::array conventional_mappings{
    ConventionalMapping{"title", "TITLE"},
    ConventionalMapping{"artist", "ARTIST"},
    ConventionalMapping{"albumartist", "ALBUMARTIST"},
    ConventionalMapping{"album", "ALBUM"},
    ConventionalMapping{"date", "DATE"},
    ConventionalMapping{"originaldate", "ORIGINALDATE"},
    ConventionalMapping{"tracknumber", "TRACKNUMBER"},
    ConventionalMapping{"totaltracks", "TOTALTRACKS"},
    // Picard-paired secondary spellings: they resolve to the same canonical
    // identity and never win the write name (the primary entry comes first).
    ConventionalMapping{"totaltracks", "TRACKTOTAL"},
    ConventionalMapping{"discnumber", "DISCNUMBER"},
    ConventionalMapping{"totaldiscs", "TOTALDISCS"},
    ConventionalMapping{"totaldiscs", "DISCTOTAL"},
    ConventionalMapping{"genre", "GENRE"},
    ConventionalMapping{"composer", "COMPOSER"},
    ConventionalMapping{"performer", "PERFORMER"},
    ConventionalMapping{"conductor", "CONDUCTOR"},
    ConventionalMapping{"lyricist", "LYRICIST"},
    ConventionalMapping{"lyrics", "LYRICS"},
    ConventionalMapping{"label", "LABEL"},
    ConventionalMapping{"catalognumber", "CATALOGNUMBER"},
    ConventionalMapping{"barcode", "BARCODE"},
    ConventionalMapping{"isrc", "ISRC"},
    ConventionalMapping{"comment", "COMMENT"},
    ConventionalMapping{"grouping", "GROUPING"},
    ConventionalMapping{"copyright", "COPYRIGHT"},
    ConventionalMapping{"bpm", "BPM"},
    ConventionalMapping{"compilation", "COMPILATION"},
    ConventionalMapping{"subtitle", "SUBTITLE"},
    ConventionalMapping{"version", "VERSION"},
    ConventionalMapping{"language", "LANGUAGE"},
    ConventionalMapping{"script", "SCRIPT"},
    ConventionalMapping{"media", "MEDIA"},
    ConventionalMapping{"discsubtitle", "DISCSUBTITLE"},
    ConventionalMapping{"originalyear", "ORIGINALYEAR"},
    ConventionalMapping{"releasetype", "RELEASETYPE"},
    ConventionalMapping{"releasestatus", "RELEASESTATUS"},
    ConventionalMapping{"releasecountry", "RELEASECOUNTRY"},
    ConventionalMapping{"encoder", "ENCODER"},
    ConventionalMapping{"artistsort", "ARTISTSORT"},
    ConventionalMapping{"albumartistsort", "ALBUMARTISTSORT"},
    ConventionalMapping{"artists", "ARTISTS"},
    ConventionalMapping{"albumartists", "ALBUMARTISTS"},
    ConventionalMapping{"musicbrainzartistid", "MUSICBRAINZ_ARTISTID"},
    ConventionalMapping{"musicbrainzalbumartistid", "MUSICBRAINZ_ALBUMARTISTID"},
    ConventionalMapping{"musicbrainztrackid", "MUSICBRAINZ_TRACKID"},
    ConventionalMapping{"musicbrainzreleasetrackid", "MUSICBRAINZ_RELEASETRACKID"},
    ConventionalMapping{"musicbrainzalbumid", "MUSICBRAINZ_ALBUMID"},
    ConventionalMapping{"musicbrainzreleasegroupid", "MUSICBRAINZ_RELEASEGROUPID"},
    ConventionalMapping{"musicbrainzworkid", "MUSICBRAINZ_WORKID"},
    ConventionalMapping{"musicbrainzdiscid", "MUSICBRAINZ_DISCID"},
    // ReplayGain 2.0 result fields (ADR-0100): one logical identity each so
    // a rescan updates the existing tag instead of growing a second column.
    ConventionalMapping{"replaygaintrackgain", "REPLAYGAIN_TRACK_GAIN"},
    ConventionalMapping{"replaygaintrackpeak", "REPLAYGAIN_TRACK_PEAK"},
    ConventionalMapping{"replaygainalbumgain", "REPLAYGAIN_ALBUM_GAIN"},
    ConventionalMapping{"replaygainalbumpeak", "REPLAYGAIN_ALBUM_PEAK"},
    // RFC 7845 Opus R128 loudness comments (ADR-0149): one logical
    // identity each, matching the canonical names the write-plan storage
    // guards already use.
    ConventionalMapping{"r128trackgain", "R128_TRACK_GAIN"},
    ConventionalMapping{"r128albumgain", "R128_ALBUM_GAIN"},
};

[[nodiscard]] core::Error mapping_error(std::string message, const std::string_view name) {
    return core::Error{
        .code = core::ErrorCode::unsupported,
        .message = std::move(message),
        .context = {{.key = "field", .value = std::string{name}}},
    };
}

[[nodiscard]] std::string conventional_name(const std::string_view canonical_name) {
    for (const auto& mapping : conventional_mappings) {
        if (mapping.canonical_name == canonical_name) {
            return std::string{mapping.property_name};
        }
    }
    return {};
}

[[nodiscard]] std::string uppercase_ascii(const std::string_view text) {
    std::string result{text};
    for (auto& character : result) {
        if (character >= 'a' && character <= 'z') {
            character = static_cast<char>(character - 'a' + 'A');
        }
    }
    return result;
}

[[nodiscard]] bool valid_utf8(const std::string_view value) {
    std::size_t index = 0U;
    while (index < value.size()) {
        const auto first = static_cast<unsigned char>(value[index]);
        if (first <= 0x7FU) {
            ++index;
            continue;
        }
        std::size_t continuation_count = 0U;
        unsigned code_point = 0U;
        unsigned minimum = 0U;
        if (first >= 0xC2U && first <= 0xDFU) {
            continuation_count = 1U;
            code_point = first & 0x1FU;
            minimum = 0x80U;
        } else if (first >= 0xE0U && first <= 0xEFU) {
            continuation_count = 2U;
            code_point = first & 0x0FU;
            minimum = 0x800U;
        } else if (first >= 0xF0U && first <= 0xF4U) {
            continuation_count = 3U;
            code_point = first & 0x07U;
            minimum = 0x10000U;
        } else {
            return false;
        }
        if (continuation_count > value.size() - index - 1U) {
            return false;
        }
        for (std::size_t offset = 1U; offset <= continuation_count; ++offset) {
            const auto continuation = static_cast<unsigned char>(value[index + offset]);
            if ((continuation & 0xC0U) != 0x80U) {
                return false;
            }
            code_point = (code_point << 6U) | (continuation & 0x3FU);
        }
        if (code_point < minimum || code_point > 0x10FFFFU ||
            (code_point >= 0xD800U && code_point <= 0xDFFFU)) {
            return false;
        }
        index += continuation_count + 1U;
    }
    return true;
}

[[nodiscard]] bool is_artwork_property(const std::string_view canonical_name) {
    return canonical_name == "picture" || canonical_name == "metadatablockpicture" ||
           canonical_name == "coverart" || canonical_name == "coverartmime";
}

} // namespace

TextPropertyIdentity resolve_text_property_identity(const std::string_view native_name) {
    const auto property_name = uppercase_ascii(native_name);
    for (const auto& mapping : conventional_mappings) {
        if (mapping.property_name == property_name) {
            return TextPropertyIdentity{.canonical_name = std::string{mapping.canonical_name},
                                        .conventional = true};
        }
    }
    return TextPropertyIdentity{.canonical_name = canonicalize_native_field_name(native_name),
                                .conventional = false};
}

bool is_conventional_metadata_field(const std::string_view canonical_name) {
    return !conventional_name(canonical_name).empty();
}

std::vector<std::string> paired_flac_property_names(const std::string_view canonical_name) {
    // Picard writes both totals spellings so every consumer finds the one it
    // reads; removal and verification cover the same pair.
    if (canonical_name == "totaltracks") {
        return {"TOTALTRACKS", "TRACKTOTAL"};
    }
    if (canonical_name == "totaldiscs") {
        return {"TOTALDISCS", "DISCTOTAL"};
    }
    return {};
}

core::Result<FlacTextFieldMapping> map_flac_text_field(const std::string_view canonical_name,
                                                       const std::string_view display_name,
                                                       const std::string_view existing_native_name,
                                                       const StagedMetadataPatchKind kind,
                                                       const std::span<const std::string> values) {
    if (canonical_name.empty()) {
        return std::unexpected(
            mapping_error("FLAC text field has no canonical identity", display_name));
    }
    if (is_artwork_property(canonical_name)) {
        return std::unexpected(
            mapping_error("artwork metadata requires the typed FLAC picture path", display_name));
    }
    if (kind == StagedMetadataPatchKind::replace_values) {
        if (values.empty()) {
            return std::unexpected(
                mapping_error("FLAC replacement requires at least one exact value", display_name));
        }
        for (const auto& value : values) {
            if (value.empty()) {
                return std::unexpected(mapping_error(
                    "the FLAC writer cannot preserve an exact empty text value", display_name));
            }
            if (!valid_utf8(value)) {
                return std::unexpected(
                    mapping_error("FLAC text values must be valid UTF-8", display_name));
            }
        }
    }

    auto property_name = existing_native_name.empty() ? conventional_name(canonical_name)
                                                      : uppercase_ascii(existing_native_name);
    if (property_name.empty()) {
        property_name = uppercase_ascii(display_name);
    }
    if (canonicalize_field_name(property_name) != canonical_name &&
        resolve_text_property_identity(property_name).canonical_name != canonical_name) {
        return std::unexpected(mapping_error(
            "the FLAC property key does not resolve to the planned field", display_name));
    }
    const TagLib::String taglib_name{property_name, TagLib::String::UTF8};
    if (!TagLib::Ogg::XiphComment::checkKey(taglib_name)) {
        return std::unexpected(
            mapping_error("the FLAC property key is not a valid Xiph-comment key", display_name));
    }
    return FlacTextFieldMapping{.property_name = std::move(property_name)};
}

} // namespace trackknife::metadata

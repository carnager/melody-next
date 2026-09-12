// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/musicbrainz/proposal_bridge.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>
#include <vector>

namespace trackknife::musicbrainz {
namespace {

[[nodiscard]] core::Error bridge_error(std::string message) {
    return core::Error{
        .code = core::ErrorCode::invalid_argument,
        .message = std::move(message),
        .context = {},
    };
}

[[nodiscard]] std::string joined_credit(const std::vector<ArtistCredit>& credits) {
    std::string joined;
    for (const auto& credit : credits) {
        joined += credit.name;
        joined += credit.join_phrase;
    }
    return joined;
}

// Picard keeps sort names beside credited names: the sort name joined with
// the same join phrases, falling back to the credited name.
[[nodiscard]] std::string joined_sort_credit(const std::vector<ArtistCredit>& credits) {
    std::string joined;
    for (const auto& credit : credits) {
        joined += credit.sort_name.empty() ? credit.name : credit.sort_name;
        joined += credit.join_phrase;
    }
    return joined;
}

[[nodiscard]] std::string ascii_lowered(std::string value) {
    std::ranges::transform(value, value.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

// Picard's releasetype: the lowercased primary type followed by every
// lowercased secondary type.
[[nodiscard]] std::vector<std::string> release_types(const Release& release) {
    std::vector<std::string> types;
    if (!release.release_group_primary_type.empty()) {
        types.push_back(ascii_lowered(release.release_group_primary_type));
    }
    for (const auto& secondary : release.release_group_secondary_types) {
        if (!secondary.empty()) {
            types.push_back(ascii_lowered(secondary));
        }
    }
    return types;
}

[[nodiscard]] std::string original_year(const std::string& first_release_date) {
    if (first_release_date.size() < 4U) {
        return {};
    }
    const auto year = first_release_date.substr(0U, 4U);
    return std::ranges::all_of(
               year, [](const unsigned char character) { return std::isdigit(character) != 0; })
               ? year
               : std::string{};
}

[[nodiscard]] std::vector<std::string> credit_ids(const std::vector<ArtistCredit>& credits) {
    std::vector<std::string> ids;
    ids.reserve(credits.size());
    for (const auto& credit : credits) {
        if (!credit.artist_id.empty()) {
            ids.push_back(credit.artist_id);
        }
    }
    return ids;
}

[[nodiscard]] std::string release_version_label(const Release& release) {
    std::string label = release.title;
    const auto append = [&label](const std::string& value) {
        if (!value.empty()) {
            label += " · ";
            label += value;
        }
    };
    append(release.date);
    append(release.country);
    append(release.disambiguation);
    append(release.label);
    append(release.catalog_number);
    return label;
}

void propose(metadata::MetadataProposalItem& item, std::string display_field,
             std::vector<std::string> values, const double confidence, std::string rationale) {
    if (values.empty() || values.front().empty()) {
        return;
    }
    item.fields.push_back(metadata::ProposedFieldValues{
        .canonical_field = metadata::canonicalize_field_name(display_field),
        .display_field = std::move(display_field),
        .match_mode = metadata::MetadataFieldMatchMode::logical,
        .values = std::move(values),
        .confidence = confidence,
        .rationale = std::move(rationale),
    });
}

} // namespace

core::Result<metadata::MetadataProposalSet>
release_metadata_proposals(const Release& release, const ReleaseAlignment& alignment,
                           const std::span<const std::size_t> item_indexes,
                           const ReleaseProposalOptions& options) {
    if (alignment.tracks.size() != item_indexes.size()) {
        return std::unexpected(
            bridge_error("release proposals need one selection item per aligned file"));
    }
    for (const auto& track : alignment.tracks) {
        if (track.release_track_index &&
            *track.release_track_index >= alignment.release_tracks.size()) {
            return std::unexpected(bridge_error("the release alignment is out of bounds"));
        }
    }

    const auto version = release_version_label(release);
    metadata::MetadataProposalSet proposals{
        .provider_name = "MusicBrainz",
        .provider_detail = version,
        .items = {},
    };
    const auto release_artist = joined_credit(release.artist_credits);
    const auto release_artist_sort = joined_sort_credit(release.artist_credits);
    const auto release_artist_ids = credit_ids(release.artist_credits);
    const auto total_discs = std::to_string(release.media.size());
    const auto types = release_types(release);
    const auto first_release_year = original_year(release.release_group_first_release_date);

    for (std::size_t position = 0U; position < alignment.tracks.size(); ++position) {
        const auto& aligned = alignment.tracks[position];
        if (!aligned.release_track_index || aligned.confidence < options.minimum_track_confidence) {
            continue;
        }
        const auto& flattened = alignment.release_tracks[*aligned.release_track_index];
        const auto& track = flattened.track;
        const auto confidence = aligned.confidence;
        std::string rationale =
            aligned.user_confirmed ? "User-confirmed assignment to \"" : "Matched to \"";
        rationale += track.title;
        rationale += "\" on ";
        rationale += version;

        metadata::MetadataProposalItem item{
            .item_index = item_indexes[position],
            .fields = {},
            .artwork = {},
        };
        propose(item, "Title", {track.title}, confidence, rationale);
        const auto track_artist = joined_credit(track.artist_credits);
        propose(item, "Artist", {track_artist.empty() ? release_artist : track_artist}, confidence,
                rationale);
        const auto track_artist_sort = joined_sort_credit(track.artist_credits);
        propose(item, "ARTISTSORT",
                {track_artist_sort.empty() ? release_artist_sort : track_artist_sort}, confidence,
                rationale);
        propose(item, "Album", {release.title}, confidence, rationale);
        propose(item, "Album Artist", {release_artist}, confidence, rationale);
        propose(item, "ALBUMARTISTSORT", {release_artist_sort}, confidence, rationale);
        propose(item, "Date", {release.date}, confidence, rationale);
        propose(item, "ORIGINALDATE", {release.release_group_first_release_date}, confidence,
                rationale);
        propose(item, "ORIGINALYEAR", {first_release_year}, confidence, rationale);
        propose(item, "Track Number", {std::to_string(track.position)}, confidence, rationale);
        propose(item, "Total Tracks", {std::to_string(flattened.medium_track_count)}, confidence,
                rationale);
        // Picard numbers single-disc releases 1/1 rather than omitting them.
        propose(item, "Disc Number", {std::to_string(flattened.medium_position)}, confidence,
                rationale);
        propose(item, "Total Discs", {total_discs}, confidence, rationale);
        propose(item, "RELEASETYPE", types, confidence, rationale);
        propose(item, "RELEASESTATUS", {ascii_lowered(release.status)}, confidence, rationale);
        propose(item, "RELEASECOUNTRY", {release.country}, confidence, rationale);
        propose(item, "SCRIPT", {release.script}, confidence, rationale);
        propose(item, "LANGUAGE", {release.language}, confidence, rationale);
        propose(item, "LABEL", {release.label}, confidence, rationale);
        propose(item, "CATALOGNUMBER", {release.catalog_number}, confidence, rationale);
        propose(item, "BARCODE", {release.barcode}, confidence, rationale);
        propose(item, "MEDIA", {flattened.medium_format}, confidence, rationale);
        propose(item, "DISCSUBTITLE", {flattened.medium_title}, confidence, rationale);
        propose(item, "ISRC", track.isrcs, confidence, rationale);
        if (options.include_identifiers) {
            propose(item, "MUSICBRAINZ_TRACKID", {track.recording_id}, confidence, rationale);
            propose(item, "MUSICBRAINZ_RELEASETRACKID", {track.track_id}, confidence, rationale);
            propose(item, "MUSICBRAINZ_ALBUMID", {release.id}, confidence, rationale);
            propose(item, "MUSICBRAINZ_RELEASEGROUPID", {release.release_group_id}, confidence,
                    rationale);
            const auto track_artist_ids = credit_ids(track.artist_credits);
            propose(item, "MUSICBRAINZ_ARTISTID",
                    track_artist_ids.empty() ? release_artist_ids : track_artist_ids, confidence,
                    rationale);
            propose(item, "MUSICBRAINZ_ALBUMARTISTID", release_artist_ids, confidence, rationale);
            propose(item, "MUSICBRAINZ_WORKID", track.work_ids, confidence, rationale);
        }
        if (!item.fields.empty()) {
            proposals.items.push_back(std::move(item));
        }
    }
    return proposals;
}

} // namespace trackknife::musicbrainz

// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/metadata/field_suggestions.hpp"

#include "trackknife/metadata/document.hpp"
#include "trackknife/metadata/flac_mapping.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace trackknife::metadata {
namespace {

using Kind = MetadataFieldSuggestionKind;
using Candidate = MetadataFieldSuggestionCandidate;

constexpr std::array catalog{
    Candidate{"Title", Kind::conventional},
    Candidate{"Artist", Kind::conventional},
    Candidate{"Album Artist", Kind::conventional},
    Candidate{"Album", Kind::conventional},
    Candidate{"Date", Kind::conventional},
    Candidate{"Original Date", Kind::conventional},
    Candidate{"Track Number", Kind::conventional},
    Candidate{"Total Tracks", Kind::conventional},
    Candidate{"Disc Number", Kind::conventional},
    Candidate{"Total Discs", Kind::conventional},
    Candidate{"Genre", Kind::conventional},
    Candidate{"Composer", Kind::conventional},
    Candidate{"Performer", Kind::conventional},
    Candidate{"Conductor", Kind::conventional},
    Candidate{"Lyricist", Kind::conventional},
    Candidate{"Lyrics", Kind::conventional},
    Candidate{"Label", Kind::conventional},
    Candidate{"Catalog Number", Kind::conventional},
    Candidate{"Barcode", Kind::conventional},
    Candidate{"ISRC", Kind::conventional},
    Candidate{"Comment", Kind::conventional},
    Candidate{"Grouping", Kind::conventional},
    Candidate{"Copyright", Kind::conventional},
    Candidate{"BPM", Kind::conventional},
    Candidate{"Compilation", Kind::conventional},
    Candidate{"Subtitle", Kind::conventional},
    Candidate{"Version", Kind::conventional},
    Candidate{"Language", Kind::conventional},
    Candidate{"Media", Kind::conventional},
    Candidate{"Encoder", Kind::conventional},
    Candidate{"Artist Sort", Kind::musicbrainz},
    Candidate{"Album Artist Sort", Kind::musicbrainz},
    Candidate{"Artists", Kind::musicbrainz},
    Candidate{"Album Artists", Kind::musicbrainz},
    Candidate{"MusicBrainz Artist Id", Kind::musicbrainz},
    Candidate{"MusicBrainz Album Artist Id", Kind::musicbrainz},
    Candidate{"MusicBrainz Track Id", Kind::musicbrainz},
    Candidate{"MusicBrainz Release Track Id", Kind::musicbrainz},
    Candidate{"MusicBrainz Album Id", Kind::musicbrainz},
    Candidate{"MusicBrainz Release Group Id", Kind::musicbrainz},
    Candidate{"MusicBrainz Work Id", Kind::musicbrainz},
    Candidate{"MusicBrainz Disc Id", Kind::musicbrainz},
};

[[nodiscard]] unsigned kind_rank(const Kind kind) noexcept {
    switch (kind) {
    case Kind::present:
        return 0U;
    case Kind::recent:
        return 1U;
    case Kind::conventional:
        return 2U;
    case Kind::musicbrainz:
        return 3U;
    }
    return 3U;
}

struct MatchRank {
    unsigned kind{0U};
    std::size_t detail{0U};
};

[[nodiscard]] std::optional<MatchRank> match_rank(const std::string_view candidate,
                                                  const std::string_view query) {
    if (query.empty()) {
        return MatchRank{.kind = 4U, .detail = 0U};
    }
    if (candidate == query) {
        return MatchRank{.kind = 0U, .detail = 0U};
    }
    if (candidate.starts_with(query)) {
        return MatchRank{.kind = 1U, .detail = candidate.size() - query.size()};
    }
    if (const auto offset = candidate.find(query); offset != std::string_view::npos) {
        return MatchRank{.kind = 2U, .detail = offset + candidate.size() - query.size()};
    }

    std::size_t cursor = 0U;
    std::size_t previous = 0U;
    std::size_t first = 0U;
    std::size_t gaps = 0U;
    bool matched_any = false;
    for (const auto character : query) {
        const auto found = candidate.find(character, cursor);
        if (found == std::string_view::npos) {
            return std::nullopt;
        }
        if (!matched_any) {
            first = found;
            matched_any = true;
        } else {
            gaps += found - previous - 1U;
        }
        previous = found;
        cursor = found + 1U;
    }
    return MatchRank{.kind = 3U, .detail = first + gaps + candidate.size() - query.size()};
}

struct RankedSuggestion {
    MetadataFieldSuggestion suggestion;
    MatchRank match;
    std::size_t input_order{0U};
};

} // namespace

std::span<const MetadataFieldSuggestionCandidate> metadata_field_suggestion_catalog() noexcept {
    return catalog;
}

std::vector<MetadataFieldSuggestion>
suggest_metadata_field_names(const std::string_view query,
                             const std::span<const MetadataFieldSuggestionCandidate> candidates,
                             const std::size_t maximum_results) {
    if (maximum_results == 0U) {
        return {};
    }
    const auto canonical_query = canonicalize_field_name(query);
    std::unordered_map<std::string, std::size_t> unique_positions;
    unique_positions.reserve(candidates.size());
    std::vector<RankedSuggestion> ranked;
    ranked.reserve(candidates.size());
    for (std::size_t input_order = 0U; input_order < candidates.size(); ++input_order) {
        const auto& candidate = candidates[input_order];
        auto match_name = canonicalize_field_name(candidate.display_name);
        if (match_name.empty()) {
            continue;
        }
        const auto match = match_rank(match_name, canonical_query);
        if (!match) {
            continue;
        }
        const auto property_identity = resolve_text_property_identity(candidate.display_name);
        const auto exact_native =
            candidate.kind == Kind::present
                ? !property_identity.conventional
                : candidate.kind == Kind::recent && !is_conventional_metadata_field(match_name);
        auto canonical_name =
            exact_native ? canonicalize_native_field_name(candidate.display_name) : match_name;
        auto address = std::string{exact_native ? "native:" : "logical:"} + canonical_name;
        MetadataFieldSuggestion suggestion{
            .display_name = std::string{candidate.display_name},
            .canonical_name = canonical_name,
            .kind = candidate.kind,
        };
        const auto existing = unique_positions.find(address);
        if (existing == unique_positions.end()) {
            unique_positions.emplace(std::move(address), ranked.size());
            ranked.push_back(RankedSuggestion{
                .suggestion = std::move(suggestion),
                .match = *match,
                .input_order = input_order,
            });
            continue;
        }
        auto& retained = ranked[existing->second];
        if (kind_rank(candidate.kind) < kind_rank(retained.suggestion.kind)) {
            retained.suggestion = std::move(suggestion);
            retained.match = *match;
            retained.input_order = input_order;
        }
    }

    std::ranges::stable_sort(ranked, [query_empty = canonical_query.empty()](const auto& left,
                                                                             const auto& right) {
        if (query_empty) {
            return std::tuple{kind_rank(left.suggestion.kind), left.input_order} <
                   std::tuple{kind_rank(right.suggestion.kind), right.input_order};
        }
        return std::tuple{left.match.kind, left.match.detail, kind_rank(left.suggestion.kind),
                          left.suggestion.canonical_name, left.input_order} <
               std::tuple{right.match.kind, right.match.detail, kind_rank(right.suggestion.kind),
                          right.suggestion.canonical_name, right.input_order};
    });
    if (ranked.size() > maximum_results) {
        ranked.resize(maximum_results);
    }
    std::vector<MetadataFieldSuggestion> suggestions;
    suggestions.reserve(ranked.size());
    for (auto& candidate : ranked) {
        suggestions.push_back(std::move(candidate.suggestion));
    }
    return suggestions;
}

} // namespace trackknife::metadata

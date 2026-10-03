// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/musicbrainz/album_lookup.hpp"

#include <algorithm>
#include <utility>

namespace trackknife::musicbrainz {

std::vector<std::string> releases_to_examine(const AlbumQuery& query,
                                             const ReleaseSearchResult& found,
                                             const std::size_t most) {
    const auto ranked = rank_release_candidates(query.tracks, found);
    std::vector<std::string> chosen;
    const auto take = [&](const bool same_count) {
        for (const auto& candidate : ranked) {
            const auto& release = found.releases[candidate.release_index];
            if (chosen.size() >= most) {
                return;
            }
            if ((release.track_count == query.tracks.size()) == same_count &&
                std::ranges::find(chosen, release.id) == chosen.end()) {
                chosen.push_back(release.id);
            }
        }
    };
    take(true);
    take(false);
    return chosen;
}

AlbumLookupResult judge_album(const AlbumQuery& query, std::vector<Release> releases,
                              const ClearMatchRule& rule,
                              const core::CancellationToken& cancellation) {
    AlbumLookupResult result;
    for (auto& release : releases) {
        if (cancellation.is_cancellation_requested()) {
            return {};
        }
        auto alignment = align_release_tracks(query.tracks, release, cancellation);
        // Paired in order, a release with as many tracks fits any album on
        // paper: below the confidence at which a match proposes anything for
        // a track, it does not fit at all.
        constexpr double fits_at_all = 0.5;
        if (alignment.matched_count == 0U || alignment.confidence < fits_at_all) {
            continue;
        }
        const auto clear = is_clear_match(alignment, query.tracks.size(), rule);
        result.candidates.push_back(AlbumCandidate{
            .release = std::move(release), .alignment = std::move(alignment), .clear = clear});
    }
    std::ranges::stable_sort(result.candidates, [](const AlbumCandidate& left,
                                                   const AlbumCandidate& right) {
        if (left.clear != right.clear) {
            return left.clear;
        }
        return left.alignment.confidence > right.alignment.confidence;
    });
    const auto clear = std::ranges::count_if(result.candidates, &AlbumCandidate::clear);
    result.outcome = clear == 1 ? AlbumLookupOutcome::matched
                     : result.candidates.empty() ? AlbumLookupOutcome::no_match
                                                 : AlbumLookupOutcome::needs_choice;
    return result;
}

} // namespace trackknife::musicbrainz

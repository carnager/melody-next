// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/cancellation.hpp"
#include "trackknife/musicbrainz/matching.hpp"
#include "trackknife/musicbrainz/web_service.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace trackknife::musicbrainz {

// ADR-0261: one album to look up -- the release id its files carry, if any,
// else what to search by -- and its files, in the order the album has them.
struct AlbumQuery {
    std::string release_id;
    std::string artist;
    std::string album;
    std::vector<LocalTrackDescriptor> tracks;
    // The files, as raw paths, by track: fingerprinted when there is
    // nothing to search by (ADR-0261, AcoustID).
    std::vector<std::string> paths{};

    friend bool operator==(const AlbumQuery&, const AlbumQuery&) = default;
};

// The releases of a search worth looking up, at most `most`: those with as
// many tracks as the album has files, best scored first, then the others.
[[nodiscard]] std::vector<std::string> releases_to_examine(const AlbumQuery& query,
                                                           const ReleaseSearchResult& found,
                                                           std::size_t most = 3U);

enum class AlbumLookupOutcome : std::uint8_t {
    // One release fits clearly: staged by itself.
    matched,
    // Some fit, none clearly, or more than one: a person picks.
    needs_choice,
    // None fits at all.
    no_match,
};

struct AlbumCandidate {
    Release release;
    ReleaseAlignment alignment;
    bool clear{false};

    friend bool operator==(const AlbumCandidate&, const AlbumCandidate&) = default;
};

struct AlbumLookupResult {
    AlbumLookupOutcome outcome{AlbumLookupOutcome::no_match};
    // Best first: clear ones, then by alignment confidence.
    std::vector<AlbumCandidate> candidates;

    friend bool operator==(const AlbumLookupResult&, const AlbumLookupResult&) = default;
};

// The looked-up releases aligned with the album's files and judged:
// matched when exactly one is a clear match -- two editions that both fit
// clearly differ in what would be written, so a person picks -- else a choice
// when any fits at all (its pairing at least 0.5 confident), else no match.
// Cancellation answers no match.
[[nodiscard]] AlbumLookupResult judge_album(const AlbumQuery& query, std::vector<Release> releases,
                                            const ClearMatchRule& rule = {},
                                            const core::CancellationToken& cancellation = {});

} // namespace trackknife::musicbrainz

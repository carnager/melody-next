// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/cancellation.hpp"
#include "trackknife/musicbrainz/web_service.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace trackknife::musicbrainz {

// What the matcher may know about one local file. Every field is optional
// knowledge, never invented: absent detail simply cannot corroborate.
struct LocalTrackDescriptor {
    std::string title;
    std::string artist;
    std::string album;
    std::optional<std::size_t> track_number;
    std::optional<std::size_t> disc_number;
    std::optional<std::int64_t> duration_ms;

    friend bool operator==(const LocalTrackDescriptor&, const LocalTrackDescriptor&) = default;
};

struct RankedRelease {
    std::size_t release_index{0U};
    int score{0};

    friend bool operator==(const RankedRelease&, const RankedRelease&) = default;
};

// Orders search candidates for presentation: the MusicBrainz search score,
// corroborated by an exact track-count match against the selection. Every
// candidate stays visible — ranking never filters versions away.
[[nodiscard]] std::vector<RankedRelease>
rank_release_candidates(std::span<const LocalTrackDescriptor> local_tracks,
                        const ReleaseSearchResult& candidates);

// One release track flattened across media, keeping its disc context.
struct FlattenedReleaseTrack {
    std::size_t medium_index{0U};
    std::size_t medium_position{0U};
    std::size_t medium_track_count{0U};
    std::size_t track_index_in_medium{0U};
    std::string medium_format;
    std::string medium_title;
    ReleaseTrack track;

    friend bool operator==(const FlattenedReleaseTrack&, const FlattenedReleaseTrack&) = default;
};

struct TrackAlignment {
    std::size_t local_index{0U};
    // Index into the flattened release track list; absent when no release
    // track could be assigned.
    std::optional<std::size_t> release_track_index;
    // [0, 1]: automatic agreement, or 1 for an explicitly confirmed assignment.
    double confidence{0.0};
    bool user_confirmed{false};

    friend bool operator==(const TrackAlignment&, const TrackAlignment&) = default;
};

struct ReleaseAlignment {
    std::vector<FlattenedReleaseTrack> release_tracks;
    std::vector<TrackAlignment> tracks;
    std::size_t matched_count{0U};
    // Mean per-track confidence over the local files, zero-counting the
    // unmatched ones, with a penalty when counts disagree.
    double confidence{0.0};

    friend bool operator==(const ReleaseAlignment&, const ReleaseAlignment&) = default;
};

// Assigns local files to a looked-up release's tracks. Preference order:
// exact (disc, track-number) permutation, then plain order when counts
// match, then a conservative greedy assignment by title similarity and
// duration proximity. Assignments are never duplicated and never invented —
// a file that fits nothing stays unmatched with zero confidence. Cancellation
// returns an empty alignment; callers must discard it.
[[nodiscard]] ReleaseAlignment
align_release_tracks(std::span<const LocalTrackDescriptor> local_tracks, const Release& release,
                     const core::CancellationToken& cancellation = {});

// Explicitly reviewed assignments may include untagged or unmatched files.
// Each release track can be assigned once; null entries stage nothing.
[[nodiscard]] core::Result<ReleaseAlignment>
confirm_release_mapping(ReleaseAlignment alignment,
                        std::span<const std::optional<std::size_t>> assignments);

} // namespace trackknife::musicbrainz

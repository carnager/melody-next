// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/musicbrainz/matching.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace trackknife::musicbrainz {

core::Result<ReleaseAlignment>
confirm_release_mapping(ReleaseAlignment alignment,
                        const std::span<const std::optional<std::size_t>> assignments) {
    const auto invalid = [](const char* message) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::invalid_argument, .message = message, .context = {}});
    };
    if (assignments.size() != alignment.tracks.size()) {
        return invalid("manual matching needs one assignment per local file");
    }
    std::set<std::size_t> used;
    alignment.matched_count = 0U;
    for (std::size_t local = 0; local < assignments.size(); ++local) {
        const auto target = assignments[local];
        if (target &&
            (*target >= alignment.release_tracks.size() || !used.insert(*target).second)) {
            return invalid("manual matching has a duplicate or out-of-range release track");
        }
        alignment.tracks[local] = TrackAlignment{.local_index = local,
                                                 .release_track_index = target,
                                                 .confidence = target ? 1.0 : 0.0,
                                                 .user_confirmed = target.has_value()};
        alignment.matched_count += target ? 1U : 0U;
    }
    alignment.confidence = assignments.empty() ? 0.0
                                               : static_cast<double>(alignment.matched_count) /
                                                     static_cast<double>(assignments.size());
    return alignment;
}

namespace {

constexpr std::size_t maximum_similarity_bytes = 512U;
constexpr std::int64_t close_duration_ms = 3'000;
constexpr std::int64_t plausible_duration_ms = 10'000;

// ASCII-folded, whitespace-collapsed comparison key. Unicode-aware folding
// is deliberately out of scope: a miss only lowers similarity, it never
// fabricates a match.
[[nodiscard]] std::string normalized(const std::string_view text) {
    std::string result;
    result.reserve(std::min(text.size(), maximum_similarity_bytes));
    auto pending_space = false;
    for (const auto character : text) {
        if (result.size() >= maximum_similarity_bytes) {
            break;
        }
        const auto lowered = character >= 'A' && character <= 'Z'
                                 ? static_cast<char>(character - 'A' + 'a')
                                 : character;
        if (std::isspace(static_cast<unsigned char>(lowered)) != 0) {
            pending_space = !result.empty();
            continue;
        }
        if (pending_space) {
            result.push_back(' ');
            pending_space = false;
        }
        result.push_back(lowered);
    }
    return result;
}

[[nodiscard]] double similarity(const std::string_view left_text,
                                const std::string_view right_text) {
    const auto left = normalized(left_text);
    const auto right = normalized(right_text);
    if (left.empty() || right.empty()) {
        return 0.0;
    }
    if (left == right) {
        return 1.0;
    }
    // Bounded Levenshtein ratio.
    const auto columns = right.size() + 1U;
    std::vector<std::size_t> previous(columns);
    std::vector<std::size_t> current(columns);
    for (std::size_t column = 0U; column < columns; ++column) {
        previous[column] = column;
    }
    for (std::size_t row = 1U; row <= left.size(); ++row) {
        current[0] = row;
        for (std::size_t column = 1U; column < columns; ++column) {
            const auto substitution =
                previous[column - 1U] + (left[row - 1U] == right[column - 1U] ? 0U : 1U);
            current[column] =
                std::min({previous[column] + 1U, current[column - 1U] + 1U, substitution});
        }
        std::swap(previous, current);
    }
    const auto distance = previous[columns - 1U];
    const auto longest = std::max(left.size(), right.size());
    return longest == 0U ? 0.0 : 1.0 - static_cast<double>(distance) / static_cast<double>(longest);
}

[[nodiscard]] double duration_agreement(const std::optional<std::int64_t> local,
                                        const std::optional<std::int64_t> release) {
    if (!local || !release) {
        return 0.5; // unknown neither helps nor hurts
    }
    const auto delta = std::llabs(*local - *release);
    if (delta <= close_duration_ms) {
        return 1.0;
    }
    if (delta <= plausible_duration_ms) {
        return 0.5;
    }
    return 0.0;
}

[[nodiscard]] double pair_confidence(const LocalTrackDescriptor& local,
                                     const FlattenedReleaseTrack& release_track,
                                     const bool position_agrees) {
    const auto title = similarity(local.title, release_track.track.title);
    const auto duration = duration_agreement(local.duration_ms, release_track.track.length_ms);
    const auto position = position_agrees ? 1.0 : 0.0;
    return 0.6 * title + 0.25 * duration + 0.15 * position;
}

[[nodiscard]] bool position_matches(const LocalTrackDescriptor& local,
                                    const FlattenedReleaseTrack& release_track,
                                    const std::size_t media_count) {
    if (!local.track_number || *local.track_number != release_track.track.position) {
        return false;
    }
    if (media_count > 1U && local.disc_number &&
        *local.disc_number != release_track.medium_position) {
        return false;
    }
    return true;
}

[[nodiscard]] std::vector<FlattenedReleaseTrack> flatten(const Release& release) {
    std::vector<FlattenedReleaseTrack> flattened;
    for (std::size_t medium_index = 0U; medium_index < release.media.size(); ++medium_index) {
        const auto& medium = release.media[medium_index];
        const auto track_count = medium.tracks.empty() ? medium.track_count : medium.tracks.size();
        for (std::size_t track_index = 0U; track_index < medium.tracks.size(); ++track_index) {
            flattened.push_back(FlattenedReleaseTrack{
                .medium_index = medium_index,
                .medium_position = medium.position == 0U ? medium_index + 1U : medium.position,
                .medium_track_count = track_count,
                .track_index_in_medium = track_index,
                .medium_format = medium.format,
                .medium_title = medium.title,
                .track = medium.tracks[track_index],
            });
        }
    }
    return flattened;
}

} // namespace

std::vector<RankedRelease>
rank_release_candidates(const std::span<const LocalTrackDescriptor> local_tracks,
                        const ReleaseSearchResult& candidates) {
    std::vector<RankedRelease> ranked;
    ranked.reserve(candidates.releases.size());
    for (std::size_t index = 0U; index < candidates.releases.size(); ++index) {
        const auto& release = candidates.releases[index];
        auto score = release.search_score;
        if (!local_tracks.empty() && release.track_count == local_tracks.size()) {
            score += 15;
        }
        ranked.push_back(RankedRelease{.release_index = index, .score = score});
    }
    std::ranges::stable_sort(ranked, [](const RankedRelease& left, const RankedRelease& right) {
        return left.score > right.score;
    });
    return ranked;
}

ReleaseAlignment align_release_tracks(const std::span<const LocalTrackDescriptor> local_tracks,
                                      const Release& release,
                                      const core::CancellationToken& cancellation) {
    ReleaseAlignment alignment{
        .release_tracks = flatten(release),
        .tracks = {},
        .matched_count = 0U,
        .confidence = 0.0,
    };
    alignment.tracks.reserve(local_tracks.size());
    if (local_tracks.empty() || alignment.release_tracks.empty()) {
        for (std::size_t local_index = 0U; local_index < local_tracks.size(); ++local_index) {
            if (cancellation.is_cancellation_requested()) {
                return {};
            }
            alignment.tracks.push_back(TrackAlignment{.local_index = local_index,
                                                      .release_track_index = std::nullopt,
                                                      .confidence = 0.0});
        }
        return alignment;
    }

    const auto media_count = release.media.size();
    std::vector<std::optional<std::size_t>> assignment(local_tracks.size());

    // 1) Exact (disc, track-number) permutation: every local file names a
    // distinct existing release position.
    {
        std::set<std::size_t> used;
        auto complete = true;
        std::vector<std::optional<std::size_t>> by_number(local_tracks.size());
        for (std::size_t local_index = 0U; local_index < local_tracks.size(); ++local_index) {
            if (cancellation.is_cancellation_requested()) {
                return {};
            }
            const auto& local = local_tracks[local_index];
            std::optional<std::size_t> found;
            for (std::size_t release_index = 0U; release_index < alignment.release_tracks.size();
                 ++release_index) {
                if (cancellation.is_cancellation_requested()) {
                    return {};
                }
                if (used.contains(release_index)) {
                    continue;
                }
                if (position_matches(local, alignment.release_tracks[release_index], media_count)) {
                    found = release_index;
                    break;
                }
            }
            if (!found) {
                complete = false;
                break;
            }
            used.insert(*found);
            by_number[local_index] = *found;
        }
        if (complete) {
            assignment = std::move(by_number);
        }
    }

    // 2) Plain order when the counts agree.
    if (assignment.front() == std::nullopt &&
        std::ranges::all_of(assignment, [](const auto& value) { return !value.has_value(); }) &&
        local_tracks.size() == alignment.release_tracks.size()) {
        for (std::size_t index = 0U; index < local_tracks.size(); ++index) {
            assignment[index] = index;
        }
    }

    // 3) Conservative greedy by title similarity and duration proximity.
    if (std::ranges::all_of(assignment, [](const auto& value) { return !value.has_value(); })) {
        struct Pair {
            std::size_t local_index;
            std::size_t release_index;
            double score;
        };
        std::vector<Pair> pairs;
        pairs.reserve(local_tracks.size() * alignment.release_tracks.size());
        for (std::size_t local_index = 0U; local_index < local_tracks.size(); ++local_index) {
            if (cancellation.is_cancellation_requested()) {
                return {};
            }
            for (std::size_t release_index = 0U; release_index < alignment.release_tracks.size();
                 ++release_index) {
                if (cancellation.is_cancellation_requested()) {
                    return {};
                }
                const auto& release_track = alignment.release_tracks[release_index];
                const auto score = pair_confidence(
                    local_tracks[local_index], release_track,
                    position_matches(local_tracks[local_index], release_track, media_count));
                if (score >= 0.5) {
                    pairs.push_back(Pair{.local_index = local_index,
                                         .release_index = release_index,
                                         .score = score});
                }
            }
        }
        std::ranges::stable_sort(
            pairs, [](const Pair& left, const Pair& right) { return left.score > right.score; });
        std::set<std::size_t> used;
        for (const auto& pair : pairs) {
            if (assignment[pair.local_index].has_value() || used.contains(pair.release_index)) {
                continue;
            }
            assignment[pair.local_index] = pair.release_index;
            used.insert(pair.release_index);
        }
    }

    if (cancellation.is_cancellation_requested()) {
        return {};
    }
    double total = 0.0;
    for (std::size_t local_index = 0U; local_index < local_tracks.size(); ++local_index) {
        TrackAlignment track{.local_index = local_index,
                             .release_track_index = assignment[local_index],
                             .confidence = 0.0};
        if (track.release_track_index) {
            const auto& release_track = alignment.release_tracks[*track.release_track_index];
            track.confidence = pair_confidence(
                local_tracks[local_index], release_track,
                position_matches(local_tracks[local_index], release_track, media_count));
            ++alignment.matched_count;
        }
        total += track.confidence;
        alignment.tracks.push_back(track);
    }
    alignment.confidence = total / static_cast<double>(local_tracks.size());
    if (local_tracks.size() != alignment.release_tracks.size()) {
        alignment.confidence *= 0.85;
    }
    return alignment;
}

} // namespace trackknife::musicbrainz

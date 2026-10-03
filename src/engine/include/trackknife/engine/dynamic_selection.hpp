// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/cancellation.hpp"
#include "trackknife/core/result.hpp"

#include <cstddef>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace trackknife::engine {

class Catalogue;

// ADR-0258: what a dynamic playlist's library rule picks from what its query
// matches.
struct DynamicSelection {
    // tkq-1, as a rule saves it.
    std::string query;
    // At most this many tracks, 1--500.
    std::size_t limit{100U};
    bool shuffle{false};
    // tkfmt-1 text to group the matches by, and how many groups to pick at
    // random -- each with the same odds, whatever its size; 0 groups: none.
    std::string group_by;
    std::size_t groups{0U};
    // How many tracks from each group, at random, kept in library order; 0:
    // all of them.
    std::size_t per_group{0U};

    friend bool operator==(const DynamicSelection&, const DynamicSelection&) = default;
};

inline constexpr std::size_t dynamic_selection_limit = 500U;

// One match: its file, and the text its group is known by.
struct DynamicMatch {
    std::string raw_path;
    std::string group;
};

// The choosing itself, from the matches in library order: with groups, that
// many chosen, each with the same odds, and from each that many tracks (or
// all), in the order found; shuffled when asked; at most the limit. What
// `exclude` names is never chosen.
[[nodiscard]] std::vector<std::string> pick_dynamic(std::vector<DynamicMatch> matches,
                                                    const DynamicSelection& selection,
                                                    const std::set<std::string>& exclude,
                                                    std::mt19937& random);

struct DynamicSelected {
    std::vector<std::string> paths;
    // How many tracks the query matched, before choosing.
    std::size_t matched{0U};
};

// Runs the rule's query on `catalogue` and picks from its matches.
[[nodiscard]] core::Result<DynamicSelected>
select_dynamic(const Catalogue& catalogue, const DynamicSelection& selection,
               const std::set<std::string>& exclude, std::mt19937& random,
               const core::CancellationToken& cancellation = {});

} // namespace trackknife::engine

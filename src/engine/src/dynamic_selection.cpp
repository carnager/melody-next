// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/engine/dynamic_selection.hpp"

#include "trackknife/engine/catalogue.hpp"
#include "trackknife/query/tkq.hpp"

#include <algorithm>
#include <unordered_map>
#include <utility>

namespace trackknife::engine {

std::vector<std::string> pick_dynamic(std::vector<DynamicMatch> matches,
                                      const DynamicSelection& selection,
                                      const std::set<std::string>& exclude, std::mt19937& random) {
    std::erase_if(matches, [&exclude](const DynamicMatch& match) {
        return exclude.contains(match.raw_path);
    });
    const auto limit = std::min(selection.limit, dynamic_selection_limit);
    std::vector<std::string> chosen;
    if (selection.groups > 0U) {
        // Groups in the order first met, each its matches in library order.
        std::vector<std::vector<std::string>> groups;
        std::unordered_map<std::string, std::size_t> group_of;
        for (auto& match : matches) {
            const auto [found, added] = group_of.emplace(match.group, groups.size());
            if (added) {
                groups.emplace_back();
            }
            groups[found->second].push_back(std::move(match.raw_path));
        }
        // Groups chosen at random, each with the same odds.
        std::vector<std::size_t> order(groups.size());
        for (std::size_t index = 0; index < order.size(); ++index) {
            order[index] = index;
        }
        std::ranges::shuffle(order, random);
        order.resize(std::min(order.size(), selection.groups));
        for (const auto index : order) {
            auto& members = groups[index];
            if (selection.per_group > 0U && members.size() > selection.per_group) {
                // That many at random, kept in the order found.
                std::vector<std::size_t> taken(members.size());
                for (std::size_t at = 0; at < taken.size(); ++at) {
                    taken[at] = at;
                }
                std::ranges::shuffle(taken, random);
                taken.resize(selection.per_group);
                std::ranges::sort(taken);
                for (const auto at : taken) {
                    chosen.push_back(std::move(members[at]));
                }
            } else {
                std::ranges::move(members, std::back_inserter(chosen));
            }
        }
    } else {
        chosen.reserve(matches.size());
        for (auto& match : matches) {
            chosen.push_back(std::move(match.raw_path));
        }
        if (selection.shuffle) {
            // A sample, not the first ones shuffled.
            std::ranges::shuffle(chosen, random);
        }
        if (chosen.size() > limit) {
            chosen.resize(limit);
        }
        return chosen;
    }
    if (selection.shuffle) {
        std::ranges::shuffle(chosen, random);
    }
    if (chosen.size() > limit) {
        chosen.resize(limit);
    }
    return chosen;
}

core::Result<DynamicSelected> select_dynamic(const Catalogue& catalogue,
                                             const DynamicSelection& selection,
                                             const std::set<std::string>& exclude,
                                             std::mt19937& random,
                                             const core::CancellationToken& cancellation) {
    if (selection.groups > 0U && selection.group_by.empty()) {
        return std::unexpected(core::Error{.code = core::ErrorCode::invalid_argument,
                                           .message = "groups need an expression to group by",
                                           .context = {}});
    }
    auto compiled = query::compile_tkq(selection.query);
    if (!compiled) {
        return std::unexpected(std::move(compiled.error()));
    }
    std::vector<DynamicMatch> matches;
    if (selection.groups > 0U) {
        // Each match's group text, made where the tags are.
        auto found = catalogue.find(*compiled, selection.group_by, 0U, cancellation);
        if (!found) {
            return std::unexpected(std::move(found.error()));
        }
        matches.reserve(found->size());
        for (auto& track : *found) {
            matches.push_back(
                {.raw_path = std::move(track.raw_path), .group = std::move(track.text)});
        }
    } else {
        auto paths = catalogue.filter_paths(*compiled, cancellation);
        if (!paths) {
            return std::unexpected(std::move(paths.error()));
        }
        matches.reserve(paths->size());
        for (auto& path : *paths) {
            matches.push_back({.raw_path = std::move(path), .group = {}});
        }
    }
    DynamicSelected selected;
    selected.matched = matches.size();
    selected.paths = pick_dynamic(std::move(matches), selection, exclude, random);
    return selected;
}

} // namespace trackknife::engine

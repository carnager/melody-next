// SPDX-License-Identifier: GPL-3.0-only

// ADR-0258: what a dynamic playlist's rule picks from its matches.

#include "trackknife/engine/dynamic_selection.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <map>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace engine = trackknife::engine;

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

// Artist "big" with 400 tracks and nine with 10 each, in library order.
[[nodiscard]] std::vector<engine::DynamicMatch> library() {
    std::vector<engine::DynamicMatch> matches;
    for (int index = 0; index < 400; ++index) {
        matches.push_back({.raw_path = "big/" + std::to_string(index), .group = "big"});
    }
    for (int artist = 0; artist < 9; ++artist) {
        for (int index = 0; index < 10; ++index) {
            const auto name = "small" + std::to_string(artist);
            matches.push_back({.raw_path = name + "/" + std::to_string(index), .group = name});
        }
    }
    return matches;
}

[[nodiscard]] std::string group_of(const std::string& path) {
    return path.substr(0, path.find('/'));
}

} // namespace

int main() {
    std::mt19937 random{20261003U};

    // No groups: shuffled, a sample of the limit; not shuffled, the first.
    {
        const engine::DynamicSelection plain{.query = "ALL",
                                             .limit = 5U,
                                             .shuffle = false,
                                             .group_by = {},
                                             .groups = 0U,
                                             .per_group = 0U};
        const auto first = engine::pick_dynamic(library(), plain, {}, random);
        require(first == (std::vector<std::string>{"big/0", "big/1", "big/2", "big/3", "big/4"}),
                "unshuffled, the first in library order");
        auto shuffled = plain;
        shuffled.shuffle = true;
        shuffled.limit = 50U;
        const auto sample = engine::pick_dynamic(library(), shuffled, {}, random);
        require(sample.size() == 50U, "shuffled, as many as the limit");
        require(
            std::ranges::any_of(sample, [](const auto& path) { return group_of(path) != "big"; }),
            "drawn from the whole match, not its first tracks");
    }

    // Groups: each artist the same odds, whatever its size.
    {
        const engine::DynamicSelection one_artist{.query = "ALL",
                                                  .limit = 500U,
                                                  .shuffle = false,
                                                  .group_by = "%artist%",
                                                  .groups = 1U,
                                                  .per_group = 0U};
        std::map<std::string, int> picked;
        for (int round = 0; round < 2000; ++round) {
            const auto chosen = engine::pick_dynamic(library(), one_artist, {}, random);
            require(!chosen.empty(), "a group is chosen");
            ++picked[group_of(chosen.front())];
            const auto group = group_of(chosen.front());
            require(std::ranges::all_of(
                        chosen, [&group](const auto& path) { return group_of(path) == group; }),
                    "one group, whole");
            require(chosen.size() == (group == "big" ? 400U : 10U), "all of its tracks");
            require(std::ranges::is_sorted(chosen,
                                           [](const auto& a, const auto& b) {
                                               return std::stoi(a.substr(a.find('/') + 1)) <
                                                      std::stoi(b.substr(b.find('/') + 1));
                                           }),
                    "in library order");
        }
        // 2000 draws over 10 groups: about 200 each; the big one is no likelier.
        require(picked["big"] > 120 && picked["big"] < 280, "the big artist is as likely as any");
        require(picked.size() == 10U, "every artist comes up");
    }

    // Ten artists, three each, shuffled: thirty tracks, three per artist.
    {
        const engine::DynamicSelection mix{.query = "ALL",
                                           .limit = 500U,
                                           .shuffle = true,
                                           .group_by = "%artist%",
                                           .groups = 10U,
                                           .per_group = 3U};
        const auto chosen = engine::pick_dynamic(library(), mix, {}, random);
        require(chosen.size() == 30U, "ten groups of three");
        std::map<std::string, int> per;
        for (const auto& path : chosen) {
            ++per[group_of(path)];
        }
        require(per.size() == 10U &&
                    std::ranges::all_of(per, [](const auto& each) { return each.second == 3; }),
                "three from each of ten");
        auto capped = mix;
        capped.limit = 7U;
        require(engine::pick_dynamic(library(), capped, {}, random).size() == 7U,
                "the limit holds");
        auto too_many = mix;
        too_many.groups = 50U;
        require(engine::pick_dynamic(library(), too_many, {}, random).size() == 30U,
                "no more groups than there are");
    }

    // What is excluded is never chosen.
    {
        std::set<std::string> exclude;
        for (int index = 0; index < 10; ++index) {
            exclude.insert("small0/" + std::to_string(index));
        }
        const engine::DynamicSelection all_groups{.query = "ALL",
                                                  .limit = 500U,
                                                  .shuffle = false,
                                                  .group_by = "%artist%",
                                                  .groups = 10U,
                                                  .per_group = 1U};
        const auto chosen = engine::pick_dynamic(library(), all_groups, exclude, random);
        require(chosen.size() == 9U, "an artist with nothing left is no group");
        require(std::ranges::none_of(
                    chosen, [&exclude](const auto& path) { return exclude.contains(path); }),
                "nothing excluded");
    }
    std::cout << "dynamic selection tests passed\n";
    return EXIT_SUCCESS;
}

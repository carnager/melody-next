// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/persistence/list_edits.hpp"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <list>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace trackknife::persistence {
namespace detail {
core::Error edit_refused(const core::ErrorCode code, std::string message) {
    return core::Error{.code = code, .message = std::move(message), .context = {}};
}
} // namespace detail

namespace {
using IdHash = detail::IdHash;
} // namespace

core::Result<void> apply_list_edits(std::vector<EngineListItem>& items,
                                    const std::vector<ListEdit>& edits) {
    return apply_list_edits(
        items, edits, [](const EngineListItem& item) { return item.entry_id; },
        [](const EngineListItem& item) { return item; });
}

std::vector<PlannedListEdit> plan_list_edits(const std::vector<ListEntryPrint>& before,
                                             const std::vector<ListEntryPrint>& wanted) {
    std::vector<PlannedListEdit> planned;
    std::unordered_map<core::StableId, std::size_t, IdHash> was;
    was.reserve(before.size());
    for (std::size_t index = 0; index < before.size(); ++index) {
        was.emplace(before[index].entry, index);
    }
    std::unordered_set<core::StableId, IdHash> kept;
    kept.reserve(wanted.size());
    for (const auto& entry : wanted) {
        kept.insert(entry.entry);
    }

    PlannedListEdit removal{.kind = ListEdit::Kind::remove, .entries = {}, .rows = {}, .after = {}};
    for (const auto& entry : before) {
        if (!kept.contains(entry.entry)) {
            removal.entries.push_back(entry.entry);
        }
    }
    if (!removal.entries.empty()) {
        planned.push_back(std::move(removal));
    }

    // The entries both hold, in the order wanted, by where they were. The
    // longest increasing run of those places stays; everything else moves.
    std::vector<std::size_t> common_rows;
    std::vector<std::size_t> places;
    for (std::size_t row = 0; row < wanted.size(); ++row) {
        if (const auto found = was.find(wanted[row].entry); found != was.end()) {
            common_rows.push_back(row);
            places.push_back(found->second);
        }
    }
    // Patience sorting: tails[k] is the index into `places` ending the best
    // run of length k + 1 found so far; back links rebuild it.
    std::vector<std::size_t> tails;
    std::vector<std::size_t> previous(places.size(), places.size());
    for (std::size_t index = 0; index < places.size(); ++index) {
        const auto slot = std::ranges::lower_bound(
            tails, places[index], {}, [&places](std::size_t at) { return places[at]; });
        const auto length = static_cast<std::size_t>(slot - tails.begin());
        if (length > 0U) {
            previous[index] = tails[length - 1U];
        }
        if (slot == tails.end()) {
            tails.push_back(index);
        } else {
            *slot = index;
        }
    }
    std::vector<bool> stays(wanted.size(), false);
    if (!tails.empty()) {
        for (auto at = tails.back(); at != places.size(); at = previous[at]) {
            stays[common_rows[at]] = true;
        }
    }

    // Runs of new or moved entries, each after the entry before it as wanted:
    // that one is either staying or was placed by an earlier run.
    std::optional<core::StableId> after;
    for (std::size_t row = 0; row < wanted.size();) {
        if (stays[row]) {
            after = wanted[row].entry;
            ++row;
            continue;
        }
        const bool fresh = !was.contains(wanted[row].entry);
        PlannedListEdit run{.kind = fresh ? ListEdit::Kind::insert : ListEdit::Kind::move,
                            .entries = {},
                            .rows = {},
                            .after = after};
        while (row < wanted.size() && !stays[row] && !was.contains(wanted[row].entry) == fresh) {
            if (fresh) {
                run.rows.push_back(row);
            } else {
                run.entries.push_back(wanted[row].entry);
            }
            after = wanted[row].entry;
            ++row;
        }
        planned.push_back(std::move(run));
    }

    PlannedListEdit update{.kind = ListEdit::Kind::update, .entries = {}, .rows = {}, .after = {}};
    for (std::size_t row = 0; row < wanted.size(); ++row) {
        if (const auto found = was.find(wanted[row].entry);
            found != was.end() && before[found->second].print != wanted[row].print) {
            update.rows.push_back(row);
        }
    }
    if (!update.rows.empty()) {
        planned.push_back(std::move(update));
    }
    return planned;
}

} // namespace trackknife::persistence

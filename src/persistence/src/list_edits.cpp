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
namespace {

struct IdHash {
    [[nodiscard]] std::size_t operator()(const core::StableId& id) const noexcept {
        // FNV-1a over the bytes: identities are random, but nothing here
        // depends on that.
        std::uint64_t hash = 14695981039346656037ULL;
        for (const auto byte : id.bytes()) {
            hash = (hash ^ byte) * 1099511628211ULL;
        }
        return static_cast<std::size_t>(hash);
    }
};

[[nodiscard]] core::Error refused(core::ErrorCode code, std::string message) {
    return core::Error{.code = code, .message = std::move(message), .context = {}};
}

} // namespace

core::Result<void> apply_list_edits(std::vector<EngineListItem>& items,
                                    const std::vector<ListEdit>& edits) {
    // Worked on a linked copy, so a refusal halfway leaves `items` untouched
    // and each edit costs what it names rather than the length of the list.
    std::list<EngineListItem> working{std::make_move_iterator(items.begin()),
                                      std::make_move_iterator(items.end())};
    items.clear();
    std::unordered_map<core::StableId, std::list<EngineListItem>::iterator, IdHash> where;
    where.reserve(working.size());
    for (auto at = working.begin(); at != working.end(); ++at) {
        where.emplace(at->entry_id, at);
    }
    const auto restore = [&items, &working] {
        items.assign(std::make_move_iterator(working.begin()),
                     std::make_move_iterator(working.end()));
    };
    // Where something placed after `after` goes.
    const auto position_after =
        [&](const std::optional<core::StableId>& after) -> core::Result<std::list<EngineListItem>::iterator> {
        if (!after) {
            return working.begin();
        }
        const auto found = where.find(*after);
        if (found == where.end()) {
            return std::unexpected(
                refused(core::ErrorCode::not_found, "an edit is placed after an entry the list does not hold"));
        }
        return std::next(found->second);
    };
    // The list as it was, for a refusal: edits already applied are undone
    // by starting again from a copy taken first.
    const std::list<EngineListItem> original = working;
    const auto refuse = [&](core::Error error) -> core::Result<void> {
        items.assign(original.begin(), original.end());
        return std::unexpected(std::move(error));
    };
    for (const auto& edit : edits) {
        switch (edit.kind) {
        case ListEdit::Kind::remove: {
            std::unordered_set<core::StableId, IdHash> named;
            for (const auto& entry : edit.entries) {
                if (!named.insert(entry).second) {
                    return refuse(refused(core::ErrorCode::invalid_argument, "an edit names an entry twice"));
                }
                const auto found = where.find(entry);
                if (found == where.end()) {
                    return refuse(refused(core::ErrorCode::not_found,
                                          "an edit removes an entry the list does not hold"));
                }
                working.erase(found->second);
                where.erase(found);
            }
            break;
        }
        case ListEdit::Kind::insert: {
            auto at = position_after(edit.after);
            if (!at) {
                return refuse(std::move(at.error()));
            }
            for (const auto& item : edit.items) {
                if (item.entry_id.is_nil() || where.contains(item.entry_id)) {
                    return refuse(refused(core::ErrorCode::invalid_argument,
                                          "an inserted entry needs an identity the list does not hold"));
                }
                const auto placed = working.insert(*at, item);
                where.emplace(item.entry_id, placed);
            }
            break;
        }
        case ListEdit::Kind::move: {
            std::unordered_set<core::StableId, IdHash> named;
            for (const auto& entry : edit.entries) {
                if (!named.insert(entry).second) {
                    return refuse(refused(core::ErrorCode::invalid_argument, "an edit names an entry twice"));
                }
                if (!where.contains(entry)) {
                    return refuse(refused(core::ErrorCode::not_found,
                                          "an edit moves an entry the list does not hold"));
                }
            }
            if (edit.after && named.contains(*edit.after)) {
                return refuse(refused(core::ErrorCode::invalid_argument,
                                      "entries cannot be moved after one of themselves"));
            }
            // Taken out first, so the anchor is found where it is once they
            // have gone.
            std::list<EngineListItem> taken;
            for (const auto& entry : edit.entries) {
                const auto found = where.find(entry);
                taken.splice(taken.end(), working, found->second);
            }
            auto at = position_after(edit.after);
            if (!at) {
                return refuse(std::move(at.error()));
            }
            working.splice(*at, taken);
            break;
        }
        case ListEdit::Kind::update: {
            for (const auto& item : edit.items) {
                const auto found = where.find(item.entry_id);
                if (found == where.end()) {
                    return refuse(refused(core::ErrorCode::not_found,
                                          "an edit updates an entry the list does not hold"));
                }
                *found->second = item;
            }
            break;
        }
        }
    }
    restore();
    return {};
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
        const auto slot = std::ranges::lower_bound(tails, places[index], {},
                                                   [&places](std::size_t at) { return places[at]; });
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

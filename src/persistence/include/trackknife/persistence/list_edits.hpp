// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "trackknife/core/result.hpp"
#include "trackknife/core/stable_id.hpp"
#include "trackknife/persistence/list_repository.hpp"

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <list>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace trackknife::persistence {

// ADR-0256: a change to a list, by entry identity. A list is sent whole once,
// when it is made, and after that only as these.
struct ListEdit {
    enum class Kind : std::uint8_t {
        // `entries` go.
        remove,
        // `items`, new, placed after `after` -- first when it is empty.
        insert,
        // `entries`, held already, taken out and placed after `after` in the
        // order given.
        move,
        // `items` replace the entries of the same identity where they are.
        update,
    };
    Kind kind{Kind::remove};
    std::vector<core::StableId> entries;
    std::vector<EngineListItem> items;
    std::optional<core::StableId> after;

    friend bool operator==(const ListEdit&, const ListEdit&) = default;
};

namespace detail {
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
[[nodiscard]] core::Error edit_refused(core::ErrorCode code, std::string message);
} // namespace detail

// Applies `edits` in order to a list of anything with an identity:
// `identity(element)` names one, and `make(item)` is what an inserted or
// updated item becomes. An entry or anchor the list does not hold, an insert
// of one it does, or an entry named twice refuses all of them and leaves
// `list` as it was. Each edit costs what it names, not the list's length.
template <typename Element, typename Identity, typename Make>
[[nodiscard]] core::Result<void> apply_list_edits(std::vector<Element>& list,
                                                  const std::vector<ListEdit>& edits,
                                                  Identity identity, Make make) {
    using Working = std::list<Element>;
    Working working{list.begin(), list.end()};
    std::unordered_map<core::StableId, typename Working::iterator, detail::IdHash> where;
    where.reserve(working.size());
    for (auto at = working.begin(); at != working.end(); ++at) {
        where.emplace(identity(*at), at);
    }
    const auto position_after = [&](const std::optional<core::StableId>& after)
        -> core::Result<typename Working::iterator> {
        if (!after) {
            return working.begin();
        }
        const auto found = where.find(*after);
        if (found == where.end()) {
            return std::unexpected(detail::edit_refused(
                core::ErrorCode::not_found, "an edit is placed after an entry the list does not hold"));
        }
        return std::next(found->second);
    };
    for (const auto& edit : edits) {
        switch (edit.kind) {
        case ListEdit::Kind::remove: {
            std::unordered_set<core::StableId, detail::IdHash> named;
            for (const auto& entry : edit.entries) {
                if (!named.insert(entry).second) {
                    return std::unexpected(detail::edit_refused(core::ErrorCode::invalid_argument,
                                                                "an edit names an entry twice"));
                }
                const auto found = where.find(entry);
                if (found == where.end()) {
                    return std::unexpected(detail::edit_refused(
                        core::ErrorCode::not_found, "an edit removes an entry the list does not hold"));
                }
                working.erase(found->second);
                where.erase(found);
            }
            break;
        }
        case ListEdit::Kind::insert: {
            auto at = position_after(edit.after);
            if (!at) {
                return std::unexpected(std::move(at.error()));
            }
            for (const auto& item : edit.items) {
                if (item.entry_id.is_nil() || where.contains(item.entry_id)) {
                    return std::unexpected(detail::edit_refused(
                        core::ErrorCode::invalid_argument,
                        "an inserted entry needs an identity the list does not hold"));
                }
                where.emplace(item.entry_id, working.insert(*at, make(item)));
            }
            break;
        }
        case ListEdit::Kind::move: {
            std::unordered_set<core::StableId, detail::IdHash> named;
            std::vector<typename Working::iterator> moving;
            moving.reserve(edit.entries.size());
            for (const auto& entry : edit.entries) {
                if (!named.insert(entry).second) {
                    return std::unexpected(detail::edit_refused(core::ErrorCode::invalid_argument,
                                                                "an edit names an entry twice"));
                }
                const auto found = where.find(entry);
                if (found == where.end()) {
                    return std::unexpected(detail::edit_refused(
                        core::ErrorCode::not_found, "an edit moves an entry the list does not hold"));
                }
                moving.push_back(found->second);
            }
            if (edit.after && named.contains(*edit.after)) {
                return std::unexpected(detail::edit_refused(
                    core::ErrorCode::invalid_argument, "entries cannot be moved after one of themselves"));
            }
            // Taken out first, so the anchor is found where it is once they
            // have gone.
            Working taken;
            for (const auto at : moving) {
                taken.splice(taken.end(), working, at);
            }
            auto at = position_after(edit.after);
            if (!at) {
                return std::unexpected(std::move(at.error()));
            }
            working.splice(*at, taken);
            break;
        }
        case ListEdit::Kind::update: {
            for (const auto& item : edit.items) {
                const auto found = where.find(item.entry_id);
                if (found == where.end()) {
                    return std::unexpected(detail::edit_refused(
                        core::ErrorCode::not_found, "an edit updates an entry the list does not hold"));
                }
                *found->second = make(item);
            }
            break;
        }
        }
    }
    list.assign(std::make_move_iterator(working.begin()), std::make_move_iterator(working.end()));
    return {};
}

// The same, for a list as the engine stores it.
[[nodiscard]] core::Result<void> apply_list_edits(std::vector<EngineListItem>& items,
                                                  const std::vector<ListEdit>& edits);

// One entry of a list as a sync compares it: its identity, and a fingerprint
// of what the engine stores for it.
struct ListEntryPrint {
    core::StableId entry;
    std::size_t print{0};
};

// An edit as `plan_list_edits` works it out: what `ListEdit` says, with the
// items of an insert or update named by their index in the list wanted rather
// than copied, for the caller to describe.
struct PlannedListEdit {
    ListEdit::Kind kind{ListEdit::Kind::remove};
    std::vector<core::StableId> entries;
    std::vector<std::size_t> rows;
    std::optional<core::StableId> after;

    friend bool operator==(const PlannedListEdit&, const PlannedListEdit&) = default;
};

// The edits that turn `before` into `wanted`, by identity: removals first,
// then runs of new or moved entries in the order wanted, each after the entry
// before it, then updates of entries whose fingerprint changed. Entries are
// moved only when they left the longest run that kept its order, so dragging
// three rows moves three. Both lists hold each identity at most once.
[[nodiscard]] std::vector<PlannedListEdit>
plan_list_edits(const std::vector<ListEntryPrint>& before,
                const std::vector<ListEntryPrint>& wanted);

} // namespace trackknife::persistence

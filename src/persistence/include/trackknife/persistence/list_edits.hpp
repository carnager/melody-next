// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "trackknife/core/result.hpp"
#include "trackknife/core/stable_id.hpp"
#include "trackknife/persistence/list_repository.hpp"

#include <cstddef>
#include <optional>
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

// Applies `edits` in order. An entry or anchor the list does not hold, an
// insert of one it does, or an entry named twice refuses all of them and
// leaves `items` as it was.
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

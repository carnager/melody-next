// SPDX-License-Identifier: GPL-3.0-only

// ADR-0256: lists travel as edits. Applying what plan_list_edits works out
// must turn the old list into the new one, whatever changed, and the edits
// must be no more than the change.

#include "trackknife/persistence/list_edits.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace core = trackknife::core;
namespace persistence = trackknife::persistence;
using persistence::ListEdit;

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

[[nodiscard]] persistence::EngineListItem item(const std::string& title) {
    persistence::EngineListItem made;
    made.raw_path = "/music/" + title + ".flac";
    made.title = title;
    return made;
}

[[nodiscard]] std::vector<persistence::ListEntryPrint>
prints(const std::vector<persistence::EngineListItem>& items) {
    std::vector<persistence::ListEntryPrint> printed;
    for (const auto& each : items) {
        printed.push_back({.entry = each.entry_id,
                           .print = std::hash<std::string>{}(each.raw_path + '\0' + each.title)});
    }
    return printed;
}

// What a client sends for a plan: the items named by row, described.
[[nodiscard]] std::vector<ListEdit>
edits_of(const std::vector<persistence::PlannedListEdit>& plan,
         const std::vector<persistence::EngineListItem>& wanted) {
    std::vector<ListEdit> edits;
    for (const auto& planned : plan) {
        ListEdit edit{
            .kind = planned.kind, .entries = planned.entries, .items = {}, .after = planned.after};
        for (const auto row : planned.rows) {
            edit.items.push_back(wanted[row]);
        }
        edits.push_back(std::move(edit));
    }
    return edits;
}

// Plans, applies, and checks the result is exactly `wanted`; answers the plan.
std::vector<persistence::PlannedListEdit>
round_trip(const std::vector<persistence::EngineListItem>& before,
           const std::vector<persistence::EngineListItem>& wanted, const std::string_view what) {
    const auto plan = persistence::plan_list_edits(prints(before), prints(wanted));
    auto applied = before;
    const auto result = persistence::apply_list_edits(applied, edits_of(plan, wanted));
    require(result.has_value(), what);
    require(applied == wanted, what);
    return plan;
}

[[nodiscard]] std::size_t entries_moved(const std::vector<persistence::PlannedListEdit>& plan) {
    std::size_t moved = 0;
    for (const auto& edit : plan) {
        if (edit.kind == ListEdit::Kind::move) {
            moved += edit.entries.size();
        }
    }
    return moved;
}

} // namespace

int main() {
    std::vector<persistence::EngineListItem> list;
    for (int index = 0; index < 10; ++index) {
        list.push_back(item("t" + std::to_string(index)));
    }

    require(persistence::plan_list_edits(prints(list), prints(list)).empty(),
            "an unchanged list needs no edits");

    {
        auto wanted = list;
        std::rotate(wanted.begin() + 2, wanted.begin() + 5, wanted.begin() + 8);
        const auto plan = round_trip(list, wanted, "a drag of three rows");
        require(plan.size() == 1U && plan.front().kind == ListEdit::Kind::move &&
                    plan.front().entries.size() == 3U,
                "dragging three rows is one move of three");
    }
    {
        auto wanted = list;
        wanted.erase(wanted.begin() + 3);
        const auto plan = round_trip(list, wanted, "a removal");
        require(plan.size() == 1U && plan.front().kind == ListEdit::Kind::remove &&
                    plan.front().entries == std::vector{list[3].entry_id},
                "removing one row is one removal of one");
    }
    {
        auto wanted = list;
        wanted.insert(wanted.begin(), item("first"));
        wanted.insert(wanted.begin() + 6, {item("m1"), item("m2")});
        wanted.push_back(item("last"));
        const auto plan = round_trip(list, wanted, "inserts at the start, middle and end");
        require(plan.size() == 3U && plan[0].after == std::nullopt &&
                    plan[1].after == wanted[5].entry_id && plan[1].rows.size() == 2U &&
                    plan[2].after == list.back().entry_id,
                "each run of new rows is inserted after the row before it");
    }
    {
        auto wanted = list;
        wanted[4].title = "retagged";
        wanted[7].raw_path = "/music/moved.flac";
        const auto plan = round_trip(list, wanted, "updates");
        require(plan.size() == 1U && plan.front().kind == ListEdit::Kind::update &&
                    plan.front().rows == std::vector<std::size_t>{4U, 7U},
                "changed rows are updated in place, and only they");
    }
    {
        auto wanted = list;
        std::ranges::reverse(wanted);
        const auto plan = round_trip(list, wanted, "a reversal");
        require(entries_moved(plan) == list.size() - 1U, "a reversal keeps one row in place");
    }

    // Refusals leave the list as it was.
    {
        auto applied = list;
        const auto missing =
            persistence::apply_list_edits(applied, {ListEdit{.kind = ListEdit::Kind::remove,
                                                             .entries = {list[0].entry_id},
                                                             .items = {},
                                                             .after = {}},
                                                    ListEdit{.kind = ListEdit::Kind::move,
                                                             .entries = {core::StableId::random()},
                                                             .items = {},
                                                             .after = {}}});
        require(!missing && missing.error().code == core::ErrorCode::not_found && applied == list,
                "an edit naming an unknown entry refuses the whole set, nothing applied");
        const auto twice = persistence::apply_list_edits(
            applied,
            {ListEdit{
                .kind = ListEdit::Kind::insert, .entries = {}, .items = {list[2]}, .after = {}}});
        require(!twice && applied == list, "an insert of an entry already held is refused");
        const auto self =
            persistence::apply_list_edits(applied, {ListEdit{.kind = ListEdit::Kind::move,
                                                             .entries = {list[2].entry_id},
                                                             .items = {},
                                                             .after = list[2].entry_id}});
        require(!self && applied == list, "entries are not moved after themselves");
        const auto anchor =
            persistence::apply_list_edits(applied, {ListEdit{.kind = ListEdit::Kind::insert,
                                                             .entries = {},
                                                             .items = {item("x")},
                                                             .after = core::StableId::random()}});
        require(!anchor && anchor.error().code == core::ErrorCode::not_found && applied == list,
                "an unknown anchor is refused");
    }

    // Whatever changes, the edits reproduce it.
    std::mt19937 random{20261003U};
    for (int round = 0; round < 2000; ++round) {
        std::vector<persistence::EngineListItem> before;
        const auto length = random() % 40U;
        for (std::size_t index = 0; index < length; ++index) {
            before.push_back(item("r" + std::to_string(round) + "-" + std::to_string(index)));
        }
        auto wanted = before;
        const auto changes = random() % 6U;
        for (std::size_t change = 0; change < changes; ++change) {
            switch (random() % 5U) {
            case 0:
                if (!wanted.empty()) {
                    wanted.erase(wanted.begin() + static_cast<long>(random() % wanted.size()));
                }
                break;
            case 1:
                wanted.insert(wanted.begin() + static_cast<long>(random() % (wanted.size() + 1U)),
                              item("new" + std::to_string(round) + "-" + std::to_string(change)));
                break;
            case 2:
                if (wanted.size() > 1U) {
                    const auto from = random() % wanted.size();
                    auto moved = wanted[from];
                    wanted.erase(wanted.begin() + static_cast<long>(from));
                    wanted.insert(
                        wanted.begin() + static_cast<long>(random() % (wanted.size() + 1U)), moved);
                }
                break;
            case 3:
                std::ranges::shuffle(wanted, random);
                break;
            default:
                if (!wanted.empty()) {
                    wanted[random() % wanted.size()].title += "'";
                }
                break;
            }
        }
        static_cast<void>(round_trip(before, wanted, "a random change is reproduced"));
    }

    // A sorted 66,000-entry list is planned and applied without trouble.
    {
        std::vector<persistence::EngineListItem> large;
        for (int index = 0; index < 66'000; ++index) {
            large.push_back(item("l" + std::to_string(index)));
        }
        auto wanted = large;
        std::ranges::sort(wanted, {}, &persistence::EngineListItem::title);
        static_cast<void>(round_trip(large, wanted, "a large sort"));
    }

    std::cout << "list edits tests passed\n";
    return EXIT_SUCCESS;
}

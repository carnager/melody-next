// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/engine/list_continuation.hpp"

#include "trackknife/core/stable_id.hpp"
#include "trackknife/engine/catalogue.hpp"
#include "trackknife/engine/dynamic_selection.hpp"
#include "trackknife/engine/player.hpp"
#include "trackknife/engine/workspace.hpp"
#include "trackknife/protocol/message.hpp"
#include "trackknife/query/tkq.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <set>
#include <utility>
#include <vector>

namespace trackknife::engine {
namespace {

using protocol::Json;

constexpr std::string_view store_key = "list-continuation.v1";

[[nodiscard]] std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// The rule, without what played in the last `days` days: a track never
// played has no age, and a missing age matches no comparison -- hence both.
// The rule's own query when it cannot be narrowed so.
[[nodiscard]] std::string without_recent(const std::string& query, const int days) {
    auto narrowed = query::narrow_tkq_source(
        query, "HISTORY(dayssinceplayed) MISSING OR HISTORY(dayssinceplayed) GREATER " +
                   std::to_string(days - 1));
    return narrowed ? std::move(*narrowed) : query;
}

// A rule as it is stored, told and set: {rule|id, name, query, ...}.
[[nodiscard]] Json rule_json(const ContinuationRule& rule) {
    return Json{{"rule", rule.rule_id},  {"name", rule.name},
                {"query", rule.query},   {"group_by", rule.group_by},
                {"groups", rule.groups}, {"per_group", rule.per_group},
                {"limit", rule.limit},   {"shuffle", rule.shuffle}};
}

[[nodiscard]] ContinuationRule rule_from(const Json& value, const char* id_key) {
    const auto count = [&value](const char* key, const std::size_t fallback) {
        const auto found = value.find(key);
        return found != value.end() && found->is_number_integer() && found->get<std::int64_t>() >= 0
                   ? found->get<std::size_t>()
                   : fallback;
    };
    return ContinuationRule{.rule_id = value.value(id_key, std::string{}),
                            .name = value.value("name", std::string{}),
                            .query = value.value("query", std::string{}),
                            .group_by = value.value("group_by", std::string{}),
                            .groups = count("groups", 0U),
                            .per_group = count("per_group", 0U),
                            .limit = std::min(count("limit", 100U), dynamic_selection_limit),
                            .shuffle = value.value("shuffle", false)};
}

[[nodiscard]] std::string first_value(const persistence::TkqRowFacts& facts,
                                      const std::string& field) {
    const auto found = facts.fields.find(field);
    return found == facts.fields.end() || found->second.empty() ? std::string{}
                                                                : found->second.front().first;
}

} // namespace

ListContinuation::ListContinuation(Workspace& workspace, const Catalogue& catalogue, EventSink sink)
    : workspace_(&workspace), catalogue_(&catalogue), sink_(std::move(sink)) {
    load();
}

void ListContinuation::load() {
    auto stored = workspace_->load_engine_state(store_key);
    if (!stored || !*stored) {
        return;
    }
    const auto document = Json::parse(**stored, nullptr, false);
    const auto lists = document.is_object() ? document.find("lists") : document.end();
    if (lists == document.end() || !lists->is_object()) {
        return;
    }
    for (const auto& [list, value] : lists->items()) {
        if (!value.is_object() || !core::StableId::parse(list)) {
            continue;
        }
        auto rule = rule_from(value, "rule");
        if (!rule.query.empty()) {
            rules_.emplace(list, std::move(rule));
        }
    }
}

void ListContinuation::store_locked() {
    Json lists = Json::object();
    for (const auto& [list, rule] : rules_) {
        lists[list] = rule_json(rule);
    }
    const Json document{{"version", 1}, {"lists", std::move(lists)}};
    static_cast<void>(workspace_->save_engine_state(store_key, document.dump(), now_ms()));
}

void ListContinuation::announce_locked() const {
    if (!sink_) {
        return;
    }
    auto lists = Json::array();
    for (const auto& [list, rule] : rules_) {
        auto told = rule_json(rule);
        told["list"] = list;
        lists.push_back(std::move(told));
    }
    sink_(protocol::Event{.name = "list.continuations",
                          .data = Json{{"continuations", std::move(lists)}}});
}

std::map<std::string, ContinuationRule> ListContinuation::all() const {
    const std::lock_guard guard{mutex_};
    return rules_;
}

core::Result<void> ListContinuation::set(const std::string& list,
                                         std::optional<ContinuationRule> rule) {
    if (!core::StableId::parse(list)) {
        return std::unexpected(core::Error{.code = core::ErrorCode::invalid_argument,
                                           .message = "a list is named by its identity",
                                           .context = {{.key = "list", .value = list}}});
    }
    if (rule) {
        if (auto compiled = query::compile_tkq(rule->query); !compiled) {
            return std::unexpected(std::move(compiled.error()));
        }
    }
    const std::lock_guard guard{mutex_};
    if (rule) {
        rules_.insert_or_assign(list, std::move(*rule));
    } else {
        rules_.erase(list);
    }
    // Looked at again at the next tick: a list whose last track already
    // plays continues as soon as it is told to.
    last_entry_.clear();
    store_locked();
    announce_locked();
    return {};
}

bool ListContinuation::tick(Player& player) {
    const auto state = player.state();
    const auto key = state.entry.to_string() + "/" + std::to_string(state.queue_revision);
    {
        const std::lock_guard guard{mutex_};
        if (state.entry.is_nil() || (key == last_entry_ && state.instance == last_instance_)) {
            return false;
        }
        last_entry_ = key;
        last_instance_ = state.instance;
    }
    return continue_if_ending(player) > 0U;
}

std::size_t ListContinuation::continue_if_ending(Player& player) {
    const auto list = player.queue_list();
    ContinuationRule rule;
    {
        const std::lock_guard guard{mutex_};
        const auto found = rules_.find(list);
        if (list.empty() || found == rules_.end()) {
            return 0U;
        }
        rule = found->second;
    }
    if (!player.ends_after_current()) {
        return 0U;
    }
    std::set<std::string> queued;
    for (const auto& entry : player.queue()) {
        queued.insert(entry.source.raw_path);
    }
    // Not what is queued; preferably not what played lately either, but
    // rather that than nothing, should the rule be narrow. A rule that picks
    // groups continues with its selection -- the next album, whole -- and one
    // that does not with batch_size tracks at random (ADR-0258).
    const bool grouped = rule.groups > 0U && !rule.group_by.empty();
    std::vector<std::string> candidates;
    for (const auto& source : {without_recent(rule.query, recent_days), rule.query}) {
        const DynamicSelection selection{.query = source,
                                         .limit = grouped ? rule.limit : batch_size,
                                         .shuffle = grouped ? rule.shuffle : true,
                                         .group_by = grouped ? rule.group_by : std::string{},
                                         .groups = grouped ? rule.groups : 0U,
                                         .per_group = grouped ? rule.per_group : 0U};
        core::Result<DynamicSelected> selected = std::unexpected(core::Error{});
        {
            std::mt19937 random;
            {
                const std::lock_guard guard{mutex_};
                random.seed(random_());
            }
            selected = select_dynamic(*catalogue_, selection, queued, random);
        }
        if (!selected) {
            std::cerr << "melodyd: continuing " << list << " (" << rule.name
                      << ") failed: " << selected.error().message << "\n";
            return 0U;
        }
        if (!selected->paths.empty()) {
            candidates = std::move(selected->paths);
            break;
        }
    }
    if (candidates.empty()) {
        return 0U;
    }
    auto tracks = catalogue_->cached_tracks(candidates);
    if (!tracks) {
        std::cerr << "melodyd: continuing " << list << " failed: " << tracks.error().message
                  << "\n";
        return 0U;
    }
    std::vector<QueueEntry> batch;
    batch.reserve(tracks->size());
    for (const auto& track : *tracks) {
        QueueEntry entry;
        entry.source.raw_path = track.raw_path;
        if (track.facts.duration_ms >= 0) {
            entry.duration_ms = track.facts.duration_ms;
        }
        entry.title = track.facts.title;
        const auto album_artist = first_value(track.facts, "albumartist");
        entry.group.artist = album_artist.empty() ? track.facts.artist : album_artist;
        entry.group.album = track.facts.album;
        batch.push_back(std::move(entry));
    }
    const auto appended = batch.size();
    player.append_to_queue(std::move(batch));
    return appended;
}

void register_list_continuation_methods(protocol::Dispatcher& dispatcher,
                                        ListContinuation& continuation) {
    dispatcher.on("list.continuations", [&continuation](const Json&) -> core::Result<Json> {
        auto lists = Json::array();
        for (const auto& [list, rule] : continuation.all()) {
            auto told = rule_json(rule);
            told["list"] = list;
            lists.push_back(std::move(told));
        }
        return Json{{"continuations", std::move(lists)}};
    });
    dispatcher.on("list.continuation.set",
                  [&continuation](const Json& params) -> core::Result<Json> {
                      const auto list = params.value("list", std::string{});
                      std::optional<ContinuationRule> rule;
                      if (const auto given = params.find("rule");
                          given != params.end() && given->is_object()) {
                          rule = rule_from(*given, "id");
                      }
                      if (auto set = continuation.set(list, std::move(rule)); !set) {
                          return std::unexpected(std::move(set.error()));
                      }
                      return Json{{"list", list}};
                  });
}

} // namespace trackknife::engine

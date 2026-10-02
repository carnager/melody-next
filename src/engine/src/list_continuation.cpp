// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/engine/list_continuation.hpp"

#include "trackknife/core/stable_id.hpp"
#include "trackknife/engine/catalogue.hpp"
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
[[nodiscard]] std::string without_recent(const std::string& query, const int days) {
    return "(" + query + ") AND (HISTORY(dayssinceplayed) MISSING OR HISTORY(dayssinceplayed) " +
           "GREATER " + std::to_string(days - 1) + ")";
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
        ContinuationRule rule{.rule_id = value.value("rule", std::string{}),
                              .name = value.value("name", std::string{}),
                              .query = value.value("query", std::string{})};
        if (!rule.query.empty()) {
            rules_.emplace(list, std::move(rule));
        }
    }
}

void ListContinuation::store_locked() {
    Json lists = Json::object();
    for (const auto& [list, rule] : rules_) {
        lists[list] = Json{{"rule", rule.rule_id}, {"name", rule.name}, {"query", rule.query}};
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
        lists.push_back(Json{
            {"list", list}, {"rule", rule.rule_id}, {"name", rule.name}, {"query", rule.query}});
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
    // rather that than nothing, should the rule be narrow.
    std::vector<std::string> candidates;
    for (const auto& source : {without_recent(rule.query, recent_days), rule.query}) {
        auto compiled = query::compile_tkq(source);
        if (!compiled) {
            std::cerr << "melodyd: the continuation of " << list << " (" << rule.name
                      << ") does not compile: " << compiled.error().message << "\n";
            return 0U;
        }
        auto paths = catalogue_->filter_paths(*compiled);
        if (!paths) {
            std::cerr << "melodyd: continuing " << list << " failed: " << paths.error().message
                      << "\n";
            return 0U;
        }
        std::erase_if(*paths, [&queued](const std::string& path) { return queued.contains(path); });
        if (!paths->empty()) {
            candidates = std::move(*paths);
            break;
        }
    }
    if (candidates.empty()) {
        return 0U;
    }
    {
        const std::lock_guard guard{mutex_};
        std::ranges::shuffle(candidates, random_);
    }
    if (candidates.size() > batch_size) {
        candidates.resize(batch_size);
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
            lists.push_back(Json{{"list", list},
                                 {"rule", rule.rule_id},
                                 {"name", rule.name},
                                 {"query", rule.query}});
        }
        return Json{{"continuations", std::move(lists)}};
    });
    dispatcher.on("list.continuation.set",
                  [&continuation](const Json& params) -> core::Result<Json> {
                      const auto list = params.value("list", std::string{});
                      std::optional<ContinuationRule> rule;
                      if (const auto given = params.find("rule");
                          given != params.end() && given->is_object()) {
                          rule = ContinuationRule{.rule_id = given->value("id", std::string{}),
                                                  .name = given->value("name", std::string{}),
                                                  .query = given->value("query", std::string{})};
                      }
                      if (auto set = continuation.set(list, std::move(rule)); !set) {
                          return std::unexpected(std::move(set.error()));
                      }
                      return Json{{"list", list}};
                  });
}

} // namespace trackknife::engine

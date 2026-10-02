// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/core/result.hpp"
#include "trackknife/engine/job_registry.hpp"
#include "trackknife/protocol/dispatch.hpp"

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <string>

namespace trackknife::engine {

class Catalogue;
class Player;
class Workspace;

// ADR-0253: the rule a list continues with -- a copy of a dynamic playlist's
// tkq-1 rule, kept by the engine so the list continues with no window open.
struct ContinuationRule {
    std::string rule_id;
    std::string name;
    std::string query;

    friend bool operator==(const ContinuationRule&, const ContinuationRule&) = default;
};

// Which lists continue, with what, and the continuing itself: as a track
// starts that its list ends after, a batch from the rule is appended to the
// queue -- not what is queued already, nor what played in the last 7 days.
class ListContinuation final {
  public:
    static constexpr std::size_t batch_size = 10U;
    static constexpr int recent_days = 7;

    ListContinuation(Workspace& workspace, const Catalogue& catalogue, EventSink sink);
    ListContinuation(const ListContinuation&) = delete;
    ListContinuation& operator=(const ListContinuation&) = delete;

    // List identity -> its rule.
    [[nodiscard]] std::map<std::string, ContinuationRule> all() const;
    // Sets or (with nothing) ends a list's continuation; kept, and told to
    // every client. A rule whose query does not compile is refused.
    [[nodiscard]] core::Result<void> set(const std::string& list,
                                         std::optional<ContinuationRule> rule);

    // Every tick of the playback watcher: continues once per track started.
    // Answers whether it appended.
    bool tick(Player& player);
    // Continues now if the queue's list has a rule and nothing would follow
    // what plays. Answers how many were appended.
    std::size_t continue_if_ending(Player& player);

  private:
    void load();
    void store_locked();
    void announce_locked() const;

    Workspace* workspace_;
    const Catalogue* catalogue_;
    EventSink sink_;
    mutable std::mutex mutex_;
    std::map<std::string, ContinuationRule> rules_;
    // What was last seen playing, so a track's start is acted on once.
    std::string last_entry_;
    std::uint64_t last_instance_{0};
    std::mt19937 random_{std::random_device{}()};
};

// list.continuations -- every continued list and its rule -- and
// list.continuation.set {list, rule?: {id, name, query}}, which sets or, with
// no rule, ends one. Changes are told as the list.continuations event.
void register_list_continuation_methods(protocol::Dispatcher& dispatcher,
                                        ListContinuation& continuation);

} // namespace trackknife::engine

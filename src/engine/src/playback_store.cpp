// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/engine/playback_store.hpp"

#include "trackknife/engine/playback_methods.hpp"

#include <utility>

namespace trackknife::engine {
namespace {

using protocol::Json;

[[nodiscard]] std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

[[nodiscard]] Json revision_to_json(const std::optional<core::LocalSourceRevision>& revision) {
    if (!revision) {
        return Json(nullptr);
    }
    Json rendered = Json::object();
    rendered["device"] = revision->device;
    rendered["inode"] = revision->inode;
    rendered["size"] = revision->size;
    rendered["modified_s"] = revision->modification_time_seconds;
    rendered["modified_ns"] = revision->modification_time_nanoseconds;
    return rendered;
}

[[nodiscard]] std::optional<core::LocalSourceRevision> revision_from_json(const Json& value) {
    if (!value.is_object()) {
        return std::nullopt;
    }
    core::LocalSourceRevision revision;
    revision.device = value.value("device", std::uint64_t{0});
    revision.inode = value.value("inode", std::uint64_t{0});
    revision.size = value.value("size", std::uint64_t{0});
    revision.modification_time_seconds = value.value("modified_s", std::int64_t{0});
    revision.modification_time_nanoseconds = value.value("modified_ns", std::int64_t{0});
    return revision;
}

} // namespace

PlaybackStore::PlaybackStore(Player& player, Workspace& workspace,
                             const std::chrono::milliseconds interval)
    : player_(&player), workspace_(&workspace), interval_(interval) {}

PlaybackStore::~PlaybackStore() { stop(); }

bool PlaybackStore::restore() {
    auto stored = workspace_->load_engine_state(queue_key);
    if (!stored || !*stored) {
        return false;
    }
    const auto document = Json::parse(**stored, nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
        // Unreadable is treated as absent. A queue this engine cannot parse is
        // not worth refusing to start over.
        return false;
    }

    Player::Persisted state;
    if (const auto entries = document.find("queue");
        entries != document.end() && entries->is_array()) {
        for (const auto& value : *entries) {
            auto entry = queue_entry_from_json(value);
            if (!entry) {
                continue; // One unreadable entry does not cost the whole queue.
            }
            state.queue.push_back(std::move(*entry));
        }
    }
    if (state.queue.empty()) {
        return false;
    }
    if (const auto entry = document.find("entry"); entry != document.end() && entry->is_string()) {
        if (auto parsed = core::StableId::parse(entry->get<std::string>())) {
            state.entry = *parsed;
        }
    }
    if (const auto entry = document.find("request_return");
        entry != document.end() && entry->is_string()) {
        if (auto parsed = core::StableId::parse(entry->get<std::string>())) {
            state.request_return = *parsed;
        }
    }
    state.playing_request = document.value("playing_request", false);
    if (const auto asks = document.find("requests"); asks != document.end() && asks->is_array()) {
        for (const auto& value : *asks) {
            if (!value.is_string()) {
                continue;
            }
            if (auto parsed = core::StableId::parse(value.get<std::string>())) {
                state.requests.push_back(*parsed);
            }
        }
    }
    if (const auto modes = document.find("modes"); modes != document.end()) {
        state.modes = modes_from_json(*modes);
    }

    // The position is a separate record because it is written far more often.
    // Its absence just means the queue comes back with nothing loaded.
    if (auto position = workspace_->load_engine_state(position_key); position && *position) {
        const auto where = Json::parse(**position, nullptr, false);
        if (!where.is_discarded() && where.is_object()) {
            // Only trust a position recorded against the entry that is still
            // anchored: otherwise it belongs to a track the engine has since
            // moved on from, and resuming would seek the wrong one.
            const auto recorded = where.value("entry", std::string{});
            if (!state.entry.is_nil() && recorded == state.entry.to_string()) {
                state.position_ms = where.value("position_ms", std::int64_t{0});
                if (const auto revision = where.find("revision"); revision != where.end()) {
                    state.revision = revision_from_json(*revision);
                }
            }
        }
    }

    static_cast<void>(player_->restore(std::move(state)));
    written_revision_ = player_->revision();
    written_anything_ = true;
    return true;
}

void PlaybackStore::persist() {
    const auto revision = player_->revision();
    const auto state = player_->persisted();

    if (revision != written_revision_ || !written_anything_) {
        Json document = Json::object();
        auto entries = Json::array();
        for (const auto& entry : state.queue) {
            entries.push_back(to_json(entry));
        }
        document["queue"] = std::move(entries);
        document["entry"] = state.entry.is_nil() ? Json(nullptr) : Json(state.entry.to_string());
        document["request_return"] =
            state.request_return.is_nil() ? Json(nullptr) : Json(state.request_return.to_string());
        document["playing_request"] = state.playing_request;
        auto asks = Json::array();
        for (const auto& ask : state.requests) {
            asks.push_back(ask.to_string());
        }
        document["requests"] = std::move(asks);
        document["modes"] = to_json(state.modes);
        if (workspace_->save_engine_state(queue_key, document.dump(), now_ms())) {
            written_revision_ = revision;
            written_anything_ = true;
        }
    }

    if (state.entry.is_nil()) {
        return;
    }
    Json where = Json::object();
    where["entry"] = state.entry.to_string();
    where["position_ms"] = state.position_ms;
    where["revision"] = revision_to_json(state.revision);
    static_cast<void>(workspace_->save_engine_state(position_key, where.dump(), now_ms()));
}

void PlaybackStore::start() {
    if (running_.exchange(true)) {
        return;
    }
    worker_ = std::thread{[this] {
        while (running_.load()) {
            persist();
            std::this_thread::sleep_for(interval_);
        }
    }};
}

void PlaybackStore::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    if (worker_.joinable()) {
        worker_.join();
    }
    // One last write on the way out, so a clean shutdown does not lose the
    // seconds since the last tick.
    persist();
}

} // namespace trackknife::engine

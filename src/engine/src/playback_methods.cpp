// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/engine/playback_methods.hpp"

#include "trackknife/protocol/message.hpp"

#include <string>
#include <utility>
#include <vector>

namespace trackknife::engine {
namespace {

using protocol::Json;

[[nodiscard]] core::Error bad_params(std::string message, std::string member) {
    return core::Error{.code = core::ErrorCode::invalid_argument,
                       .message = std::move(message),
                       .context = {{.key = "param", .value = std::move(member)}}};
}

[[nodiscard]] Json modes_to_json(const audio::PlaybackModes& modes) {
    Json rendered = Json::object();
    rendered["repeat"] = modes.repeat;
    rendered["random"] = modes.random;
    rendered["album_random"] = modes.album_random;
    rendered["single"] = static_cast<int>(modes.single);
    rendered["consume"] = static_cast<int>(modes.consume);
    return rendered;
}

[[nodiscard]] core::Result<QueueEntry> entry_from_json(const Json& value) {
    if (!value.is_object()) {
        return std::unexpected(bad_params("each entry must be an object", "entries"));
    }
    const auto path = value.find("path");
    if (path == value.end() || !path->is_string()) {
        return std::unexpected(bad_params("each entry needs an encoded path", "entries"));
    }
    auto raw_path = protocol::decode_raw_path(path->get<std::string>());
    if (!raw_path) {
        return std::unexpected(std::move(raw_path.error()));
    }
    QueueEntry entry;
    entry.source.raw_path = std::move(*raw_path);
    // An identity supplied by the client is honoured, so a client can name its
    // own entries and recognise them coming back; otherwise one is minted.
    if (const auto identity = value.find("entry"); identity != value.end()) {
        if (!identity->is_string()) {
            return std::unexpected(bad_params("an entry identity must be a string", "entries"));
        }
        auto parsed = core::StableId::parse(identity->get<std::string>());
        if (!parsed) {
            return std::unexpected(bad_params("an entry identity must be an identity", "entries"));
        }
        entry.entry_id = *parsed;
    }
    if (const auto duration = value.find("duration_ms");
        duration != value.end() && duration->is_number_integer()) {
        entry.duration_ms = duration->get<std::int64_t>();
    }
    return entry;
}

} // namespace

Json to_json(const Player::State& state) {
    Json rendered = Json::object();
    rendered["status"] = state.status;
    rendered["entry"] = state.entry.is_nil() ? Json(nullptr) : Json(state.entry.to_string());
    rendered["path"] = state.source.empty()
                           ? Json(nullptr)
                           : Json(protocol::encode_raw_path(state.source.raw_path));
    rendered["position_ms"] = state.position_ms;
    rendered["duration_ms"] = state.duration_ms;
    rendered["queue_size"] = state.queue_size;
    rendered["requests"] = state.requests;
    rendered["modes"] = modes_to_json(state.modes);
    return rendered;
}

void register_playback_methods(protocol::Dispatcher& dispatcher, Player& player) {
    dispatcher.on("playback.state",
                  [&player](const Json&) -> core::Result<Json> { return to_json(player.state()); });

    dispatcher.on("playback.queue", [&player](const Json&) -> core::Result<Json> {
        auto entries = Json::array();
        for (const auto& entry : player.queue()) {
            Json rendered = Json::object();
            rendered["entry"] = entry.entry_id.to_string();
            rendered["path"] = protocol::encode_raw_path(entry.source.raw_path);
            rendered["duration_ms"] = entry.duration_ms.value_or(-1);
            entries.push_back(std::move(rendered));
        }
        return Json{{"entries", std::move(entries)}};
    });

    dispatcher.on("playback.replace_queue", [&player](const Json& params) -> core::Result<Json> {
        const auto entries = params.find("entries");
        if (entries == params.end() || !entries->is_array()) {
            return std::unexpected(bad_params("an array of entries is required", "entries"));
        }
        std::vector<QueueEntry> queue;
        queue.reserve(entries->size());
        for (const auto& value : *entries) {
            auto entry = entry_from_json(value);
            if (!entry) {
                return std::unexpected(std::move(entry.error()));
            }
            queue.push_back(std::move(*entry));
        }
        player.replace_queue(std::move(queue));
        return to_json(player.state());
    });

    dispatcher.on("playback.play", [&player](const Json& params) -> core::Result<Json> {
        const auto identity = params.find("entry");
        if (identity == params.end() || !identity->is_string()) {
            return std::unexpected(bad_params("an entry identity is required", "entry"));
        }
        auto entry_id = core::StableId::parse(identity->get<std::string>());
        if (!entry_id) {
            return std::unexpected(bad_params("entry is not an identity", "entry"));
        }
        auto played = player.play_entry(*entry_id);
        if (!played) {
            return std::unexpected(std::move(played.error()));
        }
        return to_json(player.state());
    });

    // Up-next. A request names an entry the engine already holds, so the
    // client sends an identity rather than a source.
    dispatcher.on("playback.request", [&player](const Json& params) -> core::Result<Json> {
        const auto identity = params.find("entry");
        if (identity == params.end() || !identity->is_string()) {
            return std::unexpected(bad_params("an entry identity is required", "entry"));
        }
        auto entry_id = core::StableId::parse(identity->get<std::string>());
        if (!entry_id) {
            return std::unexpected(bad_params("entry is not an identity", "entry"));
        }
        auto requested = player.request(*entry_id);
        if (!requested) {
            return std::unexpected(std::move(requested.error()));
        }
        return to_json(player.state());
    });

    dispatcher.on("playback.requests", [&player](const Json&) -> core::Result<Json> {
        auto entries = Json::array();
        for (const auto& entry_id : player.requests()) {
            entries.push_back(entry_id.to_string());
        }
        return Json{{"entries", std::move(entries)}};
    });

    dispatcher.on("playback.clear_requests", [&player](const Json&) -> core::Result<Json> {
        player.clear_requests();
        return to_json(player.state());
    });

    const auto simple = [&player, &dispatcher](std::string method,
                                               core::Result<void> (Player::*action)()) {
        dispatcher.on(std::move(method), [&player, action](const Json&) -> core::Result<Json> {
            auto done = (player.*action)();
            if (!done) {
                return std::unexpected(std::move(done.error()));
            }
            return to_json(player.state());
        });
    };
    simple("playback.resume", &Player::resume);
    simple("playback.pause", &Player::pause);
    simple("playback.stop", &Player::stop);

    dispatcher.on("playback.seek", [&player](const Json& params) -> core::Result<Json> {
        const auto position = params.find("position_ms");
        if (position == params.end() || !position->is_number_integer()) {
            return std::unexpected(bad_params("position_ms must be an integer", "position_ms"));
        }
        auto sought = player.seek_ms(position->get<std::int64_t>());
        if (!sought) {
            return std::unexpected(std::move(sought.error()));
        }
        return to_json(player.state());
    });

    const auto step = [&player, &dispatcher](std::string method, const int direction) {
        dispatcher.on(std::move(method), [&player, direction](const Json&) -> core::Result<Json> {
            auto moved = player.step(direction);
            if (!moved) {
                return std::unexpected(std::move(moved.error()));
            }
            return to_json(player.state());
        });
    };
    step("playback.next", 1);
    step("playback.previous", -1);

    dispatcher.on("playback.set_modes", [&player](const Json& params) -> core::Result<Json> {
        auto modes = player.modes();
        // Absent members leave a mode alone, so a client can change one
        // without restating the rest and racing another client's change.
        if (const auto value = params.find("repeat"); value != params.end()) {
            if (!value->is_boolean()) {
                return std::unexpected(bad_params("repeat must be a boolean", "repeat"));
            }
            modes.repeat = value->get<bool>();
        }
        if (const auto value = params.find("random"); value != params.end()) {
            if (!value->is_boolean()) {
                return std::unexpected(bad_params("random must be a boolean", "random"));
            }
            modes.random = value->get<bool>();
        }
        if (const auto value = params.find("album_random"); value != params.end()) {
            if (!value->is_boolean()) {
                return std::unexpected(
                    bad_params("album_random must be a boolean", "album_random"));
            }
            modes.album_random = value->get<bool>();
        }
        const auto tri_state = [&params](const char* name,
                                         audio::ModeState& slot) -> core::Result<void> {
            const auto value = params.find(name);
            if (value == params.end()) {
                return {};
            }
            if (!value->is_number_integer()) {
                return std::unexpected(bad_params("a tri-state mode is 0, 1 or 2", name));
            }
            const auto raw = value->get<std::int64_t>();
            if (raw < 0 || raw > 2) {
                return std::unexpected(bad_params("a tri-state mode is 0, 1 or 2", name));
            }
            slot = audio::mode_state_from_int(static_cast<int>(raw));
            return {};
        };
        if (auto set = tri_state("single", modes.single); !set) {
            return std::unexpected(std::move(set.error()));
        }
        if (auto set = tri_state("consume", modes.consume); !set) {
            return std::unexpected(std::move(set.error()));
        }
        player.set_modes(modes);
        return to_json(player.state());
    });
}

PlaybackWatcher::PlaybackWatcher(Player& player, EventSink sink,
                                 const std::chrono::milliseconds interval)
    : player_(&player), sink_(std::move(sink)), interval_(interval) {}

PlaybackWatcher::~PlaybackWatcher() { stop(); }

void PlaybackWatcher::start() {
    if (running_.exchange(true)) {
        return;
    }
    worker_ = std::thread{[this] {
        Json previous;
        bool first = true;
        while (running_.load()) {
            auto current = to_json(player_->state());
            // Position is dropped before comparing: it moves continuously and
            // emitting on it would be a broadcast storm carrying nothing a
            // client could not work out for itself.
            auto comparable = current;
            comparable.erase("position_ms");
            if (first || comparable != previous) {
                first = false;
                previous = std::move(comparable);
                if (sink_) {
                    sink_(protocol::Event{.name = "playback.changed", .data = current});
                }
            }
            std::this_thread::sleep_for(interval_);
        }
    }};
}

void PlaybackWatcher::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    if (worker_.joinable()) {
        worker_.join();
    }
}

} // namespace trackknife::engine

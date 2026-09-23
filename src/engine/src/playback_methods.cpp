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

// ADR-0139/0141: an explicit override, sent only when the client has one.
// Every member is optional on its own -- a sidecar may carry a track gain and
// no album peak -- so each is read independently rather than demanding a
// complete set.
[[nodiscard]] core::Result<std::optional<formats::ReplayGainInfo>>
replay_gain_from_json(const Json& value) {
    if (value.is_null()) {
        return std::optional<formats::ReplayGainInfo>{};
    }
    if (!value.is_object()) {
        return std::unexpected(bad_params("replay_gain must be an object", "replay_gain"));
    }
    formats::ReplayGainInfo info;
    const auto number = [&value](const char* member,
                                 std::optional<double>& into) -> core::Result<void> {
        const auto found = value.find(member);
        if (found == value.end() || found->is_null()) {
            return {};
        }
        if (!found->is_number()) {
            return std::unexpected(
                bad_params(std::string{member} + " must be a number", "replay_gain"));
        }
        into = found->get<double>();
        return {};
    };
    if (auto read = number("track_gain_db", info.track_gain_db); !read) {
        return std::unexpected(std::move(read.error()));
    }
    if (auto read = number("track_peak", info.track_peak); !read) {
        return std::unexpected(std::move(read.error()));
    }
    if (auto read = number("album_gain_db", info.album_gain_db); !read) {
        return std::unexpected(std::move(read.error()));
    }
    if (auto read = number("album_peak", info.album_peak); !read) {
        return std::unexpected(std::move(read.error()));
    }
    // An object naming nothing is the same as sending nothing, rather than an
    // override that overrides no value and suppresses the decoder's tags.
    if (!info.track_gain_db && !info.track_peak && !info.album_gain_db && !info.album_peak) {
        return std::optional<formats::ReplayGainInfo>{};
    }
    return std::optional{info};
}

[[nodiscard]] Json replay_gain_to_json(const std::optional<formats::ReplayGainInfo>& info) {
    if (!info) {
        return Json(nullptr);
    }
    Json rendered = Json::object();
    const auto number = [&rendered](const char* member, const std::optional<double>& value) {
        rendered[member] = value ? Json(*value) : Json(nullptr);
    };
    number("track_gain_db", info->track_gain_db);
    number("track_peak", info->track_peak);
    number("album_gain_db", info->album_gain_db);
    number("album_peak", info->album_peak);
    return rendered;
}

// A selection names which audio a container holds -- a stream, a subsong --
// and a segment names a range within it, which is how one file carries a CUE
// album. Without them a segmented entry plays the whole file from the start,
// so they belong on the wire with the path rather than being a local detail.
[[nodiscard]] core::Result<formats::AudioSourceSelection> selection_from_json(const Json& value) {
    if (!value.is_object()) {
        return std::unexpected(bad_params("selection must be an object", "selection"));
    }
    formats::AudioSourceSelection selection;
    const auto index = [&value](const char* member,
                                std::optional<int>& into) -> core::Result<void> {
        const auto found = value.find(member);
        if (found == value.end() || found->is_null()) {
            return {};
        }
        if (!found->is_number_integer()) {
            return std::unexpected(
                bad_params(std::string{member} + " must be an integer", "selection"));
        }
        into = static_cast<int>(found->get<std::int64_t>());
        return {};
    };
    if (auto read = index("stream_index", selection.stream_index); !read) {
        return std::unexpected(std::move(read.error()));
    }
    if (auto read = index("subsong_index", selection.subsong_index); !read) {
        return std::unexpected(std::move(read.error()));
    }
    return selection;
}

[[nodiscard]] Json selection_to_json(const formats::AudioSourceSelection& selection) {
    Json rendered = Json::object();
    rendered["stream_index"] =
        selection.stream_index ? Json(*selection.stream_index) : Json(nullptr);
    rendered["subsong_index"] =
        selection.subsong_index ? Json(*selection.subsong_index) : Json(nullptr);
    return rendered;
}

[[nodiscard]] core::Result<formats::SampleRange> segment_from_json(const Json& value) {
    if (!value.is_object()) {
        return std::unexpected(bad_params("segment must be an object", "segment"));
    }
    const auto start = value.find("start_sample");
    if (start == value.end() || !start->is_number_integer()) {
        return std::unexpected(bad_params("a segment needs start_sample", "segment"));
    }
    formats::SampleRange segment;
    segment.start_sample = start->get<std::int64_t>();
    if (const auto end = value.find("end_sample"); end != value.end() && !end->is_null()) {
        if (!end->is_number_integer()) {
            return std::unexpected(bad_params("end_sample must be an integer", "segment"));
        }
        segment.end_sample = end->get<std::int64_t>();
    }
    return segment;
}

[[nodiscard]] Json segment_to_json(const std::optional<formats::SampleRange>& segment) {
    if (!segment) {
        return Json(nullptr);
    }
    Json rendered = Json::object();
    rendered["start_sample"] = segment->start_sample;
    rendered["end_sample"] = segment->end_sample ? Json(*segment->end_sample) : Json(nullptr);
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
    if (const auto found = value.find("group"); found != value.end() && found->is_object()) {
        entry.group.album_artist = found->value("album_artist", std::string{});
        entry.group.artist = found->value("artist", std::string{});
        entry.group.album = found->value("album", std::string{});
        entry.group.date = found->value("date", std::string{});
    }
    if (const auto found = value.find("selection"); found != value.end() && !found->is_null()) {
        auto parsed = selection_from_json(*found);
        if (!parsed) {
            return std::unexpected(std::move(parsed.error()));
        }
        entry.source.selection = *parsed;
    }
    if (const auto found = value.find("segment"); found != value.end() && !found->is_null()) {
        auto parsed = segment_from_json(*found);
        if (!parsed) {
            return std::unexpected(std::move(parsed.error()));
        }
        entry.source.segment = *parsed;
    }
    if (const auto gain = value.find("replay_gain"); gain != value.end()) {
        auto parsed = replay_gain_from_json(*gain);
        if (!parsed) {
            return std::unexpected(std::move(parsed.error()));
        }
        entry.replay_gain = std::move(*parsed);
    }
    return entry;
}

} // namespace

Json to_json(const QueueEntry& entry) {
    Json rendered = Json::object();
    rendered["entry"] = entry.entry_id.to_string();
    rendered["path"] = protocol::encode_raw_path(entry.source.raw_path);
    rendered["duration_ms"] = entry.duration_ms.value_or(-1);
    Json group = Json::object();
    group["album_artist"] = entry.group.album_artist;
    group["artist"] = entry.group.artist;
    group["album"] = entry.group.album;
    group["date"] = entry.group.date;
    rendered["group"] = std::move(group);
    rendered["selection"] = selection_to_json(entry.source.selection);
    rendered["segment"] = segment_to_json(entry.source.segment);
    rendered["replay_gain"] = replay_gain_to_json(entry.replay_gain);
    return rendered;
}

core::Result<QueueEntry> queue_entry_from_json(const Json& value) { return entry_from_json(value); }

Json to_json(const audio::PlaybackModes& modes) { return modes_to_json(modes); }

audio::PlaybackModes modes_from_json(const Json& value) {
    audio::PlaybackModes modes;
    if (!value.is_object()) {
        return modes;
    }
    modes.repeat = value.value("repeat", false);
    modes.random = value.value("random", false);
    modes.album_random = value.value("album_random", false);
    modes.single = audio::mode_state_from_int(value.value("single", 0));
    modes.consume = audio::mode_state_from_int(value.value("consume", 0));
    return modes;
}

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
    rendered["volume_percent"] = state.volume_percent;
    rendered["instance"] = state.instance;
    rendered["consumed"] =
        state.consumed.is_nil() ? Json(nullptr) : Json(state.consumed.to_string());
    rendered["gapless_entry"] =
        state.gapless_entry.is_nil() ? Json(nullptr) : Json(state.gapless_entry.to_string());
    Json gain = Json::object();
    gain["mode"] = state.replay_gain_mode == audio::ReplayGainMode::track   ? "track"
                   : state.replay_gain_mode == audio::ReplayGainMode::album ? "album"
                                                                            : "off";
    gain["preamp_with_gain_db"] = state.replay_gain_preamps.with_gain_db;
    gain["preamp_without_gain_db"] = state.replay_gain_preamps.without_gain_db;
    rendered["replay_gain"] = std::move(gain);
    return rendered;
}

void register_playback_methods(protocol::Dispatcher& dispatcher, Player& player) {
    dispatcher.on("playback.state",
                  [&player](const Json&) -> core::Result<Json> { return to_json(player.state()); });

    dispatcher.on("playback.queue", [&player](const Json&) -> core::Result<Json> {
        auto entries = Json::array();
        for (const auto& entry : player.queue()) {
            entries.push_back(to_json(entry));
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

    dispatcher.on("playback.enqueue", [&player](const Json& params) -> core::Result<Json> {
        const auto entries = params.find("entries");
        if (entries == params.end() || !entries->is_array()) {
            return std::unexpected(bad_params("an array of entries is required", "entries"));
        }
        std::vector<QueueEntry> appended;
        appended.reserve(entries->size());
        for (const auto& value : *entries) {
            auto entry = entry_from_json(value);
            if (!entry) {
                return std::unexpected(std::move(entry.error()));
            }
            appended.push_back(std::move(*entry));
        }
        player.enqueue(std::move(appended));
        return to_json(player.state());
    });

    dispatcher.on("playback.set_requests", [&player](const Json& params) -> core::Result<Json> {
        const auto entries = params.find("entries");
        if (entries == params.end() || !entries->is_array()) {
            return std::unexpected(bad_params("an array of identities is required", "entries"));
        }
        std::vector<core::StableId> wanted;
        wanted.reserve(entries->size());
        for (const auto& value : *entries) {
            if (!value.is_string()) {
                return std::unexpected(bad_params("each request is an identity", "entries"));
            }
            auto parsed = core::StableId::parse(value.get<std::string>());
            if (!parsed) {
                return std::unexpected(bad_params("each request is an identity", "entries"));
            }
            wanted.push_back(*parsed);
        }
        auto set = player.set_requests(wanted);
        if (!set) {
            return std::unexpected(std::move(set.error()));
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

    dispatcher.on("playback.set_replay_gain", [&player](const Json& params) -> core::Result<Json> {
        if (const auto value = params.find("mode"); value != params.end()) {
            if (!value->is_string()) {
                return std::unexpected(bad_params("mode must be a string", "mode"));
            }
            const auto name = value->get<std::string>();
            // "auto" is deliberately not accepted: it is a client's policy
            // about its own modes, and an engine asked to be "auto" would have
            // to know which client's shuffle to consult.
            const auto mode = name == "off"     ? audio::ReplayGainMode::off
                              : name == "track" ? audio::ReplayGainMode::track
                              : name == "album" ? audio::ReplayGainMode::album
                                                : std::optional<audio::ReplayGainMode>{};
            if (!mode) {
                return std::unexpected(bad_params("mode is off, track or album", "mode"));
            }
            auto set = player.set_replay_gain_mode(*mode);
            if (!set) {
                return std::unexpected(std::move(set.error()));
            }
        }
        // Preamps arrive together or not at all: they are one setting with two
        // halves, and the audition service takes them as a pair.
        const auto with = params.find("preamp_with_gain_db");
        const auto without = params.find("preamp_without_gain_db");
        if (with != params.end() || without != params.end()) {
            auto preamps = player.state().replay_gain_preamps;
            for (const auto& [found, slot] : {std::pair{with, &preamps.with_gain_db},
                                              std::pair{without, &preamps.without_gain_db}}) {
                if (found == params.end()) {
                    continue;
                }
                if (!found->is_number()) {
                    return std::unexpected(bad_params("a preamp must be a number", "preamp"));
                }
                *slot = found->get<float>();
            }
            auto set = player.set_replay_gain_preamps(preamps);
            if (!set) {
                return std::unexpected(std::move(set.error()));
            }
        }
        return to_json(player.state());
    });

    dispatcher.on("playback.set_volume", [&player](const Json& params) -> core::Result<Json> {
        const auto percent = params.find("percent");
        if (percent == params.end() || !percent->is_number_integer()) {
            return std::unexpected(bad_params("percent must be an integer", "percent"));
        }
        const auto value = percent->get<std::int64_t>();
        if (value < 0 || value > 100) {
            return std::unexpected(bad_params("percent must be within [0, 100]", "percent"));
        }
        auto set = player.set_volume_percent(static_cast<int>(value));
        if (!set) {
            return std::unexpected(std::move(set.error()));
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
            // The engine advances itself. This is the only thread sampling the
            // player at a fixed interval, and a second clock for the same job
            // would be two things to keep in step; an advance shows up in the
            // very next comparison below, so the change is pushed at once.
            static_cast<void>(player_->advance_if_ended());
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

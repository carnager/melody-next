// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/output/audition_wire.hpp"

#include <utility>

namespace trackknife::output {
namespace {

using protocol::Json;

[[nodiscard]] core::Error malformed(std::string field) {
    return core::Error{.code = core::ErrorCode::invalid_argument,
                       .message = "malformed audition source",
                       .context = {{.key = "field", .value = std::move(field)}}};
}

template <typename T> [[nodiscard]] Json optional_json(const std::optional<T>& value) {
    return value ? Json(*value) : Json(nullptr);
}

template <typename T>
[[nodiscard]] std::optional<T> optional_value(const Json& object, const char* key) {
    const auto found = object.find(key);
    if (found == object.end() || found->is_null()) {
        return std::nullopt;
    }
    return found->get<T>();
}

[[nodiscard]] Json gain_json(const formats::ReplayGainInfo& gain) {
    return Json{{"track_gain_db", optional_json(gain.track_gain_db)},
                {"track_peak", optional_json(gain.track_peak)},
                {"album_gain_db", optional_json(gain.album_gain_db)},
                {"album_peak", optional_json(gain.album_peak)}};
}

[[nodiscard]] formats::ReplayGainInfo gain_from_json(const Json& value) {
    return formats::ReplayGainInfo{
        .track_gain_db = optional_value<double>(value, "track_gain_db"),
        .track_peak = optional_value<double>(value, "track_peak"),
        .album_gain_db = optional_value<double>(value, "album_gain_db"),
        .album_peak = optional_value<double>(value, "album_peak"),
    };
}

[[nodiscard]] Json buffer_json(const audio::PlaybackBufferDurationConfig& buffer) {
    return Json{{"capacity_ms", buffer.capacity.count()},
                {"start_threshold_ms", buffer.start_threshold.count()}};
}

[[nodiscard]] audio::PlaybackBufferDurationConfig buffer_from_json(const Json& value) {
    return audio::PlaybackBufferDurationConfig{
        .capacity = std::chrono::milliseconds{value.value("capacity_ms", std::int64_t{0})},
        .start_threshold =
            std::chrono::milliseconds{value.value("start_threshold_ms", std::int64_t{0})},
    };
}

} // namespace

Json to_json(const Source& source) {
    Json rendered = Json::object();
    if (source.path) {
        rendered["path"] = protocol::encode_raw_path(*source.path);
    }
    if (source.url) {
        rendered["url"] = *source.url;
    }
    rendered["selection"] = Json{{"stream_index", optional_json(source.selection.stream_index)},
                                 {"subsong_index", optional_json(source.selection.subsong_index)}};
    if (source.segment) {
        rendered["segment"] = Json{{"start_sample", source.segment->start_sample},
                                   {"end_sample", optional_json(source.segment->end_sample)}};
    }
    if (source.replay_gain) {
        rendered["replay_gain"] = gain_json(*source.replay_gain);
    }
    return rendered;
}

core::Result<Source> source_from_json(const Json& value) {
    if (!value.is_object()) {
        return std::unexpected(malformed("source"));
    }
    Source source;
    if (const auto path = value.find("path"); path != value.end()) {
        if (!path->is_string()) {
            return std::unexpected(malformed("path"));
        }
        auto decoded = protocol::decode_raw_path(path->get<std::string>());
        if (!decoded) {
            return std::unexpected(malformed("path"));
        }
        source.path = std::move(*decoded);
    }
    if (const auto url = value.find("url"); url != value.end()) {
        if (!url->is_string()) {
            return std::unexpected(malformed("url"));
        }
        source.url = url->get<std::string>();
    }
    if (source.path.has_value() == source.url.has_value()) {
        // One or the other: a load that names both, or neither, is a mistake
        // rather than a choice to make on the agent.
        return std::unexpected(malformed("path or url"));
    }
    if (const auto selection = value.find("selection");
        selection != value.end() && selection->is_object()) {
        source.selection.stream_index = optional_value<int>(*selection, "stream_index");
        source.selection.subsong_index = optional_value<int>(*selection, "subsong_index");
    }
    if (const auto segment = value.find("segment");
        segment != value.end() && segment->is_object()) {
        source.segment = formats::SampleRange{
            .start_sample = segment->value("start_sample", std::int64_t{0}),
            .end_sample = optional_value<std::int64_t>(*segment, "end_sample")};
    }
    if (const auto gain = value.find("replay_gain"); gain != value.end() && gain->is_object()) {
        source.replay_gain = gain_from_json(*gain);
    }
    return source;
}

Json to_json(const audio::LocalAuditionSnapshot& snapshot) {
    Json rendered = Json::object();
    rendered["state"] = static_cast<int>(snapshot.state);
    if (snapshot.format) {
        rendered["format"] = Json{{"sample_rate", snapshot.format->sample_rate},
                                  {"channels", snapshot.format->channels},
                                  {"layout", snapshot.format->channel_layout}};
    }
    rendered["position_sample"] = snapshot.position_sample;
    rendered["end_sample"] = optional_json(snapshot.end_sample);
    // Whether a continuation is armed, not which: the engine knows which, as
    // it knows every path it asked for.
    rendered["next_armed"] = !snapshot.next_raw_path.empty();
    rendered["chain_transitions"] = snapshot.chain_transitions;
    rendered["playback_instance"] = snapshot.playback_instance;
    rendered["occurrence_token"] = snapshot.occurrence_token;
    rendered["next_occurrence_token"] = snapshot.next_occurrence_token;
    rendered["volume_percent"] = snapshot.volume_percent;
    rendered["replay_gain_mode"] = static_cast<int>(snapshot.replay_gain_mode);
    rendered["preamp_with_gain_db"] = snapshot.replay_gain_preamps.with_gain_db;
    rendered["preamp_without_gain_db"] = snapshot.replay_gain_preamps.without_gain_db;
    rendered["configured_buffer"] = buffer_json(snapshot.configured_buffer);
    if (snapshot.active_buffer) {
        rendered["active_buffer"] = buffer_json(*snapshot.active_buffer);
    }
    rendered["underruns"] = snapshot.underrun_count;
    rendered["output_target"] = optional_json(snapshot.output_target);
    rendered["default_output"] = optional_json(snapshot.default_output_target);
    rendered["output_available"] = snapshot.output_target_available;
    rendered["output_suspended"] = snapshot.output_suspended;
    auto devices = Json::array();
    for (const auto& device : snapshot.devices) {
        devices.push_back(Json{{"name", device.name}, {"description", device.description}});
    }
    rendered["devices"] = std::move(devices);
    if (snapshot.error) {
        rendered["error"] = snapshot.error->message;
    }
    return rendered;
}

audio::LocalAuditionSnapshot snapshot_from_json(const Json& value) {
    audio::LocalAuditionSnapshot snapshot;
    if (!value.is_object()) {
        return snapshot;
    }
    const auto state = value.value("state", 0);
    if (state >= 0 && state <= static_cast<int>(audio::LocalAuditionState::failed)) {
        snapshot.state = static_cast<audio::LocalAuditionState>(state);
    }
    if (const auto format = value.find("format"); format != value.end() && format->is_object()) {
        snapshot.format =
            formats::PcmFormat{.sample_rate = format->value("sample_rate", 0),
                               .channels = format->value("channels", 0),
                               .channel_layout = format->value("layout", std::string{})};
    }
    snapshot.position_sample = value.value("position_sample", std::int64_t{0});
    snapshot.end_sample = optional_value<std::int64_t>(value, "end_sample");
    snapshot.chain_transitions = value.value("chain_transitions", std::uint64_t{0});
    snapshot.playback_instance = value.value("playback_instance", std::uint64_t{0});
    snapshot.occurrence_token = value.value("occurrence_token", std::uint64_t{0});
    snapshot.next_occurrence_token = value.value("next_occurrence_token", std::uint64_t{0});
    snapshot.volume_percent = value.value("volume_percent", 100);
    const auto mode = value.value("replay_gain_mode", 0);
    if (mode >= 0 && mode <= static_cast<int>(audio::ReplayGainMode::album)) {
        snapshot.replay_gain_mode = static_cast<audio::ReplayGainMode>(mode);
    }
    snapshot.replay_gain_preamps.with_gain_db = value.value("preamp_with_gain_db", 0.0F);
    snapshot.replay_gain_preamps.without_gain_db = value.value("preamp_without_gain_db", 0.0F);
    if (const auto buffer = value.find("configured_buffer"); buffer != value.end()) {
        snapshot.configured_buffer = buffer_from_json(*buffer);
    }
    if (const auto buffer = value.find("active_buffer"); buffer != value.end()) {
        snapshot.active_buffer = buffer_from_json(*buffer);
    }
    snapshot.underrun_count = value.value("underruns", std::uint64_t{0});
    snapshot.output_target = optional_value<std::string>(value, "output_target");
    snapshot.default_output_target = optional_value<std::string>(value, "default_output");
    snapshot.output_target_available = value.value("output_available", true);
    snapshot.output_suspended = value.value("output_suspended", false);
    if (const auto devices = value.find("devices"); devices != value.end() && devices->is_array()) {
        for (const auto& device : *devices) {
            snapshot.devices.push_back(
                audio::PipeWireDevice{.name = device.value("name", std::string{}),
                                      .description = device.value("description", std::string{})});
        }
    }
    if (const auto error = value.find("error"); error != value.end() && error->is_string()) {
        snapshot.error = core::Error{
            .code = core::ErrorCode::backend, .message = error->get<std::string>(), .context = {}};
    }
    return snapshot;
}

} // namespace trackknife::output

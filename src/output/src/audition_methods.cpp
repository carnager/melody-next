// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/output/audition_methods.hpp"

#include "trackknife/core/local_sources.hpp"
#include "trackknife/output/audition_wire.hpp"

#include <utility>

namespace trackknife::output {
namespace {

using protocol::Json;

[[nodiscard]] core::Error bad_params(std::string message, std::string param) {
    return core::Error{.code = core::ErrorCode::invalid_argument,
                       .message = std::move(message),
                       .context = {{.key = "param", .value = std::move(param)}}};
}

[[nodiscard]] core::Result<Json> answered(const core::Result<void>& result) {
    if (!result) {
        return std::unexpected(result.error());
    }
    return Json::object();
}

// Where a source is on this machine.
[[nodiscard]] core::Result<std::string>
local_path(const Source& source, const std::optional<std::filesystem::path>& music_root) {
    const std::filesystem::path path{*source.path};
    if (path.is_absolute()) {
        return path.native();
    }
    if (!music_root) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::unsupported,
            .message = "a relative path needs a music root on this agent (--music-root)",
            .context = {{.key = "path", .value = *source.path}}});
    }
    return (*music_root / path).native();
}

[[nodiscard]] core::Result<Source> read_source(const Json& params) {
    const auto source = params.find("source");
    if (source == params.end()) {
        return std::unexpected(bad_params("a source is required", "source"));
    }
    return source_from_json(*source);
}

} // namespace

void register_audition_methods(protocol::Dispatcher& dispatcher,
                               audio::LocalAuditionService& audition,
                               std::optional<std::filesystem::path> music_root) {
    dispatcher.on(
        "audition.load", [&audition, music_root](const Json& params) -> core::Result<Json> {
            auto source = read_source(params);
            if (!source) {
                return std::unexpected(std::move(source.error()));
            }
            const bool play = params.value("play", true);
            const auto position_ms = params.value("position_ms", std::int64_t{0});
            if (source->url) {
                auto loaded =
                    audition.load_network_stream_and_play(*source->url, source->replay_gain);
                if (loaded && !play) {
                    loaded = audition.pause();
                }
                if (loaded && position_ms > 0) {
                    loaded = audition.seek_to_seconds(static_cast<double>(position_ms) / 1000.0);
                }
                return answered(loaded);
            }
            auto path = local_path(*source, music_root);
            if (!path) {
                return std::unexpected(std::move(path.error()));
            }
            if (!play) {
                // Held paused where the engine says: a restore, or
                // music moved here from another room.
                auto revision = core::observe_local_source_revision(*path);
                if (!revision) {
                    return std::unexpected(std::move(revision.error()));
                }
                return answered(audition.restore_paused(*path, *revision, source->selection,
                                                        source->segment, position_ms,
                                                        source->replay_gain));
            }
            return answered(source->segment
                                ? audition.load_selected_segment_and_play(*path, source->selection,
                                                                          *source->segment,
                                                                          source->replay_gain)
                                : audition.load_selected_and_play(*path, source->selection,
                                                                  source->replay_gain));
        });

    dispatcher.on(
        "audition.queue_next", [&audition, music_root](const Json& params) -> core::Result<Json> {
            auto source = read_source(params);
            if (!source) {
                return std::unexpected(std::move(source.error()));
            }
            const auto token = params.value("token", std::uint64_t{0});
            if (source->url) {
                return answered(
                    audition.queue_gapless_network_stream(*source->url, source->replay_gain));
            }
            auto path = local_path(*source, music_root);
            if (!path) {
                return std::unexpected(std::move(path.error()));
            }
            return answered(
                source->segment
                    ? audition.queue_gapless_next_selected_segment(
                          *path, source->selection, *source->segment, source->replay_gain, token)
                    : audition.queue_gapless_next_selected(*path, source->selection,
                                                           source->replay_gain, token));
        });

    dispatcher.on("audition.clear_next", [&audition](const Json&) -> core::Result<Json> {
        return answered(audition.clear_gapless_next());
    });
    dispatcher.on("audition.play", [&audition](const Json&) -> core::Result<Json> {
        return answered(audition.play());
    });
    dispatcher.on("audition.pause", [&audition](const Json&) -> core::Result<Json> {
        return answered(audition.pause());
    });
    dispatcher.on("audition.stop", [&audition](const Json&) -> core::Result<Json> {
        return answered(audition.stop());
    });
    dispatcher.on("audition.seek", [&audition](const Json& params) -> core::Result<Json> {
        const auto seconds = params.find("seconds");
        if (seconds == params.end() || !seconds->is_number()) {
            return std::unexpected(bad_params("seconds must be a number", "seconds"));
        }
        return answered(audition.seek_to_seconds(seconds->get<double>()));
    });
    dispatcher.on("audition.volume", [&audition](const Json& params) -> core::Result<Json> {
        const auto percent = params.find("percent");
        if (percent == params.end() || !percent->is_number_integer()) {
            return std::unexpected(bad_params("percent must be an integer", "percent"));
        }
        return answered(audition.set_volume_percent(percent->get<int>()));
    });
    dispatcher.on("audition.replay_gain", [&audition](const Json& params) -> core::Result<Json> {
        if (const auto mode = params.find("mode"); mode != params.end()) {
            const auto value = mode->is_number_integer() ? mode->get<int>() : -1;
            if (value < 0 || value > static_cast<int>(audio::ReplayGainMode::album)) {
                return std::unexpected(bad_params("mode is off, track or album", "mode"));
            }
            if (auto set = audition.set_replay_gain_mode(static_cast<audio::ReplayGainMode>(value));
                !set) {
                return std::unexpected(std::move(set.error()));
            }
        }
        if (params.contains("preamp_with_gain_db") || params.contains("preamp_without_gain_db")) {
            const auto current = audition.snapshot().replay_gain_preamps;
            const audio::ReplayGainPreamps preamps{
                .with_gain_db = params.value("preamp_with_gain_db", current.with_gain_db),
                .without_gain_db = params.value("preamp_without_gain_db", current.without_gain_db),
            };
            if (auto set = audition.set_replay_gain_preamps(preamps); !set) {
                return std::unexpected(std::move(set.error()));
            }
        }
        return Json::object();
    });
    dispatcher.on("audition.buffer", [&audition](const Json& params) -> core::Result<Json> {
        const audio::PlaybackBufferDurationConfig buffer{
            .capacity = std::chrono::milliseconds{params.value("capacity_ms", std::int64_t{0})},
            .start_threshold =
                std::chrono::milliseconds{params.value("start_threshold_ms", std::int64_t{0})},
        };
        if (!audio::valid_local_audition_buffer_config(buffer)) {
            return std::unexpected(bad_params("not a buffer this agent plays with", "capacity_ms"));
        }
        return answered(audition.set_buffer_config(buffer));
    });
    dispatcher.on("audition.refresh_outputs", [&audition](const Json&) -> core::Result<Json> {
        return answered(audition.refresh_output_devices());
    });
    dispatcher.on("audition.output", [&audition](const Json& params) -> core::Result<Json> {
        const auto target = params.find("target");
        if (target == params.end() || !(target->is_null() || target->is_string())) {
            return std::unexpected(bad_params("target is a sink name or null", "target"));
        }
        return answered(audition.set_output_target(
            target->is_null() ? std::nullopt : std::optional{target->get<std::string>()}));
    });
}

} // namespace trackknife::output

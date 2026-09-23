// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/output/agent_audition.hpp"

#include <chrono>
#include <utility>

namespace trackknife::output {
namespace {

using protocol::Json;

// Long enough for an agent opening a file over a slow share, short enough that
// a vanished agent does not hold the player.
constexpr std::chrono::seconds call_timeout{5};

[[nodiscard]] core::Error offline(const std::string& name) {
    return core::Error{.code = core::ErrorCode::backend,
                       .message = "the output agent is not connected",
                       .context = {{.key = "agent", .value = name}}};
}

[[nodiscard]] bool sounding(const audio::LocalAuditionState state) {
    return state == audio::LocalAuditionState::playing ||
           state == audio::LocalAuditionState::buffering ||
           state == audio::LocalAuditionState::draining;
}

[[nodiscard]] std::string percent_encoded(const std::string& text) {
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string encoded;
    for (const auto character : text) {
        const auto byte = static_cast<unsigned char>(character);
        if ((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
            (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' || byte == '.' ||
            byte == '~') {
            encoded.push_back(character);
        } else {
            encoded.push_back('%');
            encoded.push_back(digits[byte >> 4U]);
            encoded.push_back(digits[byte & 0x0FU]);
        }
    }
    return encoded;
}

} // namespace

AgentAudition::AgentAudition(std::string name, AgentPaths paths)
    : name_(std::move(name)), paths_(std::move(paths)) {}

void AgentAudition::attach(std::unique_ptr<protocol::Client> client, const bool files,
                           std::string reached) {
    std::shared_ptr<protocol::Client> shared{std::move(client)};
    std::weak_ptr<protocol::Client> weak = shared;
    shared->on_event([this, weak](const protocol::Event& event) {
        if (event.name != changed_event) {
            return;
        }
        std::function<void()> changed;
        {
            const std::lock_guard guard{mutex_};
            // A report from a connection since replaced is stale.
            if (weak.lock() != client_) {
                return;
            }
            adopt(event.data);
            changed = changed_;
        }
        if (changed) {
            changed();
        }
    });
    std::shared_ptr<protocol::Client> previous;
    {
        const std::lock_guard guard{mutex_};
        previous = std::exchange(client_, std::move(shared));
        files_ = files;
        reached_ = std::move(reached);
        // A new process knows nothing of what the old one played.
        reported_ = audio::LocalAuditionSnapshot{};
        next_armed_ = false;
        current_raw_.clear();
        next_raw_.clear();
        seen_transitions_ = 0U;
    }
    if (previous) {
        previous->close();
    }
}

bool AgentAudition::online() const {
    const std::lock_guard guard{mutex_};
    return client_ && client_->connected();
}

bool AgentAudition::files() const {
    const std::lock_guard guard{mutex_};
    return files_;
}

std::optional<AgentAudition::LastHeard> AgentAudition::last_heard() const {
    const std::lock_guard guard{mutex_};
    if (!reported_.format && reported_.state == audio::LocalAuditionState::empty) {
        return std::nullopt;
    }
    LastHeard heard;
    heard.playing = sounding(reported_.state);
    if (reported_.format && reported_.format->sample_rate > 0) {
        const auto rate = static_cast<std::int64_t>(reported_.format->sample_rate);
        heard.position_ms = reported_.position_sample / rate * 1000 +
                            reported_.position_sample % rate * 1000 / rate;
    }
    return heard;
}

void AgentAudition::on_changed(std::function<void()> callback) {
    const std::lock_guard guard{mutex_};
    changed_ = std::move(callback);
}

void AgentAudition::adopt(const Json& report) {
    auto parsed = snapshot_from_json(report);
    // A gapless handover: what was armed is what plays now.
    if (parsed.chain_transitions > seen_transitions_) {
        if (!next_raw_.empty()) {
            current_raw_ = std::exchange(next_raw_, {});
        }
        seen_transitions_ = parsed.chain_transitions;
    }
    next_armed_ = report.value("next_armed", false);
    reported_ = std::move(parsed);
}

audio::LocalAuditionSnapshot AgentAudition::snapshot() const {
    const std::lock_guard guard{mutex_};
    auto current = reported_;
    current.raw_path = current_raw_;
    current.next_raw_path = next_armed_ ? next_raw_ : std::string{};
    current.chain_transitions = seen_transitions_;
    if (!(client_ && client_->connected()) && sounding(current.state)) {
        // Gone mid-track: paused, not ended, so nothing advances past a
        // track that did not finish.
        current.state = audio::LocalAuditionState::paused;
    }
    return current;
}

core::Result<Json> AgentAudition::call(const std::string& method, const Json& params) {
    std::shared_ptr<protocol::Client> client;
    {
        const std::lock_guard guard{mutex_};
        client = client_;
    }
    if (!client || !client->connected()) {
        return std::unexpected(offline(name_));
    }
    return client->call(method, params, call_timeout);
}

core::Result<Source>
AgentAudition::source_for(const std::string& raw_path, formats::AudioSourceSelection selection,
                          std::optional<formats::SampleRange> segment,
                          std::optional<formats::ReplayGainInfo> replay_gain) const {
    Source source{.path = std::nullopt,
                  .url = std::nullopt,
                  .selection = selection,
                  .segment = segment,
                  .replay_gain = std::move(replay_gain)};
    bool files = true;
    std::string host = paths_.stream_host;
    {
        const std::lock_guard guard{mutex_};
        files = files_;
        if (host.empty()) {
            host = reached_;
        }
    }
    if (files) {
        const std::filesystem::path path{raw_path};
        if (paths_.music_root) {
            const auto relative = path.lexically_relative(*paths_.music_root);
            if (!relative.empty() && *relative.begin() != "..") {
                source.path = relative.native();
                return source;
            }
        }
        source.path = raw_path;
        return source;
    }
    if (paths_.stream_port == 0U) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::unsupported,
            .message =
                "this agent streams, and the engine serves no streams (start it with --http)",
            .context = {{.key = "agent", .value = name_}}});
    }
    if (host.empty()) {
        host = "127.0.0.1";
    }
    // An IPv6 address is bracketed in a URL, so its colons are not the port's.
    const auto authority = (host.find(':') != std::string::npos ? "[" + host + "]" : host) + ":" +
                           std::to_string(paths_.stream_port);
    // The engine checks the path against what it is playing before serving
    // it; the token is what lets the agent ask.
    source.url = "http://" + authority +
                 "/stream?path=" + percent_encoded(protocol::encode_raw_path(raw_path)) +
                 "&token=" + percent_encoded(paths_.stream_token);
    return source;
}

core::Result<void> AgentAudition::load(std::string raw_path, Source source, const bool play,
                                       const std::int64_t position_ms) {
    Json params{{"source", to_json(source)}, {"play", play}, {"position_ms", position_ms}};
    auto answered = call("audition.load", params);
    if (!answered) {
        return std::unexpected(std::move(answered.error()));
    }
    const std::lock_guard guard{mutex_};
    current_raw_ = std::move(raw_path);
    next_raw_.clear();
    next_armed_ = false;
    seen_transitions_ = reported_.chain_transitions;
    return {};
}

core::Result<void> AgentAudition::arm(std::string raw_path, Source source,
                                      const std::uint64_t occurrence_token) {
    auto answered =
        call("audition.queue_next", Json{{"source", to_json(source)}, {"token", occurrence_token}});
    if (!answered) {
        return std::unexpected(std::move(answered.error()));
    }
    const std::lock_guard guard{mutex_};
    next_raw_ = std::move(raw_path);
    // Armed as far as the engine is concerned until the agent says otherwise.
    next_armed_ = true;
    return {};
}

core::Result<void>
AgentAudition::load_selected_and_play(std::string raw_path, formats::AudioSourceSelection selection,
                                      std::optional<formats::ReplayGainInfo> replay_gain_override) {
    auto source = source_for(raw_path, selection, std::nullopt, std::move(replay_gain_override));
    if (!source) {
        return std::unexpected(std::move(source.error()));
    }
    return load(std::move(raw_path), std::move(*source), true, 0);
}

core::Result<void> AgentAudition::load_selected_segment_and_play(
    std::string raw_path, formats::AudioSourceSelection selection, formats::SampleRange segment,
    std::optional<formats::ReplayGainInfo> replay_gain_override) {
    auto source = source_for(raw_path, selection, segment, std::move(replay_gain_override));
    if (!source) {
        return std::unexpected(std::move(source.error()));
    }
    return load(std::move(raw_path), std::move(*source), true, 0);
}

core::Result<void> AgentAudition::restore_paused(
    std::string raw_path, core::LocalSourceRevision, formats::AudioSourceSelection selection,
    std::optional<formats::SampleRange> segment, const std::int64_t position_ms,
    std::optional<formats::ReplayGainInfo> replay_gain_override) {
    // The revision is this machine's view of the file; the agent sees its own
    // copy through its own mount, with its own inode numbers.
    auto source = source_for(raw_path, selection, segment, std::move(replay_gain_override));
    if (!source) {
        return std::unexpected(std::move(source.error()));
    }
    return load(std::move(raw_path), std::move(*source), false, position_ms);
}

core::Result<void> AgentAudition::queue_gapless_next_selected(
    std::string raw_path, formats::AudioSourceSelection selection,
    std::optional<formats::ReplayGainInfo> replay_gain_override,
    const std::uint64_t occurrence_token) {
    auto source = source_for(raw_path, selection, std::nullopt, std::move(replay_gain_override));
    if (!source) {
        return std::unexpected(std::move(source.error()));
    }
    return arm(std::move(raw_path), std::move(*source), occurrence_token);
}

core::Result<void> AgentAudition::queue_gapless_next_selected_segment(
    std::string raw_path, formats::AudioSourceSelection selection, formats::SampleRange segment,
    std::optional<formats::ReplayGainInfo> replay_gain_override,
    const std::uint64_t occurrence_token) {
    auto source = source_for(raw_path, selection, segment, std::move(replay_gain_override));
    if (!source) {
        return std::unexpected(std::move(source.error()));
    }
    return arm(std::move(raw_path), std::move(*source), occurrence_token);
}

core::Result<void> AgentAudition::clear_gapless_next() {
    auto answered = call("audition.clear_next", Json::object());
    if (!answered) {
        return std::unexpected(std::move(answered.error()));
    }
    const std::lock_guard guard{mutex_};
    next_raw_.clear();
    next_armed_ = false;
    return {};
}

core::Result<void> AgentAudition::play() {
    auto answered = call("audition.play", Json::object());
    return answered ? core::Result<void>{} : std::unexpected(std::move(answered.error()));
}

core::Result<void> AgentAudition::pause() {
    auto answered = call("audition.pause", Json::object());
    return answered ? core::Result<void>{} : std::unexpected(std::move(answered.error()));
}

core::Result<void> AgentAudition::stop() {
    auto answered = call("audition.stop", Json::object());
    const std::lock_guard guard{mutex_};
    // Stopped as far as the engine is concerned even if the agent is gone:
    // what it was playing is no longer this output's business.
    current_raw_.clear();
    next_raw_.clear();
    next_armed_ = false;
    reported_.state = audio::LocalAuditionState::empty;
    return answered ? core::Result<void>{} : std::unexpected(std::move(answered.error()));
}

core::Result<void> AgentAudition::seek_to_seconds(const double target_seconds) {
    auto answered = call("audition.seek", Json{{"seconds", target_seconds}});
    return answered ? core::Result<void>{} : std::unexpected(std::move(answered.error()));
}

core::Result<void> AgentAudition::set_volume_percent(const int percent) {
    auto answered = call("audition.volume", Json{{"percent", percent}});
    return answered ? core::Result<void>{} : std::unexpected(std::move(answered.error()));
}

core::Result<void> AgentAudition::set_replay_gain_mode(const audio::ReplayGainMode mode) {
    auto answered = call("audition.replay_gain", Json{{"mode", static_cast<int>(mode)}});
    return answered ? core::Result<void>{} : std::unexpected(std::move(answered.error()));
}

core::Result<void> AgentAudition::set_replay_gain_preamps(const audio::ReplayGainPreamps preamps) {
    auto answered =
        call("audition.replay_gain", Json{{"preamp_with_gain_db", preamps.with_gain_db},
                                          {"preamp_without_gain_db", preamps.without_gain_db}});
    return answered ? core::Result<void>{} : std::unexpected(std::move(answered.error()));
}

core::Result<void>
AgentAudition::set_buffer_config(const audio::PlaybackBufferDurationConfig buffer_config) {
    auto answered = call("audition.buffer",
                         Json{{"capacity_ms", buffer_config.capacity.count()},
                              {"start_threshold_ms", buffer_config.start_threshold.count()}});
    return answered ? core::Result<void>{} : std::unexpected(std::move(answered.error()));
}

core::Result<void> AgentAudition::refresh_output_devices() {
    auto answered = call("audition.refresh_outputs", Json::object());
    return answered ? core::Result<void>{} : std::unexpected(std::move(answered.error()));
}

core::Result<void> AgentAudition::set_output_target(std::optional<std::string> target) {
    auto answered =
        call("audition.output", Json{{"target", target ? Json(*target) : Json(nullptr)}});
    return answered ? core::Result<void>{} : std::unexpected(std::move(answered.error()));
}

} // namespace trackknife::output

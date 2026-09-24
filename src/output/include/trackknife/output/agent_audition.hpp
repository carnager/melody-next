// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/audio/audition.hpp"
#include "trackknife/audio/local_audition.hpp"
#include "trackknife/output/audition_wire.hpp"
#include "trackknife/output/stream_query.hpp"
#include "trackknife/protocol/client.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace trackknife::output {

// How the engine names its files to an agent (ADR-0228).
struct AgentPaths final {
    // The engine's music root. A path under it goes to an agent with files as
    // relative, to be joined to the agent's own root; any other path goes as
    // it is, which suits a mount at the same place on both machines.
    std::optional<std::filesystem::path> music_root;
    // Where an agent without files fetches a path, and the token that lets
    // it. Port 0 when the engine serves no streams. The host is the address
    // the agent reached the engine at, unless the streams are served at one
    // address only, which is then `stream_host`.
    std::uint16_t stream_port{0};
    std::string stream_host;
    std::string stream_token;
};

// ADR-0228: an output agent as the player sees it -- an audition like any
// other. Each command is a request to the agent; its state is what the agent
// last reported. It outlives its connection: an agent that drops and comes
// back is the same output, and while it is gone it reports itself paused, so
// the player never mistakes a lost agent for a finished track.
// What an agent without the music wants streamed to it: the codecs it can
// decode, and whether it wants Opus rather than the original -- a phone on
// mobile data. Told at registration, and again in its reports whenever it
// changes its mind (on Wi-Fi again, say). What it cannot decode, and a part
// of a file, it is sent converted whatever it wants.
struct StreamWishes final {
    std::vector<std::string> decodes;
    std::optional<StreamFormat> format;

    [[nodiscard]] static StreamWishes from_json(const protocol::Json& params);
    friend bool operator==(const StreamWishes&, const StreamWishes&) = default;
};

class AgentAudition final : public audio::Audition {
  public:
    AgentAudition(std::string name, AgentPaths paths);

    // A connection from this agent, replacing any earlier one. `files` is
    // what it registered: whether it opens files itself or must stream.
    // `reached` is the engine's address as this agent reached it, which is
    // where it can fetch streams too.
    void attach(std::unique_ptr<protocol::Client> client, bool files, std::string reached = {},
                StreamWishes wishes = {});
    [[nodiscard]] bool online() const;
    [[nodiscard]] bool files() const;
    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    // What it was doing when last heard from, for taking up where it left
    // off once it is back: the position, and whether it was playing.
    struct LastHeard final {
        std::int64_t position_ms{0};
        bool playing{false};
    };
    // Nothing when it has not reported since it last connected.
    [[nodiscard]] std::optional<LastHeard> last_heard() const;
    // Runs on the connection's reader thread after each report.
    void on_changed(std::function<void()> callback);
    // Runs when the agent's connection drops, on its reader thread.
    void on_offline(std::function<void()> callback);

    [[nodiscard]] audio::LocalAuditionSnapshot snapshot() const override;
    [[nodiscard]] core::Result<void>
    load_selected_and_play(std::string raw_path, formats::AudioSourceSelection selection,
                           std::optional<formats::ReplayGainInfo> replay_gain_override) override;
    [[nodiscard]] core::Result<void> load_selected_segment_and_play(
        std::string raw_path, formats::AudioSourceSelection selection, formats::SampleRange segment,
        std::optional<formats::ReplayGainInfo> replay_gain_override) override;
    [[nodiscard]] core::Result<void>
    restore_paused(std::string raw_path, core::LocalSourceRevision expected_revision,
                   formats::AudioSourceSelection selection,
                   std::optional<formats::SampleRange> segment, std::int64_t position_ms,
                   std::optional<formats::ReplayGainInfo> replay_gain_override) override;
    [[nodiscard]] core::Result<void>
    queue_gapless_next_selected(std::string raw_path, formats::AudioSourceSelection selection,
                                std::optional<formats::ReplayGainInfo> replay_gain_override,
                                std::uint64_t occurrence_token) override;
    [[nodiscard]] core::Result<void> queue_gapless_next_selected_segment(
        std::string raw_path, formats::AudioSourceSelection selection, formats::SampleRange segment,
        std::optional<formats::ReplayGainInfo> replay_gain_override,
        std::uint64_t occurrence_token) override;
    [[nodiscard]] core::Result<void> clear_gapless_next() override;
    [[nodiscard]] core::Result<void> play() override;
    [[nodiscard]] core::Result<void> pause() override;
    [[nodiscard]] core::Result<void> stop() override;
    [[nodiscard]] core::Result<void> seek_to_seconds(double target_seconds) override;
    [[nodiscard]] core::Result<void> set_volume_percent(int percent) override;
    [[nodiscard]] core::Result<void> set_replay_gain_mode(audio::ReplayGainMode mode) override;
    [[nodiscard]] core::Result<void>
    set_replay_gain_preamps(audio::ReplayGainPreamps preamps) override;
    [[nodiscard]] core::Result<void>
    set_buffer_config(audio::PlaybackBufferDurationConfig buffer_config) override;
    [[nodiscard]] core::Result<void> refresh_output_devices() override;
    [[nodiscard]] core::Result<void> set_output_target(std::optional<std::string> target) override;

  private:
    [[nodiscard]] core::Result<protocol::Json> call(const std::string& method,
                                                    const protocol::Json& params);
    // Where the engine streams `raw_path` to this agent.
    struct StreamUrl final {
        std::string url;
        // Sent as a track of its own: no selection or segment goes with it.
        bool converted{false};
    };
    [[nodiscard]] core::Result<StreamUrl>
    stream_url(const std::string& raw_path, const formats::AudioSourceSelection& selection,
               const std::optional<formats::SampleRange>& segment) const;
    // A load or arm; one naming a file the agent cannot open is sent again
    // as a stream.
    [[nodiscard]] core::Result<protocol::Json>
    call_with_fallback(const std::string& method, protocol::Json params,
                       const std::string& raw_path);
    // The engine's path, as this agent can play it.
    [[nodiscard]] core::Result<Source>
    source_for(const std::string& raw_path, formats::AudioSourceSelection selection,
               std::optional<formats::SampleRange> segment,
               std::optional<formats::ReplayGainInfo> replay_gain) const;
    [[nodiscard]] core::Result<void> load(std::string raw_path, Source source, bool play,
                                          std::int64_t position_ms);
    [[nodiscard]] core::Result<void> arm(std::string raw_path, Source source,
                                         std::uint64_t occurrence_token);
    void adopt(const protocol::Json& report);
    // Tells a newly connected agent what it was asked before.
    void send_wanted_settings();

    const std::string name_;
    const AgentPaths paths_;
    mutable std::mutex mutex_;
    std::shared_ptr<protocol::Client> client_;
    bool files_{true};
    std::string reached_;
    StreamWishes wishes_;
    std::function<void()> changed_;
    std::function<void()> offline_;
    audio::LocalAuditionSnapshot reported_;
    bool next_armed_{false};
    // The engine's own paths for what the agent is playing and has armed;
    // the agent's are its own business.
    std::string current_raw_;
    std::string next_raw_;
    std::uint64_t seen_transitions_{0U};
    // What the engine asked of this output, kept here rather than trusted to
    // the agent: a freshly started agent, or one offline when it was asked,
    // knows none of it, and is told again on connecting.
    std::optional<audio::ReplayGainMode> wanted_mode_;
    std::optional<audio::ReplayGainPreamps> wanted_preamps_;
    std::optional<audio::PlaybackBufferDurationConfig> wanted_buffer_;
};

} // namespace trackknife::output

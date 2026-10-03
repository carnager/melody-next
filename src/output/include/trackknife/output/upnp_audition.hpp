// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "trackknife/audio/audition.hpp"
#include "trackknife/audio/local_audition.hpp"
#include "trackknife/discovery/upnp.hpp"
#include "trackknife/output/stream_query.hpp"
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace trackknife::output {
struct RendererTrack final {
    StreamRequest source;
    std::string url;
    std::string metadata;
    std::int64_t duration_ms{0};
    // When the URL's ticket stops being honoured; none, never. A renderer
    // fetches again on resume and seek, so a track is prepared afresh before
    // then.
    std::optional<std::chrono::system_clock::time_point> expires;
};
// Select an advertised HTTP MIME type, preferring the original only when the
// container is known and the whole file is playable. Never assume a wildcard
// means support for a codec the renderer did not name.
[[nodiscard]] core::Result<std::optional<StreamFormat>>
renderer_stream_format(const std::string& sink, const std::string& mime, bool part);
[[nodiscard]] std::string renderer_didl(const std::string& url, const std::string& mime,
                                        std::int64_t duration_ms, const std::string& title,
                                        const std::string& artist, const std::string& album,
                                        const std::string& artwork);
[[nodiscard]] std::optional<std::int64_t> renderer_time_ms(const std::string& text);
// Apply limits that a renderer's generic MIME list cannot express. The source
// rate is the selected audio stream's decoded rate; zero means unknown.
[[nodiscard]] StreamRequest renderer_compatible_request(StreamRequest request,
                                                        const discovery::UpnpRenderer& renderer,
                                                        int source_sample_rate);

class UpnpAudition final : public audio::Audition {
  public:
    using Prepare = std::function<core::Result<RendererTrack>(const StreamRequest&,
                                                              const discovery::UpnpRenderer&)>;
    UpnpAudition(std::string udn, std::shared_ptr<discovery::UpnpControl> control, Prepare prepare,
                 bool background = true);
    ~UpnpAudition() override;
    void update(discovery::UpnpRenderer renderer);
    [[nodiscard]] bool online() const;
    [[nodiscard]] std::string name() const;
    [[nodiscard]] bool wants_playing() const;
    // Poll on the output worker; exposed for deterministic simulated-renderer tests.
    void poll();
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
    // A command's network part, run on the worker; its error, when it fails.
    using Job = std::function<core::Result<void>()>;
    // What a queued job is about: a new track makes the queued commands about
    // the old one moot; the renderer's details and its volume are not.
    enum class Topic { track, device };
    struct Queued {
        Job job;
        Topic topic{Topic::track};
    };
    // Queues `job` behind the commands before it -- or, without a worker (the
    // tests), runs it now and answers its result. The engine calls in under
    // its player's lock, so nothing here waits on the network or a transcode:
    // what a command means for the snapshot is recorded before it returns.
    core::Result<void> submit(Job job, Topic topic = Topic::track, bool replaces_track = false);
    // A command's failure, shown -- unless a newer command made it moot.
    void failed(std::uint64_t generation, core::Error error, bool stop_playing);
    core::Result<void> load_now(StreamRequest request, bool playing, std::int64_t position_ms,
                                std::uint64_t generation);
    core::Result<void> queue_next(StreamRequest request, std::uint64_t token);
    core::Result<void> arm_now(StreamRequest request, std::uint64_t token);
    core::Result<void> start_now(std::uint64_t generation);
    core::Result<RendererTrack> prepare(StreamRequest request);
    core::Result<discovery::UpnpValues> call(const std::string& action,
                                             discovery::UpnpValues arguments = {});
    [[nodiscard]] bool expiring(const RendererTrack& track) const;
    // The snapshot made to describe `source`, before or after it is prepared.
    void adopt_locked(const StreamRequest& source, std::optional<std::int64_t> duration_ms,
                      std::uint64_t token, bool transition);
    void run();
    std::shared_ptr<discovery::UpnpControl> control_;
    Prepare prepare_;
    const bool background_;

    // Held while a command or a poll talks to the renderer; only the worker
    // (or, without one, the caller) takes it. What it guards is the worker's.
    std::mutex io_mutex_;
    discovery::UpnpRenderer device_;
    std::optional<RendererTrack> current_;
    std::optional<RendererTrack> next_;
    // A track restored while paused is not sent until it is played: setting
    // it would stop whatever the speaker is playing for someone else.
    std::optional<StreamRequest> deferred_;
    // Some renderers reject Seek while STOPPED. A restored position waits
    // here until Play has put the transport into a seekable state.
    std::optional<std::int64_t> pending_seek_ms_;
    std::uint64_t next_token_{0};
    std::string sink_;
    bool gapless_supported_{true};
    bool saw_playing_{false};
    unsigned stopped_while_starting_{0};
    bool startup_rejected_{false};

    // What the engine reads, and the queue; never held across the network.
    mutable std::mutex state_mutex_;
    std::condition_variable wake_;
    discovery::UpnpRenderer renderer_;
    audio::LocalAuditionSnapshot state_;
    bool playing_requested_{false};
    // Bumped by every command that changes what plays, so a poll that began
    // before it does not overwrite it with what the renderer said then.
    std::uint64_t generation_{0};
    std::deque<Queued> jobs_;
    // A continuation the renderer refused, not offered again until the next
    // load: the player asks every tick.
    std::optional<std::pair<std::string, std::optional<formats::SampleRange>>> refused_next_;
    bool stopping_{false};
    std::thread worker_;
};
} // namespace trackknife::output

// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "trackknife/audio/audition.hpp"
#include "trackknife/audio/local_audition.hpp"
#include "trackknife/discovery/upnp.hpp"
#include "trackknife/output/stream_query.hpp"
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace trackknife::output {
struct RendererTrack final {
    StreamRequest source;
    std::string url;
    std::string metadata;
    std::int64_t duration_ms{0};
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
    core::Result<void> load(StreamRequest request, bool playing, std::int64_t position_ms);
    core::Result<void> arm(StreamRequest request, std::uint64_t token);
    core::Result<RendererTrack> prepare(StreamRequest request);
    core::Result<discovery::UpnpValues> call(const std::string& action,
                                             discovery::UpnpValues arguments = {});
    void adopt(const RendererTrack& track, std::uint64_t token, bool transition);
    void run();
    std::shared_ptr<discovery::UpnpControl> control_;
    Prepare prepare_;
    // Serialize SOAP, device updates and track preparation. Snapshot never waits
    // on network I/O: it uses only state_mutex_.
    std::mutex io_mutex_;
    mutable std::mutex state_mutex_;
    std::condition_variable wake_;
    discovery::UpnpRenderer renderer_;
    audio::LocalAuditionSnapshot state_;
    std::optional<RendererTrack> current_;
    std::optional<RendererTrack> next_;
    std::uint64_t next_token_{0};
    std::string sink_;
    bool gapless_supported_{true};
    bool playing_requested_{false};
    bool saw_playing_{false};
    bool stopping_{false};
    std::thread worker_;
};
} // namespace trackknife::output

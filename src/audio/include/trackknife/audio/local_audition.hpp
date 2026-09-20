// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/audio/local_playback.hpp"
#include "trackknife/audio/pipewire_output.hpp"
#include "trackknife/core/error.hpp"
#include "trackknife/core/local_sources.hpp"
#include "trackknife/core/result.hpp"
#include "trackknife/formats/decoder.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace trackknife::audio {

enum class LocalAuditionState {
    empty,
    loading,
    ready,
    buffering,
    playing,
    paused,
    draining,
    ended,
    failed,
};

enum class PlaybackBufferPreset {
    responsive,
    balanced,
    resilient,
};

// Stable preset identifiers are persisted by frontends. The durations size
// Trackknife's decoded-PCM ring at the source rate; they do not force a
// PipeWire graph quantum or promise a particular device latency.
[[nodiscard]] std::string_view playback_buffer_preset_id(PlaybackBufferPreset preset) noexcept;
[[nodiscard]] std::optional<PlaybackBufferPreset>
playback_buffer_preset_from_id(std::string_view id) noexcept;
[[nodiscard]] PlaybackBufferDurationConfig
playback_buffer_preset_config(PlaybackBufferPreset preset) noexcept;
[[nodiscard]] bool valid_local_audition_buffer_config(PlaybackBufferDurationConfig config) noexcept;

struct LocalAuditionConfig {
    PlaybackBufferDurationConfig buffer;
    ReplayGainMode replay_gain_mode{ReplayGainMode::off};
    ReplayGainPreamps replay_gain_preamps{};
    PipeWireOutputConfig output;
    std::chrono::milliseconds producer_period{5};
    std::size_t command_capacity{64U};

    friend bool operator==(const LocalAuditionConfig&, const LocalAuditionConfig&) = default;
};

struct LocalAuditionSnapshot {
    LocalAuditionState state{LocalAuditionState::empty};
    std::string raw_path;
    std::optional<core::LocalSourceRevision> source_revision;
    formats::AudioSourceSelection selection;
    std::optional<formats::SampleRange> segment;
    // Queued gapless continuation and the count of consumed takeovers; the
    // count increments exactly when raw_path flips to the queued source and
    // position/end rebase to the new track.
    std::string next_raw_path;
    std::optional<core::LocalSourceRevision> next_source_revision;
    formats::AudioSourceSelection next_selection;
    std::optional<formats::SampleRange> next_segment;
    std::uint64_t chain_transitions{0U};
    std::uint64_t playback_instance{0U}; // increments on loads and gapless handoffs
    // Opaque caller occurrence tokens distinguish identical consecutive sources.
    std::uint64_t occurrence_token{0U};
    std::uint64_t next_occurrence_token{0U};
    // Caller-supplied ReplayGain replacing the decoder's tag values for
    // the active and queued source (ADR-0139).
    std::optional<formats::ReplayGainInfo> replay_gain_override;
    std::optional<formats::ReplayGainInfo> next_replay_gain_override;
    std::optional<formats::PcmFormat> format;
    std::int64_t position_sample{0};
    std::optional<std::int64_t> end_sample;
    std::size_t buffered_frames{0U};
    std::uint64_t underrun_count{0U};
    formats::ReplayGainInfo effective_replay_gain_info;
    float effective_replay_gain_multiplier{1.0F};
    float decoded_peak_before_gain{0.0F};
    float decoded_peak_after_gain{0.0F};
    PlaybackBufferDurationConfig configured_buffer;
    // The ring cannot be resized while its real-time consumer is attached.
    // A differing active value means the configured value applies on the
    // next ordinary source load.
    std::optional<PlaybackBufferDurationConfig> active_buffer;
    ReplayGainMode replay_gain_mode{ReplayGainMode::off};
    ReplayGainPreamps replay_gain_preamps{};
    int volume_percent{100};
    std::optional<std::string> output_target;
    std::optional<std::string> default_output_target;
    bool output_target_available{true};
    bool output_suspended{false};
    std::uint64_t device_generation{0U};
    std::vector<PipeWireDevice> devices;
    std::optional<core::Error> device_monitor_error;
    std::optional<core::Error> output_recovery_error;
    PipeWireOutputSnapshot output;
    std::optional<core::Error> error;

    friend bool operator==(const LocalAuditionSnapshot&, const LocalAuditionSnapshot&) = default;
};

struct LocalAuditionSourceRelocation {
    std::string source_raw_path;
    std::string target_raw_path;
    core::LocalSourceRevision source_revision;
    core::LocalSourceRevision target_revision;

    friend bool operator==(const LocalAuditionSourceRelocation&,
                           const LocalAuditionSourceRelocation&) = default;
};

struct LocalAuditionSourceRelocationResult {
    std::size_t active_sources_relocated{0U};
    std::size_t queued_sources_relocated{0U};
    std::size_t pending_commands_relocated{0U};
    std::size_t revision_conflicts{0U};
    bool already_applied{false};

    friend bool operator==(const LocalAuditionSourceRelocationResult&,
                           const LocalAuditionSourceRelocationResult&) = default;
};

using LocalAuditionDependentStateCommitter = std::function<core::Result<void>()>;

// Owns every blocking local-audio operation on one serialized worker. Public
// commands only enqueue bounded work and are safe to invoke from the UI thread.
class LocalAuditionService final {
  public:
    [[nodiscard]] static core::Result<std::unique_ptr<LocalAuditionService>>
    create(LocalAuditionConfig config = {});

    LocalAuditionService(const LocalAuditionService&) = delete;
    LocalAuditionService& operator=(const LocalAuditionService&) = delete;
    LocalAuditionService(LocalAuditionService&&) = delete;
    LocalAuditionService& operator=(LocalAuditionService&&) = delete;
    ~LocalAuditionService();

    [[nodiscard]] LocalAuditionSnapshot snapshot() const;

    [[nodiscard]] core::Result<void> load_and_play(std::string raw_path);
    [[nodiscard]] core::Result<void>
    load_selected_and_play(std::string raw_path, formats::AudioSourceSelection selection,
                           std::optional<formats::ReplayGainInfo> replay_gain_override = {});
    // Network media is deliberately explicit: unlike local sources it has no
    // inode revision to observe/revalidate around decoder open.
    [[nodiscard]] core::Result<void>
    load_network_stream_and_play(std::string url,
                                 std::optional<formats::ReplayGainInfo> replay_gain_override = {});
    [[nodiscard]] core::Result<void> load_segment_and_play(std::string raw_path,
                                                           formats::SampleRange segment);
    [[nodiscard]] core::Result<void> load_selected_segment_and_play(
        std::string raw_path, formats::AudioSourceSelection selection, formats::SampleRange segment,
        std::optional<formats::ReplayGainInfo> replay_gain_override = {});
    // Queues the file to continue seamlessly when the current one ends. The
    // continuation must match the active PCM format exactly; on rejection the
    // snapshot's next_raw_path simply stays empty and the caller falls back
    // to an ordinary load at end-of-track. Seeks and loads drop the queue.
    [[nodiscard]] core::Result<void> queue_gapless_next(std::string raw_path);
    [[nodiscard]] core::Result<void>
    queue_gapless_next_selected(std::string raw_path, formats::AudioSourceSelection selection,
                                std::optional<formats::ReplayGainInfo> replay_gain_override = {},
                                std::uint64_t occurrence_token = 0U);
    [[nodiscard]] core::Result<void>
    queue_gapless_network_stream(std::string url,
                                 std::optional<formats::ReplayGainInfo> replay_gain_override = {});
    [[nodiscard]] core::Result<void> queue_gapless_next_segment(std::string raw_path,
                                                                formats::SampleRange segment);
    [[nodiscard]] core::Result<void> queue_gapless_next_selected_segment(
        std::string raw_path, formats::AudioSourceSelection selection, formats::SampleRange segment,
        std::optional<formats::ReplayGainInfo> replay_gain_override = {},
        std::uint64_t occurrence_token = 0U);
    [[nodiscard]] core::Result<void> clear_gapless_next();
    [[nodiscard]] core::Result<void> play();
    [[nodiscard]] core::Result<void> pause();
    [[nodiscard]] core::Result<void> stop();
    [[nodiscard]] core::Result<void> seek_to_sample(std::int64_t target_sample);
    // Queued time-domain seek for protocol adapters that do not know the
    // source rate when they enqueue a load followed by a seek.
    [[nodiscard]] core::Result<void> seek_to_seconds(double target_seconds);
    // Perceptual volume in percent [0, 100]; mapped cubically onto PipeWire's
    // linear stream mixer and reapplied when a new source connects.
    [[nodiscard]] core::Result<void> set_volume_percent(int percent);
    [[nodiscard]] core::Result<void> set_replay_gain_mode(ReplayGainMode mode);
    [[nodiscard]] core::Result<void> set_replay_gain_info(formats::ReplayGainInfo info);
    [[nodiscard]] core::Result<void> set_replay_gain_preamps(ReplayGainPreamps preamps);
    // Selects the decoded-PCM ring policy for the next source load. If a
    // source is active, its immutable ring remains attached and any prepared
    // gapless continuation is dropped so the new policy takes effect at the
    // next track boundary.
    [[nodiscard]] core::Result<void> set_buffer_config(PlaybackBufferDurationConfig buffer_config);
    // Starts or restarts the persistent PipeWire registry/default monitor on
    // the worker. Its initial synchronization is bounded.
    [[nodiscard]] core::Result<void> refresh_output_devices();
    // Selects the PipeWire sink for current and future sources; nullopt is the
    // system default. A loaded source reconnects in place, preserving its
    // position, volume, and play/pause state.
    [[nodiscard]] core::Result<void> set_output_target(std::optional<std::string> target);
    // Revision-qualifies and synchronously re-keys active, gapless-next, and
    // already queued source intents without reopening a decoder or disturbing
    // transport. This is a mutation-worker barrier for a file-publication
    // dependent-state transaction; never call it from the UI thread.
    [[nodiscard]] core::Result<LocalAuditionSourceRelocationResult>
    relocate_source_and_wait(LocalAuditionSourceRelocation relocation);
    // Publication callback composition: re-key audio first, run the durable
    // list/cache transaction while the new binding is visible, and compensate
    // the ephemeral audio change if that transaction fails. Recovery may
    // replay the whole idempotent sequence. Never call this from the UI thread.
    [[nodiscard]] core::Result<LocalAuditionSourceRelocationResult>
    commit_source_relocation_and_wait(
        LocalAuditionSourceRelocation relocation,
        const LocalAuditionDependentStateCommitter& dependent_state_committer);
    [[nodiscard]] core::Result<void> clear();

  private:
    struct Impl;
    explicit LocalAuditionService(std::unique_ptr<Impl> implementation);

    std::unique_ptr<Impl> implementation_;
};

} // namespace trackknife::audio

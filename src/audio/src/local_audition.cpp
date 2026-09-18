// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/audio/local_audition.hpp"

#include "trackknife/core/cancellation.hpp"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <stop_token>
#include <thread>
#include <utility>

namespace trackknife::audio {
namespace {

enum class CommandKind {
    load_and_play,
    load_network_stream_and_play,
    play,
    pause,
    stop,
    seek,
    seek_seconds,
    set_volume,
    set_replay_gain,
    set_replay_gain_info,
    set_replay_gain_preamps,
    set_buffer,
    refresh_devices,
    set_target,
    clear,
    queue_next,
    queue_network_stream,
    clear_next,
    relocate_source,
};

struct Command {
    CommandKind kind{CommandKind::play};
    std::string raw_path;
    formats::AudioSourceSelection selection;
    std::optional<formats::SampleRange> segment;
    std::int64_t target_sample{0};
    ReplayGainMode replay_gain_mode{ReplayGainMode::off};
    ReplayGainPreamps replay_gain_preamps{};
    int volume_percent{100};
    std::optional<std::string> target;
    PlaybackBufferDurationConfig buffer;
    LocalAuditionSourceRelocation relocation;
    std::size_t relocated_pending_commands{0U};
    std::optional<formats::ReplayGainInfo> replay_gain_override;
    std::shared_ptr<std::promise<core::Result<LocalAuditionSourceRelocationResult>>>
        relocation_completion;
};

// Sliders are perceptual: PipeWire's stream mixer is linear amplitude, so the
// familiar PulseAudio-style cubic taper maps percent onto it.
[[nodiscard]] double cubic_volume(const int percent) {
    const auto normalized = static_cast<double>(percent) / 100.0;
    return normalized * normalized * normalized;
}

[[nodiscard]] core::Error invalid_config(std::string message) {
    return core::Error{
        .code = core::ErrorCode::invalid_argument,
        .message = std::move(message),
        .context = {},
    };
}

[[nodiscard]] core::Error no_source_error(const std::string& operation) {
    return core::Error{
        .code = core::ErrorCode::conflict,
        .message = "cannot " + operation + " without a loaded local source",
        .context = {},
    };
}

[[nodiscard]] core::Error output_unavailable_error() {
    return core::Error{
        .code = core::ErrorCode::conflict,
        .message = "the selected PipeWire output is unavailable",
        .context = {},
    };
}

[[nodiscard]] bool output_target_available(const std::vector<PipeWireDevice>& devices,
                                           const std::optional<std::string>& target) {
    if (!target) {
        return !devices.empty();
    }
    return std::ranges::any_of(devices,
                               [&target](const auto& device) { return device.name == *target; });
}

constexpr auto maximum_buffer_capacity = std::chrono::seconds{10};
constexpr auto device_monitor_poll_period = std::chrono::milliseconds{100};
constexpr auto output_recovery_period = std::chrono::seconds{1};

[[nodiscard]] LocalAuditionState map_state(const LocalPlaybackState state) noexcept {
    switch (state) {
    case LocalPlaybackState::stopped:
        return LocalAuditionState::ready;
    case LocalPlaybackState::buffering:
        return LocalAuditionState::buffering;
    case LocalPlaybackState::playing:
        return LocalAuditionState::playing;
    case LocalPlaybackState::paused:
        return LocalAuditionState::paused;
    case LocalPlaybackState::draining:
        return LocalAuditionState::draining;
    case LocalPlaybackState::ended:
        return LocalAuditionState::ended;
    case LocalPlaybackState::failed:
        return LocalAuditionState::failed;
    }
    return LocalAuditionState::failed;
}

} // namespace

struct LocalAuditionService::Impl {
    explicit Impl(LocalAuditionConfig audition_config) : config(std::move(audition_config)) {
        published.configured_buffer = config.buffer;
        published.replay_gain_mode = config.replay_gain_mode;
        published.replay_gain_preamps = config.replay_gain_preamps;
        worker = std::jthread{[this](const std::stop_token stop_token) { run(stop_token); }};
    }

    ~Impl() {
        {
            std::lock_guard lock{cancellation_mutex};
            if (source_cancellation) {
                source_cancellation->request_cancellation();
            }
        }
        worker.request_stop();
        command_ready.notify_all();
        if (worker.joinable()) {
            worker.join();
        }
    }

    [[nodiscard]] core::Result<void> enqueue(Command command) {
        std::lock_guard lock{command_mutex};
        if (command.kind == CommandKind::load_and_play) {
            {
                std::lock_guard cancellation_lock{cancellation_mutex};
                if (source_cancellation) {
                    source_cancellation->request_cancellation();
                }
            }
            // A replacement supersedes transport/source work, but settings
            // submitted before it must execute first so the new source opens
            // with the user's latest output policy.
            std::erase_if(commands, [](const Command& pending) {
                return pending.kind != CommandKind::set_volume &&
                       pending.kind != CommandKind::set_buffer &&
                       pending.kind != CommandKind::set_replay_gain &&
                       pending.kind != CommandKind::set_replay_gain_info &&
                       pending.kind != CommandKind::set_replay_gain_preamps &&
                       pending.kind != CommandKind::set_target &&
                       pending.kind != CommandKind::relocate_source;
            });
        } else if (command.kind == CommandKind::seek || command.kind == CommandKind::seek_seconds) {
            std::erase_if(commands, [](const Command& pending) {
                return pending.kind == CommandKind::seek ||
                       pending.kind == CommandKind::seek_seconds;
            });
        } else if (command.kind == CommandKind::set_volume ||
                   command.kind == CommandKind::set_buffer ||
                   command.kind == CommandKind::set_replay_gain ||
                   command.kind == CommandKind::set_replay_gain_info ||
                   command.kind == CommandKind::set_replay_gain_preamps ||
                   command.kind == CommandKind::refresh_devices ||
                   command.kind == CommandKind::set_target) {
            std::erase_if(commands, [kind = command.kind](const Command& pending) {
                return pending.kind == kind;
            });
        } else if (command.kind == CommandKind::queue_next ||
                   command.kind == CommandKind::clear_next) {
            // Only the newest continuation intent matters.
            std::erase_if(commands, [](const Command& pending) {
                return pending.kind == CommandKind::queue_next ||
                       pending.kind == CommandKind::clear_next;
            });
        }
        if (commands.size() >= config.command_capacity) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::limit_exceeded,
                .message = "local audition command queue is full",
                .context = {{.key = "capacity", .value = std::to_string(config.command_capacity)}},
            });
        }
        if (command.kind == CommandKind::load_and_play) {
            std::lock_guard snapshot_lock{snapshot_mutex};
            published.state = LocalAuditionState::loading;
            published.raw_path = command.raw_path;
            published.source_revision.reset();
            published.selection = command.selection;
            published.segment = command.segment;
            published.next_raw_path.clear();
            published.next_source_revision.reset();
            published.next_selection = {};
            published.next_segment.reset();
            published.replay_gain_override = command.replay_gain_override;
            published.next_replay_gain_override.reset();
            published.format.reset();
            published.position_sample = 0;
            published.end_sample.reset();
            published.buffered_frames = 0U;
            published.underrun_count = 0U;
            published.output = {};
            published.error.reset();
        }
        commands.push_back(std::move(command));
        command_ready.notify_one();
        return {};
    }

    [[nodiscard]] core::Result<LocalAuditionSourceRelocationResult>
    relocate_source_and_wait(LocalAuditionSourceRelocation relocation) {
        if (std::this_thread::get_id() == worker.get_id()) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::invariant,
                .message = "local audition source relocation cannot wait on its own worker",
                .context = {},
            });
        }
        auto completion =
            std::make_shared<std::promise<core::Result<LocalAuditionSourceRelocationResult>>>();
        auto completed = completion->get_future();
        std::size_t relocated_pending_commands = 0U;
        {
            std::lock_guard lock{command_mutex};
            if (worker.get_stop_token().stop_requested()) {
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::cancelled,
                    .message = "local audition stopped before source relocation",
                    .context = {},
                });
            }
            if (commands.size() >= config.command_capacity) {
                return std::unexpected(core::Error{
                    .code = core::ErrorCode::limit_exceeded,
                    .message = "local audition command queue is full",
                    .context = {{.key = "capacity",
                                 .value = std::to_string(config.command_capacity)}},
                });
            }
            for (auto& pending : commands) {
                if ((pending.kind == CommandKind::load_and_play ||
                     pending.kind == CommandKind::queue_next) &&
                    pending.raw_path == relocation.source_raw_path) {
                    pending.raw_path = relocation.target_raw_path;
                    pending.relocation = relocation;
                    ++relocated_pending_commands;
                }
            }
            commands.push_back(Command{
                .kind = CommandKind::relocate_source,
                .raw_path = {},
                .selection = {},
                .segment = std::nullopt,
                .target_sample = 0,
                .volume_percent = 100,
                .target = {},
                .buffer = {},
                .relocation = std::move(relocation),
                .relocated_pending_commands = relocated_pending_commands,
                .replay_gain_override = {},
                .relocation_completion = completion,
            });
        }
        command_ready.notify_one();
        return completed.get();
    }

    [[nodiscard]] LocalAuditionSnapshot snapshot() const {
        std::lock_guard lock{snapshot_mutex};
        return published;
    }

    void clear_pending_next() {
        pending_next_path.clear();
        pending_next_revision.reset();
        pending_next_selection = {};
        pending_next_segment.reset();
        pending_next_replay_gain_override.reset();
    }

    // Rebases the audible-track bookkeeping when the consumer crossed into a
    // queued continuation. Runs on the worker before anything that reads or
    // republishes track-relative state.
    void acknowledge_transitions() {
        if (!source) {
            return;
        }
        while (const auto boundary = source->take_chain_crossing()) {
            track_base = *boundary;
            if (!pending_next_path.empty()) {
                current_path = std::move(pending_next_path);
                pending_next_path.clear();
                current_revision = pending_next_revision;
                pending_next_revision.reset();
                current_selection = pending_next_selection;
                pending_next_selection = {};
                current_segment = pending_next_segment;
                pending_next_segment.reset();
                current_replay_gain_override = pending_next_replay_gain_override;
                pending_next_replay_gain_override.reset();
            }
            const auto playback = source->snapshot();
            current_duration_samples = playback.end_sample
                                           ? std::optional{*playback.end_sample - track_base}
                                           : std::nullopt;
            ++chain_transitions;
        }
    }

    void publish(std::optional<core::Error> error = std::nullopt) {
        acknowledge_transitions();
        LocalAuditionSnapshot next;
        next.raw_path = current_path;
        next.source_revision = current_revision;
        next.selection = current_selection;
        next.segment = current_segment;
        next.next_raw_path = pending_next_path;
        next.next_source_revision = pending_next_revision;
        next.next_selection = pending_next_selection;
        next.next_segment = pending_next_segment;
        next.replay_gain_override = current_replay_gain_override;
        next.next_replay_gain_override = pending_next_replay_gain_override;
        next.chain_transitions = chain_transitions;
        next.error = std::move(error);
        if (source) {
            const auto playback = source->snapshot();
            next.state = map_state(playback.state);
            next.format = source->output_format();
            next.position_sample = playback.position_sample - track_base;
            next.end_sample = current_duration_samples;
            next.buffered_frames = playback.buffered_frames;
            next.underrun_count = playback.underrun_count;
            next.effective_replay_gain_info = playback.replay_gain_info;
            next.effective_replay_gain_multiplier = playback.effective_replay_gain_multiplier;
            next.decoded_peak_before_gain = playback.decoded_peak_before_gain;
            next.decoded_peak_after_gain = playback.decoded_peak_after_gain;
        }
        next.volume_percent = volume_percent;
        next.configured_buffer = config.buffer;
        next.replay_gain_mode = config.replay_gain_mode;
        next.replay_gain_preamps = config.replay_gain_preamps;
        next.active_buffer = active_buffer;
        next.output_target = config.output.target_object;
        next.default_output_target = default_output_target;
        next.output_target_available = output_available;
        next.output_suspended = output_suspended_for_device;
        next.device_generation = device_generation;
        next.devices = devices;
        next.device_monitor_error = device_monitor_error;
        next.output_recovery_error = output_recovery_error;
        if (output) {
            next.output = output->snapshot();
        }
        // A failed load stays failed across unrelated publishes (volume or
        // device updates) until the next load or clear resets it.
        if (!next.error && sticky_failure) {
            next.error = sticky_failure;
        }
        if (!source && sticky_failure) {
            next.state = LocalAuditionState::failed;
        }
        if (next.error && (!source || next.state == LocalAuditionState::failed)) {
            next.state = LocalAuditionState::failed;
        }
        std::lock_guard lock{snapshot_mutex};
        published = std::move(next);
        last_publish = std::chrono::steady_clock::now();
    }

    void fail(core::Error error) {
        if (output) {
            static_cast<void>(output->quiesce());
            output_active = false;
        }
        sticky_failure = error;
        publish(std::move(error));
        std::lock_guard lock{snapshot_mutex};
        published.state = LocalAuditionState::failed;
    }

    [[nodiscard]] core::Result<void> connect_output() {
        if (!source) {
            return std::unexpected(no_source_error("connect an output for"));
        }
        auto connected = PipeWireOutput::connect(*source, config.output);
        if (!connected) {
            return std::unexpected(std::move(connected.error()));
        }
        output.emplace(std::move(*connected));
        if (auto volume_applied = output->set_volume(cubic_volume(volume_percent));
            !volume_applied) {
            output.reset();
            return std::unexpected(std::move(volume_applied.error()));
        }
        output_active = false;
        output_suspended_for_device = false;
        output_recovery_error.reset();
        return {};
    }

    void suspend_for_output_loss() {
        if (!source) {
            return;
        }
        source->pause();
        output_suspended_for_device = true;
        output_recovery_error.reset();
        clear_pending_next();
        source->clear_next();
        if (output) {
            // A removed backend may already have put the stream into ERROR,
            // making normal quiescence impossible. Destroying the adapter
            // stops and joins its thread loop before the source can change.
            static_cast<void>(output->quiesce());
            output_active = false;
            output.reset();
        }
    }

    void recover_suspended_output() {
        if (!output_suspended_for_device || !output_available || !source || output) {
            return;
        }
        last_output_recovery_attempt = std::chrono::steady_clock::now();
        if (auto connected = connect_output(); !connected) {
            output_suspended_for_device = true;
            output_recovery_error = std::move(connected.error());
        }
    }

    void apply_device_snapshot(PipeWireDeviceSnapshot device_snapshot) {
        const bool was_available = output_available;
        devices = std::move(device_snapshot.devices);
        default_output_target = std::move(device_snapshot.default_target);
        device_generation = device_snapshot.generation;
        device_monitor_error = std::move(device_snapshot.error);
        const bool now_available = output_target_available(devices, config.output.target_object);
        if (!device_state_initialized) {
            device_state_initialized = true;
            output_available = now_available;
            publish();
            return;
        }

        output_available = now_available;
        if (was_available && !now_available) {
            suspend_for_output_loss();
        } else if (!was_available && now_available) {
            recover_suspended_output();
        }
        publish();
    }

    void poll_device_monitor() {
        if (!device_monitor || std::chrono::steady_clock::now() - last_device_monitor_poll <
                                   device_monitor_poll_period) {
            return;
        }
        last_device_monitor_poll = std::chrono::steady_clock::now();
        auto device_snapshot = device_monitor->snapshot();
        if (device_snapshot.generation != device_generation) {
            apply_device_snapshot(std::move(device_snapshot));
        } else if (output_suspended_for_device && output_available &&
                   std::chrono::steady_clock::now() - last_output_recovery_attempt >=
                       output_recovery_period) {
            recover_suspended_output();
            publish();
        }
    }

    void clear_source() {
        if (output) {
            const auto quiet = output->quiesce();
            if (!quiet) {
                fail(quiet.error());
                return;
            }
            output_active = false;
        }
        output.reset();
        source.reset();
        active_buffer.reset();
        output_suspended_for_device = false;
        output_recovery_error.reset();
        {
            std::lock_guard lock{cancellation_mutex};
            source_cancellation.reset();
        }
        current_path.clear();
        current_revision.reset();
        current_selection = {};
        current_segment.reset();
        current_replay_gain_override.reset();
        clear_pending_next();
        current_duration_samples.reset();
        track_base = 0;
        sticky_failure.reset();
        publish();
    }

    void load(Command command) {
        const auto local_source = command.kind == CommandKind::load_and_play;
        if (output) {
            const auto quiet = output->quiesce();
            if (!quiet) {
                fail(quiet.error());
                return;
            }
            output_active = false;
        }
        output.reset();
        source.reset();
        active_buffer.reset();
        output_suspended_for_device = false;
        output_recovery_error.reset();
        sticky_failure.reset();
        clear_pending_next();
        track_base = 0;
        current_path = std::move(command.raw_path);
        current_revision.reset();
        current_selection = command.selection;
        current_segment = command.segment;
        current_replay_gain_override = command.replay_gain_override;
        std::optional<core::LocalSourceRevision> observed_revision;
        if (local_source) {
            auto observed = core::observe_local_source_revision(current_path);
            if (!observed) {
                fail(std::move(observed.error()));
                return;
            }
            observed_revision = *observed;
        }
        if (local_source && command.relocation.target_raw_path == current_path &&
            *observed_revision != command.relocation.target_revision) {
            fail(core::Error{
                .code = core::ErrorCode::conflict,
                .message = "relocated local audition source no longer has its published revision",
                .context = {{.key = "path", .value = core::escape_raw_path(current_path)}},
            });
            return;
        }
        std::shared_ptr<core::CancellationSource> cancellation;
        {
            std::lock_guard lock{cancellation_mutex};
            source_cancellation = std::make_shared<core::CancellationSource>();
            cancellation = source_cancellation;
        }
        const auto clear_open_cancellation = [&] {
            std::lock_guard lock{cancellation_mutex};
            if (source_cancellation == cancellation) {
                source_cancellation.reset();
            }
        };
        auto opened =
            command.segment
                ? LocalPlayback::open_selected_segment(
                      current_path, command.selection, *command.segment, config.buffer,
                      cancellation->token(), command.replay_gain_override)
                : LocalPlayback::open_selected(current_path, command.selection, config.buffer,
                                               cancellation->token(), command.replay_gain_override);
        if (!opened) {
            clear_open_cancellation();
            fail(std::move(opened.error()));
            return;
        }
        if (local_source) {
            auto confirmed_revision = core::observe_local_source_revision(current_path);
            if (confirmed_revision && *confirmed_revision != *observed_revision) {
                clear_open_cancellation();
                fail(core::Error{
                    .code = core::ErrorCode::conflict,
                    .message = "local audition source changed while its decoder was opening",
                    .context = {{.key = "path", .value = core::escape_raw_path(current_path)}},
                });
                return;
            }
            if (!confirmed_revision &&
                confirmed_revision.error().code != core::ErrorCode::not_found) {
                clear_open_cancellation();
                fail(std::move(confirmed_revision.error()));
                return;
            }
        }
        source.emplace(std::move(*opened));
        source->set_replay_gain_mode(config.replay_gain_mode);
        source->set_replay_gain_preamps(config.replay_gain_preamps);
        current_revision = observed_revision;
        active_buffer = config.buffer;
        const auto opened_snapshot = source->snapshot();
        track_base = source->sample_range().start_sample;
        current_duration_samples = opened_snapshot.end_sample
                                       ? std::optional{*opened_snapshot.end_sample - track_base}
                                       : std::nullopt;
        if (device_state_initialized && !output_available) {
            output_suspended_for_device = true;
            publish();
            return;
        }
        if (auto connected = connect_output(); !connected) {
            fail(std::move(connected.error()));
            return;
        }
        auto started = source->play();
        if (!started) {
            fail(std::move(started.error()));
            return;
        }
        auto filled = source->fill_buffer();
        if (!filled) {
            fail(std::move(filled.error()));
            return;
        }
        auto active = output->activate();
        if (!active) {
            fail(std::move(active.error()));
            return;
        }
        output_active = true;
        publish();
    }

    void play() {
        if (!source) {
            publish(no_source_error("play"));
            return;
        }
        if (!output_available) {
            publish(output_unavailable_error());
            return;
        }
        if (!output) {
            if (auto connected = connect_output(); !connected) {
                fail(std::move(connected.error()));
                return;
            }
        }
        const bool restart = source->snapshot().state == LocalPlaybackState::ended;
        if (restart) {
            if (output_active) {
                auto drained = output->drain();
                if (!drained) {
                    fail(std::move(drained.error()));
                    return;
                }
            }
            auto quiet = output->quiesce();
            if (!quiet) {
                fail(std::move(quiet.error()));
                return;
            }
            output_active = false;
        }
        auto started = source->play();
        if (!started) {
            fail(std::move(started.error()));
            return;
        }
        if (restart) {
            track_base = source->sample_range().start_sample;
            const auto restarted_snapshot = source->snapshot();
            current_duration_samples =
                restarted_snapshot.end_sample
                    ? std::optional{*restarted_snapshot.end_sample - track_base}
                    : std::nullopt;
        }
        auto filled = source->fill_buffer();
        if (!filled) {
            fail(std::move(filled.error()));
            return;
        }
        auto active = output->activate();
        if (!active) {
            fail(std::move(active.error()));
            return;
        }
        output_active = true;
        publish();
    }

    void pause() {
        if (!source) {
            publish(no_source_error("pause"));
            return;
        }
        source->pause();
        if (output) {
            auto quiet = output->quiesce();
            if (!quiet) {
                fail(std::move(quiet.error()));
                return;
            }
            output_active = false;
        }
        publish();
    }

    void stop() {
        if (!source) {
            publish(no_source_error("stop"));
            return;
        }
        acknowledge_transitions();
        if (output) {
            auto quiet = output->quiesce();
            if (!quiet) {
                fail(std::move(quiet.error()));
                return;
            }
            output_active = false;
        }
        auto stopped = source->stop();
        // The core collapses the chain back to single-source semantics.
        track_base = 0;
        clear_pending_next();
        if (!stopped) {
            fail(std::move(stopped.error()));
            return;
        }
        track_base = source->sample_range().start_sample;
        const auto stopped_snapshot = source->snapshot();
        current_duration_samples = stopped_snapshot.end_sample
                                       ? std::optional{*stopped_snapshot.end_sample - track_base}
                                       : std::nullopt;
        publish();
    }

    void queue_next_source(Command command) {
        const auto local_source = command.kind == CommandKind::queue_next;
        if (!source) {
            publish(no_source_error("queue a gapless continuation for"));
            return;
        }
        acknowledge_transitions();
        if (source->snapshot().chain_boundary_sample) {
            publish();
            return;
        }
        source->clear_next();
        // A new duration policy needs a newly allocated ring. Keep this
        // boundary non-gapless so the following ordinary load can apply it.
        if (!output_available || (active_buffer && *active_buffer != config.buffer)) {
            source->clear_next();
            clear_pending_next();
            publish();
            return;
        }
        auto path = std::move(command.raw_path);
        std::optional<core::LocalSourceRevision> observed_revision;
        if (local_source) {
            auto observed = core::observe_local_source_revision(path);
            if (!observed) {
                source->clear_next();
                clear_pending_next();
                publish();
                return;
            }
            observed_revision = *observed;
        }
        if (local_source && command.relocation.target_raw_path == path &&
            *observed_revision != command.relocation.target_revision) {
            source->clear_next();
            clear_pending_next();
            publish(core::Error{
                .code = core::ErrorCode::conflict,
                .message = "relocated gapless source no longer has its published revision",
                .context = {{.key = "path", .value = core::escape_raw_path(path)}},
            });
            return;
        }
        // A rejected continuation (format change, unreadable file) is not a
        // playback failure: next_raw_path simply stays empty and the caller
        // falls back to an ordinary load at end-of-track.
        const auto queued =
            command.segment
                ? source->queue_next_selected_segment(path, command.selection, *command.segment, {},
                                                      command.replay_gain_override)
                : source->queue_next_selected(path, command.selection, {},
                                              command.replay_gain_override);
        if (queued) {
            bool revision_matches = !local_source;
            if (local_source) {
                auto confirmed_revision = core::observe_local_source_revision(path);
                revision_matches =
                    (confirmed_revision && *confirmed_revision == *observed_revision) ||
                    (!confirmed_revision &&
                     confirmed_revision.error().code == core::ErrorCode::not_found);
            }
            if (revision_matches) {
                pending_next_path = std::move(path);
                pending_next_revision = observed_revision;
                pending_next_selection = command.selection;
                pending_next_segment = command.segment;
                pending_next_replay_gain_override = command.replay_gain_override;
            } else {
                source->clear_next();
                clear_pending_next();
            }
        } else {
            clear_pending_next();
        }
        publish();
    }

    void relocate_source(Command command) {
        acknowledge_transitions();
        LocalAuditionSourceRelocationResult result{
            .active_sources_relocated = 0U,
            .queued_sources_relocated = 0U,
            .pending_commands_relocated = command.relocated_pending_commands,
            .revision_conflicts = 0U,
            .already_applied = false,
        };
        const auto& relocation = command.relocation;
        if (source && current_path == relocation.source_raw_path) {
            if (current_revision && *current_revision == relocation.source_revision) {
                current_path = relocation.target_raw_path;
                current_revision = relocation.target_revision;
                result.active_sources_relocated = 1U;
            } else {
                ++result.revision_conflicts;
            }
        } else if (source && current_path == relocation.target_raw_path) {
            if (current_revision && *current_revision == relocation.target_revision) {
                result.already_applied = true;
            } else {
                ++result.revision_conflicts;
            }
        }
        if (!pending_next_path.empty() && pending_next_path == relocation.source_raw_path) {
            if (pending_next_revision && *pending_next_revision == relocation.source_revision) {
                pending_next_path = relocation.target_raw_path;
                pending_next_revision = relocation.target_revision;
                result.queued_sources_relocated = 1U;
            } else {
                ++result.revision_conflicts;
            }
        } else if (!pending_next_path.empty() && pending_next_path == relocation.target_raw_path) {
            if (pending_next_revision && *pending_next_revision == relocation.target_revision) {
                result.already_applied = true;
            } else {
                ++result.revision_conflicts;
            }
        }
        result.already_applied = result.already_applied && result.active_sources_relocated == 0U &&
                                 result.queued_sources_relocated == 0U &&
                                 result.pending_commands_relocated == 0U &&
                                 result.revision_conflicts == 0U;
        publish();
        command.relocation_completion->set_value(result);
    }

    void clear_next_source() {
        acknowledge_transitions();
        if (source) {
            source->clear_next();
            // PCM already in the ring cannot be recalled. Keep its source
            // identity until the consumer crosses the committed boundary.
            if (source->snapshot().chain_boundary_sample) {
                publish();
                return;
            }
        }
        clear_pending_next();
        publish();
    }

    void seek(const std::int64_t target_sample) {
        if (!source) {
            publish(no_source_error("seek"));
            return;
        }
        acknowledge_transitions();
        const auto before = source->snapshot().state;
        const bool resume = before == LocalPlaybackState::buffering ||
                            before == LocalPlaybackState::playing ||
                            before == LocalPlaybackState::draining;
        if (output) {
            auto quiet = output->quiesce();
            if (!quiet) {
                fail(std::move(quiet.error()));
                return;
            }
            output_active = false;
        }
        // Track-relative target onto the produced domain; the flush drops any
        // queued continuation, which the caller re-queues afterwards.
        auto sought = source->seek_to_sample(track_base + target_sample);
        clear_pending_next();
        if (!sought) {
            publish(std::move(sought.error()));
            return;
        }
        if (resume && output) {
            auto started = source->play();
            if (!started) {
                fail(std::move(started.error()));
                return;
            }
            auto filled = source->fill_buffer();
            if (!filled) {
                fail(std::move(filled.error()));
                return;
            }
            auto active = output->activate();
            if (!active) {
                fail(std::move(active.error()));
                return;
            }
            output_active = true;
        }
        publish();
    }

    void refresh_devices() {
        auto monitored = PipeWireDeviceMonitor::connect(config.output.transition_timeout);
        if (!monitored) {
            device_monitor_error = std::move(monitored.error());
            publish();
            return;
        }
        device_monitor.emplace(std::move(*monitored));
        apply_device_snapshot(device_monitor->snapshot());
    }

    void set_target(std::optional<std::string> target) {
        config.output.target_object = std::move(target);
        output_available = !device_state_initialized ||
                           output_target_available(devices, config.output.target_object);
        if (!output_available) {
            suspend_for_output_loss();
            publish();
            return;
        }
        if (!source) {
            publish();
            return;
        }
        const auto before = source->snapshot().state;
        const bool resume = before == LocalPlaybackState::buffering ||
                            before == LocalPlaybackState::playing ||
                            before == LocalPlaybackState::draining;
        const bool was_suspended = output_suspended_for_device;
        if (output) {
            auto quiet = output->quiesce();
            if (!quiet) {
                fail(std::move(quiet.error()));
                return;
            }
            output_active = false;
            output.reset();
        }
        if (auto connected = connect_output(); !connected) {
            if (was_suspended) {
                output_suspended_for_device = true;
                output_recovery_error = std::move(connected.error());
                publish();
            } else {
                fail(std::move(connected.error()));
            }
            return;
        }
        if (resume) {
            auto started = source->play();
            if (!started) {
                fail(std::move(started.error()));
                return;
            }
            auto filled = source->fill_buffer();
            if (!filled) {
                fail(std::move(filled.error()));
                return;
            }
            auto active = output->activate();
            if (!active) {
                fail(std::move(active.error()));
                return;
            }
            output_active = true;
        }
        publish();
    }

    void set_volume(const int percent) {
        volume_percent = percent;
        if (output) {
            // A volume failure is reported but never tears down playback.
            if (auto applied = output->set_volume(cubic_volume(percent)); !applied) {
                publish(std::move(applied.error()));
                return;
            }
        }
        publish();
    }

    void set_buffer(const PlaybackBufferDurationConfig buffer_config) {
        config.buffer = buffer_config;
        if (source && active_buffer && *active_buffer != config.buffer) {
            source->clear_next();
            clear_pending_next();
        }
        publish();
    }

    void execute(Command command) {
        switch (command.kind) {
        case CommandKind::load_and_play:
        case CommandKind::load_network_stream_and_play:
            load(std::move(command));
            break;
        case CommandKind::play:
            play();
            break;
        case CommandKind::pause:
            pause();
            break;
        case CommandKind::stop:
            stop();
            break;
        case CommandKind::seek:
            seek(command.target_sample);
            break;
        case CommandKind::seek_seconds:
            if (source) {
                const auto samples = static_cast<long double>(command.target_sample) *
                                     source->output_format().sample_rate / 1'000.0L;
                seek(static_cast<std::int64_t>(std::min<long double>(
                    samples, static_cast<long double>(std::numeric_limits<std::int64_t>::max()))));
            } else {
                seek(0);
            }
            break;
        case CommandKind::set_replay_gain:
            config.replay_gain_mode = command.replay_gain_mode;
            if (source) {
                source->set_replay_gain_mode(config.replay_gain_mode);
            }
            publish();
            break;
        case CommandKind::set_replay_gain_info:
            current_replay_gain_override = command.replay_gain_override;
            if (source && command.replay_gain_override) {
                source->set_replay_gain_info(*command.replay_gain_override);
            }
            publish();
            break;
        case CommandKind::set_replay_gain_preamps:
            config.replay_gain_preamps = command.replay_gain_preamps;
            if (source) {
                source->set_replay_gain_preamps(config.replay_gain_preamps);
            }
            publish();
            break;
        case CommandKind::set_volume:
            set_volume(command.volume_percent);
            break;
        case CommandKind::set_buffer:
            set_buffer(command.buffer);
            break;
        case CommandKind::refresh_devices:
            refresh_devices();
            break;
        case CommandKind::set_target:
            set_target(std::move(command.target));
            break;
        case CommandKind::clear:
            clear_source();
            break;
        case CommandKind::queue_next:
        case CommandKind::queue_network_stream:
            queue_next_source(std::move(command));
            break;
        case CommandKind::clear_next:
            clear_next_source();
            break;
        case CommandKind::relocate_source:
            relocate_source(std::move(command));
            break;
        }
    }

    void produce() {
        poll_device_monitor();
        if (!source || !output) {
            return;
        }
        const auto before = source->snapshot().state;
        // The real-time renderer can finish the stream between producer
        // ticks; the terminal snapshot must still be published and the
        // output drained exactly once, or the UI keeps seeing a stale
        // "draining" forever.
        if (before == LocalPlaybackState::ended && output_active) {
            auto drained = output->drain();
            if (!drained) {
                fail(std::move(drained.error()));
                return;
            }
            auto quiet = output->quiesce();
            if (!quiet) {
                fail(std::move(quiet.error()));
                return;
            }
            output_active = false;
            publish();
            return;
        }
        if (before != LocalPlaybackState::buffering && before != LocalPlaybackState::playing &&
            before != LocalPlaybackState::draining) {
            return;
        }
        auto filled = source->fill_buffer();
        if (!filled) {
            fail(std::move(filled.error()));
            return;
        }
        if (source->snapshot().state == LocalPlaybackState::ended && output_active) {
            auto drained = output->drain();
            if (!drained) {
                fail(std::move(drained.error()));
                return;
            }
            auto quiet = output->quiesce();
            if (!quiet) {
                fail(std::move(quiet.error()));
                return;
            }
            output_active = false;
            publish();
            return;
        }
        constexpr auto publish_period = std::chrono::milliseconds{33};
        if (std::chrono::steady_clock::now() - last_publish >= publish_period) {
            publish();
        }
    }

    void run(const std::stop_token stop_token) {
        while (!stop_token.stop_requested()) {
            std::optional<Command> command;
            {
                std::unique_lock lock{command_mutex};
                command_ready.wait_for(lock, config.producer_period, [this, &stop_token] {
                    return stop_token.stop_requested() || !commands.empty();
                });
                if (stop_token.stop_requested()) {
                    break;
                }
                if (!commands.empty()) {
                    command.emplace(std::move(commands.front()));
                    commands.pop_front();
                }
            }
            if (command) {
                execute(std::move(*command));
            } else {
                produce();
            }
        }
        if (output) {
            static_cast<void>(output->quiesce());
            output_active = false;
        }
        output.reset();
        source.reset();
    }

    LocalAuditionConfig config;
    mutable std::mutex snapshot_mutex;
    LocalAuditionSnapshot published;
    std::mutex command_mutex;
    std::condition_variable command_ready;
    std::deque<Command> commands;
    std::mutex cancellation_mutex;
    std::shared_ptr<core::CancellationSource> source_cancellation;
    std::optional<LocalPlayback> source;
    std::optional<PlaybackBufferDurationConfig> active_buffer;
    std::optional<PipeWireOutput> output;
    std::optional<PipeWireDeviceMonitor> device_monitor;
    bool output_active{false};
    bool output_available{true};
    bool output_suspended_for_device{false};
    bool device_state_initialized{false};
    int volume_percent{100};
    std::vector<PipeWireDevice> devices;
    std::optional<std::string> default_output_target;
    std::optional<core::Error> device_monitor_error;
    std::optional<core::Error> output_recovery_error;
    std::uint64_t device_generation{0U};
    std::optional<core::Error> sticky_failure;
    std::string current_path;
    std::optional<core::LocalSourceRevision> current_revision;
    formats::AudioSourceSelection current_selection;
    std::optional<formats::SampleRange> current_segment;
    std::optional<formats::ReplayGainInfo> current_replay_gain_override;
    // Gapless bookkeeping: the queued continuation's path, the produced-domain
    // sample where the audible track begins, and the consumed takeover count.
    std::string pending_next_path;
    std::optional<core::LocalSourceRevision> pending_next_revision;
    formats::AudioSourceSelection pending_next_selection;
    std::optional<formats::SampleRange> pending_next_segment;
    std::optional<formats::ReplayGainInfo> pending_next_replay_gain_override;
    std::optional<std::int64_t> current_duration_samples;
    std::int64_t track_base{0};
    std::uint64_t chain_transitions{0U};
    std::chrono::steady_clock::time_point last_publish{};
    std::chrono::steady_clock::time_point last_device_monitor_poll{};
    std::chrono::steady_clock::time_point last_output_recovery_attempt{};
    std::jthread worker;
};

LocalAuditionService::LocalAuditionService(std::unique_ptr<Impl> implementation)
    : implementation_(std::move(implementation)) {}

LocalAuditionService::~LocalAuditionService() = default;

core::Result<std::unique_ptr<LocalAuditionService>>
LocalAuditionService::create(LocalAuditionConfig config) {
    if (!valid_local_audition_buffer_config(config.buffer)) {
        return std::unexpected(invalid_config(
            "local audition buffer duration and threshold must be positive, ordered, and no "
            "larger than 10 seconds"));
    }
    if (config.replay_gain_mode != ReplayGainMode::off &&
        config.replay_gain_mode != ReplayGainMode::track &&
        config.replay_gain_mode != ReplayGainMode::album) {
        return std::unexpected(invalid_config("invalid ReplayGain mode"));
    }
    if (config.output.stream_name.empty()) {
        return std::unexpected(invalid_config("local audition stream name must not be empty"));
    }
    if (config.output.transition_timeout <= std::chrono::milliseconds::zero()) {
        return std::unexpected(invalid_config("local audition output timeout must be positive"));
    }
    if (config.producer_period <= std::chrono::milliseconds::zero()) {
        return std::unexpected(invalid_config("local audition producer period must be positive"));
    }
    if (config.command_capacity == 0U) {
        return std::unexpected(invalid_config("local audition command capacity must be positive"));
    }
    return std::unique_ptr<LocalAuditionService>{
        new LocalAuditionService{std::make_unique<Impl>(std::move(config))}};
}

LocalAuditionSnapshot LocalAuditionService::snapshot() const { return implementation_->snapshot(); }

core::Result<void> LocalAuditionService::load_and_play(std::string raw_path) {
    return load_selected_and_play(std::move(raw_path), {});
}

core::Result<void> LocalAuditionService::load_selected_and_play(
    std::string raw_path, formats::AudioSourceSelection selection,
    std::optional<formats::ReplayGainInfo> replay_gain_override) {
    if (raw_path.empty()) {
        return std::unexpected(invalid_config("local audition path must not be empty"));
    }
    return implementation_->enqueue(Command{.kind = CommandKind::load_and_play,
                                            .raw_path = std::move(raw_path),
                                            .selection = selection,
                                            .segment = std::nullopt,
                                            .target_sample = 0,
                                            .volume_percent = 100,
                                            .target = {},
                                            .buffer = {},
                                            .relocation = {},
                                            .relocated_pending_commands = 0U,
                                            .replay_gain_override = replay_gain_override,
                                            .relocation_completion = {}});
}

core::Result<void> LocalAuditionService::load_network_stream_and_play(
    std::string url, std::optional<formats::ReplayGainInfo> replay_gain_override) {
    if (!url.starts_with("http://") && !url.starts_with("https://")) {
        return std::unexpected(invalid_config("network audition URL must use HTTP or HTTPS"));
    }
    return implementation_->enqueue(Command{
        .kind = CommandKind::load_network_stream_and_play,
        .raw_path = std::move(url),
        .selection = {},
        .segment = std::nullopt,
        .target_sample = 0,
        .volume_percent = 100,
        .target = {},
        .buffer = {},
        .relocation = {},
        .relocated_pending_commands = 0U,
        .replay_gain_override = replay_gain_override,
        .relocation_completion = {},
    });
}

core::Result<void> LocalAuditionService::load_segment_and_play(std::string raw_path,
                                                               const formats::SampleRange segment) {
    return load_selected_segment_and_play(std::move(raw_path), {}, segment);
}

core::Result<void> LocalAuditionService::load_selected_segment_and_play(
    std::string raw_path, formats::AudioSourceSelection selection,
    const formats::SampleRange segment,
    std::optional<formats::ReplayGainInfo> replay_gain_override) {
    if (raw_path.empty()) {
        return std::unexpected(invalid_config("local audition path must not be empty"));
    }
    return implementation_->enqueue(Command{.kind = CommandKind::load_and_play,
                                            .raw_path = std::move(raw_path),
                                            .selection = selection,
                                            .segment = segment,
                                            .target_sample = 0,
                                            .volume_percent = 100,
                                            .target = {},
                                            .buffer = {},
                                            .relocation = {},
                                            .relocated_pending_commands = 0U,
                                            .replay_gain_override = replay_gain_override,
                                            .relocation_completion = {}});
}

core::Result<void> LocalAuditionService::queue_gapless_next(std::string raw_path) {
    return queue_gapless_next_selected(std::move(raw_path), {});
}

core::Result<void> LocalAuditionService::queue_gapless_next_selected(
    std::string raw_path, formats::AudioSourceSelection selection,
    std::optional<formats::ReplayGainInfo> replay_gain_override) {
    if (raw_path.empty()) {
        return std::unexpected(invalid_config("local audition path must not be empty"));
    }
    return implementation_->enqueue(Command{.kind = CommandKind::queue_next,
                                            .raw_path = std::move(raw_path),
                                            .selection = selection,
                                            .segment = std::nullopt,
                                            .target_sample = 0,
                                            .volume_percent = 100,
                                            .target = {},
                                            .buffer = {},
                                            .relocation = {},
                                            .relocated_pending_commands = 0U,
                                            .replay_gain_override = replay_gain_override,
                                            .relocation_completion = {}});
}

core::Result<void> LocalAuditionService::queue_gapless_network_stream(
    std::string url, std::optional<formats::ReplayGainInfo> replay_gain_override) {
    if (!url.starts_with("http://") && !url.starts_with("https://")) {
        return std::unexpected(invalid_config("network audition URL must use HTTP or HTTPS"));
    }
    return implementation_->enqueue(Command{
        .kind = CommandKind::queue_network_stream,
        .raw_path = std::move(url),
        .selection = {},
        .segment = std::nullopt,
        .target_sample = 0,
        .volume_percent = 100,
        .target = {},
        .buffer = {},
        .relocation = {},
        .relocated_pending_commands = 0U,
        .replay_gain_override = replay_gain_override,
        .relocation_completion = {},
    });
}

core::Result<void>
LocalAuditionService::queue_gapless_next_segment(std::string raw_path,
                                                 const formats::SampleRange segment) {
    return queue_gapless_next_selected_segment(std::move(raw_path), {}, segment);
}

core::Result<void> LocalAuditionService::queue_gapless_next_selected_segment(
    std::string raw_path, formats::AudioSourceSelection selection,
    const formats::SampleRange segment,
    std::optional<formats::ReplayGainInfo> replay_gain_override) {
    if (raw_path.empty()) {
        return std::unexpected(invalid_config("local audition path must not be empty"));
    }
    return implementation_->enqueue(Command{.kind = CommandKind::queue_next,
                                            .raw_path = std::move(raw_path),
                                            .selection = selection,
                                            .segment = segment,
                                            .target_sample = 0,
                                            .volume_percent = 100,
                                            .target = {},
                                            .buffer = {},
                                            .relocation = {},
                                            .relocated_pending_commands = 0U,
                                            .replay_gain_override = replay_gain_override,
                                            .relocation_completion = {}});
}

core::Result<void> LocalAuditionService::clear_gapless_next() {
    return implementation_->enqueue(Command{.kind = CommandKind::clear_next,
                                            .raw_path = {},
                                            .selection = {},
                                            .segment = std::nullopt,
                                            .target_sample = 0,
                                            .volume_percent = 100,
                                            .target = {},
                                            .buffer = {},
                                            .relocation = {},
                                            .relocated_pending_commands = 0U,
                                            .replay_gain_override = {},
                                            .relocation_completion = {}});
}

core::Result<void> LocalAuditionService::play() {
    return implementation_->enqueue(Command{.kind = CommandKind::play,
                                            .raw_path = {},
                                            .selection = {},
                                            .segment = std::nullopt,
                                            .target_sample = 0,
                                            .volume_percent = 100,
                                            .target = {},
                                            .buffer = {},
                                            .relocation = {},
                                            .relocated_pending_commands = 0U,
                                            .replay_gain_override = {},
                                            .relocation_completion = {}});
}

core::Result<void> LocalAuditionService::pause() {
    return implementation_->enqueue(Command{.kind = CommandKind::pause,
                                            .raw_path = {},
                                            .selection = {},
                                            .segment = std::nullopt,
                                            .target_sample = 0,
                                            .volume_percent = 100,
                                            .target = {},
                                            .buffer = {},
                                            .relocation = {},
                                            .relocated_pending_commands = 0U,
                                            .replay_gain_override = {},
                                            .relocation_completion = {}});
}

core::Result<void> LocalAuditionService::stop() {
    return implementation_->enqueue(Command{.kind = CommandKind::stop,
                                            .raw_path = {},
                                            .selection = {},
                                            .segment = std::nullopt,
                                            .target_sample = 0,
                                            .volume_percent = 100,
                                            .target = {},
                                            .buffer = {},
                                            .relocation = {},
                                            .relocated_pending_commands = 0U,
                                            .replay_gain_override = {},
                                            .relocation_completion = {}});
}

core::Result<void> LocalAuditionService::seek_to_sample(const std::int64_t target_sample) {
    if (target_sample < 0) {
        return std::unexpected(invalid_config("local audition seek sample must not be negative"));
    }
    return implementation_->enqueue(Command{.kind = CommandKind::seek,
                                            .raw_path = {},
                                            .selection = {},
                                            .segment = std::nullopt,
                                            .target_sample = target_sample,
                                            .volume_percent = 100,
                                            .target = {},
                                            .buffer = {},
                                            .relocation = {},
                                            .relocated_pending_commands = 0U,
                                            .replay_gain_override = {},
                                            .relocation_completion = {}});
}

core::Result<void> LocalAuditionService::seek_to_seconds(const double target_seconds) {
    constexpr auto maximum_seconds =
        static_cast<double>(std::numeric_limits<std::int64_t>::max()) / 1'000.0;
    if (!std::isfinite(target_seconds) || target_seconds < 0.0 ||
        target_seconds > maximum_seconds) {
        return std::unexpected(invalid_config("local audition seek time is outside its range"));
    }
    Command command;
    command.kind = CommandKind::seek_seconds;
    command.target_sample = static_cast<std::int64_t>(std::llround(target_seconds * 1'000.0));
    return implementation_->enqueue(std::move(command));
}

core::Result<void> LocalAuditionService::set_replay_gain_mode(const ReplayGainMode mode) {
    if (mode != ReplayGainMode::off && mode != ReplayGainMode::track &&
        mode != ReplayGainMode::album) {
        return std::unexpected(invalid_config("invalid ReplayGain mode"));
    }
    Command command;
    command.kind = CommandKind::set_replay_gain;
    command.replay_gain_mode = mode;
    return implementation_->enqueue(std::move(command));
}

core::Result<void> LocalAuditionService::set_replay_gain_info(formats::ReplayGainInfo info) {
    Command command;
    command.kind = CommandKind::set_replay_gain_info;
    command.replay_gain_override = std::move(info);
    return implementation_->enqueue(std::move(command));
}

core::Result<void> LocalAuditionService::set_replay_gain_preamps(const ReplayGainPreamps preamps) {
    if (!std::isfinite(preamps.with_gain_db) || !std::isfinite(preamps.without_gain_db) ||
        std::abs(preamps.with_gain_db) > maximum_replay_gain_preamp_db ||
        std::abs(preamps.without_gain_db) > maximum_replay_gain_preamp_db) {
        return std::unexpected(invalid_config("ReplayGain preamps must be within ±20 dB"));
    }
    Command command;
    command.kind = CommandKind::set_replay_gain_preamps;
    command.replay_gain_preamps = preamps;
    return implementation_->enqueue(std::move(command));
}

core::Result<void> LocalAuditionService::set_volume_percent(const int percent) {
    if (percent < 0 || percent > 100) {
        return std::unexpected(
            invalid_config("local audition volume must be between 0 and 100 percent"));
    }
    return implementation_->enqueue(Command{.kind = CommandKind::set_volume,
                                            .raw_path = {},
                                            .selection = {},
                                            .segment = std::nullopt,
                                            .target_sample = 0,
                                            .volume_percent = percent,
                                            .target = {},
                                            .buffer = {},
                                            .relocation = {},
                                            .relocated_pending_commands = 0U,
                                            .replay_gain_override = {},
                                            .relocation_completion = {}});
}

core::Result<void>
LocalAuditionService::set_buffer_config(const PlaybackBufferDurationConfig buffer_config) {
    if (!valid_local_audition_buffer_config(buffer_config)) {
        return std::unexpected(invalid_config(
            "local audition buffer duration and threshold must be positive, ordered, and no "
            "larger than 10 seconds"));
    }
    return implementation_->enqueue(Command{.kind = CommandKind::set_buffer,
                                            .raw_path = {},
                                            .selection = {},
                                            .segment = std::nullopt,
                                            .target_sample = 0,
                                            .volume_percent = 100,
                                            .target = {},
                                            .buffer = buffer_config,
                                            .relocation = {},
                                            .relocated_pending_commands = 0U,
                                            .replay_gain_override = {},
                                            .relocation_completion = {}});
}

core::Result<void> LocalAuditionService::refresh_output_devices() {
    return implementation_->enqueue(Command{.kind = CommandKind::refresh_devices,
                                            .raw_path = {},
                                            .selection = {},
                                            .segment = std::nullopt,
                                            .target_sample = 0,
                                            .volume_percent = 100,
                                            .target = {},
                                            .buffer = {},
                                            .relocation = {},
                                            .relocated_pending_commands = 0U,
                                            .replay_gain_override = {},
                                            .relocation_completion = {}});
}

core::Result<void> LocalAuditionService::set_output_target(std::optional<std::string> target) {
    if (target && target->empty()) {
        return std::unexpected(
            invalid_config("local audition output target must be a device name or unset"));
    }
    return implementation_->enqueue(Command{.kind = CommandKind::set_target,
                                            .raw_path = {},
                                            .selection = {},
                                            .segment = std::nullopt,
                                            .target_sample = 0,
                                            .volume_percent = 100,
                                            .target = std::move(target),
                                            .buffer = {},
                                            .relocation = {},
                                            .relocated_pending_commands = 0U,
                                            .replay_gain_override = {},
                                            .relocation_completion = {}});
}

core::Result<LocalAuditionSourceRelocationResult>
LocalAuditionService::relocate_source_and_wait(LocalAuditionSourceRelocation relocation) {
    if (relocation.source_raw_path.empty() || relocation.target_raw_path.empty()) {
        return std::unexpected(
            invalid_config("local audition source relocation paths must not be empty"));
    }
    if (relocation.source_raw_path.find('\0') != std::string::npos ||
        relocation.target_raw_path.find('\0') != std::string::npos) {
        return std::unexpected(
            invalid_config("local audition source relocation paths must not contain NUL bytes"));
    }
    if (relocation.source_raw_path == relocation.target_raw_path) {
        return std::unexpected(
            invalid_config("local audition source relocation paths must be distinct"));
    }
    if (relocation.source_revision.inode == 0U || relocation.target_revision.inode == 0U) {
        return std::unexpected(
            invalid_config("local audition source relocation revisions must identify a file"));
    }
    return implementation_->relocate_source_and_wait(std::move(relocation));
}

core::Result<LocalAuditionSourceRelocationResult>
LocalAuditionService::commit_source_relocation_and_wait(
    LocalAuditionSourceRelocation relocation,
    const LocalAuditionDependentStateCommitter& dependent_state_committer) {
    if (!dependent_state_committer) {
        return std::unexpected(
            invalid_config("local audition source relocation requires dependent-state commit"));
    }
    auto relocated = relocate_source_and_wait(relocation);
    if (!relocated) {
        return std::unexpected(std::move(relocated.error()));
    }
    auto dependent = dependent_state_committer();
    if (dependent) {
        return relocated;
    }

    const auto audio_binding = snapshot();
    const bool target_audio_binding =
        (audio_binding.raw_path == relocation.target_raw_path && audio_binding.source_revision &&
         *audio_binding.source_revision == relocation.target_revision) ||
        (audio_binding.next_raw_path == relocation.target_raw_path &&
         audio_binding.next_source_revision &&
         *audio_binding.next_source_revision == relocation.target_revision);
    if (target_audio_binding) {
        auto compensated = relocate_source_and_wait(LocalAuditionSourceRelocation{
            .source_raw_path = std::move(relocation.target_raw_path),
            .target_raw_path = std::move(relocation.source_raw_path),
            .source_revision = relocation.target_revision,
            .target_revision = relocation.source_revision,
        });
        if (!compensated) {
            dependent.error().context.push_back(
                {.key = "audio_compensation_error", .value = compensated.error().message});
        } else if (compensated->revision_conflicts > 0U) {
            dependent.error().context.push_back(
                {.key = "audio_compensation_error",
                 .value = "a relocated player binding changed before compensation"});
        }
    }
    return std::unexpected(std::move(dependent.error()));
}

core::Result<void> LocalAuditionService::clear() {
    return implementation_->enqueue(Command{.kind = CommandKind::clear,
                                            .raw_path = {},
                                            .selection = {},
                                            .segment = std::nullopt,
                                            .target_sample = 0,
                                            .volume_percent = 100,
                                            .target = {},
                                            .buffer = {},
                                            .relocation = {},
                                            .relocated_pending_commands = 0U,
                                            .replay_gain_override = {},
                                            .relocation_completion = {}});
}

std::string_view playback_buffer_preset_id(const PlaybackBufferPreset preset) noexcept {
    switch (preset) {
    case PlaybackBufferPreset::responsive:
        return "responsive";
    case PlaybackBufferPreset::balanced:
        return "balanced";
    case PlaybackBufferPreset::resilient:
        return "resilient";
    }
    return "balanced";
}

std::optional<PlaybackBufferPreset>
playback_buffer_preset_from_id(const std::string_view id) noexcept {
    if (id == "responsive") {
        return PlaybackBufferPreset::responsive;
    }
    if (id == "balanced") {
        return PlaybackBufferPreset::balanced;
    }
    if (id == "resilient") {
        return PlaybackBufferPreset::resilient;
    }
    return std::nullopt;
}

PlaybackBufferDurationConfig
playback_buffer_preset_config(const PlaybackBufferPreset preset) noexcept {
    using namespace std::chrono_literals;
    switch (preset) {
    case PlaybackBufferPreset::responsive:
        return {.capacity = 250ms, .start_threshold = 50ms};
    case PlaybackBufferPreset::balanced:
        return {.capacity = 750ms, .start_threshold = 100ms};
    case PlaybackBufferPreset::resilient:
        return {.capacity = 2'000ms, .start_threshold = 250ms};
    }
    return {};
}

bool valid_local_audition_buffer_config(const PlaybackBufferDurationConfig config) noexcept {
    return config.capacity > std::chrono::milliseconds::zero() &&
           config.capacity <= maximum_buffer_capacity &&
           config.start_threshold > std::chrono::milliseconds::zero() &&
           config.start_threshold <= config.capacity;
}

} // namespace trackknife::audio

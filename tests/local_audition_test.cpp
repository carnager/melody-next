// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/audio/local_audition.hpp"
#include "trackknife/core/error.hpp"
#include "trackknife/core/stable_id.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <ranges>
#include <string_view>
#include <thread>

namespace {

int failures = 0;

void check(const bool condition, const std::string_view expression, const int line) {
    if (!condition) {
        std::cerr << "line " << line << ": check failed: " << expression << '\n';
        ++failures;
    }
}

#define CHECK(expression) check((expression), #expression, __LINE__)

void append_u16(std::array<unsigned char, 44>& header, const std::size_t offset,
                const std::uint16_t value) {
    header.at(offset) = static_cast<unsigned char>(value & 0xFFU);
    header.at(offset + 1U) = static_cast<unsigned char>((value >> 8U) & 0xFFU);
}

void append_u32(std::array<unsigned char, 44>& header, const std::size_t offset,
                const std::uint32_t value) {
    for (std::size_t byte = 0U; byte < 4U; ++byte) {
        header.at(offset + byte) =
            static_cast<unsigned char>((value >> static_cast<unsigned>(byte * 8U)) & 0xFFU);
    }
}

void write_silent_wave(const std::filesystem::path& path, const std::uint32_t frame_count) {
    constexpr std::uint32_t sample_rate = 48'000U;
    constexpr std::uint16_t channels = 1U;
    constexpr std::uint16_t bits_per_sample = 16U;
    const auto data_bytes = frame_count * (bits_per_sample / 8U);
    std::array<unsigned char, 44> header{};
    constexpr std::array riff{'R', 'I', 'F', 'F'};
    constexpr std::array wave{'W', 'A', 'V', 'E'};
    constexpr std::array format{'f', 'm', 't', ' '};
    constexpr std::array data{'d', 'a', 't', 'a'};
    std::ranges::copy(riff, header.begin());
    append_u32(header, 4U, 36U + data_bytes);
    std::ranges::copy(wave, header.begin() + 8);
    std::ranges::copy(format, header.begin() + 12);
    append_u32(header, 16U, 16U);
    append_u16(header, 20U, 1U);
    append_u16(header, 22U, channels);
    append_u32(header, 24U, sample_rate);
    append_u32(header, 28U, sample_rate * (bits_per_sample / 8U));
    append_u16(header, 32U, bits_per_sample / 8U);
    append_u16(header, 34U, bits_per_sample);
    std::ranges::copy(data, header.begin() + 36);
    append_u32(header, 40U, data_bytes);

    std::ofstream output{path, std::ios::binary};
    output.write(reinterpret_cast<const char*>(header.data()),
                 static_cast<std::streamsize>(header.size()));
    constexpr std::array<char, 2> silence{};
    for (std::uint32_t frame = 0U; frame < frame_count; ++frame) {
        output.write(silence.data(), static_cast<std::streamsize>(silence.size()));
    }
}

template <typename Predicate>
trackknife::audio::LocalAuditionSnapshot wait_for(trackknife::audio::LocalAuditionService& service,
                                                  Predicate predicate) {
    using namespace std::chrono_literals;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    auto snapshot = service.snapshot();
    while (!predicate(snapshot) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(2ms);
        snapshot = service.snapshot();
    }
    return snapshot;
}

} // namespace

int main() {
    using namespace std::chrono_literals;
    using trackknife::audio::LocalAuditionState;
    using trackknife::audio::PlaybackBufferPreset;

    CHECK(trackknife::audio::playback_buffer_preset_id(PlaybackBufferPreset::responsive) ==
          "responsive");
    CHECK(trackknife::audio::playback_buffer_preset_from_id("balanced") ==
          PlaybackBufferPreset::balanced);
    CHECK(!trackknife::audio::playback_buffer_preset_from_id("unknown"));
    CHECK((trackknife::audio::playback_buffer_preset_config(PlaybackBufferPreset::responsive) ==
           trackknife::audio::PlaybackBufferDurationConfig{.capacity = 250ms,
                                                           .start_threshold = 50ms}));
    CHECK((trackknife::audio::playback_buffer_preset_config(PlaybackBufferPreset::balanced) ==
           trackknife::audio::PlaybackBufferDurationConfig{.capacity = 750ms,
                                                           .start_threshold = 100ms}));
    CHECK((trackknife::audio::playback_buffer_preset_config(PlaybackBufferPreset::resilient) ==
           trackknife::audio::PlaybackBufferDurationConfig{.capacity = 2'000ms,
                                                           .start_threshold = 250ms}));

    auto invalid_period = trackknife::audio::LocalAuditionService::create(
        {.buffer = {}, .output = {}, .producer_period = 0ms, .command_capacity = 4U});
    CHECK(!invalid_period);
    CHECK(invalid_period.error().code == trackknife::core::ErrorCode::invalid_argument);
    auto invalid_capacity = trackknife::audio::LocalAuditionService::create(
        {.buffer = {}, .output = {}, .producer_period = 1ms, .command_capacity = 0U});
    CHECK(!invalid_capacity);
    CHECK(invalid_capacity.error().code == trackknife::core::ErrorCode::invalid_argument);

    auto service = trackknife::audio::LocalAuditionService::create(
        {.buffer = {.capacity = 250ms, .start_threshold = 50ms},
         .output = {.stream_name = "Trackknife local audition integration test",
                    .target_object = std::nullopt,
                    .transition_timeout = 3s,
                    .exclusive = false},
         .producer_period = 2ms,
         .command_capacity = 16U});
    CHECK(service.has_value());
    if (!service) {
        return 1;
    }
    CHECK((*service)->snapshot().state == LocalAuditionState::empty);
    CHECK(((*service)->snapshot().configured_buffer ==
           trackknife::audio::PlaybackBufferDurationConfig{.capacity = 250ms,
                                                           .start_threshold = 50ms}));
    CHECK(!(*service)->snapshot().active_buffer);
    CHECK(!(*service)->set_buffer_config({.capacity = 0ms, .start_threshold = 0ms}).has_value());
    CHECK(!(*service)->set_buffer_config({.capacity = 11s, .start_threshold = 100ms}).has_value());
    const auto empty_path = (*service)->load_and_play({});
    CHECK(!empty_path);
    CHECK(empty_path.error().code == trackknife::core::ErrorCode::invalid_argument);

    const auto missing_path =
        (std::filesystem::temp_directory_path() /
         ("trackknife-missing-" + trackknife::core::StableId::random().to_string()))
            .native();
    const auto balanced_buffer =
        trackknife::audio::playback_buffer_preset_config(PlaybackBufferPreset::balanced);
    // A following load supersedes transport work, but not a setting queued
    // before it: the source must observe the latest buffer policy.
    CHECK((*service)->set_replay_gain_mode(trackknife::audio::ReplayGainMode::album).has_value());
    CHECK((*service)->set_buffer_config(balanced_buffer).has_value());
    CHECK((*service)->load_and_play(missing_path).has_value());
    const auto missing = wait_for(**service, [](const auto& snapshot) {
        return snapshot.state == LocalAuditionState::failed;
    });
    CHECK(missing.state == LocalAuditionState::failed);
    CHECK(missing.raw_path == missing_path);
    CHECK(missing.error.has_value());
    CHECK(missing.configured_buffer == balanced_buffer);
    CHECK(missing.replay_gain_mode == trackknife::audio::ReplayGainMode::album);
    CHECK(!(*service)->set_replay_gain_mode(static_cast<trackknife::audio::ReplayGainMode>(99)));
    const auto responsive_buffer =
        trackknife::audio::playback_buffer_preset_config(PlaybackBufferPreset::responsive);
    CHECK((*service)->set_buffer_config(responsive_buffer).has_value());
    const auto responsive_configured = wait_for(**service, [&responsive_buffer](const auto& value) {
        return value.configured_buffer == responsive_buffer;
    });
    CHECK(responsive_configured.configured_buffer == responsive_buffer);

    // Volume is validated at the API boundary, survives without a source, and
    // is reapplied when the next source connects.
    const auto negative_volume = (*service)->set_volume_percent(-1);
    CHECK(!negative_volume);
    CHECK(negative_volume.error().code == trackknife::core::ErrorCode::invalid_argument);
    const auto oversized_volume = (*service)->set_volume_percent(101);
    CHECK(!oversized_volume);
    CHECK((*service)->set_volume_percent(40).has_value());
    const auto volume_set =
        wait_for(**service, [](const auto& snapshot) { return snapshot.volume_percent == 40; });
    CHECK(volume_set.volume_percent == 40);

    const auto path =
        std::filesystem::temp_directory_path() /
        ("trackknife-local-audition-" + trackknife::core::StableId::random().to_string() + ".wav");
    write_silent_wave(path, 480'000U);
    const auto resume_revision = trackknife::core::observe_local_source_revision(path.native());
    CHECK(resume_revision.has_value());
    if (resume_revision) {
        CHECK(
            (*service)->restore_paused(path.native(), *resume_revision, {}, {}, 1250).has_value());
        auto restored = wait_for(**service, [](const auto& snapshot) {
            return snapshot.state == LocalAuditionState::paused ||
                   snapshot.state == LocalAuditionState::failed;
        });
        CHECK(restored.state == LocalAuditionState::paused);
        CHECK(restored.position_sample == 60'000);
        CHECK(restored.output.state == trackknife::audio::PipeWireOutputState::unconnected);
        std::this_thread::sleep_for(30ms);
        CHECK((*service)->snapshot().position_sample == 60'000);
        CHECK((*service)
                  ->restore_paused(path.native(), *resume_revision, {},
                                   trackknife::formats::SampleRange{48'000, 144'000}, 500)
                  .has_value());
        restored = wait_for(**service, [](const auto& snapshot) {
            return snapshot.state == LocalAuditionState::paused ||
                   snapshot.state == LocalAuditionState::failed;
        });
        CHECK(restored.state == LocalAuditionState::paused);
        CHECK(restored.position_sample == 24'000); // relative to logical range
        auto stale = *resume_revision;
        ++stale.size;
        CHECK((*service)->restore_paused(path.native(), stale, {}, {}, 1).has_value());
        CHECK(wait_for(**service, [](const auto& s) {
                  return s.state == LocalAuditionState::failed;
              }).state == LocalAuditionState::failed);
        CHECK((*service)
                  ->restore_paused(path.native(), *resume_revision, {}, {}, 10'000)
                  .has_value());
        CHECK(wait_for(**service, [](const auto& s) {
                  return s.state == LocalAuditionState::failed;
              }).state == LocalAuditionState::failed);
        CHECK(!(*service)->restore_paused(path.native(), *resume_revision, {}, {}, -1));
    }
    CHECK((*service)->load_and_play(path.native()).has_value());
    auto active = wait_for(**service, [](const auto& snapshot) {
        return snapshot.state == LocalAuditionState::playing ||
               snapshot.state == LocalAuditionState::draining ||
               snapshot.state == LocalAuditionState::ended ||
               snapshot.state == LocalAuditionState::failed;
    });
    CHECK(active.source_revision.has_value());
    CHECK(active.replay_gain_mode == trackknife::audio::ReplayGainMode::album);

    // File publication re-keys decoder bindings by exact filesystem revision.
    // The already-open descriptor continues without a reload, seek, or output
    // transition; replay is an explicit no-op and stale evidence is refused.
    const auto relocated_path = std::filesystem::path{path.native() + ".relocated"};
    if (active.source_revision) {
        const auto original_revision = *active.source_revision;
        std::filesystem::rename(path, relocated_path);
        const auto relocated_revision =
            trackknife::core::observe_local_source_revision(relocated_path.native());
        CHECK(relocated_revision.has_value());
        if (relocated_revision) {
            const auto before_relocation = (*service)->snapshot();
            const trackknife::audio::LocalAuditionSourceRelocation relocation{
                .source_raw_path = path.native(),
                .target_raw_path = relocated_path.native(),
                .source_revision = original_revision,
                .target_revision = *relocated_revision,
            };
            std::size_t dependent_commit_count = 0U;
            const auto relocated = (*service)->commit_source_relocation_and_wait(
                relocation, [&]() -> trackknife::core::Result<void> {
                    ++dependent_commit_count;
                    CHECK((*service)->snapshot().raw_path == relocated_path.native());
                    return {};
                });
            CHECK(relocated.has_value());
            CHECK(dependent_commit_count == 1U);
            if (relocated) {
                CHECK(relocated->active_sources_relocated == 1U);
                CHECK(relocated->queued_sources_relocated == 0U);
                CHECK(relocated->pending_commands_relocated == 0U);
                CHECK(relocated->revision_conflicts == 0U);
                CHECK(!relocated->already_applied);
            }
            const auto after_relocation = (*service)->snapshot();
            CHECK(after_relocation.raw_path == relocated_path.native());
            CHECK(after_relocation.source_revision == relocated_revision);
            CHECK(after_relocation.selection == before_relocation.selection);
            CHECK(after_relocation.segment == before_relocation.segment);
            CHECK(after_relocation.format == before_relocation.format);
            CHECK(after_relocation.position_sample >= before_relocation.position_sample);

            const auto replayed = (*service)->commit_source_relocation_and_wait(
                relocation, [&]() -> trackknife::core::Result<void> {
                    ++dependent_commit_count;
                    return {};
                });
            CHECK(replayed.has_value());
            CHECK(dependent_commit_count == 2U);
            if (replayed) {
                CHECK(replayed->active_sources_relocated == 0U);
                CHECK(replayed->revision_conflicts == 0U);
                CHECK(replayed->already_applied);
            }

            auto mismatched_replay = relocation;
            ++mismatched_replay.target_revision.size;
            const auto replay_conflict =
                (*service)->relocate_source_and_wait(std::move(mismatched_replay));
            CHECK(replay_conflict.has_value());
            if (replay_conflict) {
                CHECK(replay_conflict->active_sources_relocated == 0U);
                CHECK(replay_conflict->revision_conflicts == 1U);
                CHECK(!replay_conflict->already_applied);
            }
            CHECK((*service)->snapshot().raw_path == relocated_path.native());
            CHECK((*service)->snapshot().source_revision == relocated_revision);

            auto stale_revision = *relocated_revision;
            ++stale_revision.size;
            const auto refused = (*service)->relocate_source_and_wait({
                .source_raw_path = relocated_path.native(),
                .target_raw_path = relocated_path.native() + ".wrong",
                .source_revision = stale_revision,
                .target_revision = original_revision,
            });
            CHECK(refused.has_value());
            if (refused) {
                CHECK(refused->active_sources_relocated == 0U);
                CHECK(refused->revision_conflicts == 1U);
                CHECK(!refused->already_applied);
            }
            CHECK((*service)->snapshot().raw_path == relocated_path.native());
            CHECK((*service)->snapshot().source_revision == relocated_revision);

            // Replay can encounter an already-rekeyed player before the
            // durable half fails. It must still compensate because the file
            // executor will now roll publication back.
            bool replay_failure_saw_target = false;
            const auto replay_durable_failure = (*service)->commit_source_relocation_and_wait(
                relocation, [&]() -> trackknife::core::Result<void> {
                    replay_failure_saw_target =
                        (*service)->snapshot().raw_path == relocated_path.native();
                    return std::unexpected(trackknife::core::Error{
                        .code = trackknife::core::ErrorCode::database,
                        .message = "injected replay dependent-state failure",
                        .context = {},
                    });
                });
            CHECK(!replay_durable_failure);
            CHECK(replay_failure_saw_target);
            CHECK((*service)->snapshot().raw_path == path.native());
            CHECK((*service)->snapshot().source_revision == active.source_revision);

            std::filesystem::rename(relocated_path, path);
            const auto restored_revision =
                trackknife::core::observe_local_source_revision(path.native());
            CHECK(restored_revision.has_value());
            if (restored_revision) {
                CHECK((*service)->snapshot().source_revision == restored_revision);

                // If the durable half rejects publication, the transient
                // player binding is returned to the source identity before
                // the executor restores that directory entry.
                std::filesystem::rename(path, relocated_path);
                const auto failed_target_revision =
                    trackknife::core::observe_local_source_revision(relocated_path.native());
                CHECK(failed_target_revision.has_value());
                if (failed_target_revision) {
                    bool failure_saw_target = false;
                    const auto durable_failure = (*service)->commit_source_relocation_and_wait(
                        {
                            .source_raw_path = path.native(),
                            .target_raw_path = relocated_path.native(),
                            .source_revision = *restored_revision,
                            .target_revision = *failed_target_revision,
                        },
                        [&]() -> trackknife::core::Result<void> {
                            failure_saw_target =
                                (*service)->snapshot().raw_path == relocated_path.native();
                            return std::unexpected(trackknife::core::Error{
                                .code = trackknife::core::ErrorCode::database,
                                .message = "injected dependent-state failure",
                                .context = {},
                            });
                        });
                    CHECK(!durable_failure);
                    CHECK(durable_failure.error().code == trackknife::core::ErrorCode::database);
                    CHECK(failure_saw_target);
                    CHECK((*service)->snapshot().raw_path == path.native());
                    CHECK((*service)->snapshot().source_revision == restored_revision);
                }
                std::filesystem::rename(relocated_path, path);
            }
        }
    }

    const auto next_path = std::filesystem::path{
        path.native() + "-next-" + trackknife::core::StableId::random().to_string() + ".wav"};
    const auto relocated_next_path = std::filesystem::path{next_path.native() + ".relocated"};
    write_silent_wave(next_path, 48'000U);
    CHECK((*service)->queue_gapless_next(next_path.native()).has_value());
    const auto queued = wait_for(**service, [&next_path](const auto& snapshot) {
        return snapshot.next_raw_path == next_path.native();
    });
    CHECK(queued.next_raw_path == next_path.native());
    CHECK(queued.next_source_revision.has_value());
    if (queued.next_source_revision) {
        std::filesystem::rename(next_path, relocated_next_path);
        const auto relocated_next_revision =
            trackknife::core::observe_local_source_revision(relocated_next_path.native());
        CHECK(relocated_next_revision.has_value());
        if (relocated_next_revision) {
            const auto relocated_next = (*service)->relocate_source_and_wait({
                .source_raw_path = next_path.native(),
                .target_raw_path = relocated_next_path.native(),
                .source_revision = *queued.next_source_revision,
                .target_revision = *relocated_next_revision,
            });
            CHECK(relocated_next.has_value());
            if (relocated_next) {
                CHECK(relocated_next->active_sources_relocated == 0U);
                CHECK(relocated_next->queued_sources_relocated == 1U);
                CHECK(relocated_next->revision_conflicts == 0U);
            }
            CHECK((*service)->snapshot().next_raw_path == relocated_next_path.native());
            CHECK((*service)->snapshot().next_source_revision == relocated_next_revision);
        }
    }
    CHECK((*service)->clear_gapless_next().has_value());
    const auto next_cleared =
        wait_for(**service, [](const auto& snapshot) { return snapshot.next_raw_path.empty(); });
    CHECK(next_cleared.next_raw_path.empty());
    std::filesystem::remove(next_path);
    std::filesystem::remove(relocated_next_path);

    if (active.state == LocalAuditionState::failed) {
        std::cout << "SKIP: PipeWire server/output unavailable: "
                  << (active.error ? active.error->message : "unknown error") << '\n';
        std::filesystem::remove(path);
        return failures == 0 ? 77 : 1;
    }
    CHECK(active.format.has_value());
    CHECK(active.format->sample_rate == 48'000);
    CHECK(active.format->channels == 1);
    CHECK((active.active_buffer == trackknife::audio::PlaybackBufferDurationConfig{
                                       .capacity = 250ms, .start_threshold = 50ms}));
    // The retained percent was applied cubically to the connected stream.
    CHECK(active.volume_percent == 40);
    CHECK(active.output.volume > 0.063 && active.output.volume < 0.065);
    CHECK((*service)->set_volume_percent(100).has_value());
    const auto volume_restored = wait_for(**service, [](const auto& snapshot) {
        return snapshot.volume_percent == 100 && snapshot.output.volume > 0.99;
    });
    CHECK(volume_restored.volume_percent == 100);

    CHECK((*service)->pause().has_value());
    const auto paused = wait_for(**service, [](const auto& snapshot) {
        return snapshot.state == LocalAuditionState::paused;
    });
    CHECK(paused.state == LocalAuditionState::paused);
    const auto paused_position = paused.position_sample;
    std::this_thread::sleep_for(30ms);
    CHECK((*service)->snapshot().position_sample == paused_position);

    // A new profile is published without resizing the ring beneath the RT
    // consumer. The active value changes on the following ordinary load.
    CHECK((*service)->set_buffer_config(balanced_buffer).has_value());
    const auto buffer_pending = wait_for(**service, [&balanced_buffer](const auto& snapshot) {
        return snapshot.configured_buffer == balanced_buffer;
    });
    CHECK(buffer_pending.configured_buffer == balanced_buffer);
    CHECK((buffer_pending.active_buffer == trackknife::audio::PlaybackBufferDurationConfig{
                                               .capacity = 250ms, .start_threshold = 50ms}));

    // Device selection: enumeration fills the snapshot, an explicit target
    // reconnects the paused source in place, and default restores it.
    const auto empty_target = (*service)->set_output_target(std::string{});
    CHECK(!empty_target);
    CHECK(empty_target.error().code == trackknife::core::ErrorCode::invalid_argument);
    CHECK((*service)->refresh_output_devices().has_value());
    const auto with_devices =
        wait_for(**service, [](const auto& snapshot) { return !snapshot.devices.empty(); });
    CHECK(!with_devices.devices.empty());
    CHECK(with_devices.device_generation > 0U);
    CHECK(!with_devices.device_monitor_error.has_value());
    CHECK(with_devices.output_target_available);
    if (!with_devices.devices.empty()) {
        const auto target = with_devices.devices.front().name;
        const auto unavailable_target = "trackknife.test.missing-output";
        CHECK((*service)->set_output_target(unavailable_target).has_value());
        const auto unavailable = wait_for(**service, [&unavailable_target](const auto& snapshot) {
            return snapshot.output_target == unavailable_target &&
                   !snapshot.output_target_available;
        });
        CHECK(!unavailable.output_target_available);
        CHECK(unavailable.output_suspended);
        CHECK(unavailable.state == LocalAuditionState::paused);
        CHECK(unavailable.position_sample == paused_position);
        CHECK(unavailable.output.state == trackknife::audio::PipeWireOutputState::unconnected);

        CHECK((*service)->set_output_target(target).has_value());
        const auto retargeted = wait_for(**service, [&target](const auto& snapshot) {
            return snapshot.output_target == target && snapshot.output_target_available &&
                   snapshot.state == LocalAuditionState::paused;
        });
        CHECK(retargeted.output_target == target);
        CHECK(retargeted.output_target_available);
        CHECK(!retargeted.output_suspended);
        CHECK(retargeted.state == LocalAuditionState::paused);
        CHECK(retargeted.position_sample == paused_position);
        CHECK((*service)->set_output_target(std::nullopt).has_value());
        const auto defaulted = wait_for(**service, [](const auto& snapshot) {
            return !snapshot.output_target.has_value() &&
                   snapshot.state == LocalAuditionState::paused;
        });
        CHECK(!defaulted.output_target.has_value());
    }

    CHECK((*service)->seek_to_sample(24'000).has_value());
    const auto sought = wait_for(**service, [](const auto& snapshot) {
        return snapshot.state == LocalAuditionState::paused && snapshot.position_sample == 24'000;
    });
    CHECK(sought.position_sample == 24'000);
    CHECK((*service)->play().has_value());
    const auto resumed = wait_for(**service, [](const auto& snapshot) {
        return snapshot.position_sample >= 36'000 || snapshot.state == LocalAuditionState::ended ||
               snapshot.state == LocalAuditionState::failed;
    });
    CHECK(resumed.state != LocalAuditionState::failed);
    CHECK(resumed.position_sample >= 36'000);
    CHECK(resumed.output.source_frames >= 12'000U);

    CHECK((*service)->stop().has_value());
    const auto stopped = wait_for(**service, [](const auto& snapshot) {
        return snapshot.state == LocalAuditionState::ready;
    });
    CHECK(stopped.state == LocalAuditionState::ready);
    CHECK(stopped.position_sample == 0);
    CHECK(stopped.output.state == trackknife::audio::PipeWireOutputState::paused);

    constexpr trackknife::formats::SampleRange second_segment{
        .start_sample = 12'000,
        .end_sample = 24'000,
    };
    CHECK((*service)->load_segment_and_play(path.native(), second_segment).has_value());
    const auto segment_playback = wait_for(**service, [](const auto& snapshot) {
        return (snapshot.segment.has_value() && snapshot.state != LocalAuditionState::loading) ||
               snapshot.state == LocalAuditionState::failed;
    });
    CHECK(segment_playback.state != LocalAuditionState::failed);
    CHECK(segment_playback.raw_path == path.native());
    CHECK(segment_playback.segment == second_segment);
    CHECK(segment_playback.active_buffer == balanced_buffer);
    CHECK(segment_playback.end_sample == 12'000);
    CHECK(segment_playback.position_sample >= 0);
    CHECK(segment_playback.position_sample <= 12'000);

    CHECK((*service)->clear().has_value());
    const auto cleared = wait_for(**service, [](const auto& snapshot) {
        return snapshot.state == LocalAuditionState::empty;
    });
    CHECK(cleared.state == LocalAuditionState::empty);

    // A mode change near a gapless boundary cannot recall PCM already in
    // the ring, but it must preserve that continuation's logical identity.
    CHECK((*service)->set_buffer_config(responsive_buffer).has_value());
    constexpr trackknife::formats::SampleRange first_part{.start_sample = 0, .end_sample = 48'000};
    constexpr trackknife::formats::SampleRange next_part{.start_sample = 48'000,
                                                         .end_sample = 96'000};
    CHECK((*service)->load_segment_and_play(path.native(), first_part).has_value());
    const auto playing_first = wait_for(**service, [](const auto& snapshot) {
        return snapshot.state == LocalAuditionState::playing ||
               snapshot.state == LocalAuditionState::failed;
    });
    CHECK(playing_first.state == LocalAuditionState::playing);
    CHECK((*service)->queue_gapless_next_segment(path.native(), next_part).has_value());
    const auto approaching = wait_for(**service, [](const auto& snapshot) {
        return snapshot.position_sample >= 42'000 || snapshot.state == LocalAuditionState::failed;
    });
    CHECK(approaching.next_segment == next_part);
    CHECK((*service)->clear_gapless_next().has_value());
    CHECK((*service)->queue_gapless_next_segment(path.native(), first_part).has_value());
    // A following setting gives an observable acknowledgement that both
    // queue commands above have run on the serialized worker.
    CHECK((*service)->set_replay_gain_mode(trackknife::audio::ReplayGainMode::track).has_value());
    const auto after_change = wait_for(**service, [](const auto& snapshot) {
        return snapshot.replay_gain_mode == trackknife::audio::ReplayGainMode::track;
    });
    CHECK(after_change.next_segment == next_part || after_change.segment == next_part);
    const auto crossed = wait_for(**service, [next_part](const auto& snapshot) {
        return snapshot.segment == next_part || snapshot.state == LocalAuditionState::failed;
    });
    CHECK(crossed.segment == next_part);
    CHECK(crossed.raw_path == path.native());
    CHECK(crossed.chain_transitions == playing_first.chain_transitions + 1U);
    CHECK((*service)->clear().has_value());

    std::filesystem::remove(path);
    return failures == 0 ? 0 : 1;
}

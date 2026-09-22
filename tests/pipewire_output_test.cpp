// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/audio/local_playback.hpp"
#include "trackknife/audio/pipewire_output.hpp"
#include "trackknife/core/error.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
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
    constexpr std::uint32_t sample_rate = 8'000U;
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
    for (std::uint32_t frame = 0U; frame < frame_count; ++frame) {
        constexpr std::array<char, 2> silence{};
        output.write(silence.data(), static_cast<std::streamsize>(silence.size()));
    }
}

} // namespace

int main() {
    using namespace std::chrono_literals;
    using trackknife::audio::LocalPlaybackState;

    const auto path =
        std::filesystem::temp_directory_path() / "trackknife-pipewire-output-silent.wav";
    write_silent_wave(path, 800U);

    auto source = trackknife::audio::LocalPlayback::open(
        path.native(), {.capacity_frames = 800U, .start_threshold_frames = 400U});
    CHECK(source.has_value());
    if (!source) {
        std::filesystem::remove(path);
        return 1;
    }

    const auto empty_name =
        trackknife::audio::PipeWireOutput::connect(*source, {.stream_name = "",
                                                             .target_object = std::nullopt,
                                                             .transition_timeout = 100ms,
                                                             .exclusive = false});
    CHECK(!empty_name);
    CHECK(empty_name.error().code == trackknife::core::ErrorCode::invalid_argument);
    const auto zero_timeout =
        trackknife::audio::PipeWireOutput::connect(*source, {.stream_name = "Trackknife test",
                                                             .target_object = std::nullopt,
                                                             .transition_timeout = 0ms,
                                                             .exclusive = false});
    CHECK(!zero_timeout);
    CHECK(zero_timeout.error().code == trackknife::core::ErrorCode::invalid_argument);
    const auto invalid_monitor = trackknife::audio::PipeWireDeviceMonitor::connect(0ms);
    CHECK(!invalid_monitor);
    CHECK(invalid_monitor.error().code == trackknife::core::ErrorCode::invalid_argument);

    auto output = trackknife::audio::PipeWireOutput::connect(
        *source, {.stream_name = "Trackknife silent integration test",
                  .target_object = std::nullopt,
                  .transition_timeout = 3s,
                  .exclusive = false});
    if (!output) {
        std::cout << "SKIP: PipeWire server/output unavailable: " << output.error().message << '\n';
        std::filesystem::remove(path);
        return failures == 0 ? 77 : 1;
    }

    auto initial = output->snapshot();
    CHECK(initial.state == trackknife::audio::PipeWireOutputState::paused);
    CHECK(initial.node_id.has_value());
    CHECK(initial.callback_count == 0U);
    CHECK(initial.volume == 1.0);

    // Stream volume: applied through PipeWire's mixer, clamped, and reported.
    CHECK(output->set_volume(0.25).has_value());
    CHECK(output->snapshot().volume == 0.25);
    CHECK(output->set_volume(3.0).has_value());
    CHECK(output->snapshot().volume == 1.0);

    // A reachable server enumerates at least one audio sink with usable names.
    const auto devices = trackknife::audio::list_pipewire_output_devices();
    CHECK(devices.has_value());
    CHECK(devices.has_value() && !devices->empty());
    if (devices) {
        for (const auto& device : *devices) {
            CHECK(!device.name.empty());
            CHECK(!device.description.empty());
        }
    }
    auto monitor = trackknife::audio::PipeWireDeviceMonitor::connect(3s);
    CHECK(monitor.has_value());
    if (monitor) {
        const auto initial_monitor = monitor->snapshot();
        std::this_thread::sleep_for(50ms);
        const auto monitored = monitor->snapshot();
        CHECK(monitored.generation >= initial_monitor.generation);
        CHECK(monitored.generation > 0U);
        CHECK(!monitored.error.has_value());
        CHECK(!monitored.devices.empty());
        for (const auto& device : monitored.devices) {
            CHECK(!device.name.empty());
            CHECK(!device.description.empty());
        }
        if (monitored.default_target) {
            CHECK(std::ranges::any_of(monitored.devices, [&monitored](const auto& device) {
                return device.name == *monitored.default_target;
            }));
        }
    }

    // The volume asked for before streaming must still be in force once the
    // stream is streaming. It is set while the node is only paused, and
    // anything that sets it in between -- a session manager restoring a
    // remembered value for this application -- would otherwise win, leaving
    // the first track playing at somebody else's setting until the slider is
    // touched.
    CHECK(output->set_volume(0.5).has_value());

    CHECK(source->play().has_value());
    CHECK(source->fill_buffer().has_value());
    CHECK(source->snapshot().state == LocalPlaybackState::draining);
    CHECK(source->snapshot().buffered_frames == 800U);
    CHECK(output->activate().has_value());

    // The server's own report, not what was asked for: the two can disagree,
    // and only the server's value is what comes out of the speakers.
    const auto streaming_deadline = std::chrono::steady_clock::now() + 2s;
    while (std::abs(output->snapshot().server_volume - 0.5) > 0.01 &&
           std::chrono::steady_clock::now() < streaming_deadline) {
        std::this_thread::sleep_for(5ms);
    }
    CHECK(std::abs(output->snapshot().server_volume - 0.5) <= 0.01);
    CHECK(output->set_volume(1.0).has_value());

    const auto playback_deadline = std::chrono::steady_clock::now() + 3s;
    while (source->snapshot().state != LocalPlaybackState::ended &&
           std::chrono::steady_clock::now() < playback_deadline) {
        CHECK(source->fill_buffer().has_value());
        std::this_thread::sleep_for(1ms);
    }
    CHECK(source->snapshot().state == LocalPlaybackState::ended);
    CHECK(source->snapshot().position_sample == 800);
    CHECK(output->drain().has_value());
    CHECK(output->quiesce().has_value());

    const auto final = output->snapshot();
    CHECK(final.state == trackknife::audio::PipeWireOutputState::paused);
    CHECK(final.callback_count > 0U);
    CHECK(final.device_frames >= 800U);
    CHECK(final.source_frames == 800U);
    CHECK(final.invalid_buffer_count == 0U);
    CHECK(final.error_message.empty());

    std::filesystem::remove(path);
    return failures == 0 ? 0 : 1;
}

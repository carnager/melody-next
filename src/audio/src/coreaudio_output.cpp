// SPDX-License-Identifier: GPL-3.0-only

#include "trackknife/audio/pipewire_output.hpp"

#include <AudioToolbox/AudioToolbox.h>
#include <AudioUnit/AudioUnit.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <ranges>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace trackknife::audio {
namespace {

constexpr std::size_t maximum_output_device_count = 256U;

static_assert(std::atomic<PipeWireOutputState>::is_always_lock_free);
static_assert(std::atomic_bool::is_always_lock_free);
static_assert(std::atomic<double>::is_always_lock_free);
static_assert(std::atomic_uint32_t::is_always_lock_free);
static_assert(std::atomic_uint64_t::is_always_lock_free);

[[nodiscard]] core::Error coreaudio_error(std::string message, const OSStatus status) {
    return core::Error{
        .code = core::ErrorCode::backend,
        .message = std::move(message),
        .context = {{.key = "coreaudio_status", .value = std::to_string(status)}},
    };
}

[[nodiscard]] core::Error invalid_config(std::string message) {
    return core::Error{
        .code = core::ErrorCode::invalid_argument,
        .message = std::move(message),
        .context = {},
    };
}

[[nodiscard]] std::optional<std::string> cf_string(CFStringRef value) {
    if (value == nullptr) {
        return std::nullopt;
    }
    const auto length = CFStringGetLength(value);
    const auto maximum = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8);
    if (maximum < 0) {
        return std::nullopt;
    }
    std::string converted(static_cast<std::size_t>(maximum) + 1U, '\0');
    if (!CFStringGetCString(value, converted.data(), static_cast<CFIndex>(converted.size()),
                            kCFStringEncodingUTF8)) {
        return std::nullopt;
    }
    converted.resize(std::strlen(converted.c_str()));
    return converted;
}

[[nodiscard]] core::Result<std::string> device_string(const AudioDeviceID device,
                                                      const AudioObjectPropertySelector selector,
                                                      std::string label) {
    AudioObjectPropertyAddress address{
        .mSelector = selector,
        .mScope = kAudioObjectPropertyScopeGlobal,
        .mElement = kAudioObjectPropertyElementMain,
    };
    CFStringRef value{nullptr};
    UInt32 size = sizeof(value);
    const auto status = AudioObjectGetPropertyData(device, &address, 0U, nullptr, &size, &value);
    if (status != noErr) {
        return std::unexpected(coreaudio_error("could not read CoreAudio device " + label, status));
    }
    const auto converted = cf_string(value);
    if (value != nullptr) {
        CFRelease(value);
    }
    if (!converted || converted->empty()) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::backend,
            .message = "CoreAudio device has no " + label,
            .context = {{.key = "device_id", .value = std::to_string(device)}},
        });
    }
    return *converted;
}

[[nodiscard]] core::Result<std::vector<AudioDeviceID>> audio_device_ids() {
    AudioObjectPropertyAddress address{
        .mSelector = kAudioHardwarePropertyDevices,
        .mScope = kAudioObjectPropertyScopeGlobal,
        .mElement = kAudioObjectPropertyElementMain,
    };
    UInt32 size{0U};
    auto status =
        AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0U, nullptr, &size);
    if (status != noErr) {
        return std::unexpected(coreaudio_error("could not size CoreAudio device list", status));
    }
    if (size % sizeof(AudioDeviceID) != 0U) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::backend,
            .message = "CoreAudio returned a malformed device list",
            .context = {},
        });
    }
    const auto count = static_cast<std::size_t>(size) / sizeof(AudioDeviceID);
    if (count > maximum_output_device_count) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::limit_exceeded,
            .message = "CoreAudio device count exceeds the output limit",
            .context = {{.key = "limit", .value = std::to_string(maximum_output_device_count)}},
        });
    }
    std::vector<AudioDeviceID> devices(count);
    if (size == 0U) {
        return devices;
    }
    status = AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0U, nullptr, &size,
                                        devices.data());
    if (status != noErr) {
        return std::unexpected(coreaudio_error("could not enumerate CoreAudio devices", status));
    }
    return devices;
}

[[nodiscard]] core::Result<bool> has_output_channels(const AudioDeviceID device) {
    AudioObjectPropertyAddress address{
        .mSelector = kAudioDevicePropertyStreamConfiguration,
        .mScope = kAudioObjectPropertyScopeOutput,
        .mElement = kAudioObjectPropertyElementMain,
    };
    UInt32 size{0U};
    auto status = AudioObjectGetPropertyDataSize(device, &address, 0U, nullptr, &size);
    if (status != noErr) {
        return std::unexpected(
            coreaudio_error("could not inspect CoreAudio output channels", status));
    }
    std::vector<std::byte> storage(size);
    if (size == 0U) {
        return false;
    }
    status = AudioObjectGetPropertyData(device, &address, 0U, nullptr, &size, storage.data());
    if (status != noErr) {
        return std::unexpected(coreaudio_error("could not read CoreAudio output channels", status));
    }
    const auto* buffers = reinterpret_cast<const AudioBufferList*>(storage.data());
    std::uint64_t channels{0U};
    for (UInt32 index = 0U; index < buffers->mNumberBuffers; ++index) {
        channels += buffers->mBuffers[index].mNumberChannels;
    }
    return channels > 0U;
}

struct DeviceInventory {
    std::vector<PipeWireDevice> devices;
    std::optional<std::string> default_target;
};

[[nodiscard]] core::Result<std::optional<AudioDeviceID>> default_output_device() {
    AudioObjectPropertyAddress address{
        .mSelector = kAudioHardwarePropertyDefaultOutputDevice,
        .mScope = kAudioObjectPropertyScopeGlobal,
        .mElement = kAudioObjectPropertyElementMain,
    };
    AudioDeviceID device{kAudioObjectUnknown};
    UInt32 size = sizeof(device);
    const auto status =
        AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0U, nullptr, &size, &device);
    if (status != noErr) {
        return std::unexpected(
            coreaudio_error("could not read the default CoreAudio output", status));
    }
    if (device == kAudioObjectUnknown) {
        return std::optional<AudioDeviceID>{};
    }
    return std::optional{device};
}

[[nodiscard]] core::Result<DeviceInventory> device_inventory() {
    auto ids = audio_device_ids();
    if (!ids) {
        return std::unexpected(std::move(ids.error()));
    }
    DeviceInventory inventory;
    inventory.devices.reserve(ids->size());
    for (const auto id : *ids) {
        auto output = has_output_channels(id);
        if (!output) {
            continue;
        }
        if (!*output) {
            continue;
        }
        auto uid = device_string(id, kAudioDevicePropertyDeviceUID, "UID");
        auto name = device_string(id, kAudioObjectPropertyName, "name");
        if (!uid || !name) {
            continue;
        }
        inventory.devices.push_back(
            PipeWireDevice{.name = std::move(*uid), .description = std::move(*name)});
    }
    std::ranges::sort(inventory.devices, {}, &PipeWireDevice::name);

    auto default_device = default_output_device();
    if (!default_device) {
        return std::unexpected(std::move(default_device.error()));
    }
    if (*default_device) {
        auto uid = device_string(**default_device, kAudioDevicePropertyDeviceUID, "UID");
        if (uid) {
            inventory.default_target = std::move(*uid);
        }
    }
    return inventory;
}

[[nodiscard]] core::Result<AudioDeviceID> resolve_device(const std::optional<std::string>& target) {
    if (!target) {
        auto device = default_output_device();
        if (!device) {
            return std::unexpected(std::move(device.error()));
        }
        if (!*device) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::backend,
                .message = "CoreAudio has no default output device",
                .context = {},
            });
        }
        return **device;
    }
    auto ids = audio_device_ids();
    if (!ids) {
        return std::unexpected(std::move(ids.error()));
    }
    for (const auto id : *ids) {
        auto uid = device_string(id, kAudioDevicePropertyDeviceUID, "UID");
        if (uid && *uid == *target) {
            return id;
        }
    }
    return std::unexpected(core::Error{
        .code = core::ErrorCode::backend,
        .message = "selected CoreAudio output is unavailable",
        .context = {{.key = "target", .value = *target}},
    });
}

// How long the last sample takes to be heard once handed over: the unit's
// own latency, two device buffers, and the device's latency and safety
// offset -- large on Bluetooth and AirPlay, whose tail was cut off without
// them. Device frames count at the device's rate, not the source's.
[[nodiscard]] std::chrono::nanoseconds drain_delay(const AudioUnit unit, const AudioDeviceID device,
                                                   const int sample_rate) {
    Float64 unit_latency{0.0};
    UInt32 latency_size = sizeof(unit_latency);
    static_cast<void>(AudioUnitGetProperty(unit, kAudioUnitProperty_Latency, kAudioUnitScope_Global,
                                           0U, &unit_latency, &latency_size));
    const auto frames = [device](const AudioObjectPropertySelector selector) {
        UInt32 value{0U};
        UInt32 size = sizeof(value);
        const AudioObjectPropertyAddress address{
            .mSelector = selector,
            .mScope = kAudioObjectPropertyScopeOutput,
            .mElement = kAudioObjectPropertyElementMain,
        };
        static_cast<void>(AudioObjectGetPropertyData(device, &address, 0U, nullptr, &size, &value));
        return static_cast<double>(value);
    };
    Float64 device_rate{0.0};
    UInt32 rate_size = sizeof(device_rate);
    const AudioObjectPropertyAddress rate_address{
        .mSelector = kAudioDevicePropertyNominalSampleRate,
        .mScope = kAudioObjectPropertyScopeGlobal,
        .mElement = kAudioObjectPropertyElementMain,
    };
    static_cast<void>(
        AudioObjectGetPropertyData(device, &rate_address, 0U, nullptr, &rate_size, &device_rate));
    if (device_rate <= 0.0) {
        device_rate = static_cast<double>(std::max(sample_rate, 1));
    }
    const auto device_frames = frames(kAudioDevicePropertyBufferFrameSize) * 2.0 +
                               frames(kAudioDevicePropertyLatency) +
                               frames(kAudioDevicePropertySafetyOffset);
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::duration<double>{
        std::max(0.0, unit_latency) + device_frames / device_rate});
}

} // namespace

struct PipeWireOutput::Impl {
    LocalPlayback* source;
    PipeWireOutputConfig config;
    // The source's channels; the unit always takes two, so mono is spread
    // over both speakers rather than played on the left alone.
    std::size_t source_channels;
    static constexpr std::size_t channels = 2U;
    AudioUnit unit{nullptr};
    AudioDeviceID device{kAudioObjectUnknown};
    std::atomic<PipeWireOutputState> state{PipeWireOutputState::unconnected};
    std::atomic_uint32_t active_callbacks{0U};
    std::atomic_uint64_t callback_count{0U};
    std::atomic_uint64_t device_frames{0U};
    std::atomic_uint64_t source_frames{0U};
    std::atomic_uint64_t invalid_buffer_count{0U};
    std::atomic<double> volume{1.0};
    std::chrono::nanoseconds output_drain_delay{std::chrono::milliseconds{20}};
    bool initialized{false};
    bool started{false};

    Impl(LocalPlayback& playback, PipeWireOutputConfig output_config)
        : source(&playback), config(std::move(output_config)),
          source_channels(static_cast<std::size_t>(playback.output_format().channels)) {}

    ~Impl() {
        if (started && unit != nullptr) {
            static_cast<void>(AudioOutputUnitStop(unit));
            started = false;
        }
        if (initialized && unit != nullptr) {
            static_cast<void>(AudioUnitUninitialize(unit));
            initialized = false;
        }
        if (unit != nullptr) {
            static_cast<void>(AudioComponentInstanceDispose(unit));
            unit = nullptr;
        }
    }

    static OSStatus render(void* data, AudioUnitRenderActionFlags* /*flags*/,
                           const AudioTimeStamp* /*timestamp*/, UInt32 /*bus*/, const UInt32 frames,
                           AudioBufferList* buffers) noexcept {
        auto& output = *static_cast<Impl*>(data);
        output.active_callbacks.fetch_add(1U, std::memory_order_acq_rel);
        output.callback_count.fetch_add(1U, std::memory_order_relaxed);
        const auto finish = [&output] {
            output.active_callbacks.fetch_sub(1U, std::memory_order_release);
        };
        const auto expected_bytes =
            static_cast<std::size_t>(frames) * output.channels * sizeof(float);
        if (buffers == nullptr || buffers->mNumberBuffers != 1U ||
            buffers->mBuffers[0].mData == nullptr ||
            buffers->mBuffers[0].mDataByteSize < expected_bytes) {
            output.invalid_buffer_count.fetch_add(1U, std::memory_order_relaxed);
            if (buffers != nullptr) {
                for (UInt32 index = 0U; index < buffers->mNumberBuffers; ++index) {
                    if (buffers->mBuffers[index].mData != nullptr) {
                        std::memset(buffers->mBuffers[index].mData, 0,
                                    buffers->mBuffers[index].mDataByteSize);
                    }
                }
            }
            finish();
            return noErr;
        }

        auto* data = static_cast<float*>(buffers->mBuffers[0].mData);
        auto samples = std::span<float>{data, static_cast<std::size_t>(frames) * output.channels};
        std::size_t copied = 0U;
        if (output.source_channels == 1U) {
            // Rendered into the front of the buffer, then spread to both
            // channels from the back, so nothing is read after it is written.
            copied = output.source->render(samples.first(frames));
            for (auto frame = static_cast<std::size_t>(frames); frame-- > 0U;) {
                const auto sample = data[frame];
                data[frame * 2U] = sample;
                data[frame * 2U + 1U] = sample;
            }
        } else {
            copied = output.source->render(samples);
        }
        const auto gain = static_cast<float>(output.volume.load(std::memory_order_relaxed));
        if (gain != 1.0F) {
            for (auto& sample : samples) {
                sample *= gain;
            }
        }
        output.device_frames.fetch_add(frames, std::memory_order_relaxed);
        output.source_frames.fetch_add(copied, std::memory_order_relaxed);
        finish();
        return noErr;
    }
};

PipeWireOutput::PipeWireOutput(std::unique_ptr<Impl> implementation)
    : implementation_(std::move(implementation)) {}

PipeWireOutput::PipeWireOutput(PipeWireOutput&&) noexcept = default;
PipeWireOutput& PipeWireOutput::operator=(PipeWireOutput&&) noexcept = default;
PipeWireOutput::~PipeWireOutput() = default;

core::Result<PipeWireOutput> PipeWireOutput::connect(LocalPlayback& source,
                                                     PipeWireOutputConfig config) {
    if (config.stream_name.empty()) {
        return std::unexpected(invalid_config("CoreAudio stream name must not be empty"));
    }
    if (config.transition_timeout <= std::chrono::milliseconds::zero()) {
        return std::unexpected(invalid_config("CoreAudio transition timeout must be positive"));
    }
    if (config.exclusive) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::unsupported,
            .message = "exclusive CoreAudio output is not implemented",
            .context = {},
        });
    }
    const auto& format = source.output_format();
    if (format.sample_rate <= 0 || (format.channels != 1 && format.channels != 2)) {
        return std::unexpected(invalid_config(
            "CoreAudio output currently requires positive-rate mono or stereo source PCM"));
    }
    auto selected = resolve_device(config.target_object);
    if (!selected) {
        return std::unexpected(std::move(selected.error()));
    }

    // No device chosen: the default output unit, which follows the system
    // default as it changes -- headphones unplugged, AirPlay chosen -- as
    // PipeWire's default does. A chosen device is held by its UID, and its
    // loss is seen by the device monitor.
    const bool follows_default = !config.target_object.has_value();
    auto output = std::make_unique<Impl>(source, std::move(config));
    output->device = *selected;
    output->state.store(PipeWireOutputState::connecting, std::memory_order_release);
    AudioComponentDescription description{
        .componentType = kAudioUnitType_Output,
        .componentSubType = follows_default ? kAudioUnitSubType_DefaultOutput
                                            : kAudioUnitSubType_HALOutput,
        .componentManufacturer = kAudioUnitManufacturer_Apple,
        .componentFlags = 0U,
        .componentFlagsMask = 0U,
    };
    const auto component = AudioComponentFindNext(nullptr, &description);
    if (component == nullptr) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::backend,
            .message = "could not find the CoreAudio HAL output component",
            .context = {},
        });
    }
    auto status = AudioComponentInstanceNew(component, &output->unit);
    if (status != noErr) {
        return std::unexpected(coreaudio_error("could not create CoreAudio output unit", status));
    }
    if (!follows_default) {
        status = AudioUnitSetProperty(output->unit, kAudioOutputUnitProperty_CurrentDevice,
                                      kAudioUnitScope_Global, 0U, &output->device,
                                      sizeof(output->device));
        if (status != noErr) {
            return std::unexpected(
                coreaudio_error("could not select the CoreAudio output device", status));
        }
    }

    AudioStreamBasicDescription stream_format{
        .mSampleRate = static_cast<Float64>(format.sample_rate),
        .mFormatID = kAudioFormatLinearPCM,
        .mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked,
        .mBytesPerPacket = static_cast<UInt32>(output->channels * sizeof(float)),
        .mFramesPerPacket = 1U,
        .mBytesPerFrame = static_cast<UInt32>(output->channels * sizeof(float)),
        .mChannelsPerFrame = static_cast<UInt32>(output->channels),
        .mBitsPerChannel = 32U,
        .mReserved = 0U,
    };
    status = AudioUnitSetProperty(output->unit, kAudioUnitProperty_StreamFormat,
                                  kAudioUnitScope_Input, 0U, &stream_format, sizeof(stream_format));
    if (status != noErr) {
        return std::unexpected(
            coreaudio_error("could not set the CoreAudio source format", status));
    }
    AURenderCallbackStruct callback{.inputProc = &Impl::render, .inputProcRefCon = output.get()};
    status = AudioUnitSetProperty(output->unit, kAudioUnitProperty_SetRenderCallback,
                                  kAudioUnitScope_Input, 0U, &callback, sizeof(callback));
    if (status != noErr) {
        return std::unexpected(
            coreaudio_error("could not attach the CoreAudio render callback", status));
    }
    status = AudioUnitInitialize(output->unit);
    if (status != noErr) {
        return std::unexpected(coreaudio_error("could not initialize CoreAudio output", status));
    }
    output->initialized = true;
    output->output_drain_delay = drain_delay(output->unit, output->device, format.sample_rate);
    output->state.store(PipeWireOutputState::paused, std::memory_order_release);
    return PipeWireOutput{std::move(output)};
}

PipeWireOutputSnapshot PipeWireOutput::snapshot() const {
    const auto& output = *implementation_;
    return PipeWireOutputSnapshot{
        .state = output.state.load(std::memory_order_acquire),
        .node_id = output.device == kAudioObjectUnknown
                       ? std::nullopt
                       : std::optional<std::uint32_t>{output.device},
        .callback_count = output.callback_count.load(std::memory_order_acquire),
        .device_frames = output.device_frames.load(std::memory_order_acquire),
        .source_frames = output.source_frames.load(std::memory_order_acquire),
        .invalid_buffer_count = output.invalid_buffer_count.load(std::memory_order_acquire),
        .volume = output.volume.load(std::memory_order_acquire),
        .server_volume = output.volume.load(std::memory_order_acquire),
        .error_message = {},
    };
}

core::Result<void> PipeWireOutput::set_volume(const double volume) {
    implementation_->volume.store(std::clamp(volume, 0.0, 1.0), std::memory_order_release);
    return {};
}

core::Result<void> PipeWireOutput::activate() {
    auto& output = *implementation_;
    if (output.started) {
        return {};
    }
    const auto status = AudioOutputUnitStart(output.unit);
    if (status != noErr) {
        output.state.store(PipeWireOutputState::error, std::memory_order_release);
        return std::unexpected(coreaudio_error("could not start CoreAudio output", status));
    }
    output.started = true;
    output.state.store(PipeWireOutputState::streaming, std::memory_order_release);
    return {};
}

core::Result<void> PipeWireOutput::quiesce() {
    auto& output = *implementation_;
    if (output.started) {
        const auto status = AudioOutputUnitStop(output.unit);
        if (status != noErr) {
            return std::unexpected(coreaudio_error("could not stop CoreAudio output", status));
        }
        output.started = false;
    }
    const auto deadline = std::chrono::steady_clock::now() + output.config.transition_timeout;
    while (output.active_callbacks.load(std::memory_order_acquire) != 0U) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return std::unexpected(core::Error{
                .code = core::ErrorCode::backend,
                .message = "CoreAudio callback did not quiesce before timeout",
                .context = {},
            });
        }
        std::this_thread::yield();
    }
    const auto reset = AudioUnitReset(output.unit, kAudioUnitScope_Global, 0U);
    if (reset != noErr) {
        return std::unexpected(coreaudio_error("could not reset CoreAudio output", reset));
    }
    output.state.store(PipeWireOutputState::paused, std::memory_order_release);
    return {};
}

core::Result<void> PipeWireOutput::drain() {
    auto& output = *implementation_;
    if (output.source->snapshot().state != LocalPlaybackState::ended) {
        return std::unexpected(core::Error{
            .code = core::ErrorCode::conflict,
            .message = "CoreAudio output can drain only after the local source reaches its end",
            .context = {},
        });
    }
    const auto wait = std::min(
        output.output_drain_delay,
        std::chrono::duration_cast<std::chrono::nanoseconds>(output.config.transition_timeout));
    if (wait > std::chrono::nanoseconds::zero()) {
        std::this_thread::sleep_for(wait);
    }
    return {};
}

struct PipeWireDeviceMonitor::Impl {
    mutable std::mutex mutex;
    mutable std::vector<PipeWireDevice> devices;
    mutable std::optional<std::string> default_target;
    mutable std::optional<core::Error> error;
    mutable std::uint64_t generation{1U};
};

PipeWireDeviceMonitor::PipeWireDeviceMonitor(std::unique_ptr<Impl> implementation)
    : implementation_(std::move(implementation)) {}

PipeWireDeviceMonitor::PipeWireDeviceMonitor(PipeWireDeviceMonitor&&) noexcept = default;
PipeWireDeviceMonitor& PipeWireDeviceMonitor::operator=(PipeWireDeviceMonitor&&) noexcept = default;
PipeWireDeviceMonitor::~PipeWireDeviceMonitor() = default;

core::Result<PipeWireDeviceMonitor>
PipeWireDeviceMonitor::connect(const std::chrono::milliseconds timeout) {
    if (timeout <= std::chrono::milliseconds::zero()) {
        return std::unexpected(invalid_config("CoreAudio monitor timeout must be positive"));
    }
    auto inventory = device_inventory();
    if (!inventory) {
        return std::unexpected(std::move(inventory.error()));
    }
    auto monitor = std::make_unique<Impl>();
    monitor->devices = std::move(inventory->devices);
    monitor->default_target = std::move(inventory->default_target);
    return PipeWireDeviceMonitor{std::move(monitor)};
}

PipeWireDeviceSnapshot PipeWireDeviceMonitor::snapshot() const {
    auto& monitor = *implementation_;
    auto inventory = device_inventory();
    std::lock_guard lock{monitor.mutex};
    if (!inventory) {
        monitor.error = std::move(inventory.error());
        ++monitor.generation;
    } else {
        if (inventory->devices != monitor.devices ||
            inventory->default_target != monitor.default_target || monitor.error) {
            monitor.devices = std::move(inventory->devices);
            monitor.default_target = std::move(inventory->default_target);
            monitor.error.reset();
            ++monitor.generation;
        }
    }
    return PipeWireDeviceSnapshot{.devices = monitor.devices,
                                  .default_target = monitor.default_target,
                                  .generation = monitor.generation,
                                  .error = monitor.error};
}

core::Result<std::vector<PipeWireDevice>>
list_pipewire_output_devices(const std::chrono::milliseconds timeout) {
    if (timeout <= std::chrono::milliseconds::zero()) {
        return std::unexpected(invalid_config("CoreAudio enumeration timeout must be positive"));
    }
    auto inventory = device_inventory();
    if (!inventory) {
        return std::unexpected(std::move(inventory.error()));
    }
    return std::move(inventory->devices);
}

} // namespace trackknife::audio

// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/audio/local_playback.hpp"
#include "trackknife/core/result.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace trackknife::audio {

enum class PipeWireOutputState {
    unconnected,
    connecting,
    paused,
    streaming,
    error,
};

struct PipeWireOutputConfig {
    std::string stream_name{"Trackknife local playback"};
    std::optional<std::string> target_object;
    std::chrono::milliseconds transition_timeout{2'000};
    bool exclusive{false};

    friend bool operator==(const PipeWireOutputConfig&, const PipeWireOutputConfig&) = default;
};

struct PipeWireOutputSnapshot {
    PipeWireOutputState state{PipeWireOutputState::unconnected};
    std::optional<std::uint32_t> node_id;
    std::uint64_t callback_count{0U};
    std::uint64_t device_frames{0U};
    std::uint64_t source_frames{0U};
    std::uint64_t invalid_buffer_count{0U};
    double volume{1.0};
    // What the server reports for the stream, which is not always what was
    // asked for: a session manager restoring a remembered volume, or an
    // external mixer, sets it too.
    double server_volume{1.0};
    std::string error_message;

    friend bool operator==(const PipeWireOutputSnapshot&, const PipeWireOutputSnapshot&) = default;
};

struct PipeWireDevice {
    std::string name;        // node.name — usable as a stream target_object
    std::string description; // human-readable device label

    friend bool operator==(const PipeWireDevice&, const PipeWireDevice&) = default;
};

struct PipeWireDeviceSnapshot {
    std::vector<PipeWireDevice> devices;
    std::optional<std::string> default_target;
    std::uint64_t generation{0U};
    std::optional<core::Error> error;

    friend bool operator==(const PipeWireDeviceSnapshot&, const PipeWireDeviceSnapshot&) = default;
};

// Owns one persistent registry connection. Global add/remove events and the
// session manager's default.audio.sink metadata increment generation; callers
// take immutable snapshots without running PipeWire work on their own thread.
class PipeWireDeviceMonitor final {
  public:
    [[nodiscard]] static core::Result<PipeWireDeviceMonitor>
    connect(std::chrono::milliseconds timeout = std::chrono::milliseconds{2'000});

    PipeWireDeviceMonitor(PipeWireDeviceMonitor&&) noexcept;
    PipeWireDeviceMonitor& operator=(PipeWireDeviceMonitor&&) noexcept;
    PipeWireDeviceMonitor(const PipeWireDeviceMonitor&) = delete;
    PipeWireDeviceMonitor& operator=(const PipeWireDeviceMonitor&) = delete;
    ~PipeWireDeviceMonitor();

    [[nodiscard]] PipeWireDeviceSnapshot snapshot() const;

  private:
    struct Impl;
    explicit PipeWireDeviceMonitor(std::unique_ptr<Impl> implementation);

    std::unique_ptr<Impl> implementation_;
};

// Enumerates the currently available PipeWire audio sinks via one bounded
// registry roundtrip. Blocking — intended for a worker thread, never the UI.
[[nodiscard]] core::Result<std::vector<PipeWireDevice>>
list_pipewire_output_devices(std::chrono::milliseconds timeout = std::chrono::milliseconds{2'000});

// The source is not owned and must outlive this output. Call quiesce before
// resetting, seeking, moving, or destroying the LocalPlayback source.
class PipeWireOutput final {
  public:
    PipeWireOutput(PipeWireOutput&&) noexcept;
    PipeWireOutput& operator=(PipeWireOutput&&) noexcept;
    PipeWireOutput(const PipeWireOutput&) = delete;
    PipeWireOutput& operator=(const PipeWireOutput&) = delete;
    ~PipeWireOutput();

    [[nodiscard]] static core::Result<PipeWireOutput> connect(LocalPlayback& source,
                                                              PipeWireOutputConfig config = {});

    [[nodiscard]] PipeWireOutputSnapshot snapshot() const;
    [[nodiscard]] core::Result<void> activate();

    // Linear soft volume [0, 1] applied by PipeWire's stream mixer; samples
    // rendered by the source are never modified by Trackknife itself.
    [[nodiscard]] core::Result<void> set_volume(double volume);

    // Deactivation waits for the paused state, flushes queued device data, and
    // waits for an in-flight real-time callback to leave the source.
    [[nodiscard]] core::Result<void> quiesce();

    // Drain is valid once the source has reached its end. It waits until
    // PipeWire reports that every already-queued device buffer was consumed.
    [[nodiscard]] core::Result<void> drain();

  private:
    struct Impl;
    explicit PipeWireOutput(std::unique_ptr<Impl> implementation);

    std::unique_ptr<Impl> implementation_;
};

} // namespace trackknife::audio

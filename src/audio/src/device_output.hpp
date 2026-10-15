// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// The platform's own output -- PipeWire, CoreAudio, or none -- behind
// PipeWireOutput, which may instead render offline (offline_output.hpp).
// Each backend file defines it; the contract is PipeWireOutput's.

#include "trackknife/audio/pipewire_output.hpp"

#include <memory>

namespace trackknife::audio {

class DeviceOutput final {
  public:
    DeviceOutput(DeviceOutput&&) noexcept;
    DeviceOutput& operator=(DeviceOutput&&) noexcept;
    DeviceOutput(const DeviceOutput&) = delete;
    DeviceOutput& operator=(const DeviceOutput&) = delete;
    ~DeviceOutput();

    [[nodiscard]] static core::Result<DeviceOutput> connect(LocalPlayback& source,
                                                            PipeWireOutputConfig config = {});
    [[nodiscard]] PipeWireOutputSnapshot snapshot() const;
    [[nodiscard]] core::Result<void> activate();
    [[nodiscard]] core::Result<void> set_volume(double volume);
    [[nodiscard]] core::Result<void> quiesce();
    [[nodiscard]] core::Result<void> drain();

  private:
    struct Impl;
    explicit DeviceOutput(std::unique_ptr<Impl> implementation);
    std::unique_ptr<Impl> implementation_;
};

} // namespace trackknife::audio

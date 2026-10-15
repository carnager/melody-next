// SPDX-License-Identifier: GPL-3.0-only
#include "trackknife/audio/pipewire_output.hpp"

#include "device_output.hpp"
#include "offline_output.hpp"

#include <optional>
#include <utility>

namespace trackknife::audio {

// The platform's device, or -- offline_speed set -- no device at all.
struct PipeWireOutput::Impl {
    std::optional<DeviceOutput> device;
    std::unique_ptr<OfflineOutput> offline;
};

PipeWireOutput::PipeWireOutput(std::unique_ptr<Impl> implementation)
    : implementation_(std::move(implementation)) {}
PipeWireOutput::PipeWireOutput(PipeWireOutput&&) noexcept = default;
PipeWireOutput& PipeWireOutput::operator=(PipeWireOutput&&) noexcept = default;
PipeWireOutput::~PipeWireOutput() = default;

core::Result<PipeWireOutput> PipeWireOutput::connect(LocalPlayback& source,
                                                     PipeWireOutputConfig config) {
    auto implementation = std::make_unique<Impl>();
    if (config.offline_speed != 0.0) {
        auto offline = OfflineOutput::connect(source, config);
        if (!offline) {
            return std::unexpected(std::move(offline.error()));
        }
        implementation->offline = std::move(*offline);
    } else {
        auto device = DeviceOutput::connect(source, std::move(config));
        if (!device) {
            return std::unexpected(std::move(device.error()));
        }
        implementation->device.emplace(std::move(*device));
    }
    return PipeWireOutput{std::move(implementation)};
}

PipeWireOutputSnapshot PipeWireOutput::snapshot() const {
    return implementation_->offline ? implementation_->offline->snapshot()
                                    : implementation_->device->snapshot();
}

core::Result<void> PipeWireOutput::activate() {
    return implementation_->offline ? implementation_->offline->activate()
                                    : implementation_->device->activate();
}

core::Result<void> PipeWireOutput::set_volume(const double volume) {
    return implementation_->offline ? implementation_->offline->set_volume(volume)
                                    : implementation_->device->set_volume(volume);
}

core::Result<void> PipeWireOutput::quiesce() {
    return implementation_->offline ? implementation_->offline->quiesce()
                                    : implementation_->device->quiesce();
}

core::Result<void> PipeWireOutput::drain() {
    return implementation_->offline ? implementation_->offline->drain()
                                    : implementation_->device->drain();
}

} // namespace trackknife::audio

// SPDX-License-Identifier: GPL-3.0-only
// Builds that explicitly disable local audio keep the adapter unavailable.
#include "trackknife/audio/pipewire_output.hpp"
#include <utility>
namespace trackknife::audio {
namespace {
core::Error unavailable() {
    return {.code = core::ErrorCode::unsupported,
            .message = "local audio is not available on this platform",
            .context = {}};
}
} // namespace
struct PipeWireOutput::Impl {};
PipeWireOutput::PipeWireOutput(std::unique_ptr<Impl> impl) : implementation_(std::move(impl)) {}
PipeWireOutput::PipeWireOutput(PipeWireOutput&&) noexcept = default;
PipeWireOutput& PipeWireOutput::operator=(PipeWireOutput&&) noexcept = default;
PipeWireOutput::~PipeWireOutput() = default;
core::Result<PipeWireOutput> PipeWireOutput::connect(LocalPlayback&, PipeWireOutputConfig) {
    return std::unexpected(unavailable());
}
PipeWireOutputSnapshot PipeWireOutput::snapshot() const { return {}; }
core::Result<void> PipeWireOutput::activate() { return std::unexpected(unavailable()); }
core::Result<void> PipeWireOutput::set_volume(double) { return std::unexpected(unavailable()); }
core::Result<void> PipeWireOutput::quiesce() { return {}; }
core::Result<void> PipeWireOutput::drain() { return {}; }
struct PipeWireDeviceMonitor::Impl {};
PipeWireDeviceMonitor::PipeWireDeviceMonitor(std::unique_ptr<Impl> impl)
    : implementation_(std::move(impl)) {}
PipeWireDeviceMonitor::PipeWireDeviceMonitor(PipeWireDeviceMonitor&&) noexcept = default;
PipeWireDeviceMonitor& PipeWireDeviceMonitor::operator=(PipeWireDeviceMonitor&&) noexcept = default;
PipeWireDeviceMonitor::~PipeWireDeviceMonitor() = default;
core::Result<PipeWireDeviceMonitor> PipeWireDeviceMonitor::connect(std::chrono::milliseconds) {
    return std::unexpected(unavailable());
}
PipeWireDeviceSnapshot PipeWireDeviceMonitor::snapshot() const { return {}; }
core::Result<std::vector<PipeWireDevice>> list_pipewire_output_devices(std::chrono::milliseconds) {
    return std::vector<PipeWireDevice>{};
}
} // namespace trackknife::audio

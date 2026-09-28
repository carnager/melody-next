// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "trackknife/core/result.hpp"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace trackknife::discovery {

using UpnpValues = std::map<std::string, std::string>;
struct UpnpService final {
    std::string type;
    std::string control_url;
    std::string event_url;
};
struct UpnpRenderer final {
    std::string udn;
    std::string name;
    std::string address; // Engine address on the discovery interface, never loopback.
    UpnpService transport;
    UpnpService rendering;
    UpnpService connection;
    bool online{true};
};

// Pure XML boundaries are shared by the control point and fixture tests.
[[nodiscard]] core::Result<UpnpRenderer> parse_upnp_renderer(const std::string& xml,
                                                             const std::string& location,
                                                             const std::string& address);
[[nodiscard]] UpnpValues parse_upnp_values(const std::string& xml);
[[nodiscard]] UpnpValues parse_upnp_last_change(const std::string& xml);
[[nodiscard]] std::string upnp_xml_escape(const std::string& text);

// Injectable transport: tests can model a renderer without multicast or hardware.
class UpnpControl {
  public:
    virtual ~UpnpControl() = default;
    [[nodiscard]] virtual core::Result<UpnpValues>
    action(const UpnpService& service, const std::string& name, const UpnpValues& arguments) = 0;
    [[nodiscard]] virtual UpnpValues take_events(const std::string& udn) = 0;
};

// One SDK instance per engine. Its bounded callback queue is drained by one
// worker; callbacks never perform HTTP or call into the player on SDK threads.
class UpnpDiscovery final : public UpnpControl {
  public:
    using Changed = std::function<void(const UpnpRenderer&)>;
    [[nodiscard]] static core::Result<std::shared_ptr<UpnpDiscovery>>
    start(Changed changed, const std::string& interface = {});
    ~UpnpDiscovery() override;
    void begin();
    void stop();
    [[nodiscard]] core::Result<UpnpValues> action(const UpnpService& service,
                                                  const std::string& name,
                                                  const UpnpValues& arguments) override;
    [[nodiscard]] UpnpValues take_events(const std::string& udn) override;

  private:
    struct Impl;
    explicit UpnpDiscovery(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace trackknife::discovery

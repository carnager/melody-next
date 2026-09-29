// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "trackknife/output/stream_query.hpp"

#include <sys/socket.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace trackknife::agent {

// ADR-0239: how an agent reaches its engine, and so what it asks streamed.
enum class Route { nearby, away };

// What an agent without the files asks for on each route. Empty: the
// original files.
struct StreamChoice final {
    std::optional<output::StreamFormat> nearby;
    std::optional<output::StreamFormat> away{output::StreamFormat{.bitrate_kbps = 128}};

    [[nodiscard]] const std::optional<output::StreamFormat>& on(Route route) const {
        return route == Route::nearby ? nearby : away;
    }
    friend bool operator==(const StreamChoice&, const StreamChoice&) = default;
};

// A rate as flags and settings give it: 0 the original files, else Opus at
// 16 to 512 kbps. Anything else: nothing.
[[nodiscard]] std::optional<int> parse_kbps(std::string_view text);
[[nodiscard]] std::optional<output::StreamFormat> format_for_kbps(int kbps);
// "the original files", "Opus 128 kbps".
[[nodiscard]] std::string describe(const std::optional<output::StreamFormat>& format);

// One address of one interface on this machine.
struct InterfaceAddress final {
    std::string name;
    sockaddr_storage address{};
    sockaddr_storage netmask{};
    bool loopback{false};
    // WireGuard, tun, PPP: whatever goes in comes out somewhere else.
    bool tunnel{false};
};

struct RouteFinding final {
    Route route{Route::away};
    // For the log: "on wlan0's network", "through wg0".
    std::string why;
};

// The route from a connection's two ends and this machine's interfaces.
[[nodiscard]] RouteFinding classify_route(const sockaddr_storage& local,
                                          const sockaddr_storage& peer,
                                          const std::vector<InterfaceAddress>& interfaces);
// The route of a connected socket, as it is now.
[[nodiscard]] RouteFinding route_of(int descriptor);

} // namespace trackknife::agent

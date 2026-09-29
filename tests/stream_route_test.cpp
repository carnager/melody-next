// SPDX-License-Identifier: GPL-3.0-only

// ADR-0239: what an agent without the files asks for, by how it reaches the
// engine.

#include "agent/stream_route.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>

#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

using trackknife::agent::classify_route;
using trackknife::agent::InterfaceAddress;
using trackknife::agent::Route;

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::abort();
    }
}

[[nodiscard]] sockaddr_storage v4(const char* text) {
    sockaddr_storage storage{};
    auto& in = reinterpret_cast<sockaddr_in&>(storage);
    in.sin_family = AF_INET;
    require(::inet_pton(AF_INET, text, &in.sin_addr) == 1, "a test address parses");
    return storage;
}

[[nodiscard]] sockaddr_storage v6(const char* text) {
    sockaddr_storage storage{};
    auto& in6 = reinterpret_cast<sockaddr_in6&>(storage);
    in6.sin6_family = AF_INET6;
    require(::inet_pton(AF_INET6, text, &in6.sin6_addr) == 1, "a test address parses");
    return storage;
}

[[nodiscard]] InterfaceAddress interface(const char* name, sockaddr_storage address,
                                         sockaddr_storage netmask, const bool tunnel = false,
                                         const bool loopback = false) {
    return InterfaceAddress{.name = name,
                            .address = address,
                            .netmask = netmask,
                            .loopback = loopback,
                            .tunnel = tunnel};
}

} // namespace

int main() {
    // The laptop of the report: Wi-Fi somewhere else, WireGuard home.
    const std::vector<InterfaceAddress> laptop{
        interface("lo", v4("127.0.0.1"), v4("255.0.0.0"), false, true),
        interface("wlan0", v4("192.168.52.57"), v4("255.255.255.224")),
        interface("fritzbox", v4("192.168.0.202"), v4("255.255.255.0"), true),
        interface("wlan0", v6("fdd4:8114:b99::57"), v6("ffff:ffff:ffff:ffff::"))};

    const auto tunnelled = classify_route(v4("192.168.0.202"), v4("192.168.0.200"), laptop);
    require(tunnelled.route == Route::away,
            "an engine through WireGuard is away, though it is on the tunnel's subnet");
    require(tunnelled.why == "through fritzbox", "and the log says through which");

    const auto next_door = classify_route(v4("192.168.52.57"), v4("192.168.52.40"), laptop);
    require(next_door.route == Route::nearby, "an engine on the Wi-Fi's own subnet is nearby");
    require(next_door.why == "on wlan0's network", "and the log says on which");

    require(classify_route(v4("192.168.52.57"), v4("192.168.52.100"), laptop).route == Route::away,
            "one past the netmask is through a router: away");
    require(classify_route(v4("192.168.52.57"), v4("93.184.216.34"), laptop).route == Route::away,
            "one on the internet is away");
    require(classify_route(v4("127.0.0.1"), v4("127.0.0.1"), laptop).route == Route::nearby,
            "an engine on this machine is nearby");
    require(classify_route(v6("fdd4:8114:b99::57"), v6("fdd4:8114:b99::200"), laptop).route ==
                Route::nearby,
            "IPv6 on the same prefix is nearby");
    require(classify_route(v4("10.9.9.9"), v4("10.9.9.1"), laptop).route == Route::away,
            "an address no interface has: cannot tell, so away");

    sockaddr_storage unix_socket{};
    unix_socket.ss_family = AF_UNIX;
    require(classify_route(unix_socket, {}, {}).route == Route::nearby,
            "a unix socket is this machine");

    using trackknife::agent::parse_kbps;
    require(parse_kbps("0") == 0, "0 is the original files");
    require(parse_kbps("128") == 128 && parse_kbps("16") == 16 && parse_kbps("512") == 512,
            "Opus from 16 to 512 kbps");
    require(!parse_kbps("8") && !parse_kbps("600") && !parse_kbps("-1") && !parse_kbps("128k") &&
                !parse_kbps(""),
            "anything else is refused");

    trackknife::agent::StreamChoice defaults;
    require(!defaults.on(Route::nearby) && defaults.on(Route::away) &&
                defaults.on(Route::away)->bitrate_kbps == 128,
            "by default: the original nearby, Opus 128 away");
    return EXIT_SUCCESS;
}

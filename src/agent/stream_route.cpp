// SPDX-License-Identifier: GPL-3.0-only

#include "agent/stream_route.hpp"

#include <ifaddrs.h>
#include <net/if.h>
#ifdef __linux__
#include <net/if_arp.h>
#endif
#include <netinet/in.h>

#include <algorithm>
#include <charconv>
#include <cstring>
#include <fstream>
#include <span>

namespace trackknife::agent {
namespace {

// The bytes of an IPv4 or IPv6 address, nothing for any other family.
[[nodiscard]] std::span<const unsigned char> address_bytes(const sockaddr_storage& storage) {
    if (storage.ss_family == AF_INET) {
        const auto& in = reinterpret_cast<const sockaddr_in&>(storage);
        return {reinterpret_cast<const unsigned char*>(&in.sin_addr), sizeof in.sin_addr};
    }
    if (storage.ss_family == AF_INET6) {
        const auto& in6 = reinterpret_cast<const sockaddr_in6&>(storage);
        return {reinterpret_cast<const unsigned char*>(&in6.sin6_addr), sizeof in6.sin6_addr};
    }
    return {};
}

[[nodiscard]] bool same_address(const sockaddr_storage& a, const sockaddr_storage& b) {
    const auto left = address_bytes(a);
    const auto right = address_bytes(b);
    return a.ss_family == b.ss_family && !left.empty() && std::ranges::equal(left, right);
}

// Whether `peer` is on the network `local` and `mask` make.
[[nodiscard]] bool on_network(const sockaddr_storage& local, const sockaddr_storage& mask,
                              const sockaddr_storage& peer) {
    const auto here = address_bytes(local);
    const auto bits = address_bytes(mask);
    const auto there = address_bytes(peer);
    if (here.empty() || local.ss_family != peer.ss_family || mask.ss_family != local.ss_family ||
        bits.size() != here.size()) {
        return false;
    }
    for (std::size_t index = 0; index < here.size(); ++index) {
        if ((here[index] & bits[index]) != (there[index] & bits[index])) {
            return false;
        }
    }
    return true;
}

void copy_address(const sockaddr* from, sockaddr_storage& to) {
    if (from == nullptr) {
        return;
    }
    const auto size = from->sa_family == AF_INET    ? sizeof(sockaddr_in)
                      : from->sa_family == AF_INET6 ? sizeof(sockaddr_in6)
                                                    : 0U;
    std::memcpy(&to, from, size);
}

// A link that carries no hardware addresses: WireGuard and tun say so.
[[nodiscard]] bool headerless(const std::string& name) {
#ifdef __linux__
    std::ifstream type{"/sys/class/net/" + name + "/type"};
    int value = 0;
    return type >> value && value == ARPHRD_NONE;
#else
    static_cast<void>(name);
    return false;
#endif
}

[[nodiscard]] std::vector<InterfaceAddress> interface_addresses() {
    std::vector<InterfaceAddress> found;
    ifaddrs* list = nullptr;
    if (::getifaddrs(&list) != 0) {
        return found;
    }
    for (const auto* entry = list; entry != nullptr; entry = entry->ifa_next) {
        if (entry->ifa_addr == nullptr ||
            (entry->ifa_addr->sa_family != AF_INET && entry->ifa_addr->sa_family != AF_INET6)) {
            continue;
        }
        InterfaceAddress address{.name = entry->ifa_name,
                                 .loopback = (entry->ifa_flags & IFF_LOOPBACK) != 0U,
                                 .tunnel = (entry->ifa_flags & IFF_POINTOPOINT) != 0U};
        copy_address(entry->ifa_addr, address.address);
        copy_address(entry->ifa_netmask, address.netmask);
        if (!address.loopback && !address.tunnel) {
            address.tunnel = headerless(address.name);
        }
        found.push_back(std::move(address));
    }
    ::freeifaddrs(list);
    return found;
}

} // namespace

std::optional<int> parse_kbps(const std::string_view text) {
    int kbps = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), kbps);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    if (kbps != 0 && (kbps < 16 || kbps > 512)) {
        return std::nullopt;
    }
    return kbps;
}

std::optional<output::StreamFormat> format_for_kbps(const int kbps) {
    if (kbps <= 0) {
        return std::nullopt;
    }
    return output::StreamFormat{.bitrate_kbps = std::clamp(kbps, 16, 512)};
}

std::string describe(const std::optional<output::StreamFormat>& format) {
    return format ? "Opus " + std::to_string(format->bitrate_kbps) + " kbps"
                  : std::string{"the original files"};
}

RouteFinding classify_route(const sockaddr_storage& local, const sockaddr_storage& peer,
                            const std::vector<InterfaceAddress>& interfaces) {
    if (local.ss_family == AF_UNIX) {
        return {.route = Route::nearby, .why = "on this machine"};
    }
    const auto carrier = std::ranges::find_if(interfaces, [&local](const InterfaceAddress& entry) {
        return same_address(entry.address, local);
    });
    if (carrier == interfaces.end()) {
        return {.route = Route::away, .why = "by a way this machine cannot tell"};
    }
    if (carrier->loopback) {
        return {.route = Route::nearby, .why = "on this machine"};
    }
    if (carrier->tunnel) {
        return {.route = Route::away, .why = "through " + carrier->name};
    }
    if (on_network(carrier->address, carrier->netmask, peer)) {
        return {.route = Route::nearby, .why = "on " + carrier->name + "'s network"};
    }
    return {.route = Route::away, .why = "through a router, from " + carrier->name};
}

RouteFinding route_of(const int descriptor) {
    sockaddr_storage local{};
    sockaddr_storage peer{};
    socklen_t local_size = sizeof local;
    socklen_t peer_size = sizeof peer;
    if (::getsockname(descriptor, reinterpret_cast<sockaddr*>(&local), &local_size) != 0) {
        return {.route = Route::away, .why = "by a way this machine cannot tell"};
    }
    if (local.ss_family == AF_UNIX) {
        return classify_route(local, peer, {});
    }
    if (::getpeername(descriptor, reinterpret_cast<sockaddr*>(&peer), &peer_size) != 0) {
        return {.route = Route::away, .why = "by a way this machine cannot tell"};
    }
    return classify_route(local, peer, interface_addresses());
}

} // namespace trackknife::agent

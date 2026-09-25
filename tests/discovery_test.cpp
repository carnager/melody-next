// SPDX-License-Identifier: GPL-3.0-only

// Engines announcing themselves on multicast DNS, and being found.

#include "trackknife/core/stable_id.hpp"
#include "trackknife/discovery/dns.hpp"
#include "trackknife/discovery/mdns.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>
#include <thread>

namespace {

namespace discovery = trackknife::discovery;

void require(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::abort();
    }
}

[[nodiscard]] bool eventually(const std::function<bool()>& condition,
                              const std::chrono::milliseconds patience = std::chrono::seconds{8}) {
    const auto deadline = std::chrono::steady_clock::now() + patience;
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
    return condition();
}

void codec_round_trips() {
    discovery::Message message;
    message.response = true;
    message.questions.push_back({.name = "_melody._tcp.local", .type = discovery::RecordType::ptr});
    // And one asking to be answered directly: the bit survives too.
    message.questions.push_back(
        {.name = "_melody._tcp.local", .type = discovery::RecordType::ptr, .unicast = true});
    message.answers.push_back({.name = "_melody._tcp.local",
                               .type = discovery::RecordType::ptr,
                               .ttl = 120,
                               .target = "gemenon._melody._tcp.local",
                               .port = 0,
                               .strings = {},
                               .address = 0});
    message.additionals.push_back({.name = "gemenon._melody._tcp.local",
                                   .type = discovery::RecordType::srv,
                                   .ttl = 120,
                                   .target = "gemenon.local",
                                   .port = 6603,
                                   .strings = {},
                                   .address = 0});
    message.additionals.push_back({.name = "gemenon._melody._tcp.local",
                                   .type = discovery::RecordType::txt,
                                   .ttl = 120,
                                   .target = {},
                                   .port = 0,
                                   .strings = {"id=abc", "auth=0"},
                                   .address = 0});
    message.additionals.push_back({.name = "gemenon.local",
                                   .type = discovery::RecordType::a,
                                   .ttl = 120,
                                   .target = {},
                                   .port = 0,
                                   .strings = {},
                                   .address = 0xC0A800C8U});
    const auto bytes = discovery::encode(message);
    const auto decoded = discovery::decode(bytes.data(), bytes.size());
    require(decoded.has_value() && *decoded == message, "a message survives encoding");

    // Others compress names (RFC 1035 §4.1.4): a pointer back to an earlier
    // name is read as that name.
    const std::vector<std::uint8_t> compressed{
        0, 0, 0x84, 0, 0, 0, 0, 1, 0, 0, 0, 0,
        // _melody._tcp.local PTR, pointing its target's tail back at offset 12
        7, '_', 'm', 'e', 'l', 'o', 'd', 'y', 4, '_', 't', 'c', 'p', 5, 'l', 'o', 'c', 'a', 'l', 0,
        0, 12, 0, 1, 0, 0, 0, 120, 0, 6,
        3, 'd', 'e', 'n', 0xC0, 12};
    const auto read = discovery::decode(compressed.data(), compressed.size());
    require(read.has_value() && read->answers.size() == 1U &&
                read->answers.front().target == "den._melody._tcp.local",
            "a compressed name is followed");
    // A pointer loop, or a packet cut short, is refused rather than read.
    const std::vector<std::uint8_t> looping{0, 0, 0x84, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0xC0, 12, 0, 12, 0, 1};
    require(!discovery::decode(looping.data(), looping.size()).has_value(),
            "a name pointing at itself is refused");
    require(!discovery::decode(compressed.data(), 20U).has_value(), "a short packet is refused");
}

bool engines_are_found() {
    // A service of its own, so a real engine on this network is not mixed in.
    const auto service = "_melody-test-" +
                         trackknife::core::StableId::random().to_string().substr(0, 8) + "._tcp";
    auto browser = discovery::Browser::start({}, service);
    if (!browser) {
        std::cerr << "discovery: " << browser.error().message << "; skipping\n";
        return false;
    }
    auto announcer = discovery::Announcer::start(
        discovery::Advertisement{.instance = "gemenon.test",
                                 .port = 6603,
                                 .txt = {{"id", "engine-1"}, {"auth", "1"}, {"http", "6604"}}},
        service);
    require(announcer.has_value(), "an engine announces itself");
    if (!eventually([&] { return !(*browser)->found().empty(); })) {
        std::cerr << "discovery: nothing heard -- no multicast here; skipping\n";
        return false;
    }
    const auto found = (*browser)->found().front();
    require(found.instance == "gemenon test", "found by its name (a dot cannot be in a label)");
    require(found.port == 6603, "with its port");
    require(!found.address.empty(), "and where it answered from");
    require(found.txt.at("id") == "engine-1" && found.txt.at("auth") == "1" &&
                found.txt.at("http") == "6604",
            "and what it says about itself");
    // Stopping says goodbye: gone at once, not when its records run out.
    announcer->reset();
    require(eventually([&] { return (*browser)->found().empty(); }, std::chrono::seconds{4}),
            "a stopped engine is gone from the list");
    return true;
}

// Each network hears the one address the engine has on it. gemenon once
// told the LAN its Docker bridge's address too, and a phone that took that
// one could not reach it. Heard as a listener on the group hears it.
bool each_network_hears_its_own_address() {
    const int listener = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    require(listener >= 0, "a socket opens");
    const int on = 1;
    static_cast<void>(::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)));
    static_cast<void>(::setsockopt(listener, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on)));
    sockaddr_in bound{};
    bound.sin_family = AF_INET;
    bound.sin_port = htons(5353);
    bound.sin_addr.s_addr = htonl(INADDR_ANY);
    ip_mreq membership{};
    ::inet_pton(AF_INET, "224.0.0.251", &membership.imr_multiaddr);
    membership.imr_interface.s_addr = htonl(INADDR_ANY);
    if (::bind(listener, reinterpret_cast<const sockaddr*>(&bound), sizeof(bound)) != 0 ||
        ::setsockopt(listener, IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership, sizeof(membership)) != 0) {
        ::close(listener);
        std::cerr << "discovery: cannot listen on the mDNS port; skipping\n";
        return false;
    }
    const auto service = "_melody-test-" +
                         trackknife::core::StableId::random().to_string().substr(0, 8) + "._tcp";
    auto announcer = discovery::Announcer::start(
        discovery::Advertisement{.instance = "addresses", .port = 6603, .txt = {}}, service);
    require(announcer.has_value(), "an engine announces itself");
    int heard = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
    std::array<std::uint8_t, 9000> buffer{};
    while (std::chrono::steady_clock::now() < deadline) {
        pollfd watched{.fd = listener, .events = POLLIN, .revents = 0};
        if (::poll(&watched, 1, 200) <= 0) {
            continue;
        }
        const auto size = ::recv(listener, buffer.data(), buffer.size(), 0);
        if (size <= 0) {
            continue;
        }
        const auto message = discovery::decode(buffer.data(), static_cast<std::size_t>(size));
        if (!message || !message->response ||
            std::ranges::none_of(message->answers, [&service](const discovery::Record& record) {
                return record.name == service + ".local";
            })) {
            continue;
        }
        const auto addresses = std::ranges::count_if(
            message->additionals,
            [](const discovery::Record& record) { return record.type == discovery::RecordType::a; });
        require(addresses == 1, "an announcement names one address, the one on its network");
        ++heard;
    }
    ::close(listener);
    if (heard == 0) {
        std::cerr << "discovery: no announcement heard -- no multicast here; skipping\n";
        return false;
    }
    return true;
}

// Someone asking just after someone else is answered at once: an engine
// multicasts its answer at most once a second, so a client starting a
// moment after another heard nothing until it asked again -- and melody-cli
// --engine NAME, waiting a little over a second, now and then gave up.
bool a_late_asker_is_answered_at_once() {
    const auto service = "_melody-test-" +
                         trackknife::core::StableId::random().to_string().substr(0, 8) + "._tcp";
    auto announcer = discovery::Announcer::start(
        discovery::Advertisement{.instance = "late", .port = 6603, .txt = {{"id", "engine-2"}}},
        service);
    require(announcer.has_value(), "an engine announces itself");
    // Past its first announcement, well before its second.
    std::this_thread::sleep_for(std::chrono::milliseconds{250});
    auto first = discovery::Browser::start({}, service);
    if (!first) {
        return false;
    }
    // However long it takes: this only tells a network without multicast.
    if (!eventually([&] { return !(*first)->found().empty(); }, std::chrono::seconds{3})) {
        std::cerr << "discovery: nothing heard -- no multicast here; skipping\n";
        return false;
    }
    auto second = discovery::Browser::start({}, service);
    require(second.has_value(), "a second browser starts");
    require(eventually([&] { return !(*second)->found().empty(); }, std::chrono::milliseconds{400}),
            "one asking right after another is answered at once, not a second later");
    return true;
}

} // namespace

int main() {
    codec_round_trips();
    static_cast<void>(each_network_hears_its_own_address());
    const bool networked = engines_are_found() && a_late_asker_is_answered_at_once();
    std::cout << "discovery: " << (networked ? "2" : "1") << " scenarios\n";
    return EXIT_SUCCESS;
}

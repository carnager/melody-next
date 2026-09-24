// SPDX-License-Identifier: GPL-3.0-only

// Engines announcing themselves on multicast DNS, and being found.

#include "trackknife/core/stable_id.hpp"
#include "trackknife/discovery/dns.hpp"
#include "trackknife/discovery/mdns.hpp"

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
                              const std::chrono::seconds patience = std::chrono::seconds{8}) {
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

} // namespace

int main() {
    codec_round_trips();
    const bool networked = engines_are_found();
    std::cout << "discovery: " << (networked ? "2" : "1") << " scenarios\n";
    return EXIT_SUCCESS;
}

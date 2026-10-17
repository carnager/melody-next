// SPDX-License-Identifier: GPL-3.0-only

// ADR-0286: whom an engine plays for -- the engines found, the [engine NAME]
// sections, and an address used only while the network does not find it.

#include "agent/guests.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using trackknife::agent::GuestsConfig;
using trackknife::agent::ListedEngine;
using trackknife::agent::plan_guests;
using trackknife::agent::Wanted;
using trackknife::discovery::Found;

int failures = 0;

void check(const bool condition, const std::string_view expression, const int line) {
    if (!condition) {
        std::cerr << "line " << line << ": check failed: " << expression << '\n';
        ++failures;
    }
}

#define CHECK(expression) check((expression), #expression, __LINE__)

[[nodiscard]] Found engine(const std::string& name, const std::string& id,
                           const std::string& address) {
    return Found{.instance = name,
                 .address = address,
                 .port = 6603,
                 .txt = {{"id", id}, {"proto", "1"}, {"auth", "1"}}};
}

[[nodiscard]] GuestsConfig config() {
    return GuestsConfig{.name = "caprica",
                        .password = "shared",
                        .music_root = std::filesystem::path{"/mnt/nas"},
                        .own_id = "caprica-id",
                        .already = {},
                        .stream = {}};
}

[[nodiscard]] ListedEngine listed(const std::string& name) {
    return ListedEngine{
        .name = name, .address = {}, .music_root = {}, .stream = {}, .password = {}, .play = true};
}

[[nodiscard]] const Wanted* named(const std::vector<Wanted>& plan, const std::string& name) {
    for (const auto& wanted : plan) {
        if (wanted.name == name) {
            return &wanted;
        }
    }
    return nullptr;
}

void playsForWhatIsFound() {
    const std::vector found{engine("gemenon", "gemenon-id", "192.168.0.200"),
                            engine("caprica", "caprica-id", "192.168.0.10"),
                            engine("gemenon", "gemenon-id", "10.8.0.1")};
    const auto plan = plan_guests(config(), found, true);
    // Itself not; an engine heard twice once.
    CHECK(plan.size() == 1U);
    const auto* gemenon = named(plan, "gemenon");
    CHECK(gemenon != nullptr);
    if (gemenon != nullptr) {
        CHECK(gemenon->identity == "gemenon-id");
        CHECK(gemenon->where == "192.168.0.200:6603");
        CHECK(gemenon->agent.server.token == "shared");
        CHECK(gemenon->agent.name == "caprica");
        CHECK(gemenon->agent.music_root == std::filesystem::path{"/mnt/nas"});
        CHECK(!gemenon->agent.stream_only);
    }

    // Without a password, an engine that wants one is not played for.
    auto bare = config();
    bare.password.clear();
    CHECK(plan_guests(bare, found, true).empty());

    // The top-level agent off: found engines are not played for.
    auto off = config();
    off.play_found = false;
    CHECK(plan_guests(off, found, true).empty());
}

void listedEnginesAreTheirOwn() {
    auto with = config();
    auto kitchen = listed("kitchen");
    kitchen.play = false;
    auto gemenon = listed("gemenon");
    gemenon.music_root = std::filesystem::path{"/mnt/gemenon"};
    gemenon.password = "its own";
    gemenon.stream = trackknife::agent::StreamChoice{.nearby = {}, .away = {}};
    with.listed = {kitchen, gemenon};
    const std::vector found{engine("gemenon", "gemenon-id", "192.168.0.200"),
                            engine("kitchen", "kitchen-id", "192.168.0.30"),
                            engine("bedside", "bedside-id", "192.168.0.31")};
    const auto plan = plan_guests(with, found, true);
    CHECK(plan.size() == 2U);
    // agent = no: left out.
    CHECK(named(plan, "kitchen") == nullptr);
    CHECK(named(plan, "bedside") != nullptr);
    const auto* played = named(plan, "gemenon");
    CHECK(played != nullptr);
    if (played != nullptr) {
        CHECK(played->agent.music_root == std::filesystem::path{"/mnt/gemenon"});
        CHECK(played->agent.server.token == "its own");
        CHECK(!played->agent.stream.away);
    }
    // Not listed and the top-level agent off, a listed one still plays.
    with.play_found = false;
    const auto only = plan_guests(with, found, true);
    CHECK(only.size() == 1U && named(only, "gemenon") != nullptr);
}

void anAddressIsTheFallback() {
    auto with = config();
    auto gemenon = listed("gemenon");
    gemenon.address =
        trackknife::protocol::Endpoint{.socket = {}, .host = "10.8.0.1", .port = 6603, .token = {}};
    with.listed = {gemenon};

    // Not found, and the network has had its time: reached by address, with
    // the shared password.
    auto plan = plan_guests(with, {}, true);
    CHECK(plan.size() == 1U);
    if (plan.size() == 1U) {
        CHECK(plan[0].identity == "address:gemenon");
        CHECK(plan[0].where == "10.8.0.1:6603");
        CHECK(plan[0].agent.server.token == "shared");
    }
    // Not yet: the network may still find it.
    CHECK(plan_guests(with, {}, false).empty());
    // Found: played for as found, not by its address too.
    plan = plan_guests(with, {engine("gemenon", "gemenon-id", "192.168.0.200")}, true);
    CHECK(plan.size() == 1U && plan[0].identity == "gemenon-id" &&
          plan[0].where == "192.168.0.200:6603");
    // Found and left out (agent = no): not reached by address either.
    with.listed[0].play = false;
    CHECK(plan_guests(with, {}, true).empty());
    // The top-level agent off does not stop a listed address.
    with.listed[0].play = true;
    with.play_found = false;
    CHECK(plan_guests(with, {}, true).size() == 1U);
}

void aSectionNamedAfterItsAddress() {
    // Found under its own name at the address a section names: that
    // section's, and not reached by address as well.
    auto with = config();
    auto host = listed("192.168.0.200");
    host.address = trackknife::protocol::Endpoint{
        .socket = {}, .host = "192.168.0.200", .port = 6603, .token = {}};
    host.where = "192.168.0.200:6603";
    host.music_root = std::filesystem::path{"/mnt/gemenon"};
    with.listed = {host};
    const auto plan = plan_guests(with, {engine("gemenon", "gemenon-id", "192.168.0.200")}, true);
    CHECK(plan.size() == 1U);
    if (plan.size() == 1U) {
        CHECK(plan[0].identity == "gemenon-id");
        CHECK(plan[0].agent.music_root == std::filesystem::path{"/mnt/gemenon"});
    }
}

void alreadyPlayedForIsNotTwice() {
    auto with = config();
    with.already = {"10.8.0.1:6603"};
    CHECK(plan_guests(with, {engine("gemenon", "gemenon-id", "10.8.0.1")}, true).empty());
}

} // namespace

int main() {
    playsForWhatIsFound();
    listedEnginesAreTheirOwn();
    anAddressIsTheFallback();
    aSectionNamedAfterItsAddress();
    alreadyPlayedForIsNotTwice();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

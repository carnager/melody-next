// SPDX-License-Identifier: GPL-3.0-only

#include "agent/guests.hpp"

#include <netdb.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include <algorithm>
#include <array>
#include <iostream>
#include <set>
#include <utility>

namespace trackknife::agent {
namespace {

// "host:port" with the host as an IPv4 address, so a name and the address
// it stands for are recognised as one engine.
[[nodiscard]] std::string numeric(const std::string& endpoint) {
    const auto colon = endpoint.rfind(':');
    if (colon == std::string::npos) {
        return endpoint;
    }
    const auto host = endpoint.substr(0, colon);
    addrinfo hints{};
    hints.ai_family = AF_INET;
    addrinfo* found = nullptr;
    if (::getaddrinfo(host.c_str(), nullptr, &hints, &found) != 0 || found == nullptr) {
        return endpoint;
    }
    std::array<char, INET_ADDRSTRLEN> text{};
    const auto* address = reinterpret_cast<const sockaddr_in*>(found->ai_addr);
    ::inet_ntop(AF_INET, &address->sin_addr, text.data(), text.size());
    ::freeaddrinfo(found);
    return std::string{text.data()} + endpoint.substr(colon);
}

} // namespace

Guests::Guests(Config config, SpeakerArbiter& arbiter)
    : config_(std::move(config)), arbiter_(&arbiter) {
    for (auto& endpoint : config_.already) {
        endpoint = numeric(endpoint);
    }
}

Guests::~Guests() { stop(); }

bool Guests::start() {
    auto browser =
        discovery::Browser::start([this](const std::vector<discovery::Found>& found) { update(found); });
    if (!browser) {
        std::cerr << "melody: cannot look for engines: " << browser.error().message << "\n";
        return false;
    }
    browser_ = std::move(*browser);
    return true;
}

void Guests::stop() {
    browser_.reset();
    const std::lock_guard guard{mutex_};
    for (auto& [where, guest] : guests_) {
        arbiter_->remove_guest(*guest.agent);
        guest.agent->stop();
    }
    guests_.clear();
}

std::vector<std::string> Guests::engines() const {
    const std::lock_guard guard{mutex_};
    std::vector<std::string> names;
    for (const auto& [where, guest] : guests_) {
        names.push_back(guest.name);
    }
    return names;
}

void Guests::update(const std::vector<discovery::Found>& found) {
    std::set<std::string> present;
    const std::lock_guard guard{mutex_};
    for (const auto& engine : found) {
        const auto id = engine.txt.contains("id") ? engine.txt.at("id") : std::string{};
        const auto where = engine.address + ":" + std::to_string(engine.port);
        if ((!config_.own_id.empty() && id == config_.own_id) ||
            std::ranges::contains(config_.already, where)) {
            continue;
        }
        present.insert(where);
        if (guests_.contains(where)) {
            continue;
        }
        const bool wants_password = engine.txt.contains("auth") && engine.txt.at("auth") == "1";
        if (wants_password && config_.password.empty()) {
            std::cerr << "melody: " << engine.instance
                      << " wants a password; not playing for it\n";
            continue;
        }
        auto audition = audio::LocalAuditionService::create();
        if (!audition) {
            std::cerr << "melody: no audio here to play " << engine.instance
                      << " on: " << audition.error().message << "\n";
            continue;
        }
        static_cast<void>((*audition)->refresh_output_devices());
        auto agent = Agent::create(
            AgentConfig{.server = protocol::Endpoint{.socket = {},
                                                     .host = engine.address,
                                                     .port = engine.port,
                                                     .token = wants_password ? config_.password
                                                                             : std::string{}},
                        .name = config_.name,
                        .music_root = config_.music_root,
                        .stream_only = !config_.music_root.has_value()},
            std::move(*audition));
        if (!agent) {
            std::cerr << "melody: cannot play for " << engine.instance << ": "
                      << agent.error().message << "\n";
            continue;
        }
        (*agent)->start();
        arbiter_->add_guest(engine.instance, **agent);
        std::cerr << "melody: playing for " << engine.instance << " at " << where << "\n";
        guests_.emplace(where, Guest{.name = engine.instance, .agent = std::move(*agent)});
    }
    for (auto guest = guests_.begin(); guest != guests_.end();) {
        if (present.contains(guest->first)) {
            ++guest;
            continue;
        }
        std::cerr << "melody: " << guest->second.name << " is gone\n";
        arbiter_->remove_guest(*guest->second.agent);
        guest->second.agent->stop();
        guest = guests_.erase(guest);
    }
}

} // namespace trackknife::agent
